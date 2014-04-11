#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <math.h>

#include "ccmpred.h"
#include "sequence.h"
#include "io.h"
#include "conjugrad.h"
#include "util.h"
#include "parseopt.h"

#ifdef CUDA
#include <cuda.h>
#include <cuda_runtime_api.h>
#include "evaluate_cuda.h"
#endif

#ifdef OPENMP
#include <omp.h>
#include "evaluate_cpu_omp.h"
#endif

#ifdef CURSES
#include <unistd.h>
#include <curses.h>
#include <term.h>
#endif

#include "evaluate_cpu.h"

/** Callback for LBFGS optimization to show progress
 * @param[in] instance The user data passed to the LBFGS optimizer
 * @param[in] x The current variable assignments
 * @param[in] g The current gradients
 * @param[in] fx The current negative log-likelihood value
 * @param[in] xnorm The euclidean norm of the variables
 * @param[in] gnorm The euclidean norm of the gradients
 * @param[in] step The step size for the current iteration
 * @param[in] nvar The number of variables
 * @param[in] k The number of the current iteration
 * @param[in] ls The number of evaluations called for the current iteration
 */
static int progress(
	void *instance,
	const conjugrad_float_t *x,
	const conjugrad_float_t *g,
	const conjugrad_float_t fx,
	const conjugrad_float_t xnorm,
	const conjugrad_float_t gnorm,
	const conjugrad_float_t step,
	int n,
	int k,
	int ls
) {
	//printf("iter\teval\tf(x)    \t║x║     \t║g║     \tstep\n");
	printf("%-4d\t%-4d\t%-8g\t%-8g\t%-8.8g\t%-3.3g\n", k, ls, fx, xnorm, gnorm, step);
	return 0;
}

/** Init the pairwise emission potentials with single emission potentials as observed in the MSA
 * @param[out] x The matrix to initialize
 * @param[in] ncol The number of columns in the MSA (i.e. L)
 * @param[in] nrow The number of rows in the MSA (i.e. N)
 * @param[in] msa The MSA to read column frequencies from
 */
void init_bias(conjugrad_float_t *x, userdata *ud) {

	int ncol = ud->ncol;
	int nrow = ud->nrow;
	int nsingle = ncol * (N_ALPHA - 1);
	int nsingle_padded = nsingle + N_ALPHA_PAD - (nsingle % N_ALPHA_PAD);
	int nvar_padded = nsingle_padded + ncol * ncol * N_ALPHA * N_ALPHA_PAD;

	conjugrad_float_t *x1 = x;

	//memset(x, 0, sizeof(conjugrad_float_t) * ud->nvar);
	memset(x, 0, sizeof(conjugrad_float_t) * nvar_padded);

	for(int j = 0; j < ncol; j++) {

		int aacounts[N_ALPHA];
		for(int a = 0; a < N_ALPHA; a++) {
			// init with pseudocounts
			aacounts[a] = 1;
		}

		// calculate weights for column
		for(int i = 0; i < nrow; i++) {
			aacounts[X(i,j)]++;
		}

		int aasum = nrow + N_ALPHA;

		conjugrad_float_t aafrac[N_ALPHA];
		for(int a = 0; a < N_ALPHA; a++) {
			aafrac[a] = ((conjugrad_float_t)aacounts[a]) / aasum;
		}

		// we set the weights of the other amino acids relative to the gap column at index 20 (to avoid degeneracy)
		conjugrad_float_t aanorm = flog(aafrac[20]);
		for(int a = 0; a < N_ALPHA - 1; a++) {
			V(j, a) = flog( aafrac[a] ) - aanorm;
		}

	}

}

#ifdef CURSES
/**
 * Detect if we can generate colored output
 */
bool detect_colors() {
	int erret = 0;
	if(setupterm(NULL, 1, &erret) == ERR) {
		return false;
	}

	// colorize if the terminal supports colors and we're writing to a terminal
	return has_colors() && isatty(STDOUT_FILENO);
}
#endif

void logo(bool color) {


	if(color) {
		printf(" _____ _____ _____               _ \n");
		printf("|\x1b[30;42m     |     |     |\x1b[0m___ ___ ___ _|\x1b[30;44m |\x1b[0m\n");
		printf("|\x1b[30;42m   --|   --| | | |\x1b[44m . |  _| -_| . |\x1b[0m\n");
		printf("|\x1b[30;42m_____|_____|_|_|_|\x1b[44m  _|_|\x1b[0m \x1b[30;44m|___|___|\x1b[0m version %s\n", __VERSION);
		printf("                  |\x1b[30;44m_|\x1b[0m\n\n");
	} else {
		printf(" _____ _____ _____               _ \n");
		printf("|     |     |     |___ ___ ___ _| |\n");
		printf("|   --|   --| | | | . |  _| -_| . |\n");
		printf("|_____|_____|_|_|_|  _|_| |___|___|\n");
		printf("                  |_|              \n\n");
	}
}

char* concat(char *s1, char *s2) {
	size_t l1 = strlen(s1);
	size_t l2 = strlen(s2);
	char *result = malloc(l1 + l2 + 1);
	if(result == NULL) {
		die("Cannot malloc new string!");
	}
	memcpy(result, s1, l1);
	memcpy(result+l1, s2, l2+1);
	return result;
}

/**
 * Print a pretty usage message
 * @param[in] exename The name of the executable
 * @param[in] long_usage The length of the usage message to display: 0: short usage, 1: usage and options
 */
void usage(char* exename, int long_usage) {
	printf("Usage: %s [options] input.aln output.mat\n\n", exename);

	if(long_usage) {
		printf("Options:\n");
#ifdef CUDA
		printf("\t-d DEVICE \tCalculate on CUDA device number DEVICE (set to -1 to use CPU) [default: 0]\n");
#endif
#ifdef OPENMP
		printf("\t-t THREADS\tCalculate using THREADS threads on the CPU (automatically disables CUDA if available) [default: 1]\n");
#endif
		printf("\t-n NUMITER\tCompute a maximum of NUMITER operations [default: 50]\n");
		printf("\t-r RAWFILE\tStore raw prediction matrix in RAWFILE\n");
		printf("\t-i INIFILE\tRead initial weights from INIFILE\n");
		printf("\t-w IDTHRES\tSet sequence reweighting identity threshold to IDTHRES [default: 0.8]\n");
		printf("\t-l LFACTOR\tSet pairwise regularization coefficients to LFACTOR * (L-1) [default: 0.2]\n");
		printf("\t-A        \tDisable average product correction (APC)\n");
		printf("\t-R        \tRe-normalize output matrix to [0,1]\n");
		printf("\t-h        \tDisplay help\n");
		printf("\n\n");
	}

	exit(1);
}

int main(int argc, char **argv)
{
	char *rawfilename = NULL;
	int numiter = 50;
	int use_apc = 1;
	int use_normalization = 0;
	conjugrad_float_t lambda_single = F001; // 0.01
	conjugrad_float_t lambda_pair = FInf;
	conjugrad_float_t lambda_pair_factor = F02; // 0.2

	parse_option *optList, *thisOpt;

	char *optstr;
	char *old_optstr = malloc(1);
	old_optstr[0] = 0;
	optstr = concat("r:i:n:w:l:ARh?", old_optstr);
	free(old_optstr);

#ifdef OPENMP
	int numthreads = 1;
	old_optstr = optstr;
	optstr = concat("t:", optstr);
	free(old_optstr);
#endif

#ifdef CUDA
	int use_def_gpu = 0;
	old_optstr = optstr;
	optstr = concat("d:", optstr);
	free(old_optstr);
#endif

	optList = parseopt(argc, argv, optstr);
	free(optstr);

	char* msafilename = NULL;
	char* matfilename = NULL;
	char* initfilename = NULL;

	conjugrad_float_t reweighting_threshold = F08; // 0.8

	while(optList != NULL) {
		thisOpt = optList;
		optList = optList->next;

		switch(thisOpt->option) {
#ifdef OPENMP
			case 't':
				numthreads = atoi(thisOpt->argument);

#ifdef CUDA
				use_def_gpu = -1; // automatically disable GPU if number of threads specified
#endif
				break;
#endif
#ifdef CUDA
			case 'd':
				use_def_gpu = atoi(thisOpt->argument);
				break;
#endif
			case 'r':
				rawfilename = thisOpt->argument;
				break;
			case 'i':
				initfilename = thisOpt->argument;
				break;
			case 'n':
				numiter = atoi(thisOpt->argument);
				break;
			case 'w':
				reweighting_threshold = (conjugrad_float_t)atof(thisOpt->argument);
				break;
			case 'l':
				lambda_pair_factor = (conjugrad_float_t)atof(thisOpt->argument);
				break;
			case 'A':
				use_apc = 0;
				break;
			case 'R':
				use_normalization = 1;
				break;
			case 'h':
			case '?':
				usage(argv[0], 1);
				break;

			case 0:
				if(msafilename == NULL) {
					msafilename = thisOpt->argument;
				} else if(matfilename == NULL) {
					matfilename = thisOpt->argument;
				} else {
					usage(argv[0], 0);
				}
				break;
			default:
				die("Unknown argument"); 
		}

		free(thisOpt);
	}

	if(msafilename == NULL || matfilename == NULL) {
		usage(argv[0], 0);
	}


	FILE *msafile = fopen(msafilename, "r");
	if( msafile == NULL) {
		printf("Cannot open %s!\n\n", msafilename);
		return 2;
	}

	int ncol, nrow;
	unsigned char* msa = read_msa(msafile, &ncol, &nrow);
	fclose(msafile);

	int nsingle = ncol * (N_ALPHA - 1);
	int nvar = nsingle + ncol * ncol * N_ALPHA * N_ALPHA;
	int nsingle_padded = nsingle + N_ALPHA_PAD - (nsingle % N_ALPHA_PAD);
	int nvar_padded = nsingle_padded + ncol * ncol * N_ALPHA * N_ALPHA_PAD;

#ifdef CURSES
	bool color = detect_colors();
#else
	bool color = false;
#endif

	logo(color);

#ifdef CUDA
	int num_devices, dev_ret;
	struct cudaDeviceProp prop;
	dev_ret = cudaGetDeviceCount(&num_devices);
	if(dev_ret != CUDA_SUCCESS) {
		num_devices = 0;
	}


	if(num_devices == 0) {
		printf("No CUDA devices available, ");
		use_def_gpu = -1;
	} else if (use_def_gpu < -1 || use_def_gpu >= num_devices) {
		printf("Error: %d is not a valid device number. Please choose a number between 0 and %d\n", use_def_gpu, num_devices - 1);
		exit(1);
	} else {
		printf("Found %d CUDA devices, ", num_devices);
	}

	if (use_def_gpu != -1) {
		cudaSetDevice(use_def_gpu);
		cudaGetDeviceProperties(&prop, use_def_gpu);
		printf("using device #%d: %s\n", use_def_gpu, prop.name);

		size_t mem_free, mem_total;
		cudaMemGetInfo(&mem_free, &mem_total);

		size_t mem_needed = nrow * ncol * 2 + // MSAs
		                    sizeof(conjugrad_float_t) * nrow * ncol * 2 + // PC, PCS
		                    sizeof(conjugrad_float_t) * nrow * ncol * N_ALPHA_PAD + // PCN
		                    sizeof(conjugrad_float_t) * nrow + // Weights
		                    (sizeof(conjugrad_float_t) * ((N_ALPHA - 1) * ncol + ncol * ncol * N_ALPHA * N_ALPHA_PAD)) * 4;

		setlocale(LC_NUMERIC, "");
		printf("Total GPU RAM:  %'15u\n", mem_total);
		printf("Free GPU RAM:   %'15u\n", mem_free);
		printf("Needed GPU RAM: %'15u ", mem_needed);

		if(mem_needed <= mem_free) {
			printf("✓\n");
		} else {
			printf("⚠\n");
		}


	} else {
		printf("using CPU");

#ifdef OPENMP
		printf(" (%d thread(s))", numthreads);
#endif
		printf("\n");

	}
#else // CUDA
	printf("using CPU");
#ifdef OPENMP
	printf(" (%d thread(s))\n", numthreads);
#endif // OPENMP
	printf("\n");
#endif // CUDA

	conjugrad_float_t *x = conjugrad_malloc(nvar_padded);
	if( x == NULL) {
		die("ERROR: Not enough memory to allocate variables!");
	}
	memset(x, 0, sizeof(conjugrad_float_t) * nvar_padded);

	// Auto-set lambda_pair
	if(isnan(lambda_pair)) {
		lambda_pair = lambda_pair_factor * (ncol - 1);
	}

	// fill up user data struct for passing to evaluate
	userdata *ud = (userdata *)malloc( sizeof(userdata) );
	if(ud == 0) { die("Cannot allocate memory for user data!"); }
	ud->msa = msa;
	ud->ncol = ncol;
	ud->nrow = nrow;
	ud->nsingle = nsingle;
	ud->nvar = nvar;
	ud->lambda_single = lambda_single;
	ud->lambda_pair = lambda_pair;
	ud->weights = conjugrad_malloc(nrow);
	ud->reweighting_threshold = reweighting_threshold;

	if(initfilename == NULL) {
		// Initialize emissions to pwm
		init_bias(x, ud);
	} else {
		// Load potentials from file
		read_raw(initfilename, ud, x);
	}

	// optimize with default parameters
	conjugrad_parameter_t *param = conjugrad_init();

	param->max_iterations = numiter;
	param->max_linesearch = 5;
	param->alpha_mul = F05;
	param->ftol = 1e-4;
	param->wolfe = F02;


	int (*init)(void *) = init_cpu;
	int (*destroy)(void *) = destroy_cpu;
	conjugrad_evaluate_t evaluate = evaluate_cpu;

#ifdef OPENMP
	omp_set_num_threads(numthreads);
	if(numthreads > 1) {
		init = init_cpu_omp;
		destroy = destroy_cpu_omp;
		evaluate = evaluate_cpu_omp;
	}
#endif

#ifdef CUDA
	if(use_def_gpu != -1) {
		init = init_cuda;
		destroy = destroy_cuda;
		evaluate = evaluate_cuda;
	}
#endif

	init(ud);

	printf("\nWill optimize %d %ld-bit variables\n\n", nvar, sizeof(conjugrad_float_t) * 8);

	if(color) { printf("\x1b[1m"); }
	printf("iter\teval\tf(x)    \t║x║     \t║g║     \tstep\n");
	if(color) { printf("\x1b[0m"); }


	conjugrad_float_t fx;
	int ret;
#ifdef CUDA
	if(use_def_gpu != -1) {
		conjugrad_float_t *d_x;
		cudaError_t err = cudaMalloc((void **) &d_x, sizeof(conjugrad_float_t) * nvar_padded);
		if (cudaSuccess != err) {
			printf("CUDA error No. %d while allocation memory for d_x\n", err);
			exit(1);
		}
		err = cudaMemcpy(d_x, x, sizeof(conjugrad_float_t) * nvar_padded, cudaMemcpyHostToDevice);
		if (cudaSuccess != err) {
			printf("CUDA error No. %d while copying parameters to GPU\n", err);
			exit(1);
		}
		ret = conjugrad_gpu(nvar_padded, d_x, &fx, evaluate, progress, ud, param);
		err = cudaMemcpy(x, d_x, sizeof(conjugrad_float_t) * nvar_padded, cudaMemcpyDeviceToHost);
		if (cudaSuccess != err) {
			printf("CUDA error No. %d while copying parameters back to CPU\n", err);
			exit(1);
		}
		err = cudaFree(d_x);
		if (cudaSuccess != err) {
			printf("CUDA error No. %d while freeing memory for d_x\n", err);
			exit(1);
		}
	} else {
	ret = conjugrad(nvar_padded, x, &fx, evaluate, progress, ud, param);
	}
#else
	ret = conjugrad(nvar_padded, x, &fx, evaluate, progress, ud, param);
#endif


	printf("\n");
	printf("%s with status code %d - ", (ret < 0 ? "Exit" : "Done"), ret);

	if(ret == CONJUGRAD_SUCCESS) {
		printf("Success!\n");
	} else if(ret == CONJUGRAD_ALREADY_MINIMIZED) {
		printf("Already minimized!\n");
	} else if(ret == CONJUGRADERR_MAXIMUMITERATION) {
		printf("Maximum number of iterations reached.\n");
	} else {
		printf("Unknown status code! Check the libLBFGS manual!\n");
	}

	printf("\nFinal fx = %f\n\n", fx);

	FILE* out = fopen(matfilename, "w");
	if(out == NULL) {
		printf("Cannot open %s for writing!\n\n", matfilename);
		return 3;
	}

	conjugrad_float_t outmat[ncol * ncol];

	if(rawfilename != NULL) {
		printf("Writing raw output to %s\n", rawfilename);
		FILE* rawfile = fopen(rawfilename, "w");

		if(rawfile == NULL) {
			printf("Cannot open %s for writing!\n\n", rawfilename);
			return 4;
		}

		write_raw(rawfile, x, ncol);

		fclose(rawfile);
	}

	sum_submatrices(x, outmat, ncol);

	if(use_apc) {
		apc(outmat, ncol);
	}

	if(use_normalization) {
		normalize(outmat, ncol);
	}

	write_matrix(out, outmat, ncol, ncol);

	fflush(out);
	fclose(out);

	destroy(ud);

	conjugrad_free(x);
	conjugrad_free(ud->weights);
	free(ud);
	free(msa);
	free(param);
	
	printf("Output can be found in %s\n", matfilename);

	return 0;
}
