#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/**
  CONFIGURATION
 **/
// maximum number of allowed features
#define MAX_NUM_FEAT 1000000
// negative class label (default is -1 for SVM-Light formatted files)
#define NEGATIVE -1
// either use log2 (exact) or approxLn2 (approximate)

/**
  STRUCTURES
 **/
struct test_node {
    int nexmpls;
    int *exmpls;
    int npos;
    int nneg;

    int test;
	float threshold; // if less than or equal then left else right
    int nchildren;
    struct test_node *children;
};
typedef struct test_node test;

#define error(msg) { printf("%s at line %d.\n", msg, __LINE__); exit(-1); }
#define TRUE 1
#define FALSE 0

// variables to be used throughout
char *train_fn, *test_fn;
int num_trees = 500;
int num_threads = 0;
int num_feat = -1, num_feat_tpn = -1;
int num_train = 0;
int *feat_not_zero_c;
int *targets;
int *raw_data;
int **data;
int temp;

// for multithreading
float *predictions;

// function prototypes
void *RF();
void FreeTree(test *t);
float runTree(test *t, float *features);
void readRecursive(test* root, FILE* fp);
test* readModel(const char* name, int *nf);


/**
  LOG APPROXIMATION ( http://www.musicdsp.org/showone.php?id=91 )
 **/
//float approxLn2( float f ) {
//    assert( f > 0. );
//    assert( sizeof(f) == sizeof(int) );
//    assert( sizeof(f) == 4 );
//    int i = (*(int *)&f);
//    return (((i&0x7f800000)>>23)-0x7f)+(i&0x007fffff)/(float)0x800000;
//}

/**
  MAIN PROGRAM
 **/
int main(int argc, char **argv)
{

    int c;
    opterr = 0;
    while ((c = getopt (argc, argv, "t:")) != -1)
        switch (c)
        {
            case 't':
                num_trees = atoi(optarg);
                break;
            default:
                error("usage: sparse_RF [-t trees] test\n");
        }

    if  (optind >= argc)
        error("usage: sparse_RF [-t trees] test\n");
    test_fn = argv[optind];

    /*********************/
    int i;
    int feature, read_class, last_feature;
    float value;

    // test file
    FILE *test_fp = fopen(test_fn, "r");
    if (test_fp == NULL) error("Error reading test data at line %i.\n");
 
	// calculate test set size
	int num_test = 0;
	read_class = TRUE;
	last_feature = -1;
	for (i = 1; !feof(test_fp); ) {
		char str[2];

		fscanf(test_fp, "%*[ \t]");

		if (fscanf(test_fp, "%1[\n\r]", &str[0]) == 1) {
			read_class = TRUE;
			++i;

			// newline
		} else if (read_class && fscanf(test_fp, "%i", &feature) == 1) {
			read_class = FALSE;
			last_feature = -1;

			// read in class (into "feature")
			++num_test;
		} else if (fscanf(test_fp, "%1[#]", &str[0]) == 1)  {
			while (fscanf(test_fp, "%1[^\n\r]", &str[0]) == 1); // read up until end-of-line
		} else if (fscanf(test_fp, "%i:%f", &feature, &value) == 2 && --feature > last_feature) {
			last_feature = feature;

			// read in feature:value pair
		} else if (!feof(test_fp)) {
			char e[80];
			sprintf(e, "Error reading line %i of test data.\n", i);
			error(e);
		}
	}
    if (num_test == 0)
        error("Test set has no examples.\n");

    predictions = (float *)calloc(num_test, sizeof(float));
    if (predictions == NULL) error("Unable to allocate memory at line %i.\n");

    RF();

    // output final predictions
    for (i = 0; i < num_test; ++i) {
        printf("%f\n", predictions[i] / num_trees);
    }

    free(predictions);
    fclose(test_fp);
    free(data);
    free(raw_data);
    free(targets);
    free(feat_not_zero_c);
    return 0;
}

void *RF()
{
    int i, j;
    float value;
    int feature, read_class, last_feature;

    // test file
    FILE *test_fp = fopen(test_fn, "r");
    if (test_fp == NULL) error("Error reading test data at line %i.\n");

    // setup variables used in all trees
    test *root=0;
    num_feat=MAX_NUM_FEAT;
    float *example = malloc(num_feat * sizeof(float));
    if (example == NULL) error("Unable to allocate memory at line %i.\n");

    int tree;

    for (tree = 0; tree < num_trees; ++tree) {
        // reset stuff from last iteration
        rewind(test_fp);

        // initialize random seed
        srand(time(NULL));

        // initialize first (root) test node
        char buf[128];
        sprintf(buf,"tree.%d",tree);
        root=readModel(buf,&num_feat);

		// read test file line-by-line and output predictions
		read_class = TRUE;
		last_feature = -1;
		for (i = 1, j = -1; !feof(test_fp); ) {
			char str[2];

			fscanf(test_fp, "%*[ \t]");

			if (fscanf(test_fp, "%1[\n\r]", &str[0]) == 1) {
				read_class = TRUE;
				++i;

				// newline
			} else if (read_class && fscanf(test_fp, "%i", &feature) == 1) {
				read_class = FALSE;
				last_feature = -1;

				// read in class (into "feature")
				if (++j != 0) {
					float p = runTree(root, example);
					predictions[j - 1] += p;
				}

				// zero out (i.e. make default) feature list
				int k;
				for (k = 0; k < num_feat; ++k) {
					example[k] = 0;
				}
			} else if (fscanf(test_fp, "%1[#]", &str[0]) == 1) {
				while (fscanf(test_fp, "%1[^\n\r]", &str[0]) == 1); // read up until end-of-line
			} else if (fscanf(test_fp, "%i:%f", &feature, &value) == 2 && --feature > last_feature) {
				last_feature = feature;

				// read in feature:value pair
				if (feature >= num_feat) // disregard features we didn't see in training
					continue;

				example[feature] = value;
			} else if (!feof(test_fp)) {
				char e[80];
				sprintf(e, "Error reading line %i of test data.\n", i);
				error(e);
			}
		}
		if (j != -1) {
			float p = runTree(root, example);
			predictions[j] += p;
		}

		FreeTree(root);
		// don't free root->exmpls

		fprintf(stderr, "tree %i done\n", tree);
	}

//	free(root->exmpls);
	free(root);
	free(example);
	fclose(test_fp);
	return NULL;
}

// recursively frees the tree from memory
void FreeTree(test *t)
{
	int i;
	if (t->nchildren == 0)
		return;
	for (i = 0; i < t->nchildren; ++i) {
//		free(t->children[i].exmpls);
		FreeTree(&(t->children[i]));
	}
	free(t->children);
	t->children=0;
}

// returns the prediction of tree with root t on the case corresponding to features
float runTree(test *t, float *features)
{
	if (t->test == -1) {
		if (t->npos == 0)
			return 0;
		else if (t->nneg == 0)
			return 1;
		else
			return (t->npos / (float)(t->npos + t->nneg));
	}

	return runTree(&(t->children[features[t->test] > t->threshold]), features);
}

void readRecursive (test* root, FILE* fp){
    int i;
    fscanf(fp,"%d%d%f",&(root->test),&(root->nchildren),&(root->threshold));
    if(root->test<0)
        fscanf(fp, "%d%d",&(root->npos),&(root->nneg));
    else{
        root->children=malloc(sizeof(test)*root->nchildren);
        for(i=0; i<root->nchildren; i++)
            readRecursive (&(root->children[i]), fp);
    }
}

test* readModel(const char* name, int* nf){
    FILE* fp=fopen(name,"r");
    fscanf(fp, "%d",nf);
    test* root=malloc(sizeof(test));
    readRecursive(root,fp);
    if(fscanf(fp,"%*s")!=EOF)
        fprintf(stderr,"Garbage at the end of tree file %s\n", name);
    return root;
}

