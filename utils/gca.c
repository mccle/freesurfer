#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "mri.h"
#include "error.h"
#include "diag.h"
#include "utils.h"
#include "gca.h"
#include "proto.h"
#include "fio.h"
#include "transform.h"
#include "macros.h"
#include "utils.h"
#include "cma.h"
#include "flash.h"

int Ggca_label = -1 ;
int Ggca_x = -1 ;
int Ggca_y = -1 ;
int Ggca_z = -1 ;

/* this is the hack section */
double PRIOR_FACTOR = 0.1 ;
#define LABEL_UNDETERMINED   255

static int total_pruned = 0 ;

#define MIN_DET 1e-7   /* less than this, and the covariance matrix is poorly conditioned */

#define MAX_LABELS_PER_GCAN         5
#define MAX_DIFFERENT_LABELS        500
#define MIN_VAR                     (5*5)   /* should make this configurable */
#define BIG_AND_NEGATIVE            -10000000.0
#define VERY_UNLIKELY               1e-10
#define UNKNOWN_DIST                4  /* within 4 mm of some known label */
#define GCA_OLD_VERSION             2.0
#define GCA_VERSION                 4.0
#define DEFAULT_MAX_LABELS_PER_GCAN 4

static double gcaComputeSampleConditionalLogDensity(GCA_SAMPLE *gcas, float *vals, int ninputs, int label) ;
static VECTOR *load_sample_mean_vector(GCA_SAMPLE *gcas, VECTOR *v_means, int ninputs) ;
static MATRIX *load_sample_covariance_matrix(GCA_SAMPLE *gcas, MATRIX *m_cov, int ninputs) ;
static double sample_covariance_determinant(GCA_SAMPLE *gcas, int ninputs) ;
static GC1D *findClosestValidGC(GCA *gca, int x, int y, int z, int label, int check_var) ;

static int    MRIorderIndices(MRI *mri, short *x_indices, short *y_indices, 
                              short *z_indices) ;
static int gcaCheck(GCA *gca) ;
static double gcaVoxelGibbsLogLikelihood(GCA *gca, MRI *mri_labels, 
                                         MRI *mri_inputs, int x, int y, int z, 
                                         TRANSFORM *transform, double gibbs_coef) ;
static int copy_gcs(int nlabels, GC1D *gcs_src, GC1D *gcs_dst, int ninputs) ;
static int free_gcs(GC1D *gcs, int nlabels, int ninputs) ;
static GC1D *alloc_gcs(int nlabels, int flags, int ninputs) ;
static double gcaNbhdGibbsLogLikelihood(GCA *gca, MRI *mri_labels, 
                                        MRI *mri_inputs, int x, int y, int z, 
                                        TRANSFORM *transform, double gibbs_coef) ;
static double gcaGibbsImpossibleConfiguration(GCA *gca, MRI *mri_labels, 
                                              int x, int y, int z, TRANSFORM *transform) ;
static GCA_SAMPLE *gcaExtractLabelAsSamples(GCA *gca, MRI *mri_labeled, 
                                            TRANSFORM *transform, 
                                            int *pnsamples, int label) ;
#if 0
static GCA_SAMPLE *gcaExtractRegionLabelAsSamples(GCA *gca, MRI *mri_labeled, 
                                                  TRANSFORM *transform, 
                                                  int *pnsamples, int label,
                                                  int xn, int yn, int zn, 
                                                  int wsize) ;
#endif
static GCA_SAMPLE *gcaExtractThresholdedRegionLabelAsSamples(GCA *gca, MRI *mri_labeled, 
                                                             TRANSFORM *transform, 
                                                             int *pnsamples, int label,
                                                             int xn, int yn, int zn, 
                                                             int wsize, float pthresh) ;
/*static double gcaComputeSampleConditionalDensity(GCA_SAMPLE *gcas, float *vals, int ninputs, int label) ;*/
static double gcaComputeLogDensity(GC1D *gc, float *vals, int ninputs, float prior, int label) ;
static double gcaComputeSampleLogDensity(GCA_SAMPLE *gcas, float *vals, int ninputs) ;
int MRIcomputeVoxelPermutation(MRI *mri, short *x_indices, short *y_indices,
                               short *z_indices);
GCA *gcaAllocMax(int ninputs, float prior_spacing, float node_spacing, int width, int height, int depth, 
                 int max_labels, int flags) ;
static int GCAupdateNode(GCA *gca, MRI *mri, int xn, int yn, int zn,
                         float *vals,int label, GCA *gca_prune, int noint);
static int GCAupdateNodeCovariance(GCA *gca, MRI *mri, int xn, int yn, int zn,
																	 float *vals,int label);
static int GCAupdatePrior(GCA *gca, MRI *mri, int xn, int yn, int zn,int label);
static int GCAupdateNodeGibbsPriors(GCA *gca,MRI*mri,int xn,int yn,int zn,
                              int x, int y, int z, int label);
#if 0
static int different_nbr_labels(GCA *gca, int x, int y, int z, int wsize,
                                 int label, float pthresh) ;
#endif
static int different_nbr_max_labels(GCA *gca, int x, int y, int z, int wsize,
																		int label) ;
static int gcaRegionStats(GCA *gca, int x0, int y0, int z0, int wsize,
                   float *priors, float *vars, float means[MAX_DIFFERENT_LABELS][MAX_GCA_INPUTS]) ;
static int gcaFindBestSample(GCA *gca, int x, int y, int z, int best_label,
                             int wsize, GCA_SAMPLE *gcas);
#if 0
static int gcaFindClosestMeanSample(GCA *gca, float *means, float min_prior,
                                    int x, int y, int z, 
                                    int label, int wsize, GCA_SAMPLE *gcas);
static GC1D *gcaFindHighestPriorGC(GCA *gca, int x, int y, int z,int label,
                                 int wsize) ;
#endif
static double gcaGibbsImageLogLikelihood(GCA *gca, MRI *mri_labels, 
                                         MRI *mri_inputs, TRANSFORM *transform) ;

static int mriFillRegion(MRI *mri, int x,int y,int z,int fill_val,int whalf);
static int gcaFindMaxPriors(GCA *gca, float *max_priors) ;
static int gcaFindIntensityBounds(GCA *gca, float *pmin, float *pmax) ;
static int dump_gcan(GCA *gca, GCA_NODE *gcan, FILE *fp, int verbose, GCA_PRIOR *gcap) ;
static GCA_NODE *findSourceGCAN(GCA *gca, MRI *mri_src,TRANSFORM *transform,
                                int x,int y,int z);
static int gcaComputeLabelStats(GCA *gca, int target_label, float *pvar, float *means);
#if 0
static int getLabelMean(GCA_NODE *gcan, int label, float *pvar, float *means, int ninputs) ;
#endif
static int   borderVoxel(MRI *mri, int x, int y, int z) ;
static int   GCAmaxLikelihoodBorderLabel(GCA *gca, MRI *mri_inputs, 
                                         MRI *mri_labels, TRANSFORM *transform,
                                         int x, int y, int z, float min_ratio);


float getPrior(GCA_PRIOR *gcap, int label) ;
GCA_PRIOR *getGCAP(GCA *gca, MRI *mri, TRANSFORM *transform, int xv, int yv, int zv) ;
static int gcaNodeToPrior(GCA *gca, int xn, int yn, int zn, int *pxp, int *pyp, int *pzp) ;
static HISTOGRAM *gcaHistogramSamples(GCA *gca, GCA_SAMPLE *gcas, MRI *mri, 
                                      TRANSFORM *transform, int nsamples, 
                                      HISTOGRAM *histo, int frame) ;
int GCApriorToNode(GCA *gca, int xp, int yp, int zp, int *pxn, int *pyn, int *pzn) ;

/* arrays for indexing 6-connected neighbors */
static int xnbr_offset[] = { 1, -1, 0, 0,  0,  0} ;
static int ynbr_offset[] = { 0, 0,  1, -1, 0,  0} ;
static int znbr_offset[] = { 0, 0,  0, 0,  1, -1} ;
int check_finite(char *where, double what) ;

GCA_PRIOR *
getGCAP(GCA *gca, MRI *mri, TRANSFORM *transform, int xv, int yv, int zv)
{
  int       xp, yp, zp ;
  GCA_PRIOR *gcap ;

  GCAsourceVoxelToPrior(gca, mri, transform, xv, yv, zv, &xp, &yp, &zp) ;
  gcap = &gca->priors[xp][yp][zp] ;
  return(gcap) ;
}

float
getPrior(GCA_PRIOR *gcap, int label)
{
  int n ;

  for (n = 0 ; n < gcap->nlabels ; n++)
    if (gcap->labels[n] == label)
      break ;
  if (n >= gcap->nlabels)
  {
    if (gcap->total_training > 0)
      return(0.1f/(float)gcap->total_training) ; /* make it unlikely */
    else
      return(VERY_UNLIKELY) ;
  }
  return(gcap->priors[n]) ;
}

int
GCAisPossible(GCA *gca, MRI *mri, int label, TRANSFORM *transform, int x, int y, int z) 
{
  int       n ;
	GCA_PRIOR *gcap ;

	gcap = getGCAP(gca, mri, transform, x, y, z) ;
	if (gcap == NULL)
		return(0) ;

  for (n = 0 ; n < gcap->nlabels ; n++)
    if (gcap->labels[n] == label)
			return(1) ;
	return(0) ;
}

#if 0
static float get_voxel_prior(GCA *gca, MRI *mri, TRANSFORM *transform, 
                             int xv, int yv, int zv, int label);
static float
get_voxel_prior(GCA *gca, MRI *mri, TRANSFORM *transform, int xv, int yv, int zv, int label)
{
  int       xn, yn, zn ;
  GCA_PRIOR *gcap ;

  gcap = getGCAP(gca, mri, transform, xv, yv, zv) ;
  return(getPrior(gcap, label)) ;
}
#endif

static float
get_node_prior(GCA *gca, int label, int xn, int yn, int zn)
{
  int       xp, yp, zp ;
  GCA_PRIOR *gcap ;

  gcaNodeToPrior(gca, xn, yn, zn, &xp, &yp, &zp) ;
  gcap = &gca->priors[xp][yp][zp] ;
  return(getPrior(gcap, label)) ;
}

static int
gcaNodeToPrior(GCA *gca, int xn, int yn, int zn, int *pxp, int *pyp, int *pzp)
{
  *pxp = xn * (gca->node_spacing / gca->prior_spacing) ;
  *pyp = yn * (gca->node_spacing / gca->prior_spacing) ;
  *pzp = zn * (gca->node_spacing / gca->prior_spacing) ;
  return(NO_ERROR) ;
}
int
GCApriorToNode(GCA *gca, int xp, int yp, int zp, int *pxn, int *pyn, int *pzn)
{
  *pxn = xp * (gca->prior_spacing / gca->node_spacing) ;
  *pyn = yp * (gca->prior_spacing / gca->node_spacing) ;
  *pzn = zp * (gca->prior_spacing / gca->node_spacing) ;
  return(NO_ERROR) ;
}

static int
dump_gcan(GCA *gca, GCA_NODE *gcan, FILE *fp, int verbose, GCA_PRIOR *gcap)
{
  int       n, i, j, n1 ;
  GC1D      *gc ;
  float     prior ;
	VECTOR    *v_means = NULL ;
	MATRIX    *m_cov = NULL ;

  for (n = 0 ; n < gcan->nlabels ; n++)
  {
		for (n1 = 0 ; n1 < gcap->nlabels ; n1++)
			if (gcap->labels[n1] == gcan->labels[n])
				break ;
		if (n1 >= gcap->nlabels)
			continue ;
    prior = getPrior(gcap, gcan->labels[n]) ;
    if (FZERO(prior))
      continue ;
    gc = &gcan->gcs[n] ;
		v_means = load_mean_vector(gc, v_means, gca->ninputs) ;
		m_cov = load_covariance_matrix(gc, m_cov, gca->ninputs) ;
    fprintf(fp, "%d: label %-30.30s, prior %2.3f, ntr %d, means " ,
            n, cma_label_to_name(gcan->labels[n]), prior, gc->ntraining) ;
		for (i = 0 ; i < gca->ninputs ; i++)
			fprintf(fp, "%2.1f ", VECTOR_ELT(v_means,i+1)) ;
		fprintf(fp, ", cov:\n") ; MatrixPrint(fp, m_cov) ;
    if (verbose) 
		{
			for (i = 0 ; i < gca->ninputs ; i++)
			{
				for (j = 0 ; j < gca->ninputs ; j++)
				{
					fprintf(fp, "%2.1f\t", *MATRIX_RELT(m_cov,i+1,j+1)) ;
				}
				fprintf(fp, "\n") ;
			}

			for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
			{
				fprintf(fp, "\tnbr %d (%d,%d,%d): %d labels\n",
								i, xnbr_offset[i], ynbr_offset[i], znbr_offset[i], 
								gc->nlabels[i]) ;
				for (j = 0 ; j < gc->nlabels[i] ; j++)
				{
					fprintf(fp, "\t\tlabel %s, prior %2.3f\n", 
									cma_label_to_name(gc->labels[i][j]),
									gc->label_priors[i][j]) ;
				}
			}
		}
  }
	MatrixFree(&m_cov) ; VectorFree(&v_means);
  return(NO_ERROR) ;
}


int
GCAvoxelToNode(GCA *gca, MRI *mri, int xv, int yv, int zv, int *pxn, 
               int *pyn, int *pzn)
{
  float   xscale, yscale, zscale, offset ;

  xscale = mri->xsize / gca->node_spacing ;
  yscale = mri->ysize / gca->node_spacing ;
  zscale = mri->zsize / gca->node_spacing ;
  offset = (gca->node_spacing-1.0)/2.0 ;
  *pxn = nint((xv-offset) * xscale) ;
  if (*pxn < 0)
    *pxn = 0 ;
  else if (*pxn >= gca->node_width)
    *pxn = gca->node_width-1 ;

  *pyn = nint((yv-offset) * yscale) ;
  if (*pyn < 0)
    *pyn = 0 ;
  else if (*pyn >= gca->node_height)
    *pyn = gca->node_height-1 ;

  *pzn = nint((zv-offset) * zscale) ;
  if (*pzn < 0)
    *pzn = 0 ;
  else if (*pzn >= gca->node_depth)
    *pzn = gca->node_depth-1 ;

  return(NO_ERROR) ;
}

int
GCAvoxelToPrior(GCA *gca, MRI *mri, int xv, int yv, int zv, int *pxp, 
               int *pyp, int *pzp)
{
  float   xscale, yscale, zscale, offset ;

  xscale = mri->xsize / gca->prior_spacing ;
  yscale = mri->ysize / gca->prior_spacing ;
  zscale = mri->zsize / gca->prior_spacing ;
  offset = (gca->prior_spacing-1.0)/2.0 ;
  *pxp = nint((xv-offset) * xscale) ;
  if (*pxp < 0)
    *pxp = 0 ;
  else if (*pxp >= gca->prior_width)
    *pxp = gca->prior_width-1 ;

  *pyp = nint((yv-offset) * yscale) ;
  if (*pyp < 0)
    *pyp = 0 ;
  else if (*pyp >= gca->prior_height)
    *pyp = gca->prior_height-1 ;

  *pzp = nint((zv-offset) * zscale) ;
  if (*pzp < 0)
    *pzp = 0 ;
  else if (*pzp >= gca->prior_depth)
    *pzp = gca->prior_depth-1 ;

  return(NO_ERROR) ;
}

int
GCAsourceVoxelToPrior(GCA *gca, MRI *mri, TRANSFORM *transform,
                      int xv, int yv, int zv, int *pxp, int *pyp, int *pzp)
{
  float   xt, yt, zt ;

  TransformSample(transform, xv*mri->xsize, yv*mri->ysize, zv*mri->zsize, &xt, &yt, &zt) ;

  /* should change this to remove cast, but backwards comp for now */
  xt = nint(xt/mri->xsize) ; yt = nint(yt/mri->ysize) ; zt = nint(zt/mri->zsize) ; 
  GCAvoxelToPrior(gca, mri, xt, yt, zt, pxp, pyp, pzp) ;
  return(NO_ERROR) ;
}

int
GCAsourceVoxelToNode(GCA *gca, MRI *mri, TRANSFORM *transform,int xv, int yv, int zv, 
                     int *pxn, int *pyn, int *pzn)
{
  float   xt, yt, zt ;

  TransformSample(transform, xv*mri->xsize, yv*mri->ysize, zv*mri->zsize, &xt, &yt, &zt) ;

  /* should change this to remove cast, but backwards comp for now */
  xt = nint(xt/mri->xsize) ; yt = nint(yt/mri->ysize) ; zt = nint(zt/mri->zsize); 
  GCAvoxelToNode(gca, mri, xt, yt, zt, pxn, pyn, pzn) ;
  return(NO_ERROR) ;
}

int
GCAnodeToVoxel(GCA *gca, MRI *mri, int xn, int yn, int zn, int *pxv, 
               int *pyv, int *pzv)
{
  float   xscale, yscale, zscale, offset ;

#if 1
  xscale = mri->xsize / gca->node_spacing ;
  yscale = mri->ysize / gca->node_spacing ;
  zscale = mri->zsize / gca->node_spacing ;
#else
  xscale = 1.0f / gca->node_spacing ;
  yscale = 1.0f / gca->node_spacing ;
  zscale = 1.0f / gca->node_spacing ;
#endif
  offset = (gca->node_spacing-1.0)/2.0 ;
  *pxv = nint(xn / xscale + offset) ;
  if (*pxv < 0)
    *pxv = 0 ;
  else if (*pxv >= mri->width)
    *pxv = mri->width-1 ;

  *pyv = nint(yn / yscale + offset) ;
  if (*pyv < 0)
    *pyv = 0 ;
  else if (*pyv >= mri->height)
    *pyv = mri->height-1 ;

  *pzv = nint(zn / zscale + offset) ;
  if (*pzv < 0)
    *pzv = 0 ;
  else if (*pzv >= mri->depth)
    *pzv = mri->depth-1 ;

  return(NO_ERROR) ;
}
int
GCApriorToVoxel(GCA *gca, MRI *mri, int xp, int yp, int zp, int *pxv, 
               int *pyv, int *pzv)
{
  float   xscale, yscale, zscale, offset ;

  xscale = mri->xsize / gca->prior_spacing ;
  yscale = mri->ysize / gca->prior_spacing ;
  zscale = mri->zsize / gca->prior_spacing ;
  offset = (gca->prior_spacing-1.0)/2.0 ;
  *pxv = nint(xp / xscale + offset) ;
  if (*pxv < 0)
    *pxv = 0 ;
  else if (*pxv >= mri->width)
    *pxv = mri->width-1 ;

  *pyv = nint(yp / yscale + offset) ;
  if (*pyv < 0)
    *pyv = 0 ;
  else if (*pyv >= mri->height)
    *pyv = mri->height-1 ;

  *pzv = nint(zp / zscale + offset) ;
  if (*pzv < 0)
    *pzv = 0 ;
  else if (*pzv >= mri->depth)
    *pzv = mri->depth-1 ;

  return(NO_ERROR) ;
}


GCA  *
GCAalloc(int ninputs, float prior_spacing, float node_spacing, int width, int height, int depth, int flags)
{
  return(gcaAllocMax(ninputs, prior_spacing, node_spacing, width,height,depth,
                     DEFAULT_MAX_LABELS_PER_GCAN, flags));
}

GCA *
gcaAllocMax(int ninputs, float prior_spacing, float node_spacing, int width, int height, int depth, 
            int max_labels, int flags)
{
  GCA       *gca ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  int       x, y, z ;

  gca = calloc(1, sizeof(GCA)) ;
  if (!gca)
    ErrorExit(ERROR_NOMEMORY, "GCAalloc: could not allocate struct") ;

  gca->ninputs = ninputs ;
  gca->prior_spacing = prior_spacing ;
  gca->node_spacing = node_spacing ;

  /* ceil gives crazy results, I don't know why */
  gca->node_width = (int)(((float)width/node_spacing)+.99) ;
  gca->node_height = (int)((float)height/node_spacing+.99) ;
  gca->node_depth = (int)(((float)depth/node_spacing)+.99) ;
  gca->prior_width = (int)(((float)width/prior_spacing)+.99) ;
  gca->prior_height = (int)((float)height/prior_spacing+.99) ;
  gca->prior_depth = (int)(((float)depth/prior_spacing)+.99) ;
  gca->flags = flags ;

  gca->nodes = (GCA_NODE ***)calloc(gca->node_width, sizeof(GCA_NODE **)) ;
  if (!gca->nodes)
    ErrorExit(ERROR_NOMEMORY, "GCAalloc: could not allocate nodes") ;
  for (x = 0 ; x < gca->node_width ; x++)
  {
    gca->nodes[x] = (GCA_NODE **)calloc(gca->node_height, sizeof(GCA_NODE *)) ;
    if (!gca->nodes[x])
      ErrorExit(ERROR_NOMEMORY, "GCAalloc: could not allocate %dth **",x) ;

    for (y = 0 ; y < gca->node_height ; y++)
    {
      gca->nodes[x][y] = (GCA_NODE *)calloc(gca->node_depth, sizeof(GCA_NODE)) ;
      if (!gca->nodes[x][y])
        ErrorExit(ERROR_NOMEMORY,"GCAalloc: could not allocate %d,%dth *",x,y);
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        gcan->max_labels = max_labels ;

        if (max_labels > 0)
        {
          /* allocate new ones */
          gcan->gcs = alloc_gcs(gcan->max_labels, flags, gca->ninputs) ;
          if (!gcan->gcs)
            ErrorExit(ERROR_NOMEMORY, "GCANalloc: couldn't allocate gcs to %d",
                      gcan->max_labels) ;

          gcan->labels = (unsigned char *)calloc(gcan->max_labels, sizeof(unsigned char)) ;
          if (!gcan->labels)
            ErrorExit(ERROR_NOMEMORY,
                      "GCANalloc: couldn't allocate labels to %d",
                      gcan->max_labels) ;
        }
      }
    }
  }
  
  gca->priors = (GCA_PRIOR ***)calloc(gca->prior_width, sizeof(GCA_PRIOR **)) ;
  if (!gca->priors)
    ErrorExit(ERROR_NOMEMORY, "GCAalloc: could not allocate priors") ;
  for (x = 0 ; x < gca->prior_width ; x++)
  {
    gca->priors[x] = (GCA_PRIOR **)calloc(gca->prior_height, sizeof(GCA_PRIOR *)) ;
    if (!gca->priors[x])
      ErrorExit(ERROR_NOMEMORY, "GCAalloc: could not allocate %dth **",x) ;

    for (y = 0 ; y < gca->prior_height ; y++)
    {
      gca->priors[x][y] = (GCA_PRIOR *)calloc(gca->prior_depth, sizeof(GCA_PRIOR)) ;
      if (!gca->priors[x][y])
        ErrorExit(ERROR_NOMEMORY,"GCAalloc: could not allocate %d,%dth *",x,y);
      for (z = 0 ; z < gca->prior_depth ; z++)
      {
        gcap = &gca->priors[x][y][z] ;
        gcap->max_labels = max_labels ;

        if (max_labels > 0)
        {
          /* allocate new ones */
          gcap->labels = (unsigned char *)calloc(max_labels, sizeof(unsigned char)) ;
          if (!gcap->labels)
            ErrorExit(ERROR_NOMEMORY, "GCANalloc: couldn't allocate labels to %d",
                      gcap->max_labels) ;

          gcap->priors = (float *)calloc(max_labels, sizeof(float)) ;
          if (!gcap->priors)
            ErrorExit(ERROR_NOMEMORY,
                      "GCANalloc: couldn't allocate priors to %d", max_labels) ;
        }
      }
    }
  }
  
  return(gca) ;
}

int
GCAfree(GCA **pgca)
{
  GCA  *gca ;
  int  x, y, z ;

  gca = *pgca ;
  *pgca = NULL ;

  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        GCANfree(&gca->nodes[x][y][z], gca->ninputs) ;
      }
      free(gca->nodes[x][y]) ;
    }
    free(gca->nodes[x]) ;
  }
  free(gca->nodes) ;

  for (x = 0 ; x < gca->prior_width ; x++)
  {
    for (y = 0 ; y < gca->prior_height ; y++)
    {
      for (z = 0 ; z < gca->prior_depth ; z++)
      {
        free(gca->priors[x][y][z].labels) ;
        free(gca->priors[x][y][z].priors) ;
      }
      free(gca->priors[x][y]) ;
    }
    free(gca->priors[x]) ;
  }

  free(gca->priors) ;
  free(gca) ;
  return(NO_ERROR) ;
}

int
GCANfree(GCA_NODE *gcan, int ninputs)
{
  if (gcan->nlabels)
  {
    free(gcan->labels) ;
    free_gcs(gcan->gcs, gcan->nlabels, ninputs) ;
  }
  return(NO_ERROR) ;
}
int
GCAtrainCovariances(GCA *gca, MRI *mri_inputs, MRI *mri_labels, TRANSFORM *transform)
{
  int    x, y, z, width, height, depth, label, xn, yn, zn ;
	float  vals[MAX_GCA_INPUTS] ;


  /* convert transform to voxel coordinates */

  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  width = mri_labels->width ; height = mri_labels->height; 
  depth = mri_labels->depth ;
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        label = MRIvox(mri_labels, x, y, z) ;
#if 0
        if (!label)
          continue ;
#endif

				load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;

        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;

        GCAupdateNodeCovariance(gca, mri_inputs, xn, yn, zn, vals,label) ;
        if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z && 
            (label == Ggca_label || Ggca_label < 0))
        {
          GC1D *gc ;
					int  i, nsamples ;
					MATRIX *m ;
          gc = GCAfindGC(gca, xn, yn, zn, label) ;
          if (gc)
					{
						nsamples = gc->ntraining - gc->n_just_priors ;  /* for no-intensity training */
						if (nsamples < 1)
							nsamples = 1  ;
            printf("voxel(%d,%d,%d) = ", x, y, z) ;
						for (i = 0 ; i < gca->ninputs ; i++)
							printf("%d ", nint(vals[i])) ;
						
            printf(" --> node(%d,%d,%d), "
                   "label %s (%d), mean ",xn, yn, zn, cma_label_to_name(label),label) ;
						for (i = 0 ; i < gca->ninputs ; i++)
							printf("%2.1f ", gc->means[i]) ;
						printf("\ncovariances (det=%f):\n",covariance_determinant(gc, gca->ninputs)/pow((double)nsamples,(double)gca->ninputs)) ;
						m = load_covariance_matrix(gc, NULL, gca->ninputs) ;
						MatrixScalarMul(m, 1.0/(nsamples), m) ;
						MatrixPrint(stdout, m) ;
						MatrixFree(&m) ;
					}
        }
      }
    }
  }

  return(NO_ERROR) ;
}
#include <unistd.h>
int
GCAtrain(GCA *gca, MRI *mri_inputs, MRI *mri_labels, TRANSFORM *transform, GCA *gca_prune,
         int noint)
{
  int    x, y, z, width, height, depth, label, xn, yn, zn,
    /*i, xnbr, ynbr, znbr, xn_nbr, yn_nbr, zn_nbr,*/ xp, yp, zp ;
	float  vals[MAX_GCA_INPUTS] ;
	static int first_time = 1 ;
	FILE *logfp = NULL ;
	static int logging = 0 ;

	if (first_time)
	{
		first_time = 0 ;
		logging = getenv("GCA_LOG") != NULL ;
		printf("logging image intensities to GCA log file 'gca*.log'\n") ;
		for (label = 0 ; label <= MAX_CMA_LABEL ; label++)
		{
			char fname[STRLEN] ;
			sprintf(fname, "gca%d.log", label) ;
			if (FileExists(fname))
				unlink(fname) ;
		}
	}


  /* convert transform to voxel coordinates */

  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  width = mri_labels->width ; height = mri_labels->height; 
  depth = mri_labels->depth ;
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        label = MRIvox(mri_labels, x, y, z) ;
#if 0
        if (!label)
          continue ;
#endif

				load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;

        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
        GCAsourceVoxelToPrior(gca, mri_inputs, transform, x, y, z, &xp, &yp, &zp) ;
        GCAupdatePrior(gca, mri_inputs, xp, yp, zp, label) ;

        if ((GCAupdateNode(gca, mri_inputs, xn, yn, zn, 
                          vals,label,gca_prune, noint) == NO_ERROR) &&
            !(gca->flags & GCA_NO_MRF))
          GCAupdateNodeGibbsPriors(gca, mri_labels, xn, yn, zn, x, y,z, label);

        if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z && 
            (label == Ggca_label || Ggca_label < 0))
        {
          GC1D *gc ;
					int  i ;
          gc = GCAfindGC(gca, xn, yn, zn, label) ;
          if (gc)
					{
						if (logging)
						{
							char fname[STRLEN] ;
							sprintf(fname, "gca%d.log", label) ;
							logfp = fopen(fname, "a") ;
						}
            printf("voxel(%d,%d,%d) = ", x, y, z) ;
						for (i = 0 ; i < gca->ninputs ; i++)
						{
							printf("%2.1f ", (vals[i])) ;
							if (logging)
								fprintf(logfp, "%2.1f ", (vals[i])) ;
						}
						
            printf(" --> node(%d,%d,%d), "
                   "label %s (%d), mean ",xn, yn, zn, cma_label_to_name(label),label) ;
						for (i = 0 ; i < gca->ninputs ; i++)
							printf("%2.1f ", gc->means[i] / gc->ntraining) ;
						printf("\n") ;
						if (logging)
						{
							fprintf(logfp, "\n") ;
							fclose(logfp) ;
						}
					}
        }
        if (gca->flags & GCA_NO_MRF)
          continue ;
#if 0
        for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
        {
          xnbr = x+xnbr_offset[i] ; 
          ynbr = y+ynbr_offset[i] ; 
          znbr = z+znbr_offset[i] ; 
          GCAsourceVoxelToNode(gca, mri_inputs, transform, 
                               xnbr, ynbr, znbr,&xn_nbr,&yn_nbr,&zn_nbr) ;
          if (xn_nbr == xn && yn_nbr == yn && zn_nbr == zn)
            continue ;  /* only update if it is a different node */

          if (GCAupdateNode(gca, mri_inputs, xn_nbr, yn_nbr, zn_nbr, 
                            vals,label,gca_prune,noint) == NO_ERROR)
            GCAupdateNodeGibbsPriors(gca, mri_labels, xn_nbr, yn_nbr, zn_nbr, 
                                     x, y,z, label);
        }
#endif
      }
    }
  }

  return(NO_ERROR) ;
}
int
GCAwrite(GCA *gca, char *fname)
{
  FILE      *fp ;
  int       x, y, z, n, i, j ; 
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  GC1D      *gc ;

  fp  = fopen(fname, "wb") ;
  if (!fp)
    ErrorReturn(ERROR_NOFILE,
                (ERROR_NOFILE,
                 "GCAwrite: could not open GCA %s for writing",fname)) ;

  fwriteFloat(GCA_VERSION, fp) ;
  fwriteFloat(gca->prior_spacing, fp) ;
  fwriteFloat(gca->node_spacing, fp) ;
  fwriteInt(gca->prior_width,fp);fwriteInt(gca->prior_height,fp); fwriteInt(gca->prior_depth,fp);
  fwriteInt(gca->node_width,fp);fwriteInt(gca->node_height,fp); fwriteInt(gca->node_depth,fp);
  fwriteInt(gca->ninputs,fp) ;
  fwriteInt(gca->flags, fp) ;

  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == 139 && y == 103 && z == 139)  /* wm should be pallidum */
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        fwriteInt(gcan->nlabels, fp) ;
        fwriteInt(gcan->total_training, fp) ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					int  r, c ;
          gc = &gcan->gcs[n] ;
          fputc((int)gcan->labels[n],fp) ;
					for (r = 0 ; r < gca->ninputs ; r++)
						fwriteFloat(gc->means[r], fp) ;
					for (r = i = 0 ; r < gca->ninputs ; r++)
						for (c = r ;  c < gca->ninputs ; c++, i++)
							fwriteFloat(gc->covars[i], fp) ;

          if (gca->flags & GCA_NO_MRF)
            continue ;
          for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
          {
            fwriteInt(gc->nlabels[i], fp) ;
            for (j = 0 ; j < gc->nlabels[i] ; j++)
            {
              fwriteInt((int)gc->labels[i][j], fp) ;
              fwriteFloat(gc->label_priors[i][j], fp) ;
            }
          }
        }
      }
    }
  }

  for (x = 0 ; x < gca->prior_width ; x++)
  {
    for (y = 0 ; y < gca->prior_height ; y++)
    {
      for (z = 0 ; z < gca->prior_depth ; z++)
      {
        if (x == 139 && y == 103 && z == 139)  /* wm should be pallidum */
          DiagBreak() ;
        gcap = &gca->priors[x][y][z] ;
        fwriteInt(gcap->nlabels, fp) ;
        fwriteInt(gcap->total_training, fp) ;
        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          fputc((int)gcap->labels[n],fp) ;
          fwriteFloat(gcap->priors[n], fp) ;
        }
      }
    }
  }

	if (gca->type == GCA_FLASH)
	{
		int  n ;

		fwriteInt(FILE_TAG, fp) ;  /* beginning of tagged section */

		/* all tags are format: <int: tag> <int: num> <parm> <parm> .... */
		fwriteInt(TAG_GCA_TYPE, fp) ;
		fwriteInt(1, fp) ;   
		fwriteInt(gca->type, fp) ;   

		fwriteInt(TAG_PARAMETERS, fp) ;
		fwriteInt(3, fp) ;   /* currently only storing 3 parameters */
		for (n = 0 ; n < gca->ninputs ; n++)
		{
			fwriteFloat(gca->TRs[n], fp) ;
			fwriteFloat(gca->FAs[n], fp) ;
			fwriteFloat(gca->TEs[n], fp) ;
		}
	}

  fclose(fp) ;
  return(NO_ERROR) ;
}

GCA *
GCAread(char *fname)
{
  FILE      *fp ;
  int       x, y, z, n, i, j ; 
  GCA       *gca ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  GC1D      *gc ;
  float     version, node_spacing, prior_spacing ;
  int       node_width, node_height, node_depth, prior_width, prior_height, prior_depth, 
            ninputs, flags ;

  fp  = fopen(fname, "rb") ;
  if (!fp)
    ErrorReturn(NULL,
                (ERROR_NOFILE,
                 "GCAread: could not open GCA %s for reading",fname)) ;

  version = freadFloat(fp) ;
  if (version < GCA_VERSION)
  {
    node_spacing = freadFloat(fp) ;
    node_width = freadInt(fp); node_height = freadInt(fp); node_depth = freadInt(fp);
    ninputs = freadInt(fp) ;
    if (version == 3.0)
      flags = freadInt(fp) ;
    else
      flags = 0 ;
    
    gca = gcaAllocMax(ninputs, node_spacing, node_spacing, node_spacing*node_width, 
                      node_spacing*node_height, node_spacing*node_depth, 0, flags) ;  
    if (!gca)
      ErrorReturn(NULL, (Gerror, NULL)) ;
    
    for (x = 0 ; x < gca->node_width ; x++)
    {
      for (y = 0 ; y < gca->node_height ; y++)
      {
        for (z = 0 ; z < gca->node_depth ; z++)
        {
          if (x == 28 && y == 39 && z == 39)
            DiagBreak() ;
          gcan = &gca->nodes[x][y][z] ;
          gcan->nlabels = freadInt(fp) ;
          gcan->total_training = freadInt(fp) ;
          gcan->labels = (unsigned char *)calloc(gcan->nlabels, sizeof(unsigned char)) ;
          if (!gcan->labels)
            ErrorExit(ERROR_NOMEMORY, "GCAread(%s): could not allocate %d "
                      "labels @ (%d,%d,%d)", fname, gcan->nlabels, x, y, z) ;
          gcan->gcs = alloc_gcs(gcan->nlabels, flags, gca->ninputs) ;
          if (!gcan->gcs)
            ErrorExit(ERROR_NOMEMORY, "GCAread(%s); could not allocated %d gcs "
                      "@ (%d,%d,%d)", fname, gcan->nlabels, x, y, z) ;
          
          for (n = 0 ; n < gcan->nlabels ; n++)
          {
						int r, c;
            gc = &gcan->gcs[n] ;
            gcan->labels[n] = (unsigned char)fgetc(fp) ;
						for (r = 0 ; r < gca->ninputs ; r++)
							gc->means[r] = freadFloat(fp) ;
						for (i = r = 0 ; r < gca->ninputs ; r++)
							for (c = r ; c < gca->ninputs ; c++, i++)
								gc->covars[i] = freadFloat(fp) ;
            if (gca->flags & GCA_NO_MRF)
              continue ;
            for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
            {
              gc->nlabels[i] = freadInt(fp) ;
              
#if 1
              /* allocate new ones */
              gc->label_priors[i] = 
                (float *)calloc(gc->nlabels[i],sizeof(float));
              if (!gc->label_priors[i])
                ErrorExit(ERROR_NOMEMORY, "GCAread(%s): "
                          "couldn't expand gcs to %d", fname,gc->nlabels) ;
              gc->labels[i] = (unsigned char *)calloc(gc->nlabels[i], sizeof(unsigned char)) ;
              if (!gc->labels)
                ErrorExit(ERROR_NOMEMORY, 
                          "GCAread(%s): couldn't expand labels to %d",
                          fname, gc->nlabels[i]) ;
#endif
              for (j = 0 ; j < gc->nlabels[i] ; j++)
              {
                gc->labels[i][j] = (unsigned char)freadInt(fp) ;
                gc->label_priors[i][j] = freadFloat(fp) ;
              }
            }
          }
        }
      }
    }
  }
  else   /* current version - stores priors at different resolution than densities */
  {
    if (!FEQUAL(version, GCA_VERSION))
    {
      fclose(fp) ;
      ErrorReturn(NULL, (ERROR_BADFILE, "GCAread(%s), version #%2.1f found, %2.1f expected",
                         fname, version, GCA_VERSION)) ;
    }
    prior_spacing = freadFloat(fp) ; node_spacing = freadFloat(fp) ;
    prior_width = freadInt(fp); prior_height = freadInt(fp); prior_depth = freadInt(fp);
    node_width = freadInt(fp); node_height = freadInt(fp); node_depth = freadInt(fp);
    ninputs = freadInt(fp) ; 
    flags = freadInt(fp) ;
    
    gca = gcaAllocMax(ninputs, prior_spacing, node_spacing, node_spacing*node_width, 
                      node_spacing*node_height, node_spacing*node_depth, 0, flags) ;  
    if (!gca)
      ErrorReturn(NULL, (Gdiag, NULL)) ;
    
    for (x = 0 ; x < gca->node_width ; x++)
    {
      for (y = 0 ; y < gca->node_height ; y++)
      {
        for (z = 0 ; z < gca->node_depth ; z++)
        {
          if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
            DiagBreak() ;
          gcan = &gca->nodes[x][y][z] ;
          gcan->nlabels = freadInt(fp) ;
          gcan->total_training = freadInt(fp) ;
          gcan->labels = (unsigned char *)calloc(gcan->nlabels, sizeof(unsigned char)) ;
          if (!gcan->labels)
            ErrorExit(ERROR_NOMEMORY, "GCAread(%s): could not allocate %d "
                      "labels @ (%d,%d,%d)", fname, gcan->nlabels, x, y, z) ;
          gcan->gcs = alloc_gcs(gcan->nlabels, flags, gca->ninputs) ;
          if (!gcan->gcs)
            ErrorExit(ERROR_NOMEMORY, "GCAread(%s); could not allocated %d gcs "
                      "@ (%d,%d,%d)", fname, gcan->nlabels, x, y, z) ;
          
          for (n = 0 ; n < gcan->nlabels ; n++)
          {
						int  r, c ;

            gc = &gcan->gcs[n] ;
						
            gcan->labels[n] = (unsigned char)fgetc(fp) ;
						
						for (r = 0 ; r < gca->ninputs ; r++)
							gc->means[r] = freadFloat(fp) ;
						for (i = r = 0 ; r < gca->ninputs ; r++)
							for (c = r ; c < gca->ninputs ; c++, i++)
								gc->covars[i] = freadFloat(fp) ;
            if (gca->flags & GCA_NO_MRF)
              continue ;
            for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
            {
              gc->nlabels[i] = freadInt(fp) ;
              
              /* allocate new ones */
              gc->label_priors[i] = 
                (float *)calloc(gc->nlabels[i],sizeof(float));
              if (!gc->label_priors[i])
                ErrorExit(ERROR_NOMEMORY, "GCAread(%s): "
                          "couldn't expand gcs to %d", fname,gc->nlabels) ;
              gc->labels[i] = (unsigned char *)calloc(gc->nlabels[i], sizeof(unsigned char)) ;
              if (!gc->labels)
                ErrorExit(ERROR_NOMEMORY, 
                          "GCAread(%s): couldn't expand labels to %d",
                          fname, gc->nlabels[i]) ;
              for (j = 0 ; j < gc->nlabels[i] ; j++)
              {
                gc->labels[i][j] = (unsigned char)freadInt(fp) ;
                gc->label_priors[i][j] = freadFloat(fp) ;
              }
            }
          }
        }
      }
    }

    for (x = 0 ; x < gca->prior_width ; x++)
    {
      for (y = 0 ; y < gca->prior_height ; y++)
      {
        for (z = 0 ; z < gca->prior_depth ; z++)
        {
          if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
            DiagBreak() ;
          gcap = &gca->priors[x][y][z] ;
          gcap->nlabels = freadInt(fp) ;
          gcap->total_training = freadInt(fp) ;

          gcap->labels = (unsigned char *)calloc(gcap->nlabels, sizeof(unsigned char)) ;
          if (!gcap->labels)
            ErrorExit(ERROR_NOMEMORY, "GCAread(%s): could not allocate %d "
                      "labels @ (%d,%d,%d)", fname, gcap->nlabels, x, y, z) ;
          gcap->priors = (float *)calloc(gcap->nlabels, sizeof(float)) ;
          if (!gcap->priors)
            ErrorExit(ERROR_NOMEMORY, "GCAread(%s): could not allocate %d "
                      "priors @ (%d,%d,%d)", fname, gcap->nlabels, x, y, z) ;
          for (n = 0 ; n < gcap->nlabels ; n++)
          {
            gcap->labels[n] = (unsigned char)fgetc(fp) ;
            gcap->priors[n] = freadFloat(fp) ;
          }
        }
      }
    }
    
  }

	for (x = 0 ; x < gca->node_width ; x++)
	{
		for (y = 0 ; y < gca->node_height ; y++)
		{
			for (z = 0 ; z < gca->node_depth ; z++)
			{
				int xp, yp, zp ;
				
				if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
					DiagBreak() ;
				gcan = &gca->nodes[x][y][z] ;
				gcaNodeToPrior(gca, x, y, z, &xp, &yp, &zp) ;
				gcap = &gca->priors[xp][yp][zp] ;
				for (n = 0 ; n < gcan->nlabels ; n++)
				{
					gc = &gcan->gcs[n] ;
					gc->ntraining = gcan->total_training * getPrior(gcap,gcan->labels[n]) ;
				}
			}
		}
	}

	if (!feof(fp))
	{
		int  n, tag, nparms ;

		tag = freadInt(fp) ;
		
		if (tag == FILE_TAG) /* beginning of tagged section */
		{
			while (!feof(fp))
			{
				/* all tags are format: <int: tag> <int: num> <parm> <parm> .... */
				tag = freadInt(fp) ;
				switch (tag)
				{
				case TAG_GCA_TYPE:
					freadInt(fp) ;   /* skip num=1 */
					gca->type = freadInt(fp) ;   
					printf("setting gca type = %d\n", gca->type) ;
					break ;
				case TAG_PARAMETERS:
					nparms = freadInt(fp) ;   /* how many MR parameters are stored */
					printf("reading %d MR parameters out of GCA header...\n", nparms) ;
					for (n = 0 ; n < gca->ninputs ; n++)
					{
						gca->TRs[n] = freadFloat(fp) ;
						gca->FAs[n] = freadFloat(fp) ;
						gca->TEs[n] = freadFloat(fp) ;
						printf("input %d: TR=%2.1f msec, FA=%2.1f deg, TE=%2.1f msec\n",
									 n, gca->TRs[n], DEGREES(gca->FAs[n]), gca->TEs[n]) ;
					}
					break ;
				default:
					ErrorPrintf(ERROR_BADFILE, "GCAread(%s): unknown tag %x\n", fname, tag) ;
					break ;
				}
			}
		}
	}

  fclose(fp) ;
  return(gca) ;
}

static int
GCAupdatePrior(GCA *gca, MRI *mri, int xn, int yn, int zn, int label)
{
  int       n ;
  GCA_PRIOR *gcap ;

  if (label >= MAX_CMA_LABEL)
    ErrorReturn(ERROR_BADPARM,
                (ERROR_BADPARM, 
                 "GCAupdatePrior(%d, %d, %d, %d): label out of range",
                 xn, yn, zn, label)) ;

  if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z)
    DiagBreak() ;


  gcap = &gca->priors[xn][yn][zn] ;

  for (n = 0 ; n < gcap->nlabels ; n++)
  {
    if (gcap->labels[n] == label)
      break ;
  }
  if (n >= gcap->nlabels)  /* have to allocate a new classifier */
  {

    if (n >= gcap->max_labels)
    {
      int  old_max_labels ;
      char *old_labels ;
      float *old_priors ;

      old_max_labels = gcap->max_labels ; 
      gcap->max_labels += 2 ;
      old_labels = gcap->labels ;
      old_priors = gcap->priors ;

      /* allocate new ones */
      gcap->priors = (float *)calloc(gcap->max_labels, sizeof(float)) ;
      
      if (!gcap->priors)
        ErrorExit(ERROR_NOMEMORY, "GCANupdatePriors: couldn't expand priors to %d",
                  gcap->max_labels) ;
      gcap->labels = (unsigned char *)calloc(gcap->max_labels, sizeof(unsigned char)) ;
      if (!gcap->labels)
        ErrorExit(ERROR_NOMEMORY, "GCANupdatePriors: couldn't expand labels to %d",
                  gcap->max_labels) ;

      /* copy the old ones over */
      memmove(gcap->priors, old_priors, old_max_labels*sizeof(float)) ;
      memmove(gcap->labels, old_labels, old_max_labels*sizeof(unsigned char)) ;

      /* free the old ones */
      free(old_priors) ; free(old_labels) ;
    }
    gcap->nlabels++ ;
  }

  /* these will be updated when training is complete */
  gcap->priors[n] += 1.0f ;
  gcap->total_training++ ;
  gcap->labels[n] = label ;

  return(NO_ERROR) ;
}

static int
GCAupdateNodeCovariance(GCA *gca, MRI *mri, int xn, int yn, int zn, float *vals,int label)
{
  int      n, r, c, v ;
  GCA_NODE *gcan ;
  GC1D     *gc ;


  if (label >= MAX_CMA_LABEL)
    ErrorReturn(ERROR_BADPARM,
                (ERROR_BADPARM, 
                 "GCAupdateNodeCovariance(%d, %d, %d, %d): label out of range",
                 xn, yn, zn, label)) ;

  if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z && label == Ggca_label)
    DiagBreak() ;
  if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z)
    DiagBreak() ;


  gcan = &gca->nodes[xn][yn][zn] ;

  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == label)
      break ;
  }
  if (n >= gcan->nlabels)
		ErrorExit(ERROR_BADPARM, "GCAupdateNodeCovariance(%d, %d, %d, %d): could not find label",
							xn, yn, zn, label) ;

  gc = &gcan->gcs[n] ;

	/* remove means from vals */
	for (r = 0 ; r < gca->ninputs ; r++)
		vals[r] -= gc->means[r] ;

  /* these will be normalized when training is complete */
	for (v = r = 0 ; r < gca->ninputs ; r++)
	{
		for (c = r ; c < gca->ninputs ; c++, v++)
			gc->covars[v] += vals[r]*vals[c] ; 
	}

	/* put means back in vals so caller isn't confused */
	for (r = 0 ; r < gca->ninputs ; r++)
		vals[r] += gc->means[r] ;

  return(NO_ERROR) ;
}
static int
GCAupdateNode(GCA *gca, MRI *mri, int xn, int yn, int zn, float *vals, int label,
              GCA *gca_prune, int noint)
{
  int      n, i ;
  GCA_NODE *gcan ;
  GC1D     *gc ;


  if (label >= MAX_CMA_LABEL)
    ErrorReturn(ERROR_BADPARM,
                (ERROR_BADPARM, 
                 "GCAupdateNode(%d, %d, %d, %d): label out of range",
                 xn, yn, zn, label)) ;

  if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z && label == Ggca_label)
    DiagBreak() ;
  if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z)
    DiagBreak() ;


  if (label > 0 && gca_prune != NULL && !noint)
  {
    GCA_NODE *gcan_prune ;
    GC1D     *gc_prune ;
    int      nprune ;

     gcan_prune = &gca_prune->nodes[xn][yn][zn] ;

     for (nprune = 0 ; nprune < gcan_prune->nlabels ; nprune++)
     {
       if (gcan_prune->labels[nprune] == label)
         break ;
     }
     if (nprune >= gcan_prune->nlabels)
       ErrorPrintf(ERROR_BADPARM, "WARNING: pruning GCA at (%d,%d,%d) doesn't "
                   "contain label %d", xn, yn, zn, label) ;
     gc_prune = &gcan_prune->gcs[nprune] ;

     if (sqrt(GCAmahDist(gc_prune, vals, gca->ninputs)) > 2) /* more than 2 stds from mean */
     {
#if 0
       if (xn == 23 && yn == 30 && zn == 32)
       {
         printf(
                "pruning val %2.0f, label %d @ (%d,%d,%d),u=%2.1f, std=%2.1f\n"
                ,val, label, xn,yn,zn,mean, std) ;
         DiagBreak() ;
       }
#endif
       total_pruned++ ;
       return(ERROR_BAD_PARM) ;
     }
  }
    

  gcan = &gca->nodes[xn][yn][zn] ;

  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == label)
      break ;
  }
  if (n >= gcan->nlabels)  /* have to allocate a new classifier */
  {

    if (n >= gcan->max_labels)
    {
      int  old_max_labels ;
      char *old_labels ;
      GC1D *old_gcs ;

      old_max_labels = gcan->max_labels ; 
      gcan->max_labels += 2 ;
      old_labels = gcan->labels ;
      old_gcs = gcan->gcs ;

      /* allocate new ones */
#if 0
      gcan->gcs = (GC1D *)calloc(gcan->max_labels, sizeof(GC1D)) ;
#else
      gcan->gcs = alloc_gcs(gcan->max_labels, gca->flags, gca->ninputs) ;
#endif
      
      if (!gcan->gcs)
        ErrorExit(ERROR_NOMEMORY, "GCANupdateNode: couldn't expand gcs to %d",
                  gcan->max_labels) ;
      gcan->labels = (unsigned char *)calloc(gcan->max_labels, sizeof(unsigned char)) ;
      if (!gcan->labels)
        ErrorExit(ERROR_NOMEMORY, "GCANupdateNode: couldn't expand labels to %d",
                  gcan->max_labels) ;

      /* copy the old ones over */
#if 0
      memmove(gcan->gcs, old_gcs, old_max_labels*sizeof(GC1D)) ;
#else
      copy_gcs(old_max_labels, old_gcs, gcan->gcs, gca->ninputs) ;
#endif
      memmove(gcan->labels, old_labels, old_max_labels*sizeof(unsigned char)) ;

      /* free the old ones */
      free(old_gcs) ; free(old_labels) ;
    }
    gcan->nlabels++ ;
  }

  gc = &gcan->gcs[n] ;

  /* these will be updated when training is complete */
  if (noint)
    gc->n_just_priors++ ;
  else
  { 
	for (i = 0 ; i < gca->ninputs ; i++)
		gc->means[i] += vals[i] ; 
		/*gc->var += val*val ; */
	}
  if (gc->n_just_priors >= gc->ntraining)
    DiagBreak() ;
  gcan->total_training++ ;
  
  gcan->labels[n] = label ;
  gc->ntraining++ ;

  return(NO_ERROR) ;
}
int
GCAcompleteMeanTraining(GCA *gca)
{
  int       x, y, z, n, total_nodes, total_gcs, i, j, holes_filled, total_brain_gcs,
            total_brain_nodes, r ;
  float     nsamples ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  GC1D      *gc ;

  total_nodes = gca->node_width*gca->node_height*gca->node_depth ; 
  total_brain_nodes = total_gcs = total_brain_gcs = 0 ;
  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        total_gcs += gcan->nlabels ;
        if (gcan->nlabels > 1 || !IS_UNKNOWN(gcan->labels[0]))
        {
          total_brain_gcs += gcan->nlabels ;
          total_brain_nodes++ ;
        }
          
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
          nsamples = gc->ntraining ;
          if ((gca->flags & GCA_NO_MRF) == 0)
          {
            for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
            {
              for (j = 0 ; j < gc->nlabels[i] ; j++)
              {
                gc->label_priors[i][j] /= (float)nsamples ;
                check_finite("GCAcompleteMeanTraining: label_priors",
                             gc->label_priors[i][j]) ;
              }
            }
          }

          nsamples -= gc->n_just_priors ;  /* for no-intensity training */
          if (nsamples > 0)
          {
						for (r = 0 ; r < gca->ninputs ; r++)
						{
							gc->means[r] /= nsamples ;
							check_finite("GCAcompleteMeanTraining: mean", gc->means[r]) ;
						}
          }
          else
          {
						int r ;
						for (r = 0 ; r < gca->ninputs ; r++)
							gc->means[r] = -1 ;  /* mark it for later processing */
          }
        }
      }
    }
  }

  for (x = 0 ; x < gca->prior_width ; x++)
  {
    for (y = 0 ; y < gca->prior_height ; y++)
    {
      for (z = 0 ; z < gca->prior_depth ; z++)
      {
        gcap = &gca->priors[x][y][z] ;
        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          gcap->priors[n] /= (float)gcap->total_training ;
          check_finite("GCAcompleteMeanTraining: priors", gcap->priors[n]) ;
        }

      }
    }
  }
  printf("filling holes in the GCA...\n") ;
  for (holes_filled = x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
					nsamples = gc->ntraining - gc->n_just_priors ;
          if (nsamples <= 0)
          {
            GC1D *gc_nbr ;
						int  r, i ;

            gc_nbr = findClosestValidGC(gca, x, y, z, gcan->labels[n], 0) ;
            if (!gc_nbr)
            {
              ErrorPrintf(ERROR_BADPARM, 
                          "gca(%d,%d,%d,%d) - could not find valid nbr label "
                          "%s (%d)", x,  y, z, n, 
                          cma_label_to_name(gcan->labels[n]),
                          gcan->labels[n]) ;
              continue ;
            }
            holes_filled++ ;
						for (i = r = 0 ; r < gca->ninputs ; r++)
						{
							gc->means[r] = gc_nbr->means[r] ; 
						}
            if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
              printf("filling hole @ (%d, %d, %d)\n", x, y, z) ;
          }
        }
      }
    }
  }


  printf("%d classifiers: %2.1f per node, %2.2f in brain (%d holes filled)\n", 
         total_gcs, (float)total_gcs/(float)total_nodes,
         (float)total_brain_gcs/(float)total_brain_nodes,holes_filled) ;
  if (total_pruned > 0)
  {
    printf("%d samples pruned during training\n", total_pruned) ;
    total_pruned = 0 ;
  }
  return(NO_ERROR) ;
}

int
GCAcompleteCovarianceTraining(GCA *gca)
{
  int       x, y, z, n, holes_filled, r, c,v, nregs = 0, nsamples ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;

  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
          
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						DiagBreak() ;
          gc = &gcan->gcs[n] ;
          nsamples = gc->ntraining - gc->n_just_priors ;  /* for no-intensity training */
          if (nsamples > 0)
          {
						for (r = v = 0 ; r < gca->ninputs ; r++)
						{
							check_finite("GCAcompleteCovarianceTraining: mean", gc->means[r]) ;
							if (nsamples > 1)
							{
								for (c = r ; c < gca->ninputs ; c++, v++)
								{
									gc->covars[v] /= (float)(nsamples-1) ;
									check_finite("GCAcompleteCovarianceTraining: covar", gc->covars[v]) ;
									if (r == c)  /* diagonal should be positive definite */
									{
										if (gc->covars[v] < -0.1)
											DiagBreak() ;
									}
								}
							}
						}
          }
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(gcan->labels[n] == Ggca_label || Ggca_label < 0))
					{
						MATRIX *m ;
						double det ;

						det = covariance_determinant(gc, gca->ninputs) ;
						printf("final covariance matrix for %s (nsamples=%d, det=%f):\n", cma_label_to_name(gcan->labels[n]), 
									 nsamples, det) ;
						m = load_covariance_matrix(gc, NULL, gca->ninputs) ;
						MatrixPrint(stdout, m) ;
						MatrixFree(&m) ;
					}
        }
      }
    }
  }

	holes_filled = 0 ;
#if 0
  printf("filling holes in the GCA...\n") ;
  for (holes_filled = x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						DiagBreak() ;
          gc = &gcan->gcs[n] ;
          nsamples = gc->ntraining - gc->n_just_priors ;  /* for no-intensity training */
#define MIN_GC_SAMPLES(gca) ((gca->ninputs*(gca->ninputs+1))/2)
          if (gc->means[0] < 0 || nsamples < MIN_GC_SAMPLES(gca))
          {
            GC1D *gc_nbr ;
						int  r, c, i ;

            gc_nbr = findClosestValidGC(gca, x, y, z, gcan->labels[n], 1) ;
            if (!gc_nbr)
            {
              ErrorPrintf(ERROR_BADPARM, 
                          "gca(%d,%d,%d,%d) - could not find valid nbr label "
                          "%s (%d)", x,  y, z, n, 
                          cma_label_to_name(gcan->labels[n]),
                          gcan->labels[n]) ;
              continue ;
            }
            holes_filled++ ;
						for (i = r = 0 ; r < gca->ninputs ; r++)
						{
							gc->means[r] = gc_nbr->means[r] ; 
							for (c = r ; c < gca->ninputs ; c++, i++)
								gc->covars[i] = gc_nbr->covars[i] ;
						}
            if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
								(Ggca_label == gcan->labels[n] || Ggca_label < 0))
              printf("filling hole @ (%d, %d, %d)\n", x, y, z) ;
          }
        }
      }
    }
  }
#endif

	GCAfixSingularCovarianceMatrices(gca) ;   /* shouldn't need the code that follows, but haven't made sure yet */

	/* find and fix singular covariance matrices */
  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					MATRIX *m_cov, *m_cov_inv ;
					double det ;

					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						DiagBreak() ;
          gc = &gcan->gcs[n] ;
					m_cov = load_covariance_matrix(gc, NULL, gca->ninputs) ;
					m_cov_inv = MatrixInverse(m_cov, NULL) ;
					det = covariance_determinant(gc, gca->ninputs) ;
					if (det <= 0 )
					{
						MatrixFree(&m_cov_inv) ; m_cov_inv = NULL ;
					}
					if (m_cov_inv == NULL)  /* singular */
					{
						MATRIX *m_I ;
						m_I = MatrixIdentity(gca->ninputs, NULL) ;
						MatrixScalarMul(m_I, MIN_VAR, m_I) ;
						MatrixAdd(m_cov, m_I, m_cov) ;
						m_cov_inv = MatrixInverse(m_cov, NULL) ;
						if (m_cov_inv == NULL)
							ErrorExit(ERROR_BADPARM, "GCAcompleteCovarianceTraining: cannot regularize singular covariance matrix at (%d,%d,%d):%d",
												x, y, z, n) ;
						det = covariance_determinant(gc, gca->ninputs) ; 
						if (det < 0)
							DiagBreak() ;
						for (v = r = 0 ; r < gca->ninputs ; r++)
							for (c = r ; c < gca->ninputs ; c++, v++)
								gc->covars[v] = *MATRIX_RELT(m_cov, r+1, c+1) ;
						MatrixFree(&m_I) ;
						nregs++ ;
					}

					MatrixFree(&m_cov_inv) ;
					MatrixFree(&m_cov) ;
				}
			}
		}
	}
  gcaCheck(gca) ;

  if (total_pruned > 0)
  {
    printf("%d samples pruned during training\n", total_pruned) ;
    total_pruned = 0 ;
  }
  return(NO_ERROR) ;
}

MRI  *
GCAlabel(MRI *mri_inputs, GCA *gca, MRI *mri_dst, TRANSFORM *transform)
{
  int       x, y, z, width, height, depth, label, xn, yn, zn, n ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
	GC1D      *gc ;
  float    /*dist,*/ max_p, p, vals[MAX_GCA_INPUTS] ;

  if (!mri_dst)
  {
    mri_dst = MRIalloc(mri_inputs->width, mri_inputs->height, mri_inputs->depth,MRI_UCHAR) ;
    if (!mri_dst)
      ErrorExit(ERROR_NOMEMORY, "GCAlabel: could not allocate dst") ;
    MRIcopyHeader(mri_inputs, mri_dst) ;
  }


  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)  
          DiagBreak() ; 

        if (x == width/2 && y == height/2 && z == depth/2)
          DiagBreak() ;

        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
				load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
#if 0
        if (x == 153 && y == 119 && z == 117)  /* wm should be hippo (1484) */
        {
          Gx = xn ; Gy = yn ; Gz = zn ;
          DiagBreak() ;
        }
#endif

        gcan = &gca->nodes[xn][yn][zn] ;
        gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;
        
        label = 0 ; max_p = 2*GIBBS_NEIGHBORS*BIG_AND_NEGATIVE ;
        for (n = 0 ; n < gcap->nlabels ; n++)
        {
#if 0
					p = GCAcomputePosteriorDensity(gcap, gcan, n, vals, gca->ninputs) ;
#else
          gc = GCAfindGC(gca, xn, yn, zn, gcap->labels[n]) ;
					if (gc == NULL)
						continue ;
					p = gcaComputeLogDensity(gc, vals, gca->ninputs, gcap->priors[n],gcap->labels[n]) ;
#endif
          if (p > max_p)
          {
            max_p = p ;
            label = gcap->labels[n] ;
          }
        }
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
        {
					int i ;
          printf("(%d, %d, %d): inputs=", x, y, z) ;
					for (i = 0 ; i < gca->ninputs ; i++)
						printf("%2.1f ", vals[i]) ;
				
          printf("label %s (%d), log(p)=%2.2e, node (%d, %d, %d)\n",
                 cma_label_to_name(label), label, max_p, xn, yn, zn) ;
          dump_gcan(gca, gcan, stdout, 0, gcap) ;
        }
        MRIvox(mri_dst, x, y, z) = label ;
      }
    }
  }

  return(mri_dst) ;
}

MRI  *
GCAlabelProbabilities(MRI *mri_inputs, GCA *gca, MRI *mri_dst, TRANSFORM *transform)
{
  int       x, y, z, width, height, depth, label,
            xn, yn, zn, n ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  GC1D      *gc ;
  double    max_p, p, total_p ;
	float     vals[MAX_GCA_INPUTS] ;

  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  if (!mri_dst)
  {
    mri_dst = MRIalloc(width, height, depth, MRI_UCHAR) ;
    if (!mri_dst)
      ErrorExit(ERROR_NOMEMORY, "GCAlabel: could not allocate dst") ;
    MRIcopyHeader(mri_inputs, mri_dst) ;
  }

  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (x == 152 && y == 126 && z == 127)
          DiagBreak() ;
        if (x == 63 && y == 107 && z == 120)
          DiagBreak() ; 

				load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
        
        gcan = &gca->nodes[xn][yn][zn] ;
        gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;
        label = 0 ; max_p = 2*GIBBS_NEIGHBORS*BIG_AND_NEGATIVE ;
        for (total_p = 0.0, n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
          
          /* compute 1-d Mahalanobis distance */
					p = GCAcomputePosteriorDensity(gcap, gcan, n, vals, gca->ninputs) ;
          if (p > max_p)
          {
            max_p = p ;
            label = gcan->labels[n] ;
          }
          total_p += p ;
        }
        max_p = 255.0* max_p / total_p ; if (max_p > 255) max_p = 255 ;
        MRIvox(mri_dst, x, y, z) = (BUFTYPE)max_p ;
      }
    }
  }

  return(mri_dst) ;
}
MRI  *
GCAcomputeProbabilities(MRI *mri_inputs, GCA *gca, MRI *mri_labels, 
                        MRI *mri_dst, TRANSFORM *transform)
{
  int       x, y, z, width, height, depth, label,
            xn, yn, zn, n ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  GC1D      *gc ;
  double    label_p, p, total_p ;
	float     vals[MAX_GCA_INPUTS] ;

  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  if (!mri_dst)
  {
    mri_dst = MRIalloc(width, height, depth, MRI_UCHAR) ;
    if (!mri_dst)
      ErrorExit(ERROR_NOMEMORY, "GCAlabel: could not allocate dst") ;
    MRIcopyHeader(mri_inputs, mri_dst) ;
  }


  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (x == 152 && y == 126 && z == 127)
          DiagBreak() ;
        if (x == 63 && y == 107 && z == 120)
          DiagBreak() ; 

				load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
        label = MRIvox(mri_labels, x, y, z) ;

        gcan = &gca->nodes[xn][yn][zn] ;
        gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;
        for (label_p = total_p = 0.0, n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
          
					p = GCAcomputePosteriorDensity(gcap, gcan, n, vals, gca->ninputs) ;
          if (label == gcan->labels[n])
          {
            label_p = p ;
          }
          total_p += p ;
        }
        label_p = 255.0* label_p / total_p ; if (label_p > 255) label_p = 255 ;
        MRIvox(mri_dst, x, y, z) = (BUFTYPE)label_p ;
      }
    }
  }

  return(mri_dst) ;
}

#define STARTING_T   500

MRI  *
GCAannealUnlikelyVoxels(MRI *mri_inputs, GCA *gca, MRI *mri_dst,TRANSFORM *transform, 
          int max_iter, MRI  *mri_fixed)
{
  int       x, y, z, width, depth, height, *x_indices, *y_indices, *z_indices,
            nindices, index, iter, nchanged, xn, yn, zn, n, nbad,
            old_label  ;
  double    log_likelihood, T, delta_E, p, rn, new_ll, old_ll,
            total_likelihood ;
  GCA_NODE  *gcan ;
  MRI       *mri_bad ;

  width = mri_inputs->width ; height = mri_inputs->height ; 
  depth = mri_inputs->depth ;
  mri_bad = MRIalloc(width, height, depth, MRI_UCHAR) ;


  for (nindices = x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (mri_fixed && MRIvox(mri_fixed, x, y, z) > 0)
          continue ;
        log_likelihood = 
          gcaVoxelGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y, z,transform,
                                     PRIOR_FACTOR);
        
        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
        gcan = &gca->nodes[xn][yn][zn] ;
        if (log_likelihood < log(1.0f/(float)gcan->total_training))
        {
          MRIvox(mri_bad, x, y, z) = 1 ;
          nindices++ ;
        }
      }
    }
  }

  printf("annealing %d voxels...\n", nindices) ;
  x_indices = (int *)calloc(nindices, sizeof(int)) ;
  y_indices = (int *)calloc(nindices, sizeof(int)) ;
  z_indices = (int *)calloc(nindices, sizeof(int)) ;
  for (index = x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (MRIvox(mri_bad, x, y, z) > 0)
        {
          x_indices[index] = x ; y_indices[index] = y ; z_indices[index] = z ;
          index++ ;
        }
      }
    }
  }

  MRIfree(&mri_bad) ;
  T = STARTING_T ; iter = 0 ;
  do
  {
    total_likelihood = 0.0 ;
    for (nbad = nchanged = index = 0 ; index < nindices ; index++)
    {
      x = x_indices[index] ; y = y_indices[index] ; z = z_indices[index] ;
      if (x == 155 && y == 126 && z == 128)
        DiagBreak() ;
      
      /* find the node associated with this coordinate and classify */
      GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
      gcan = &gca->nodes[xn][yn][zn] ;
      
      if (gcan->nlabels == 1)
        continue ;
      n = (int)randomNumber(0.0, (double)gcan->nlabels-0.0001) ;
      if (gcan->labels[n] == MRIvox(mri_dst, x, y, z))
        continue ;
      old_ll = 
        gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y, z, transform,
                                  PRIOR_FACTOR) ;
      old_label = MRIvox(mri_dst, x, y, z) ;
      MRIvox(mri_dst, x, y, z) = gcan->labels[n] ;
      new_ll = 
        gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y, z, transform,
                                  PRIOR_FACTOR) ;
      delta_E = new_ll - old_ll ;
      p = exp(delta_E / T) ;
      rn = randomNumber(0.0, 1.0) ;
      
      if (p > rn)
      {
        if (new_ll < log(1.0f/(float)gcan->total_training))
          nbad++ ;
        nchanged++ ;
        total_likelihood += new_ll ;
      }
      else
      {
        total_likelihood += old_ll ;
        if (old_ll < log(1.0f/(float)gcan->total_training))
          nbad++ ;
        MRIvox(mri_dst, x, y, z) = old_label ;
      }
    }
    T = T * 0.99 ;
    fprintf(stderr, "%03d: T = %2.2f, nchanged %d, nbad = %d, ll=%2.2f\n",
            iter, T, nchanged, nbad, total_likelihood/(double)nindices) ;
    if (!nchanged)
      break ;
  } while (iter++ < max_iter) ;

  free(x_indices) ; free(y_indices) ; free(z_indices) ;
  return(mri_dst) ;
}

GCA  *
GCAreduce(GCA *gca_src)
{
#if 0
  GCA       *gca_dst ;
  int       xs, ys, zs, xd, yd, zd, swidth, sheight, sdepth,
            ns, nd, dwidth, dheight, ddepth, dof ;
  float     spacing = gca_src->spacing ;
  GCA_NODE  *gcan_src, *gcan_dst ;
  GC1D      *gc_src, *gc_dst ;

  swidth = gca_src->width ; sheight = gca_src->height; sdepth = gca_src->depth;
  
  gca_dst = 
    GCAalloc(gca_src->ninputs, spacing*2, spacing*swidth,
             spacing*gca_src->height,spacing*sdepth, gca_src->flags) ;


  dwidth = gca_dst->width ; dheight = gca_dst->height; ddepth = gca_dst->depth;

  for (zs = 0 ; zs < sdepth ; zs++)
  {
    for (ys = 0 ; ys < sheight ; ys++)
    {
      for (xs = 0 ; xs < swidth ; xs++)
      {
        xd = xs/2 ; yd = ys/2 ; zd = zs/2 ;
        if (xd == 15 && yd == 22 && zd == 35)
          DiagBreak() ;
        gcan_src = &gca_src->nodes[xs][ys][zs] ;
        gcan_dst = &gca_dst->nodes[xd][yd][zd] ;
        
        for (ns = 0 ; ns < gcan_src->nlabels ; ns++)
        {
          gc_src = &gcan_src->gcs[ns] ;
          dof = gc_src->prior * gcan_src->total_training ;
          
          /* find label in destination */
          for (nd = 0 ; nd < gcan_dst->nlabels ; nd++)
          {
            if (gcan_dst->labels[nd] == gcan_src->labels[ns])
              break ;
          }
          if (nd >= gcan_dst->nlabels)  /* couldn't find it */
          {
            if (nd >= gcan_dst->max_labels)
            {
              int  old_max_labels ;
              char *old_labels ;
              GC1D *old_gcs ;
              
              old_max_labels = gcan_dst->max_labels ; 
              gcan_dst->max_labels += 2 ;
              old_labels = gcan_dst->labels ;
              old_gcs = gcan_dst->gcs ;
              
              /* allocate new ones */
#if 0
              gcan_dst->gcs = 
                (GC1D *)calloc(gcan_dst->max_labels, sizeof(GC1D)) ;
#else
              gcan_dst->gcs = alloc_gcs(gcan_dst->max_labels, gca_dst->flags, gca_dst->ninputs) ;
#endif
              if (!gcan_dst->gcs)
                ErrorExit(ERROR_NOMEMORY, 
                          "GCANreduce: couldn't expand gcs to %d",
                          gcan_dst->max_labels) ;
              gcan_dst->labels = 
                (unsigned char *)calloc(gcan_dst->max_labels, sizeof(unsigned char)) ;
              if (!gcan_dst->labels)
                ErrorExit(ERROR_NOMEMORY, 
                          "GCANupdateNode: couldn't expand labels to %d",
                          gcan_dst->max_labels) ;
              
              /* copy the old ones over */
#if 0
              memmove(gcan_dst->gcs, old_gcs, old_max_labels*sizeof(GC1D));
#else
              copy_gcs(old_max_labels, old_gcs, gcan_dst->gcs, gca->ninputs) ;
#endif
              
              memmove(gcan_dst->labels, old_labels, 
                      old_max_labels*sizeof(unsigned char)) ;
              
              /* free the old ones */
              free(old_gcs) ; free(old_labels) ;
            }
            gcan_dst->nlabels++ ;
          }
          gc_dst = &gcan_dst->gcs[nd] ;
          gc_dst->mean += dof*gc_src->mean ;
          gc_dst->var += dof*(gc_src->var) ;
          gc_dst->prior += dof ;
          gcan_dst->total_training += dof ;
          gcan_dst->labels[nd] = gcan_src->labels[ns] ;
        }
      }
    }
  }

  for (xd = 0 ; xd < gca_dst->width ; xd++)
  {
    for (yd = 0 ; yd < gca_dst->height ; yd++)
    {
      for (zd = 0 ; zd < gca_dst->depth ; zd++)
      {
        gcan_dst = &gca_dst->nodes[xd][yd][zd] ;
        for (nd = 0 ; nd < gcan_dst->nlabels ; nd++)
        {
          float var ;

          gc_dst = &gcan_dst->gcs[nd] ;
          dof = gc_dst->prior ;/* prior is count of # of occurences now */
          gc_dst->mean /= dof ;
          var = gc_dst->var / dof ;
          if (var < -0.1)
            DiagBreak() ;
          if (var < MIN_VAR)
            var = MIN_VAR ;
          gc_dst->var = var ;
          gc_dst->prior /= (float)gcan_dst->total_training ;
        }
      }
    }
  }
  return(gca_dst) ;
#else
  /* have to update to include priors at different resolution than nodes */
  ErrorReturn(NULL, (ERROR_UNSUPPORTED, "GCAreduce: not currently supported") ;) ;
#endif
}


typedef struct
{
  int   label ;
  float prob ;
	int   index ;
} LABEL_PROB ;

static int compare_sort_probabilities(const void *plp1, const void *plp2);
MRI *
GCAclassify(MRI *mri_inputs,GCA *gca,MRI *mri_dst,TRANSFORM *transform,int max_labels)
{
  int        x, y, z, width, height, depth,
             xn, yn, zn, n ;
  GCA_NODE   *gcan ;
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  float      max_p, p, total_p ;
  LABEL_PROB label_probs[1000] ;
	float      vals[MAX_GCA_INPUTS] ;

  if (max_labels > MAX_LABELS_PER_GCAN || max_labels <= 0)
    max_labels = MAX_LABELS_PER_GCAN ;

  if (!mri_dst)
  {
    int width = mri_inputs->width, height = mri_inputs->height, 
        depth = mri_inputs->depth ;

    mri_dst = MRIallocSequence(width, height, depth, MRI_UCHAR, 2*max_labels) ;
    if (!mri_dst)
      ErrorExit(ERROR_NOMEMORY, "GCAlabel: could not allocate dst") ;
    MRIcopyHeader(mri_inputs, mri_dst) ;
  }

  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (x == 67 && y == 87 && z == 114)
          DiagBreak() ; 

				load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;

        /* find the node associated with this coordinate and classify */
        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
        gcan = &gca->nodes[xn][yn][zn] ;
        gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;
        max_p = 2*GIBBS_NEIGHBORS*BIG_AND_NEGATIVE ;
        for (total_p = 0.0, n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;

					p = GCAcomputePosteriorDensity(gcap, gcan, n, vals, gca->ninputs) ;
          total_p += p ;
          label_probs[n].prob = p ; label_probs[n].label = gcan->labels[n] ;
        }
        /* now sort the labels and probabilities */
        qsort(label_probs, gcan->nlabels, sizeof(LABEL_PROB), 
              compare_sort_probabilities) ;

        for (n = 0 ; n < max_labels ; n++)
        {
          if (n < gcan->nlabels)
          {
            MRIseq_vox(mri_dst, x, y, z, n*2) = label_probs[n].label ;
            MRIseq_vox(mri_dst, x, y, z, n*2+1) = 
              (BUFTYPE)nint(255.0*label_probs[n].prob/total_p) ;
          }
          else
          {
            MRIseq_vox(mri_dst, x, y, z, n*2) = 255 ;
            MRIseq_vox(mri_dst, x, y, z, n*2+1) = 0 ;
          }
        }
      }
    }
  }

  return(mri_dst) ;
}
 

static int
compare_sort_probabilities(const void *plp1, const void *plp2)
{
  LABEL_PROB  *lp1, *lp2 ;

  lp1 = (LABEL_PROB *)plp1 ;
  lp2 = (LABEL_PROB *)plp2 ;

  if (lp1->prob > lp2->prob)
    return(1) ;
  else if (lp1->prob < lp2->prob)
    return(-1) ;

  return(0) ;
}

int
GCAremoveOutlyingSamples(GCA *gca, GCA_SAMPLE *gcas, MRI *mri_inputs,
                         TRANSFORM *transform,int nsamples, float nsigma)
{
  int        x, y, z, width, height, depth,
             i, nremoved, xp, yp, zp ;
  double     dist ;
	float      vals[MAX_GCA_INPUTS] ;
	/*	GC1D       *gc ;*/


  /* go through each GC in the sample and compute the probability of
     the image at that point.
  */
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  TransformInvert(transform, mri_inputs) ;
  for (nremoved = i = 0 ; i < nsamples ; i++)
  {
    if (i == Gdiag_no)
      DiagBreak() ;
    if (Gdiag_no == gcas[i].label)
      DiagBreak() ;
    if (gcas[i].label <= 0)
      continue ;

    xp = gcas[i].xp ; yp = gcas[i].yp ; zp = gcas[i].zp ; 
    GCApriorToSourceVoxel(gca, mri_inputs, transform, xp, yp, zp, &x, &y, &z) ;
		load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;

    if (xp == Ggca_x && yp == Ggca_y && zp == Ggca_z)
      DiagBreak() ;

#if 0
    gc = GCAfindPriorGC(gca, xp, yp, zp, gcas[i].label) ;
    if (gc == NULL)
    {
      ErrorPrintf(ERROR_BADPARM, "gc %d not found in GCAremoveOutlyingSamples!", i) ;
      continue ;
    }
#endif

    dist = sqrt(GCAsampleMahDist(&gcas[i], vals, gca->ninputs)) ;
    if (dist >= nsigma)
    {
      nremoved++ ;
      gcas[i].log_p = BIG_AND_NEGATIVE ;
      gcas[i].label = 0 ;
    }

  }

	if (DIAG_VERBOSE_ON)
		printf("%d outlying samples removed...\n", nremoved) ;

  return(NO_ERROR) ;
}
float
GCAnormalizedLogSampleProbability(GCA *gca, GCA_SAMPLE *gcas, 
                               MRI *mri_inputs, TRANSFORM *transform, int nsamples)
{
  int        x, y, z, width, height, depth,
             xn, yn, zn, i, n, xp, yp, zp ;
  GCA_NODE   *gcan ;
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  double     total_log_p, log_p, norm_log_p ;
	float      vals[MAX_GCA_INPUTS] ;


  /* go through each GC in the sample and compute the probability of
     the image at that point.
  */
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  TransformInvert(transform, mri_inputs) ;
  for (total_log_p = 0.0, i = 0 ; i < nsamples ; i++)
  {
    if (i == Gdiag_no)
      DiagBreak() ;
    if (Gdiag_no == gcas[i].label)
      DiagBreak() ;

    xp = gcas[i].xp ; yp = gcas[i].yp ; zp = gcas[i].zp ; 
    GCApriorToSourceVoxel(gca, mri_inputs, transform, xp, yp, zp, &x, &y, &z) ;
    GCApriorToNode(gca, xp, yp, zp, &xn, &yn, &zn) ;
		load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
    
    gcan = &gca->nodes[xn][yn][zn] ;
    gcap = &gca->priors[xp][yp][zp] ;
    if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z)
      DiagBreak() ;
    
    for (norm_log_p = 0.0f, n = 0 ; n < gcan->nlabels ; n++)
    {
      gc = &gcan->gcs[n] ;
      norm_log_p += GCAcomputeConditionalDensity(gc, vals, gca->ninputs, gcan->labels[n]) ;
    }
    norm_log_p = log(norm_log_p) ;
    gc = GCAfindPriorGC(gca, xp, yp, zp, gcas[i].label) ;
    log_p = GCAcomputeConditionalDensity(gc, vals, gca->ninputs, gcas[i].label) ;
    log_p = log(log_p) + log(gcas[i].prior) ;
    log_p -= norm_log_p ;
    total_log_p += log_p ;
    gcas[i].log_p = log_p ;

    if (!check_finite("1", total_log_p))
    {
      fprintf(stderr, 
              "total log p not finite at (%d, %d, %d)\n", x, y, z) ;
      DiagBreak() ;
    }
  }

  return((float)total_log_p) ;
}

float
GCAcomputeLogSampleProbability(GCA *gca, GCA_SAMPLE *gcas, 
                               MRI *mri_inputs, TRANSFORM *transform, int nsamples)
{
  int        x, y, z, width, height, depth, i, xp, yp, zp ;
  float      vals[MAX_GCA_INPUTS] ;
  double     total_log_p, log_p ;

  /* go through each GC in the sample and compute the probability of
     the image at that point.
  */
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  TransformInvert(transform, mri_inputs) ;
  for (total_log_p = 0.0, i = 0 ; i < nsamples ; i++)
  {
    if (i == Gdiag_no)
      DiagBreak() ;
    if (Gdiag_no == gcas[i].label)
      DiagBreak() ;
		if (i == Gdiag_no || 
 				(gcas[i].xp == Gx && gcas[i].yp == Gy && gcas[i].zp == Gz))
 			DiagBreak() ;
		

    xp = gcas[i].xp ; yp = gcas[i].yp ; zp = gcas[i].zp ; 
    GCApriorToSourceVoxel(gca,mri_inputs, transform, xp, yp, zp, &x, &y, &z) ;
    gcas[i].x = x ; gcas[i].y = y ; gcas[i].z = z ;
	 load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;

    log_p = gcaComputeSampleLogDensity(&gcas[i], vals, gca->ninputs) ;
    total_log_p += log_p ;
    gcas[i].log_p = log_p ;

    if (!check_finite("2", total_log_p))
    {
      fprintf(stderr, 
              "total log p not finite at (%d, %d, %d)\n", 
                                    x, y, z) ;
      DiagBreak() ;
    }
  }

  return((float)total_log_p) ;
}
float
GCAcomputeLogSampleProbabilityUsingCoords(GCA *gca, GCA_SAMPLE *gcas, 
                               MRI *mri_inputs, TRANSFORM *transform, int nsamples)
{
  int        x, y, z, width, height, depth, xp, yp, zp,
             xn, yn, zn, i ;
  float      vals[MAX_GCA_INPUTS] ;
  double     total_log_p, log_p ;


  /* go through each GC in the sample and compute the probability of
     the image at that point.
  */
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  for (total_log_p = 0.0, i = 0 ; i < nsamples ; i++)
  {
    if (i == Gdiag_no)
      DiagBreak() ;
    if (Gdiag_no == gcas[i].label)
      DiagBreak() ;

    xp = gcas[i].xp ; yp = gcas[i].yp ; zp = gcas[i].zp ; 
    GCApriorToNode(gca, xp, yp, zp, &xn, &yn, &zn) ;
    x = gcas[i].x ; y = gcas[i].y ; z = gcas[i].z ; 
		load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;

		log_p = gcaComputeSampleLogDensity(&gcas[i], vals, gca->ninputs) ;
    total_log_p += log_p ;
    gcas[i].log_p = log_p ;

    if (!check_finite("3", total_log_p))
    {
      DiagBreak() ;
      fprintf(stderr, 
              "total log p not finite at (%d, %d, %d)\n", x, y, z) ;
    }
  }

  return((float)total_log_p) ;
}
/*
  compute the probability of the image given the transform and the class
  stats.
*/
float
GCAcomputeLogImageProbability(GCA *gca, MRI *mri_inputs, MRI *mri_labels,
                              TRANSFORM *transform)
{
  int        x, y, z, width, height, depth,
             xn, yn, zn, n, label ;
  GCA_NODE   *gcan ;
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  double     total_log_p ;
	float      vals[MAX_GCA_INPUTS] ;


  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ;
  for (total_log_p = 0.0, x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        if (x == 85 && y == 89 && z == 135)
          DiagBreak() ; 

				load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
        label = MRIvox(mri_labels, x, y, z) ;

        /* find the node associated with this coordinate and classify */
        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
        gcan = &gca->nodes[xn][yn][zn] ;
        gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          if (gcan->labels[n] == label)
            break ;
        }
        if (n < gcan->nlabels)
        {
          gc = &gcan->gcs[n] ;
         
          total_log_p += gcaComputeLogDensity(gc, vals, gca->ninputs, getPrior(gcap, label), label) ;
#if 0
            -log(sqrt(gc->var)) - 
            0.5 * (dist*dist/gc->var) +
            log(getPrior(gcap, label)) ;
#endif
          if (!check_finite("4", total_log_p))
          {
            DiagBreak() ;
            fprintf(stderr, 
                    "total log p not finite at (%d, %d, %d) n = %d,\n", x, y, z, n) ;
          }
        }
      }
    }
  }

  return((float)total_log_p) ;
}

#if 0
int
GCAsampleStats(GCA *gca, MRI *mri, int class, 
               Real x, Real y, Real z,
               Real *pmean, Real *pvar, Real *pprior)
{
  int    xm, xp, ym, yp, zm, zp, width, height, depth, n ;
  Real   val, xmd, ymd, zmd, xpd, ypd, zpd ;  /* d's are distances */
  Real   prior, mean, var, wt ;
  GCAN   *gcan ;
  GC1D   *gc ;

  width = gca->node_width ; height = gca->node_height ; depth = gca->node_depth ;

  xm = MAX((int)x, 0) ;
  xp = MIN(width-1, xm+1) ;
  ym = MAX((int)y, 0) ;
  yp = MIN(height-1, ym+1) ;
  zm = MAX((int)z, 0) ;
  zp = MIN(depth-1, zm+1) ;

  xmd = x - (float)xm ;
  ymd = y - (float)ym ;
  zmd = z - (float)zm ;
  xpd = (1.0f - xmd) ;
  ypd = (1.0f - ymd) ;
  zpd = (1.0f - zmd) ;

  prior = mean = var = 0.0 ;


  gcan = &gca->nodes[xm][ym][zm] ;
  wt = xpd * ypd * zpd 
  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == class)
      break ;
  }
  if (n < gcan->nlabels)   /* found it */
  {
    gc = &gcan->gcs[n] ;
    prior += wt * gc->prior ;
  }

  gcan = &gca->nodes[xp][ym][zm] ;
  wt = xmd * ypd * zpd 

  gcan = &gca->nodes[xm][yp][zm] ;
  wt = xpd * ymd * zpd ;

  gcan = &gca->nodes[xp][yp][zm] ;
  wt = xmd * ymd * zpd ;

  gcan = &gca->nodes[xm][ym][zp] ;
  wt = xpd * ypd * zmd ;

  gcan = &gca->nodes[xp][ym][zp] ;
  wt = xmd * ypd * zmd ;

  gcan = &gca->nodes[xm][yp][zp] ;
  wt = xpd * ymd * zmd ;

  gcan = &gca->nodes[xp][yp][zp] ;
  wt = xmd * ymd * zmd ;
  return(NO_ERROR) ;
}
#endif  

#define MIN_SPACING   8

int
GCAtransformSamples(GCA *gca_src, GCA *gca_dst, GCA_SAMPLE *gcas, int nsamples)
{
  int       scale, i, label, xd, yd, zd, xs, ys, zs, n, xk, yk, zk, 
            xd0, yd0, zd0, min_y_i, xdn, ydn, zdn ;
  float     max_p, min_v, vscale, min_y, prior ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  GC1D      *gc ;
  MRI       *mri_found ;

  vscale = 1 ;
  mri_found = MRIalloc(gca_dst->node_width*vscale, gca_dst->node_height*vscale, 
                       gca_dst->node_depth*vscale, MRI_UCHAR) ;

  scale = gca_src->prior_spacing / gca_dst->prior_spacing ;
  min_y = 10000 ; min_y_i = -1 ;
  for (i = 0 ; i < nsamples ; i++)
  {
    label = gcas[i].label ; 
    
    xs = gcas[i].xp ; ys = gcas[i].yp ; zs = gcas[i].zp ; 
    xd0 = (int)(xs * scale) ; yd0 = (int)(ys * scale) ; 
    zd0 = (int)(zs * scale) ;
    max_p = -1.0 ;  /* find appropriate label with highest prior in dst */
    min_v =  10000000.0f ;
    for (xk = -scale/2 ; xk <= scale/2 ; xk++)
    {
      xd = MIN(MAX(0, xd0+xk), gca_dst->prior_width-1) ;
      for (yk = -scale/2 ; yk <= scale/2 ; yk++)
      {
        yd = MIN(MAX(0, yd0+yk), gca_dst->prior_height-1) ;
        for (zk = -scale/2 ; zk <= scale/2 ; zk++)
        {
          zd = MIN(MAX(0, zd0+zk), gca_dst->prior_height-1) ;
          if (MRIvox(mri_found, (int)(xd*vscale), (int)(yd*vscale), 
                     (int)(zd*vscale)))
            continue ;
          GCApriorToNode(gca_dst, xd, yd, zd, &xdn, &ydn, &zdn) ;
          gcan = &gca_dst->nodes[xdn][ydn][zdn] ;
          gcap = &gca_dst->priors[xd][yd][zd] ;
          for (n = 0 ; n < gcan->nlabels ; n++)
          {
            gc = &gcan->gcs[n] ;
            prior = getPrior(gcap, gcan->labels[n]) ;
            if (gcan->labels[n] == label && 
                (prior > max_p /*|| 
																 (FEQUAL(prior,max_p) && gc->var < min_v)*/))
            {
							/*              min_v = gc->var ;*/
              max_p = prior ;
              gcas[i].xp = xd ; gcas[i].yp = yd ; gcas[i].zp = zd ; 
            }
          }
        }
      }
    }
    if (max_p < 0)
    {
      fprintf(stderr, "WARNING: label %d not found at (%d,%d,%d)\n",
              label, gcas[i].xp, gcas[i].yp, gcas[i].zp) ;
      DiagBreak() ;
    }
    MRIvox(mri_found, (int)(gcas[i].xp*vscale),
           (int)(gcas[i].yp*vscale), (int)(gcas[i].zp*vscale))= 1;
    if (gcas[i].yp < min_y)
    {
      min_y = gcas[i].yp ;
      min_y_i = i ;
    }
  }

  i = min_y_i ;
  fprintf(stderr, "min_y = (%d, %d, %d) at i=%d, label=%d\n",
          gcas[i].xp, gcas[i].yp, gcas[i].zp, i, gcas[i].label) ;
  MRIfree(&mri_found) ;
  return(NO_ERROR) ;
}

/* don't use a label with prior less than this */
#define MIN_MAX_PRIORS 0.5


static int exclude_classes[] = 
{
  0 /*1, 6, 21, 22, 23, 24, 25, 30, 57, 61, 62, 63*/
} ;


#define TILES     3
#define MAX_PCT   .1

static int compare_gca_samples(const void *pc1, const void *pc2) ;

static int
compare_gca_samples(const void *pgcas1, const void *pgcas2)
{
  register GCA_SAMPLE *gcas1, *gcas2 ;

  gcas1 = (GCA_SAMPLE *)pgcas1 ;
  gcas2 = (GCA_SAMPLE *)pgcas2 ;

/*  return(c1 > c2 ? 1 : c1 == c2 ? 0 : -1) ;*/
  if (FEQUAL(gcas1->prior, gcas2->prior))
  {
#if 0
    if (FEQUAL(gcas1->var, gcas2->var))
    {
      int  zv1, zv2 ;

      zv1 = gcas1->zn%10 ; zv2 = gcas2->zn%10 ;
      if (zv1 == zv2)
      {
        int  yv1, yv2 ;

        yv1 = gcas1->yn%10 ; zv2 = gcas2->yn%10 ;      
        return(yv1-yv2) ;
      }

      return(zv1-zv2) ;
    }
#endif

#if 0
    if (gcas1->var > gcas2->var)
      return(1) ;
    else
      return(-1) ;
#else
		/* check size of determinants */
		return(1) ;
#endif
  }

  if (gcas1->prior > gcas2->prior)
    return(-1) ;
  else if (gcas1->prior < gcas2->prior)
    return(1) ;

  return(0) ;
}
#define MAX_SPACING   16  /* mm */

GCA_SAMPLE *
GCAfindStableSamplesByLabel(GCA *gca, int nsamples, float min_prior)
{
  GCA_SAMPLE *gcas, *ordered_labels[MAX_DIFFERENT_LABELS], *gcas2 ;
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  int        found[MAX_DIFFERENT_LABELS], x, y, z, width, height, depth, n,
             label, nfound, samples_added[MAX_DIFFERENT_LABELS] ;
  float      histo[MAX_DIFFERENT_LABELS], spacing, scale ;
  int        volume[TILES][TILES], max_class, total, extra, total_found,
             label_counts[MAX_DIFFERENT_LABELS], 
             current_index[MAX_DIFFERENT_LABELS],
             ordered_label_counts[MAX_DIFFERENT_LABELS], i, index,
             *x_indices, *y_indices, *z_indices, nindices ;

  memset(histo, 0, sizeof(histo)) ;
  memset(label_counts, 0, sizeof(label_counts)) ;
  memset(samples_added, 0, sizeof(samples_added)) ;
  memset(current_index, 0, sizeof(current_index)) ;
  memset(ordered_label_counts, 0, sizeof(ordered_label_counts)) ;
  memset(found, 0, sizeof(found)) ;
  memset(volume, 0, sizeof(volume)) ;
  gcas = calloc(nsamples, sizeof(GCA_SAMPLE )) ;
  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;

  /* compute the max priors and min variances for each class */
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        gcap = &gca->priors[x][y][z] ;
        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          label = gcap->labels[n] ;
          if (label == Gdiag_no)
            DiagBreak() ;
          histo[label] += (float)nint(gcap->total_training*gcap->priors[n]) ;
          label_counts[label]++ ;
        }
      }
    }
  }

  for (max_class = MAX_DIFFERENT_LABELS ; max_class >= 0 ; max_class--)
    if (histo[max_class] > 0)
      break ;
  fprintf(stderr, "max class = %d\n", max_class) ;

  /* count total # of samples */
  for (total = 0, n = 1 ; n <= max_class ; n++)
    total += histo[n] ;

  fprintf(stderr, "%d total training samples found\n", total) ;

  /* turn histogram into samples/class */
  for (n = 1 ; n <= max_class ; n++)
    histo[n] = (float)nint(0.25+histo[n]*(float)nsamples/total) ;

  for (n = 0 ; n < sizeof(exclude_classes)/sizeof(exclude_classes[0]) ; n++)
    histo[exclude_classes[n]] = 0 ;

  /* crop max # per class */
  for (n = 1 ; n <= max_class ; n++)
    if (histo[n] > nint(MAX_PCT*nsamples))
      histo[n] = nint(MAX_PCT*nsamples) ;
      
  for (extra = 0, n = 1 ; n <= max_class ; n++)
    extra += histo[n] ;
  extra = nsamples-extra ;  /* from rounding */
  printf("%d extra samples for redistribution...\n", extra) ;

  /* first add to classes with only one sample */
  for (n = 1 ; extra > 0 && n <= max_class ; n++)
  {
    if (histo[n] == 1)
    {
      histo[n]++ ;
      extra-- ;
    }
  }  

  while (extra > 0)  /* add 1 to each class */
  {
    for (n = 1 ; extra > 0 && n <= max_class ; n++)
    {
      if (histo[n] >= 1)
      {
        histo[n]++ ;
        extra-- ;
      }
    }
  }

  {
    FILE *fp ;
    fp = fopen("classes.dat", "w") ;
    for (n = 1 ; n <= max_class ; n++)
      if (!FZERO(histo[n]))
        fprintf(fp, "%d  %2.1f\n", n, histo[n]) ;
    fclose(fp) ;
  }

  /* allocate arrays that will be used for sorting */
  for (n = 1 ; n <= max_class ; n++)
    if (histo[n] > 0)
    {
      ordered_labels[n] = 
        (GCA_SAMPLE *)calloc(label_counts[n], sizeof(GCA_SAMPLE)) ;
      if (!ordered_labels[n])
        ErrorExit(ERROR_NO_MEMORY, 
                  "GCAfindStableSamplesByLabel: could not allocate %d samples "
                  "for label %d", label_counts[n], n) ;
    }


  nindices = width*height*depth ;
  x_indices = (int *)calloc(nindices, sizeof(int)) ;
  y_indices = (int *)calloc(nindices, sizeof(int)) ;
  z_indices = (int *)calloc(nindices, sizeof(int)) ;

  for (i = 0 ; i < nindices ; i++)
  {
    x_indices[i] = i % width ;
    y_indices[i] = (i/width) % height ;
    z_indices[i] = (i / (width*height)) % depth ;
  }
  for (i = 0 ; i < nindices ; i++)
  {
    int tmp ;
    index = (int)randomNumber(0.0, (double)(nindices-0.0001)) ;

    tmp = x_indices[index] ; 
    x_indices[index] = x_indices[i] ; x_indices[i] = tmp ;

    tmp = y_indices[index] ; 
    y_indices[index] = y_indices[i] ; y_indices[i] = tmp ;

    tmp = z_indices[index] ; 
    z_indices[index] = z_indices[i] ; z_indices[i] = tmp ;
  }

  for (index = 0 ; index < nindices ; index++)
  {
    x = x_indices[index] ; y = y_indices[index] ; z = z_indices[index] ;
    gcap = &gca->priors[x][y][z] ;
    for (n = 0 ; n < gcap->nlabels ; n++)
    {
      label = gcap->labels[n] ;
      gc = GCAfindPriorGC(gca, x, y, z, label) ;
      if (histo[label] > 0)
      {
        i = ordered_label_counts[label] ;
        ordered_label_counts[label]++ ;
				ordered_labels[label][i].means = (float *)calloc(gca->ninputs, sizeof(float)) ;
				ordered_labels[label][i].covars = (float *)calloc((gca->ninputs*(gca->ninputs+1))/2, sizeof(float)) ;
				if (!ordered_labels[label][i].means || !ordered_labels[label][i].covars)
					ErrorExit(ERROR_NOMEMORY, "could not allocate mean (%d) and covariance (%d) matrices",
										gca->ninputs, gca->ninputs*(gca->ninputs+1)/2) ;
        ordered_labels[label][i].xp = x ;
        ordered_labels[label][i].yp = y ;
        ordered_labels[label][i].zp = z ;
        ordered_labels[label][i].label = label ;
        ordered_labels[label][i].prior = getPrior(gcap, label) ;
        if (!gc)
        {
					int  r, c, v ;
					for (v = r = 0 ; r < gca->ninputs ; r++)
					{
						ordered_labels[label][i].means[r] = 0.0 ;
						for (c = r ; c < gca->ninputs ; c++, v++)
						{
							if (c==r)
								ordered_labels[label][i].covars[v] = 1.0 ;
							else
								ordered_labels[label][i].covars[v] = 0 ;
						}
					}
        }
        else
        {
					int  r, c, v ;
					for (v = r = 0 ; r < gca->ninputs ; r++)
					{
						ordered_labels[label][i].means[r] = gc->means[r] ;
						for (c = r ; c < gca->ninputs ; c++, v++)
						{
							ordered_labels[label][i].covars[v] = gc->covars[v] ;
						}
					}
        }
      }
    }
  }

  free(x_indices) ; free(y_indices) ; free(z_indices) ;

  /* sort each list of labels by prior, then by variance */
  for (n = 1 ; n <= max_class ; n++)
    if (histo[n] > 0)
    {
      qsort(ordered_labels[n], ordered_label_counts[n], sizeof(GCA_SAMPLE), 
            compare_gca_samples) ;
    }

  total_found = nfound = 0 ;

  for (spacing = MAX_SPACING ; spacing >= gca->node_spacing ; spacing /= 2)
  {
    MRI  *mri_found ;
    int  xv, yv, zv ;

    for (n = 1 ; n <= max_class ; n++) 
      current_index[n] = 0 ;

    scale = gca->prior_spacing / spacing ;
    mri_found = MRIalloc(width*scale, height*scale, depth*scale, MRI_UCHAR);
    for (i = 0 ; i < total_found ; i++)
    {
      xv = gcas[i].xp*scale ; yv = gcas[i].yp*scale ; zv = gcas[i].zp*scale ;
      MRIvox(mri_found, xv, yv, zv) = 1 ;
    }

    do
    {

      nfound = 0 ;
      for (n = 1 ; n <= max_class ; n++) 
      {
        if (samples_added[n] < histo[n])/* add another sample from this class*/
        {
          while ((ordered_labels[n][current_index[n]].label < 0) &&
                 current_index[n] < ordered_label_counts[n])
            current_index[n]++ ;
          if (current_index[n] < ordered_label_counts[n])
          {
            gcas2 = &ordered_labels[n][current_index[n]] ;
            current_index[n]++ ; 
            xv = gcas2->xp*scale ; yv = gcas2->yp*scale ; zv = gcas2->zp*scale;
            
            if (n == Gdiag_no)
              DiagBreak() ;
            
            if (!MRIvox(mri_found, xv, yv, zv)) /* none at this location yet */
            {
              if (gcas2->label == Gdiag_no)
                DiagBreak() ;
              samples_added[n]++ ;
              gcas[total_found+nfound++] = *gcas2 ;
              MRIvox(mri_found, xv, yv, zv) = gcas2->label ;
              gcas2->label = -1 ;
              if (nfound+total_found >= nsamples)
                break ;
            }
          }
        }
      }

      total_found += nfound ;
    } while ((nfound > 0) && total_found < nsamples) ;
    MRIfree(&mri_found) ;
  } 

  if (total_found != nsamples)
  {
    ErrorPrintf(ERROR_BADPARM, "could only find %d samples!\n", total_found) ;
  }
  return(gcas) ;
}


GCA_SAMPLE *
GCAfindContrastSamples(GCA *gca, int *pnsamples, int min_spacing,
                       float min_prior)
{
#if 0
  GCA_SAMPLE *gcas ;
  GC1D       *gc ;
  int        x, y, z, width, height, depth, label, nfound, prior_stride,
             label_counts[MAX_DIFFERENT_LABELS], best_label, nzeros,
             xi, yi, zi, xk, yk, zk, i, j, k, labels[3][3][3], found ;
  float      max_prior, total_mean,
             priors[3][3][3][MAX_DIFFERENT_LABELS], best_means[MAX_GCA_INPUTS], best_sigma,
             means[3][3][3][MAX_DIFFERENT_LABELS][MAX_GCA_INPUTS], mean, sigma,
             vars[3][3][3][MAX_DIFFERENT_LABELS] ;

  memset(label_counts, 0, sizeof(label_counts)) ;
  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  gcas = calloc(width*height*depth, sizeof(GCA_SAMPLE)) ;
  
  prior_stride = min_spacing / gca->node_spacing ;

  total_mean = 0.0 ;

  /* compute the max priors and min variances for each class */
  for (nzeros = nfound = x = 0 ; x < width ; x += prior_stride)
  {
    for (y = 0 ; y < height ; y += prior_stride)
    {
      for (z = 0 ; z < depth ; z += prior_stride)
      {
        if (abs(x-31)<=prior_stride &&
            abs(y-22)<=prior_stride &&
            abs(z-36)<=prior_stride)
          DiagBreak() ;
        for (xk = -1 ; xk <= 1 ; xk++)
        {
          xi = x+xk ; i = xk+1 ;
          if (xi < 0 || xi >= width)
            continue ;
          for (yk = -1 ; yk <= 1 ; yk++)
          {
            yi = y+yk ; j = yk + 1 ;
            if (yi < 0 || yi >= height)
              continue ;
            for (zk = -1 ; zk <= 1 ; zk++)
            {
              zi = z+zk ; k = zk+1 ;
              if (zi < 0 || zi >= depth)
                continue ;
              gcaRegionStats(gca, xi, yi, zi, 
                             prior_stride/3, 
                             priors[i][j][k], 
                             vars[i][j][k], 
                             means[i][j][k]) ;
              if (priors[i][j][k][4] > .5*min_prior ||  /* left lat ven */
                  priors[i][j][k][5] > .5*min_prior ||  /* inf left lat ven */
                  priors[i][j][k][14] > .5*min_prior ||  /* 3rd ven */
                  priors[i][j][k][15] > .5*min_prior ||  /* 4th ven */
                  priors[i][j][k][43] > .5*min_prior ||
                  priors[i][j][k][44] > .5*min_prior)
                DiagBreak() ;
              max_prior = 0 ;

              /* find highest prior label */
              for (label = 0 ; label < MAX_DIFFERENT_LABELS ; label++)
              {
                if (priors[i][j][k][label] > max_prior)
                {
                  max_prior = priors[i][j][k][label] ;
                  labels[i][j][k] = label ;
                }
              }
            }
          }
        }

        /* search nbrhd for high-contrast stable label */
        best_label = labels[1][1][1] ; found = 0 ;
        best_sigma = sqrt(vars[1][1][1][best_label]) ;
				for (r = 0 ; r < gca->ninputs ; r++)
					best_means[r] = means[1][1][1][best_label][r] ; 

        gc = gcaFindHighestPriorGC(gca, x, y, z, best_label,prior_stride/3);
        if (!gc || get_node_prior(gca, best_label, x, y, z) < min_prior)
          continue ;

        for (xk = -1 ; xk <= 1 ; xk++)
        {
          xi = x+xk ; i = xk+1 ;
          if (xi < 0 || xi >= width)
            continue ;
          for (yk = -1 ; yk <= 1 ; yk++)
          {
            yi = y+yk ; j = yk + 1 ;
            if (yi < 0 || yi >= height)
              continue ;
            for (zk = -1 ; zk <= 1 ; zk++)
            {
              zi = z+zk ; k = zk+1 ;
              if (zi < 0 || zi >= depth)
                continue ;
              if (i == 1 && j == 1 && k == 1)
                continue ;
              label = labels[i][j][k] ;
              if (priors[i][j][k][label] < min_prior || label == best_label)
                continue ;
              
              mean = means[i][j][k][label] ; 
              sigma = sqrt(vars[i][j][k][label]) ;
              gc = gcaFindHighestPriorGC(gca, xi, yi, zi, label,prior_stride/3);
              if (!gc || get_node_prior(gca, label, xi, yi, zi) < min_prior)
                continue ;
              if (abs(best_mean - mean) > 4*abs(best_sigma+sigma))
              {
#if 0
                if (gcaFindBestSample(gca, xi, yi, zi, label, prior_stride/3, 
                         &gcas[nfound]) == NO_ERROR)
#else
                if (gcaFindClosestMeanSample(gca, mean, min_prior,
                                             xi, yi, zi, label, 
                                             prior_stride/3, &gcas[nfound]) == 
                    NO_ERROR)
#endif
                {
                  if (label == Gdiag_no)
                    DiagBreak() ;
                  found = 1 ;
                  if (label > 0)
                    total_mean += means[i][j][k][label] ;
                  else
                    nzeros++ ;
                  label_counts[label]++ ;
                  nfound++ ;
                }
              }
            }
          }
        }

        if (found)   /* add central label */
        {
          if (best_label == Gdiag_no)
            DiagBreak() ;
#if 0
          if (gcaFindBestSample(gca, x, y, z, best_label, prior_stride/3, 
                                &gcas[nfound]) == NO_ERROR)
#else
          if (gcaFindClosestMeanSample(gca, best_mean, min_prior, x, y, z, 
                                       best_label, prior_stride/3, 
                                       &gcas[nfound]) == 
              NO_ERROR)
#endif
          {
            if (best_label > 0)
						{
							for (r = 0 ; r < gca->ninputs ; r++)
								total_means[r] += means[1][1][1][best_label][r] ;
						}
            else
              nzeros++ ;
            label_counts[best_label]++ ;
            nfound++ ;
          }
        }
      }
    }
  }

  fprintf(stderr, "total sample mean = %2.1f (%d zeros)\n", 
          total_mean/((float)nfound-nzeros), nzeros) ;

  {
    int  n ;
    FILE *fp ;

    fp = fopen("classes.dat", "w") ;
    for (n = 0 ; n < MAX_DIFFERENT_LABELS ; n++)
      if (label_counts[n] > 0)
        fprintf(fp, "%d  %d\n", n, label_counts[n]) ;
    fclose(fp) ;
  }

  *pnsamples = nfound ;

  return(gcas) ;
#else
	ErrorReturn(NULL, (ERROR_UNSUPPORTED, "GCAfindContrastSamples: no longer supported")) ;
#endif
}
#if 1
GCA_SAMPLE *
GCAfindStableSamples(GCA *gca, int *pnsamples, int min_spacing,float min_prior, int *exclude_list)
{
  GCA_SAMPLE *gcas ;
  int        xi, yi, zi, width, height, depth, label, nfound, 
             label_counts[MAX_DIFFERENT_LABELS], best_label, nzeros, r ;
  float      max_prior, total_means[MAX_GCA_INPUTS], /*mean_dist,*/ best_mean_dist,
             priors[MAX_DIFFERENT_LABELS], means[MAX_DIFFERENT_LABELS][MAX_GCA_INPUTS], 
             vars[MAX_DIFFERENT_LABELS], max_priors[MAX_DIFFERENT_LABELS],
             prior_stride, x, y, z, min_unknown[MAX_GCA_INPUTS], max_unknown[MAX_GCA_INPUTS], prior_factor ;
  MRI        *mri_filled ;

#define MIN_UNKNOWN_DIST  2

  gcaFindMaxPriors(gca, max_priors) ;
  gcaFindIntensityBounds(gca, min_unknown, max_unknown) ;
  printf("bounding unknown intensity as < ") ;
	for (r = 0 ; r < gca->ninputs ; r++)
		printf("%2.1f ", min_unknown[r]) ;
  printf("or > ") ;
	for (r = 0 ; r < gca->ninputs ; r++)
		printf("%2.1f ", max_unknown[r]) ;
	printf("\n") ;


  memset(label_counts, 0, sizeof(label_counts)) ;
  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  gcas = calloc(width*height*depth, sizeof(GCA_SAMPLE)) ;
  if (!gcas)
    ErrorExit(ERROR_NOMEMORY, "%s: could not allocate %dK x %d samples\n",
              "GCAfindStableSamples", width*height*depth/1024, sizeof(GCA_SAMPLE)) ;
#if 0
	for (n = 0 ; n < width*height*depth ; n++)
	{
		gcas[n].means = (float *)calloc(gca->ninputs, sizeof(float)) ;
		gcas[n].covars = (float *)calloc((gca->ninputs*(gca->ninputs+1))/2, sizeof(float)) ;
		if (!gcas[n].means || !gcas[n].covars)
			ErrorExit(ERROR_NOMEMORY, "GCAfindStableSamples: could not allocate mean (%d) and covariance (%d) matrices",
								gca->ninputs, gca->ninputs*(gca->ninputs+1)/2) ;
	}
#endif

#if 0
  mri_filled = MRIalloc(width*gca->prior_spacing+1,height*gca->prior_spacing+1, 
                        depth*gca->prior_spacing+1,MRI_UCHAR);
#else
  mri_filled = MRIalloc(width*gca->prior_spacing,height*gca->prior_spacing, 
                        depth*gca->prior_spacing,MRI_UCHAR);
#endif

  prior_stride = (float)min_spacing / (float)gca->prior_spacing ;
	if (prior_stride >= 2.5)
		prior_factor = .5 ;
	else if (prior_stride >= 1.5)
		prior_factor = .75 ;
	else 
		prior_factor = .9 ;

	for (r = 0 ; r < gca->ninputs ; r++)
		total_means[r] = 0.0 ;

  /* compute the max priors and min variances for each class */
  for (nzeros = nfound = x = 0 ; x < width ; x += prior_stride)
  {
    xi = nint(x) ;
    for (y = 0 ; y < height ; y += prior_stride)
    {
      yi = nint(y) ;
      for (z = 0 ; z < depth ; z += prior_stride)
      {
        zi = nint(z) ;
        if (xi == Gx && yi == Gy && zi == Gz)
          DiagBreak() ;
        if (abs(x-31)<=prior_stride &&
            abs(y-22)<=prior_stride &&
            abs(z-36)<=prior_stride)
          DiagBreak() ;
        gcaRegionStats(gca, x, y, z, prior_stride/2, priors, vars, means) ;

        if (priors[4] > .5*min_prior ||  /* left lat ven */
            priors[5] > .5*min_prior ||  /* inf left lat ven */
            priors[14] > .5*min_prior ||  /* 3rd ven */
            priors[15] > .5*min_prior ||  /* 4th ven */
            priors[43] > .5*min_prior ||
            priors[44] > .5*min_prior)
          DiagBreak() ;
            
        best_mean_dist = 0.0 ;
        max_prior = -1 ;

        if ((different_nbr_max_labels(gca, x, y, z, 1, 0) > 0) &&
						(exclude_list && exclude_list[0] == 0) &&
            (priors[0] >= min_prior) &&
            (priors[0] >= .9*max_priors[0]))
          best_label = 0 ;
        else 
          best_label = -1 ;

        for (label = 1 ; label < MAX_DIFFERENT_LABELS ; label++)
        {
					/* ignore it if:
						 1. It's prior is less than min_prior and 
						     it's prior is less than the max for this class.
						 2. It's prior is less than 1/2 of min prior regardless.
					*/
          if (((priors[label] < min_prior) && (priors[label] < prior_factor*max_priors[label])) || 
							(priors[label] < .5*min_prior) ||
							(exclude_list && exclude_list[label] > 0))
            continue ;

          if ((best_label == 0) ||
              (priors[label] > max_prior) ||
              (FEQUAL(priors[label], max_prior) && 
               label_counts[best_label] > label_counts[label]))
          {
            best_label = label ;
            max_prior = priors[label] ;
          }
        }
#if 1
        if (nfound > 0)
        {
          double p = randomNumber(0, 1.0) ;
          if (p < ((double)label_counts[best_label] / nfound))
            continue ;
        }
#endif
        if (best_label >= 0)
        {
          if (best_label == 0)
          {
#if 1
            if (gcaFindBestSample(gca, x, y, z, best_label, prior_stride/2, 
                                  &gcas[nfound]) == NO_ERROR)
#else
            gcaFindClosestMeanSample(gca, 255, min_prior, x, y, z, 0,
                                     prior_stride/2, &gcas[nfound]);
            if (means[0] > 100)
#endif
            {
              int  xv, yv, zv ;
              
              if (((means[0] <= min_unknown) ||
                   (means[0] >= max_unknown))) /* disabled check */
              {
                GCApriorToVoxel(gca, mri_filled, x, y, z, &xv, &yv, &zv) ;
                if (MRIvox(mri_filled, xv, yv, zv) == 0)
                {
                  mriFillRegion(mri_filled, xv, yv, zv, 1, MIN_UNKNOWN_DIST) ;
                  /*                MRIvox(mri_filled, xv, yv, zv) = 1 ;*/
                  nzeros++ ;
                  label_counts[best_label]++ ;
                  nfound++ ;
                }
              }
              else
                DiagBreak() ;
            }
          }
          else
          {
            /* find best gc with this label */
            if (gcaFindBestSample(gca, x, y, z, best_label, prior_stride/2, 
                                  &gcas[nfound]) == NO_ERROR)
            {
							for (r = 0 ; r < gca->ninputs ; r++)
								total_means[r] += means[best_label][r] ;
              label_counts[best_label]++ ;
              nfound++ ;
            }
          }
        }
      }
    }
  }


  fprintf(stderr, "total sample mean = %2.1f (%d zeros)\n", 
          total_means[0]/((float)nfound-nzeros), nzeros) ;

  if (getenv("GCA_WRITE_CLASS"))
  {
    int  n ;
    FILE *fp ;

    fp = fopen("classes.dat", "w") ;
    if (fp)
    {
      for (n = 0 ; n < MAX_DIFFERENT_LABELS ; n++)
        if (label_counts[n] > 0)
          fprintf(fp, "%d  %d\n", n, label_counts[n]) ;
      fclose(fp) ;
    }
  }

  *pnsamples = nfound ;

  MRIfree(&mri_filled) ;
  return(gcas) ;
}
#else
GCA_SAMPLE *
GCAfindStableSamples(GCA *gca, int *pnsamples, int min_spacing,float min_prior)
{
  GCA_SAMPLE *gcas ;
  GCA_PRIOR  *gcap ;
  GC1D       *gc, *best_gc ;
  int        x, y, z, width, height, depth, n, label, nfound, prior_stride,
             label_counts[MAX_DIFFERENT_LABELS], best_n, best_label,
             xk, yk, zk, xi, yi, zi, best_x, best_y, best_z, nzeros ;
  float      max_prior, total_mean, mean_dist, best_mean_dist, prior ;

  memset(label_counts, 0, sizeof(label_counts)) ;
  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  gcas = calloc(width*height*depth, sizeof(GCA_SAMPLE)) ;
  
  prior_stride = min_spacing / gca->prior_spacing ;

  total_mean = 0.0 ;

  /* compute the max priors and min variances for each class */
  for (nzeros = nfound = x = 0 ; x < width ; x += prior_stride)
  {
    for (y = 0 ; y < height ; y += prior_stride)
    {
      for (z = 0 ; z < depth ; z += prior_stride)
      {
        if (abs(x-31)<=prior_stride &&
            abs(y-22)<=prior_stride &&
            abs(z-36)<=prior_stride)
          DiagBreak() ;
        best_n = best_label = -1 ; best_gc = NULL ; best_mean_dist = 0.0 ;
        max_prior = -1 ; best_x = best_y = best_z = -1 ;
        for (xk = 0 ; xk < prior_stride ; xk++)
        {
          xi = MIN(x+xk, width-1) ;
          for (yk = 0 ; yk < prior_stride ; yk++)
          {
            yi = MIN(y+yk, height-1) ;
            for (zk = 0 ; zk < prior_stride ; zk++)
            {
              zi = MIN(z+zk, depth-1) ;
              if (xi == 31 && yi == 22 && zi == 36)
                DiagBreak() ;
          
              gcap = &gca->prior[xi][yi][zi] ;
              for (n = 0 ; n < gcap->nlabels ; n++)
              {
                label = gcap->labels[n] ;
                gc = GCAfindPriorGC(gca, xi, yi, zi, label) ;
                if (label == Gdiag_no)
                  DiagBreak() ;
                if ((label <= 0 && best_label >= 0) || gcap->priors[n] < min_prior)
                  continue ;

                if (label == 0 && 
                    !different_nbr_max_labels(gca, xi, yi, zi, 1, 0))
                  continue ;
                
                if (nfound > nzeros)
                {
                  mean_dist = (gc->mean-total_mean/((float)nfound-nzeros)) ;
                  mean_dist = sqrt(mean_dist * mean_dist) ;
                  if (best_label >= 0 && 2*mean_dist < best_mean_dist)
                    continue ;
                }
                else
                  mean_dist = 0 ;

                prior = gcap->priors[n] ;
                if ((best_label == 0) ||
                    (prior > max_prior) ||
                    (FEQUAL(prior, max_prior) && 
                     label_counts[best_label] > label_counts[label]) ||
                    (mean_dist > 2*best_mean_dist)
#if 0
                    || label_counts[best_label] > 5*label_counts[label]
#endif
                    )
                {
                  if (label == Gdiag_no)
                    DiagBreak() ;
                  best_label = label ; best_gc = gc ;
                  max_prior = prior ; best_n = n ;
                  if (nfound > nzeros)
                  {
                    best_mean_dist = 
                      (gc->mean-total_mean/(float)(nfound-nzeros)) ;
                    best_mean_dist = sqrt(best_mean_dist * best_mean_dist) ;
                  }
                  best_x = xi ; best_y = yi ; best_z = zi ;
                }
              }
            }
          }
        }

        if (best_label == 0 && 
            !different_nbr_max_labels(gca, best_x, best_y,best_z, 1, 0))
          continue ;
                
        if (x == 4 && y == 3 && z == 6)
          DiagBreak() ;
        if (best_n >= 0)
        {
          gcas[nfound].xp = best_x ; gcas[nfound].yp = best_y ;
          gcas[nfound].zp = best_z ;
          gcas[nfound].label = best_label ;
          gcas[nfound].var = best_gc->var ;
          gcas[nfound].mean = best_gc->mean ;
          gcas[nfound].prior = gcap->priors[best_n] ;
          if (best_label > 0)
            total_mean += best_gc->mean ;
          else
            nzeros++ ;
          label_counts[best_label]++ ;
          nfound++ ;
        }
      }
    }
  }

  fprintf(stderr, "total sample mean = %2.1f (%d zeros)\n", 
          total_mean/((float)nfound-nzeros), nzeros) ;

  {
    int  n ;
    FILE *fp ;

    fp = fopen("classes.dat", "w") ;
    for (n = 0 ; n < MAX_DIFFERENT_LABELS ; n++)
      if (label_counts[n] > 0)
        fprintf(fp, "%d  %d\n", n, label_counts[n]) ;
    fclose(fp) ;
  }

  *pnsamples = nfound ;

  return(gcas) ;
}
#endif
int
GCAwriteSamples(GCA *gca, MRI *mri, GCA_SAMPLE *gcas, int nsamples,
                char *fname)
{
  int    n, xv, yv, zv ;
  MRI    *mri_dst ;

  mri_dst = MRIalloc(mri->width, mri->height, mri->depth, MRI_UCHAR) ;

  for (n = 0 ; n < nsamples ; n++)
  {
    if (gcas[n].label == Gdiag_no)
      DiagBreak() ;
    GCApriorToVoxel(gca, mri_dst, gcas[n].xp, gcas[n].yp, gcas[n].zp, 
                   &xv, &yv, &zv) ;
    if (gcas[n].label > 0)
      MRIvox(mri_dst, xv, yv, zv) = gcas[n].label ;
    else
      MRIvox(mri_dst, xv, yv, zv) = 29 ;  /* Left undetermined - visible */
    if (DIAG_VERBOSE_ON && Gdiag & DIAG_SHOW)
      printf("label %d: (%d, %d, %d) <-- (%d, %d, %d)\n",
             gcas[n].label,gcas[n].xp,gcas[n].yp,gcas[n].zp, xv, yv, zv) ;
  }
  MRIwrite(mri_dst, fname) ;
  MRIfree(&mri_dst) ;
  return(NO_ERROR) ;
}
MRI *
GCAmri(GCA *gca, MRI *mri)
{
  int       frame, width, height, depth, x, y, z, xp, yp, zp, n, xn, yn, zn ;
  float     val ;
  GC1D      *gc ;
  GCA_PRIOR *gcap ;

  if (!mri)
    mri = MRIallocSequence(gca->node_width, gca->node_height, gca->node_depth, MRI_UCHAR, gca->ninputs) ;
  width = mri->width ; height = mri->height ; depth = mri->depth ;

	for (frame = 0 ; frame < gca->ninputs ; frame++)
	{
		for (x = 0 ; x < width ; x++)
		{
			for (y = 0 ; y < height ; y++)
			{
				for (z = 0 ; z < depth ; z++)
				{
					GCAvoxelToPrior(gca, mri, x, y, z, &xp, &yp, &zp) ;
					GCAvoxelToNode(gca, mri, x, y, z, &xn, &yn, &zn) ;
					gcap = &gca->priors[xp][yp][zp] ;
					for (val = 0.0, n = 0 ; n < gcap->nlabels ; n++)
					{
						gc = GCAfindGC(gca, xn, yn, zn, gcap->labels[n]) ;
						if (gc)
							val += gc->means[frame] * gcap->priors[n] ;
					}
					switch (mri->type)
					{
					default:
						ErrorReturn(NULL,
												(ERROR_UNSUPPORTED, 
												 "GCAmri: unsupported image type %d", mri->type)) ;
					case MRI_UCHAR:
						MRIseq_vox(mri, x, y, z, frame) = (unsigned char)val ;
						break ;
					case MRI_SHORT:
						MRISseq_vox(mri, x, y, z, frame) = (short)val ;
						break ;
					}
				}
			}
		}
	}
  return(mri) ;
}
MRI *
GCAlabelMri(GCA *gca, MRI *mri, int label, TRANSFORM *transform)
{
  int      width, height, depth, x, y, z, xn, yn, zn, n, frame ;
  float    val ;
  GC1D     *gc = NULL ;
  GCA_NODE *gcan ;
  MRI      *mri_norm ;

  if (!mri)
    mri = MRIallocSequence(gca->node_width, gca->node_height, gca->node_depth, MRI_UCHAR, gca->ninputs) ;
  width = mri->width ; height = mri->height ; depth = mri->depth ;
  mri_norm = MRIalloc(width, height, depth, MRI_SHORT) ;

	for (frame = 0 ; frame < gca->ninputs ; frame++)
	{
		MRIclear(mri_norm) ;
		for (x = 0 ; x < width ; x++)
		{
			for (y = 0 ; y < height ; y++)
			{
				for (z = 0 ; z < depth ; z++)
				{
					if (x == width/2 && y == height/2 && z == depth/2)
						DiagBreak() ;
					if (x == 19 && y == 14 && z == 15)
						DiagBreak() ;
					
					GCAsourceVoxelToNode(gca, mri, transform, x, y, z, &xn, &yn, &zn) ;
					gcan = &gca->nodes[xn][yn][zn] ;
					if (gcan->nlabels < 1)
						continue ;
					for (n = 0 ; n < gcan->nlabels ; n++)
					{
						gc = &gcan->gcs[n] ;
						if (gcan->labels[n] == label)
							break ;
					}
					if (n >= gcan->nlabels)
						continue ;
					val = gc->means[frame] ;
					switch (mri->type)
					{
					default:
						ErrorReturn(NULL,
												(ERROR_UNSUPPORTED, 
												 "GCAlabelMri: unsupported image type %d", mri->type)) ;
					case MRI_UCHAR:
						MRIseq_vox(mri, x, y, z,frame) = (unsigned char)val ;
						break ;
					case MRI_SHORT:
						MRISseq_vox(mri, x, y, z,frame) = (short)val ;
						break ;
					case MRI_FLOAT:
						MRIFseq_vox(mri, x, y, z, frame) = (float)val ;
						break ;
					}
					MRISvox(mri_norm, x, y, z) += 1 ;
				}
			}
		}
		for (x = 0 ; x < width ; x++)
		{
			for (y = 0 ; y < height ; y++)
			{
				for (z = 0 ; z < depth ; z++)
				{
					if (MRISvox(mri_norm, x, y, z) > 0)
						MRISseq_vox(mri, x, y, z, frame) = 
							nint((float)MRISseq_vox(mri,x,y,z,frame)/(float)MRISvox(mri_norm,x,y,z)) ;
				}
			}
		}
	}
  MRIfree(&mri_norm) ;
  return(mri) ;
}

static int
different_nbr_max_labels(GCA *gca, int x, int y, int z, int wsize, int label)
{
  int      xk, yk, zk, xi, yi, zi, nbrs, n, width, height, depth, max_label ;
  GCA_PRIOR *gcap ;
	double     max_p ;

  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  for (nbrs = 0, xk = -wsize ; xk <= wsize ; xk++)
  {
    xi = x+xk ;
    if (xi < 0 || xi >= width)
      continue ;

    for (yk = -wsize ; yk <= wsize ; yk++)
    {
      yi = y+yk ;
      if (yi < 0 || yi >= height)
        continue ;

      for (zk = -wsize ; zk <= wsize ; zk++)
      {
        zi = z+zk ;
        if (zi < 0 || zi >= depth)
          continue ;
        
        gcap = &gca->priors[xi][yi][zi] ;

				max_p = gcap->priors[0] ; max_label = gcap->labels[0] ;
        for (n = 1 ; n < gcap->nlabels ; n++)
          if (gcap->priors[n] >= max_p)
					{
						max_label = gcap->labels[n] ; max_p = gcap->priors[n] ;
					}
				if (max_label != label)
					nbrs++ ;
      }
    }
  }
  return(nbrs) ;
}
#if 0
static int 
different_nbr_labels(GCA *gca, int x, int y, int z, int wsize, int label, float pthresh)
{
  int      xk, yk, zk, xi, yi, zi, nbrs, n, width, height, depth ;
  GCA_PRIOR *gcap ;

  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  for (nbrs = 0, xk = -wsize ; xk <= wsize ; xk++)
  {
    xi = x+xk ;
    if (xi < 0 || xi >= width)
      continue ;

    for (yk = -wsize ; yk <= wsize ; yk++)
    {
      yi = y+yk ;
      if (yi < 0 || yi >= height)
        continue ;

      for (zk = -wsize ; zk <= wsize ; zk++)
      {
        zi = z+zk ;
        if (zi < 0 || zi >= depth)
          continue ;
        
        gcap = &gca->priors[xi][yi][zi] ;

        for (n = 0 ; n < gcap->nlabels ; n++)
          if ((gcap->labels[n] != label) && (gcap->priors[n] >= pthresh))
            nbrs++ ;
      }
    }
  }
  return(nbrs) ;
}
#endif
static int
gcaRegionStats(GCA *gca, int x, int y, int z, int wsize,
               float *priors, float *vars, float means[MAX_DIFFERENT_LABELS][MAX_GCA_INPUTS])
{
  int        xk, yk, zk, xi, yi, zi, width, height, depth, label, n,
             total_training, r, l ;
  GCA_PRIOR  *gcap  ;
  GC1D       *gc ;
  float      dof ;

  if (x == 28 && y == 24 && z == 36)
    DiagBreak() ;

  memset(priors, 0, MAX_DIFFERENT_LABELS*sizeof(float)) ;
  memset(vars, 0, MAX_DIFFERENT_LABELS*sizeof(float)) ;
	for (l = 0 ; l < MAX_DIFFERENT_LABELS ; l++)
		for (r = 0 ; r < gca->ninputs ; r++)
			means[l][r] = 0 ;

  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  total_training = 0 ;
  for (xk = -wsize ; xk <= wsize ; xk++)
  {
    xi = x+xk ;
    if (xi < 0 || xi >= width)
      continue ;

    for (yk = -wsize ; yk <= wsize ; yk++)
    {
      yi = y+yk ;
      if (yi < 0 || yi >= height)
        continue ;

      for (zk = -wsize ; zk <= wsize ; zk++)
      {
        zi = z+zk ;
        if (zi < 0 || zi >= depth)
          continue ;
        
        gcap = &gca->priors[xi][yi][zi] ;
        total_training += gcap->total_training ;

        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          label = gcap->labels[n] ;
          if (label == Gdiag_no)
            DiagBreak() ;
          gc = GCAfindPriorGC(gca, xi, yi, zi, label) ;
          dof = (gcap->priors[n] * (float)gcap->total_training) ;
          
          vars[label] +=  dof * covariance_determinant(gc, gca->ninputs) ;
					for (r = 0 ; r < gca->ninputs ; r++)
						means[label][r] += dof * gc->means[r] ;
          priors[label] += dof ;
        }
      }
    }
  }

  for (label = 0 ; label < MAX_DIFFERENT_LABELS ; label++)
  {
    dof = priors[label] ;
    if (dof > 0)
    {
      vars[label] /= dof ;
      priors[label] /= total_training ;
			for (r = 0 ; r < gca->ninputs ; r++)
				means[label][r] /= dof ;
    }
  }
  return(NO_ERROR) ;
}
static int
gcaFindBestSample(GCA *gca, int x, int y, int z,int label,int wsize,
                  GCA_SAMPLE *gcas)
{
  int        xk, yk, zk, xi, yi, zi, width, height, depth, n,
             best_n, best_x, best_y, best_z, xn, yn, zn, r, c, v ;
  GCA_PRIOR  *gcap ;
  GC1D       *gc, *best_gc ;
  float      max_prior, min_var, prior ;

  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  max_prior = 0.0 ; min_var = 10000.0f ; best_gc = NULL ;
  best_x = best_y = best_z = -1 ; best_n = -1 ;
  for (xk = -wsize ; xk <= wsize ; xk++)
  {
    xi = x+xk ;
    if (xi < 0 || xi >= width)
      continue ;

    for (yk = -wsize ; yk <= wsize ; yk++)
    {
      yi = y+yk ;
      if (yi < 0 || yi >= height)
        continue ;

      for (zk = -wsize ; zk <= wsize ; zk++)
      {
        zi = z+zk ;
        if (zi < 0 || zi >= depth)
          continue ;

        gcap = &gca->priors[xi][yi][zi] ;

        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          if (gcap->labels[n] != label)
            continue ;
          DiagBreak() ;
          prior = gcap->priors[n] ;
          GCApriorToNode(gca, xi, yi, zi, &xn, &yn, &zn) ;
          gc = GCAfindGC(gca, xn, yn, zn, label) ;
          if (!gc)
            continue ;
          
          if (prior > max_prior
#if 0
							||
              (FEQUAL(prior, max_prior) && (gc->var < min_var)) ||
              (label == 0 && gc->mean > best_gc->mean)
#endif
							)
          {
            max_prior = prior ; /*min_var = gc->var ;*/
            best_gc = gc ; best_x = xi ; best_y = yi ; best_z = zi ; 
            best_n = n ;
          }
        }
      }
    }
  }

  if (best_x < 0)
  {
    ErrorPrintf(ERROR_BADPARM, 
                "could not find GC1D for label %d at (%d,%d,%d)\n", 
                label, x, y, z) ;
    return(ERROR_BADPARM) ;
  }
  if (best_x == 145/4 && best_y == 89/4 && best_z == 125/4)
    DiagBreak() ;
  gcas->xp = best_x ; gcas->yp = best_y ; gcas->zp = best_z ;
  gcas->label = label ; 
	gcas->means = (float *)calloc(gca->ninputs, sizeof(float)) ;
	gcas->covars = (float *)calloc((gca->ninputs*(gca->ninputs+1))/2, sizeof(float)) ;
	if (!gcas->means || !gcas->covars)
		ErrorExit(ERROR_NOMEMORY, "GCAfindStableSamples: could not allocate mean (%d) and covariance (%d) matrices",
							gca->ninputs, gca->ninputs*(gca->ninputs+1)/2) ;
	for (r = v = 0 ; r < gca->ninputs ; r++)
	{
		gcas->means[r] = best_gc->means[r] ;
		for (c = r ; c < gca->ninputs ; c++, v++)
			gcas->covars[v] = best_gc->covars[v] ; 
	}
	gcas->prior = max_prior ;

  return(NO_ERROR) ;
}
#if 0
static GC1D *
gcaFindHighestPriorGC(GCA *gca, int x, int y, int z,int label,int wsize)
{
  int        xk, yk, zk, xi, yi, zi, width, height, depth, n ;
  GCA_PRIOR  *gcap  ;
  GC1D       *best_gc ;
  float      max_prior, prior ;

  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;
  max_prior = 0.0 ; best_gc = NULL ;
  for (xk = -wsize ; xk <= wsize ; xk++)
  {
    xi = x+xk ;
    if (xi < 0 || xi >= width)
      continue ;

    for (yk = -wsize ; yk <= wsize ; yk++)
    {
      yi = y+yk ;
      if (yi < 0 || yi >= height)
        continue ;

      for (zk = -wsize ; zk <= wsize ; zk++)
      {
        zi = z+zk ;
        if (zi < 0 || zi >= depth)
          continue ;
        
        gcap = &gca->priors[xi][yi][zi] ;

        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          if (gcap->labels[n] != label)
            continue ;
          prior = gcap->priors[n] ;
          if (prior > max_prior)
          {
            max_prior = prior ; 
            best_gc = GCAfindPriorGC(gca, x, y, z, label) ; 
          }
        }
      }
    }
  }

  if (best_gc == NULL)
  {
    ErrorPrintf(ERROR_BADPARM, 
                "could not find GC for label %d at (%d,%d,%d)\n", 
                label, x, y, z) ;
    return(NULL) ;
  }

  return(best_gc) ;
}
static int
gcaFindClosestMeanSample(GCA *gca, float *means, float min_prior, int x, int y, 
                         int z, int label, int wsize, GCA_SAMPLE *gcas)
{
  int        xk, yk, zk, xi, yi, zi, width, height, depth, n,
             best_x, best_y, best_z, r, c, v ;
  GCA_NODE   *gcan  ;
  GC1D       *gc, *best_gc ;
  float      min_dist, best_prior, prior, dist ;

  width = gca->node_width ; height = gca->node_height ; depth = gca->node_depth ;
  min_dist = 1000000.0f ; best_gc = NULL ; best_prior = 0.0 ;
  best_x = best_y = best_z = -1 ;
  for (xk = -wsize ; xk <= wsize ; xk++)
  {
    xi = x+xk ;
    if (xi < 0 || xi >= width)
      continue ;

    for (yk = -wsize ; yk <= wsize ; yk++)
    {
      yi = y+yk ;
      if (yi < 0 || yi >= height)
        continue ;

      for (zk = -wsize ; zk <= wsize ; zk++)
      {
        zi = z+zk ;
        if (zi < 0 || zi >= depth)
          continue ;
        
        gcan = &gca->nodes[xi][yi][zi] ;

        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
          prior = get_node_prior(gca, gcan->labels[n], xi, yi, zi) ;
          if (gcan->labels[n] != label || prior < min_prior)
            continue ;
					for (dist = 0, r = 0 ; r < gca->ninputs ; r++)
						dist += SQR(gc->means[r] - means[r]) ;
          if (dist < min_dist)
          {
            min_dist = dist ;
            best_gc = gc ; best_x = xi ; best_y = yi ; best_z = zi ; 
            best_prior = prior ;
          }
        }
      }
    }
  }

  if (best_x < 0)
  {
    ErrorPrintf(ERROR_BADPARM, 
                "could not find GC for label %d at (%d,%d,%d)\n", 
                label, x, y, z) ;
    return(ERROR_BADPARM) ;
  }
  if (best_x == 141/4 && best_y == 37*4 && best_z == 129*4)
    DiagBreak() ;
  gcas->xp = best_x ; gcas->yp = best_y ; gcas->zp = best_z ;
  gcas->label = label ; 
	for (v = r = 0 ; r < gca->ninputs ; r++)
	{
		gcas->means[r] = best_gc->means[r] ;
		for (c = r ; c < gca->ninputs ; c++, v++)
			gcas->covars[v] = best_gc->covars[v] ;
	}

	gcas->prior = best_prior ;
  return(NO_ERROR) ;
}
#endif
int
GCAtransformAndWriteSamples(GCA *gca, MRI *mri, GCA_SAMPLE *gcas, 
                            int nsamples,char *fname,TRANSFORM *transform)
{
  int    n, xv, yv, zv, label ;
  MRI    *mri_dst ;

  mri_dst = MRIalloc(mri->width, mri->height, mri->depth, MRI_UCHAR) ;
  
  TransformInvert(transform, mri) ;
  for (n = 0 ; n < nsamples ; n++)
  {
    if (gcas[n].label == Gdiag_no)
      DiagBreak() ;
    GCApriorToSourceVoxel(gca, mri_dst, transform,
                          gcas[n].xp, gcas[n].yp, gcas[n].zp, 
                          &xv, &yv, &zv) ;
    if (gcas[n].label > 0)
      label = gcas[n].label ;
    else if (gcas[n].label == 0)
      label = 29 ;  /* Left undetermined - visible */
    else
      label = 0 ;  /* Left undetermined - visible */
    mriFillRegion(mri_dst, xv, yv, zv, label, 0) ;
    if (gcas[n].x == Gx && gcas[n].y == Gy && gcas[n].z == Gz)
      DiagBreak() ;
    gcas[n].x = xv ;
    gcas[n].y = yv ;
    gcas[n].z = zv ;
    if (gcas[n].x == Gx && gcas[n].y == Gy && gcas[n].z == Gz)
      DiagBreak() ;
    if (DIAG_VERBOSE_ON && Gdiag & DIAG_SHOW)
      printf("label %d: (%d, %d, %d) <-- (%d, %d, %d)\n",
             gcas[n].label,gcas[n].xp,gcas[n].yp,gcas[n].zp, xv, yv, zv) ;
  }
  fprintf(stderr, "writing samples to %s...\n", fname) ;
  MRIwrite(mri_dst, fname) ;
  MRIfree(&mri_dst) ;

  return(NO_ERROR) ;
}

static int
mriFillRegion(MRI *mri, int x, int y, int z, int fill_val, int whalf)
{
  int   xi, xk, yi, yk, zi, zk ;

  for (xk = -whalf ; xk <= whalf ; xk++)
  {
    xi = mri->xi[x+xk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri->yi[y+yk] ;
      for (zk = -whalf ; zk <= whalf ; zk++)
      {
        zi = mri->zi[z+zk] ;
        MRIvox(mri, xi, yi, zi) = fill_val ;
      }
    }
  }
  return(NO_ERROR) ;
}

static int
gcaFindIntensityBounds(GCA *gca, float *pmin, float *pmax)
{
  int      width, depth, height, x, y, z, n, label, r ;
  GCA_NODE *gcan ;
  GC1D     *gc ;
  float    mn[MAX_GCA_INPUTS], mx[MAX_GCA_INPUTS], offset ;

  for (r = 0 ; r < gca->ninputs ; r++)
    {
      mn[r] = 100000.0f ; mx[r] = -mn[r] ;
    }
  width = gca->node_width ; height = gca->node_height ; depth = gca->node_depth ;

  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
          label = gcan->labels[n] ;
          if (label == 0)  /* don't include unknowns */
            continue ;
          if (get_node_prior(gca, label, x, y, z) < 0.1)
            continue ;  /* exclude unlikely stuff (errors in labeling) */
          offset = 0.5*pow(covariance_determinant(gc, gca->ninputs),1.0/(double)gca->ninputs) ;
					for (r = 0 ; r < gca->ninputs ; r++)
					{
						if (gc->means[r] + offset > mx[r])
						{
							mx[r] = gc->means[r] + offset ;
						}
						if (gc->means[r]  < mn[r])
						{
							mn[r] = gc->means[r] ;
						}
#if 0						
						if (mn[r] < 5)
							mn = 5 ;
						if (mx[r] > 225)
							mx[r] = 225 ;
#endif
					}
        }
      }
    }
  }
  for (r = 0 ; r < gca->ninputs ; r++)
	{
		pmin[r] = mn[r] ; pmax[r] = mx[r] ;
	}
  return(NO_ERROR) ;
}
static int
gcaFindMaxPriors(GCA *gca, float *max_priors)
{
  int       width, depth, height, x, y, z, n, label ;
  GCA_PRIOR *gcap ;

  memset(max_priors, 0, MAX_DIFFERENT_LABELS*sizeof(float)) ;
  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;

  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        gcap = &gca->priors[x][y][z] ;
        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          label = gcap->labels[n] ;
          if (gcap->priors[n] > max_priors[label])
					{
						if (label == Ggca_label)
							DiagBreak() ;
						if ((gcap->priors[n] > 0.89) && (label == Ggca_label))
							DiagBreak() ;
            max_priors[label] = gcap->priors[n] ;
					}
        }
      }
    }
  }
  return(NO_ERROR) ;
}

static int xn_bad = 15 ;
static int yn_bad = 24 ;
static int zn_bad = 27 ;
static int xl_bad = 56 ;
static int yl_bad = 99 ;
static int zl_bad = 106 ;
static int label_bad = 42 ;
static int nbr_label_bad = 42 ;
static int bad_i = 0 ;

static int
GCAupdateNodeGibbsPriors(GCA *gca, MRI*mri, int xn, int yn, int zn,
                   int xl, int yl, int zl, int label)
{
  int       n, i, xnbr, ynbr, znbr, nbr_label ;
  GCA_NODE *gcan ;
  GC1D     *gc ;


  if (xl == xl_bad && yl == yl_bad && zl == zl_bad)
    DiagBreak() ;
  if ((xn == 16 && yn == 26 && zn == 27 && label == 0))
    DiagBreak() ;
  if (((xn == xn_bad && yn == yn_bad && zn == zn_bad && label == label_bad)))
    DiagBreak() ;

  gcan = &gca->nodes[xn][yn][zn] ;

  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == label)
      break ;
  }
  if (n >= gcan->nlabels)  /* have to allocate a new classifier */
    ErrorExit(ERROR_BADPARM, "gca(%d, %d, %d): could not find label %d",
              xn, yn, zn, label) ;

  gc = &gcan->gcs[n] ;
  for (i = 0 ; i < GIBBS_NEIGHBORHOOD ; i++)
  {
    /* coordinates of neighboring point */
    xnbr = mri->xi[xl+xnbr_offset[i]] ;
    ynbr = mri->yi[yl+ynbr_offset[i]] ;
    znbr = mri->zi[zl+znbr_offset[i]] ;

    nbr_label = MRIvox(mri, xnbr, ynbr, znbr) ;
    if (xn == xn_bad && yn == yn_bad && zn == zn_bad && label == label_bad &&
        nbr_label == nbr_label_bad)
      DiagBreak() ;
    if (xn == xn_bad && yn == yn_bad && zn == zn_bad && label == label_bad &&
        nbr_label == nbr_label_bad && i == bad_i)
      DiagBreak() ;

    /* now see if this label exists already as a nbr */
    for (n = 0 ; n < gc->nlabels[i] ; n++)
      if (gc->labels[i][n] == nbr_label)
        break ;

    if (xn == 16 && yn == 26 && zn == 27 && n > 0)
      DiagBreak() ;
    if (((xn == 16 && yn == 25 && zn == 27 && label == 42)) && (n > 0))
      DiagBreak() ;

    if (n >= gc->nlabels[i])   /* not there - reallocate stuff */
    {
#if 1
      char *old_labels ;
      float *old_label_priors ;

      if (xl == 54 && yl == 105 && zl == 112)
        DiagBreak() ;

      old_labels = gc->labels[i] ;
      old_label_priors = gc->label_priors[i] ;

      /* allocate new ones */
      gc->label_priors[i] = (float *)calloc(gc->nlabels[i]+1, sizeof(float)) ;
      if (!gc->label_priors[i])
        ErrorExit(ERROR_NOMEMORY, "GCAupdateNodeGibbsPriors: "
                  "couldn't expand gcs to %d" ,gc->nlabels[i]+1) ;
      gc->labels[i] = (unsigned char *)calloc(gc->nlabels[i]+1, sizeof(unsigned char)) ;
      if (!gc->labels[i])
        ErrorExit(ERROR_NOMEMORY, 
                  "GCANupdateNode: couldn't expand labels to %d",
                  gc->nlabels[i]+1) ;

      if (gc->nlabels[i] > 0)  /* copy the old ones over */
      {
        memmove(gc->label_priors[i], old_label_priors, 
                gc->nlabels[i]*sizeof(float)) ;
        memmove(gc->labels[i], old_labels, gc->nlabels[i]*sizeof(unsigned char)) ;

        /* free the old ones */
        free(old_label_priors) ; free(old_labels) ;
      }
      gc->labels[i][gc->nlabels[i]++] = nbr_label ;
#else
      if (n >= MAX_NBR_LABELS)   /* not there - reallocate stuff */
      {
        ErrorPrintf(ERROR_NOMEMORY, 
                    "GCAupdateNodeGibbsPriors: gca(%d,%d,%d) has more than %d "
                    "different neighbors", xn, yn, zn, MAX_NBR_LABELS);
        continue ;
      }
      else
        gc->labels[i][gc->nlabels[i]++] = nbr_label ;
#endif
    }
    gc->label_priors[i][n] += 1.0f ;
  }

  return(NO_ERROR) ;
}

#if 0
MRI  *
GCAreclassifyUsingGibbsPriors(MRI *mri_inputs, GCA *gca, MRI *mri_dst,TRANSFORM *transform,
                            int max_iter)
{
  int      x, y, z, width, height, depth, label, val, iter,
           xn, yn, zn, n, i, j, nchanged, xnbr, ynbr, znbr, nbr_label,
           index, nindices ;
  short    *x_indices, *y_indices, *z_indices ;
  GCA_NODE *gcan ;
  GC1D     *gc ;
  double   dist, max_p, p, prior, ll, lcma = 0.0, new_ll, old_ll ;
  MRI      *mri_changed, *mri_probs ;

  nindices = mri_dst->width * mri_dst->height * mri_dst->depth ;
  x_indices = (short *)calloc(nindices, sizeof(short)) ;
  y_indices = (short *)calloc(nindices, sizeof(short)) ;
  z_indices = (short *)calloc(nindices, sizeof(short)) ;
  if (!x_indices || !y_indices || !z_indices)
    ErrorExit(ERROR_NOMEMORY, "GCAreclassifyUsingGibbsPriors: "
              "could not allocate index set") ;


  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ; iter = 0 ;
  if (!mri_dst)
  {
    mri_dst = MRIalloc(width, height, depth, MRI_UCHAR) ;
    if (!mri_dst)
      ErrorExit(ERROR_NOMEMORY, "GCAlabel: could not allocate dst") ;
    MRIcopyHeader(mri_inputs, mri_dst) ;
  }

  mri_changed = MRIclone(mri_dst, NULL) ;


  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  for (x = 0 ; x < width ; x++)
    for (y = 0 ; y < height ; y++)
        for (z = 0 ; z < depth ; z++)
          MRIvox(mri_changed,x,y,z) = 1 ;

#if 0
  {
    MRI   *mri_cma ;
    char  fname[STRLEN], *cp ;

    strcpy(fname, mri_inputs->fname) ;
    cp = strrchr(fname, '/') ;
    if (cp)
    {
      strcpy(cp+1, "parc") ;
      mri_cma = MRIread(fname) ;
      if (mri_cma)
      {

        ll = gcaGibbsImageLogLikelihood(gca, mri_dst, mri_inputs, lta) ;
        lcma = gcaGibbsImageLogLikelihood(gca, mri_cma, mri_inputs, lta) ;
        lcma /= (double)(width*depth*height) ;
        ll /= (double)(width*depth*height) ;
        fprintf(stderr, "image ll: %2.3f (CMA=%2.3f)\n", ll, lcma) ;
        MRIfree(&mri_cma) ;
      }
    }
  }
#endif

  do
  {
    if (iter == 0)
    {
      mri_probs = GCAlabelProbabilities(mri_inputs, gca, NULL, lta) ;
      MRIorderIndices(mri_probs, x_indices, y_indices, z_indices) ;
      MRIfree(&mri_probs) ;
    }
    else
      MRIcomputeVoxelPermutation(mri_inputs, x_indices, y_indices,
                                 z_indices) ;
      
    nchanged = 0 ;
    for (index = 0 ; index < nindices ; index++)
    {
      x = x_indices[index] ; y = y_indices[index] ; z = z_indices[index] ;
      GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
      if (x == 100 && y == 104 && z == 130)
        DiagBreak() ;

#if 1
      if (MRIvox(mri_changed, x, y, z) == 0)
        continue ;
#endif

      if (x == 63 && y == 107 && z == 120)
        DiagBreak() ; 
          
      val = MRIvox(mri_inputs, x, y, z) ;
          
          
      /* find the node associated with this coordinate and classify */
      GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
      gcan = &gca->nodes[xn][yn][zn] ;
      label = 0 ; max_p = 2*GIBBS_NEIGHBORS*BIG_AND_NEGATIVE ;
      for (n = 0 ; n < gcan->nlabels ; n++)
      {
        gc = &gcan->gcs[n] ;
        
        /* compute 1-d Mahalanobis distance */
        dist = (val-gc->mean) ;
#define USE_LOG_PROBS  1
        p = -log(sqrt(gc->var)) - .5*(dist*dist/gc->var) + log(gc->prior);
        
        for (prior = 0.0f, i = 0 ; i < GIBBS_NEIGHBORS ; i++)
        {
          xnbr = mri_dst->xi[x+xnbr_offset[i]] ;
          ynbr = mri_dst->yi[y+ynbr_offset[i]] ;
          znbr = mri_dst->zi[z+znbr_offset[i]] ;
          nbr_label = MRIvox(mri_dst, xnbr, ynbr, znbr) ;
          for (j = 0 ; j < gc->nlabels[i] ; j++)
          {
            if (nbr_label == gc->labels[i][j])
              break ;
          }
          if (j < gc->nlabels[i])
          {
            if (!FZERO(gc->label_priors[i][j]))
              prior += log(gc->label_priors[i][j]) ;
            else
              prior += log(0.1f/(float)(gcan->total_training) ; /*BIG_AND_NEGATIVE*/
          }
          else
          {
            if (x == 141 && y == 62 && z == 126)
              DiagBreak() ;
            prior += log(0.1f/(float)(gcan->total_training)); /*2*GIBBS_NEIGHBORS*BIG_AND_NEGATIVE */ /* never occurred - make it unlikely */
          }
        }
        p += prior ;
        if (p > max_p)
        {
          max_p = p ;
          label = gcan->labels[n] ;
        }
      }
      
      if (FZERO(max_p))
      {
        label = 0 ; max_p = 2*GIBBS_NEIGHBORS*BIG_AND_NEGATIVE ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
          
          /* compute 1-d Mahalanobis distance */
          dist = (val-gc->mean) ;
          if (FZERO(gc->var))  /* make it a delta function */
          {
            if (FZERO(dist))
              p = 1.0 ;
            else
              p = 0.0 ;
          }
          else
            p = 1 / sqrt(gc->var * 2 * M_PI) * exp(-dist*dist/gc->var) ;
          
          p *= gc->prior ;
          if (p > max_p)
          {
            max_p = p ;
            label = gcan->labels[n] ;
          }
        }
      }
      
      if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
      {
        printf(
             "(%d, %d, %d): old label %s (%d), new label %s (%d) (p=%2.3f)\n",
             x, y, z, cma_label_to_name(MRIvox(mri_dst,x,y,z)),
             MRIvox(mri_dst,x,y,z), cma_label_to_name(label), label, max_p) ;
        if (label == Ggca_label)
        {
          DiagBreak() ;
        }
      }
      
      if (MRIvox(mri_dst, x, y, z) != label)
      {
        int old_label = MRIvox(mri_dst, x, y, z) ;
        if (x == 100 && y == 104 && z == 130)
          DiagBreak() ;
        old_ll = 
          gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y, z, transform,
                                    PRIOR_FACTOR) ;
        MRIvox(mri_dst, x, y, z) = label ;
        new_ll = 
          gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y, z, transform,
                                    PRIOR_FACTOR) ;
        if (new_ll > old_ll)
        {
          MRIvox(mri_changed, x, y, z) = 1 ;
          nchanged++ ;
        }
        else
        {
          MRIvox(mri_dst, x, y, z) = old_label ;
          MRIvox(mri_changed, x, y, z) = 0 ;
        }
      }
      else
        MRIvox(mri_changed, x, y, z) = 0 ;
    }
    ll = gcaGibbsImageLogLikelihood(gca, mri_dst, mri_inputs, lta) ;
    ll /= (double)(width*depth*height) ;
    if (!FZERO(lcma))
      printf("pass %d: %d changed. image ll: %2.3f (CMA=%2.3f)\n", 
             iter+1, nchanged, ll, lcma) ;
    else
      printf("pass %d: %d changed. image ll: %2.3f\n", 
             iter+1, nchanged, ll) ;
    MRIdilate(mri_changed, mri_changed) ;

  } while (nchanged > 0 && iter++ < max_iter) ;

  free(x_indices) ; free(y_indices) ; free(z_indices) ;
  MRIfree(&mri_changed) ;


  return(mri_dst) ;
}
#endif

MRI  *
GCAanneal(MRI *mri_inputs, GCA *gca, MRI *mri_dst,TRANSFORM *transform, 
          int max_iter)
{
  int      x, y, z, width, height, depth, label, val, iter,
           xn, yn, zn, n, nchanged,
           index, nindices, old_label ;
  short    *x_indices, *y_indices, *z_indices ;
  GCA_NODE *gcan ;
  double   ll, lcma = 0.0, old_ll, new_ll, min_ll ;
  MRI      *mri_changed, *mri_probs ;

  printf("performing simulated annealing...\n") ;

  nindices = mri_dst->width * mri_dst->height * mri_dst->depth ;
  x_indices = (short *)calloc(nindices, sizeof(short)) ;
  y_indices = (short *)calloc(nindices, sizeof(short)) ;
  z_indices = (short *)calloc(nindices, sizeof(short)) ;
  if (!x_indices || !y_indices || !z_indices)
    ErrorExit(ERROR_NOMEMORY, "GCAanneal: "
              "could not allocate index set") ;


  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ; iter = 0 ;
  if (!mri_dst)
  {
    mri_dst = MRIalloc(width, height, depth, MRI_UCHAR) ;
    if (!mri_dst)
      ErrorExit(ERROR_NOMEMORY, "GCAlabel: could not allocate dst") ;
    MRIcopyHeader(mri_inputs, mri_dst) ;
  }

  mri_changed = MRIclone(mri_dst, NULL) ;

  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  for (x = 0 ; x < width ; x++)
    for (y = 0 ; y < height ; y++)
        for (z = 0 ; z < depth ; z++)
          MRIvox(mri_changed,x,y,z) = 1 ;

  old_ll = gcaGibbsImageLogLikelihood(gca, mri_dst, mri_inputs, transform) ;
  old_ll /= (double)(width*depth*height) ;
#if 0
  {
    MRI   *mri_cma ;
    char  fname[STRLEN], *cp ;

    strcpy(fname, mri_inputs->fname) ;
    cp = strrchr(fname, '/') ;
    if (cp)
    {
      strcpy(cp+1, "parc") ;
      mri_cma = MRIread(fname) ;
      if (mri_cma)
      {

        lcma = gcaGibbsImageLogLikelihood(gca, mri_cma, mri_inputs, transform) ;
        lcma /= (double)(width*depth*height) ;
        fprintf(stderr, "image ll: %2.3f (CMA=%2.3f)\n", old_ll, lcma) ;
        MRIfree(&mri_cma) ;
      }
    }
  }
#endif

  do
  {
    if (iter == 0)
    {
      mri_probs = GCAlabelProbabilities(mri_inputs, gca, mri_dst, transform) ;
      MRIorderIndices(mri_probs, x_indices, y_indices, z_indices) ;
      MRIfree(&mri_probs) ;
    }
    else
      MRIcomputeVoxelPermutation(mri_inputs, x_indices, y_indices,
                                 z_indices) ;
      
    nchanged = 0 ;
    for (index = 0 ; index < nindices ; index++)
    {
      x = x_indices[index] ; y = y_indices[index] ; z = z_indices[index] ;
      if (x == 155 && y == 126 && z == 128)
        DiagBreak() ;

#if 1
      if (MRIvox(mri_changed, x, y, z) == 0)
        continue ;
#endif

      if (x == 63 && y == 107 && z == 120)
        DiagBreak() ; 
          
      val = MRIvox(mri_inputs, x, y, z) ;
          
      /* find the node associated with this coordinate and classify */
      GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
      gcan = &gca->nodes[xn][yn][zn] ;


      label = old_label = MRIvox(mri_dst, x, y, z) ;
      min_ll = gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y,z,transform,
                                         PRIOR_FACTOR);

      for (n = 0 ; n < gcan->nlabels ; n++)
      {
        if (gcan->labels[n] == old_label)
          continue ;
        MRIvox(mri_dst, x, y, z) = gcan->labels[n] ;
        new_ll = 
          gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y,z,transform,
                                    PRIOR_FACTOR);
        if (new_ll > min_ll)
        {
          min_ll = new_ll ; label = gcan->labels[n] ;
        }
      }
      if (label != old_label)
      {
        nchanged++ ;
        MRIvox(mri_changed, x, y, z) = 1 ;
      }
      else
        MRIvox(mri_changed, x, y, z) = 0 ;
      MRIvox(mri_dst, x, y, z) = label ;
    }
    if (nchanged > 10000)
    {
      ll = gcaGibbsImageLogLikelihood(gca, mri_dst, mri_inputs, transform) ;
      ll /= (double)(width*depth*height) ;
      if (!FZERO(lcma))
        printf("pass %d: %d changed. image ll: %2.3f (CMA=%2.3f)\n", 
               iter+1, nchanged, ll, lcma) ;
      else
        printf("pass %d: %d changed. image ll: %2.3f\n", iter+1, nchanged, ll);
    }
    else
      printf("pass %d: %d changed.\n", iter+1, nchanged) ;
    MRIdilate(mri_changed, mri_changed) ;

  } while (nchanged > 0 && iter++ < max_iter) ;

  free(x_indices) ; free(y_indices) ; free(z_indices) ;
  MRIfree(&mri_changed) ;

  return(mri_dst) ;
}

char *gca_write_fname = NULL ;
int gca_write_iterations = 0 ;

#define MAX_PRIOR_FACTOR 1.0
#define MIN_PRIOR_FACTOR 1

MRI  *
GCAreclassifyUsingGibbsPriors(MRI *mri_inputs, GCA *gca, MRI *mri_dst,TRANSFORM *transform,
                            int max_iter, MRI *mri_fixed, int restart,
                              void (*update_func)(MRI *))
{
  int      x, y, z, width, height, depth, label, val, iter,
           n, nchanged, min_changed, index, nindices, old_label, fixed ;
  short    *x_indices, *y_indices, *z_indices ;
  GCA_PRIOR *gcap ;
  double   ll, lcma = 0.0, old_ll, new_ll, min_ll ;
  MRI      *mri_changed, *mri_probs /*, *mri_zero */ ;

  fixed = (mri_fixed != NULL) ;

#if 0
  GCAannealUnlikelyVoxels(mri_inputs, gca, mri_dst, transform, max_iter*100,
                          mri_fixed) ;
  {
    char  fname[STRLEN], *cp ;

    strcpy(fname, mri_inputs->fname) ;
    cp = strrchr(fname, '/') ;
    if (cp)
    {
      strcpy(cp+1, "anneal") ;
      fprintf(stderr, "writing results of annealing to %s...\n", fname) ;
      MRIwrite(mri_dst, fname) ;
    }
  }
#endif

  nindices = mri_dst->width * mri_dst->height * mri_dst->depth ;
  x_indices = (short *)calloc(nindices, sizeof(short)) ;
  y_indices = (short *)calloc(nindices, sizeof(short)) ;
  z_indices = (short *)calloc(nindices, sizeof(short)) ;
  if (!x_indices || !y_indices || !z_indices)
    ErrorExit(ERROR_NOMEMORY, "GCAreclassifyUsingGibbsPriors: "
              "could not allocate index set") ;


#if 0
  mri_zero = MRIclone(mri_inputs, NULL) ;
#endif
  width = mri_inputs->width ; height = mri_inputs->height; 
  depth = mri_inputs->depth ; iter = 0 ;
  if (!mri_dst)
  {
    mri_dst = MRIalloc(width, height, depth, MRI_UCHAR) ;
    if (!mri_dst)
      ErrorExit(ERROR_NOMEMORY, "GCAlabel: could not allocate dst") ;
    MRIcopyHeader(mri_inputs, mri_dst) ;
  }

  mri_changed = MRIclone(mri_dst, NULL) ;


  /* go through each voxel in the input volume and find the canonical
     voxel (and hence the classifier) to which it maps. Then update the
     classifiers statistics based on this voxel's intensity and label.
  */
  for (x = 0 ; x < width ; x++)
    for (y = 0 ; y < height ; y++)
        for (z = 0 ; z < depth ; z++)
        {
          if (restart && mri_fixed)
          {
            if (MRIvox(mri_fixed, x, y, z))
              MRIvox(mri_changed,x,y,z) = 1 ;
          }
          else
            MRIvox(mri_changed,x,y,z) = 1 ;
        }

  if (restart && mri_fixed)
    MRIdilate(mri_changed, mri_changed) ;

  if (!restart)
  {
    old_ll = gcaGibbsImageLogLikelihood(gca, mri_dst, mri_inputs, transform) ;
    old_ll /= (double)(width*depth*height) ;
  }
  else
    old_ll = 0 ;
#if 0
  {
    MRI   *mri_cma ;
    char  fname[STRLEN], *cp ;

    strcpy(fname, mri_inputs->fname) ;
    cp = strrchr(fname, '/') ;
    if (cp)
    {
      strcpy(cp+1, "parc") ;
      mri_cma = MRIread(fname) ;
      if (mri_cma)
      {
        lcma = gcaGibbsImageLogLikelihood(gca, mri_cma, mri_inputs, transform) ;
        lcma /= (double)(width*depth*height) ;
        fprintf(stderr, "image ll: %2.3f (CMA=%2.3f)\n", old_ll, lcma) ;
        MRIfree(&mri_cma) ;
      }
    }
  }
#endif

  PRIOR_FACTOR = MIN_PRIOR_FACTOR ;
  do
  {
    if (restart)
    {
      for (index = x = 0 ; x < width ; x++)
        for (y = 0 ; y < height ; y++)
          for (z = 0 ; z < depth ; z++)
          {
            if (MRIvox(mri_fixed, x, y, z) == 0 &&
                (MRIvox(mri_changed,x,y,z) > 0))
            {
              x_indices[index] = x ;
              y_indices[index] = y ;
              z_indices[index] = z ;
              index++ ;
            }
          }
      nindices = index ;
    }
    else if (iter == 0)
    {
      /*      char  fname[STRLEN], *cp ;*/
      /*      int   nfixed ;*/

      if (gca_write_iterations)
      {
        char fname[STRLEN] ;
        sprintf(fname, "%s%03d.mgh", gca_write_fname, iter) ;
        printf("writing snapshot to %s...\n", fname) ;
        MRIwrite(mri_dst, fname) ;
      }
      mri_probs = GCAlabelProbabilities(mri_inputs, gca, NULL, transform) ;
      MRIorderIndices(mri_probs, x_indices, y_indices, z_indices) ;
      MRIfree(&mri_probs) ;
    }
    else
      MRIcomputeVoxelPermutation(mri_inputs, x_indices, y_indices,
                                 z_indices) ;
      
    nchanged = 0 ;
    for (index = 0 ; index < nindices ; index++)
    {
      x = x_indices[index] ; y = y_indices[index] ; z = z_indices[index] ;
      if (x == 139 && y == 103 && z == 139)  /* wm should be pallidum */
        DiagBreak() ;
      if (x == 92 && y == 115 && z == 117)
        DiagBreak() ;
      if (mri_fixed && MRIvox(mri_fixed, x, y, z))
        continue ;

      if (MRIvox(mri_changed, x, y, z) == 0)
        continue ;

      if (x == 156 && y == 124 && z == 135)  /* wm should be amygdala */
        DiagBreak() ; 
          
      val = MRIvox(mri_inputs, x, y, z) ;
          
      /* find the node associated with this coordinate and classify */
      gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;
      if (gcap->nlabels == 1)
        continue ;

      label = old_label = MRIvox(mri_dst, x, y, z) ;
      min_ll = gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y,z,transform,
                                         PRIOR_FACTOR);

#if 0
      if (min_ll < BIG_AND_NEGATIVE/2 && mri_zero)
        MRIvox(mri_zero, x, y, z) = 255 ;
#endif
      for (n = 0 ; n < gcap->nlabels ; n++)
      {
        if (gcap->labels[n] == old_label)
          continue ;
        MRIvox(mri_dst, x, y, z) = gcap->labels[n] ;
        new_ll = 
          gcaNbhdGibbsLogLikelihood(gca, mri_dst, mri_inputs, x, y,z,transform,
                                    PRIOR_FACTOR);
        if (new_ll > min_ll)
        {
          min_ll = new_ll ; label = gcap->labels[n] ;
        }
      }
      if (x == Ggca_x && y == Ggca_y && z == Ggca_z && 
          (label == Ggca_label || old_label == Ggca_label || Ggca_label < 0))
      {
        int       xn, yn, zn ;
        GCA_NODE *gcan ;
        
        GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
        gcan = &gca->nodes[xn][yn][zn] ;
        printf(
         "(%d, %d, %d): old label %s (%d), new label %s (%d) (log(p)=%2.3f)\n",
         x, y, z, cma_label_to_name(old_label), old_label, 
         cma_label_to_name(label), label, min_ll) ;
        dump_gcan(gca, gcan, stdout, 0, gcap) ;
        if (label == Right_Caudate)
          DiagBreak() ;
      }
#if 0
      if (((old_label == Left_Cerebral_White_Matter) && 
          (label == Left_Cerebral_Cortex)) ||
          ((old_label == Right_Cerebral_White_Matter) && 
           (label == Right_Cerebral_Cortex)))  /* don't let wm become gm */
        label = old_label ;
#endif
      if (label != old_label)
      {
        nchanged++ ;
        MRIvox(mri_changed, x, y, z) = 1 ;
      }
      else
        MRIvox(mri_changed, x, y, z) = 0 ;
      MRIvox(mri_dst, x, y, z) = label ;
    }
    if (nchanged > 10000 && iter < 2 && !restart)
    {
      ll = gcaGibbsImageLogLikelihood(gca, mri_dst, mri_inputs, transform) ;
      ll /= (double)(width*depth*height) ;
      if (!FZERO(lcma))
        printf("pass %d: %d changed. image ll: %2.3f (CMA=%2.3f), PF=%2.3f\n", 
               iter+1, nchanged, ll, lcma, PRIOR_FACTOR) ;
      else
        printf("pass %d: %d changed. image ll: %2.3f, PF=%2.3f\n", 
               iter+1, nchanged, ll, PRIOR_FACTOR) ;
    }
    else
      printf("pass %d: %d changed.\n", iter+1, nchanged) ;
    MRIdilate(mri_changed, mri_changed) ;
    if (update_func)
      (*update_func)(mri_dst) ;

#if 0
    if (!iter && DIAG_VERBOSE_ON)
    {
      char  fname[STRLEN], *cp ;
      /*      int   nvox ;*/
      
      strcpy(fname, mri_inputs->fname) ;
      cp = strrchr(fname, '/') ;
      if (cp)
      {
        strcpy(cp+1, "zero") ;
        nvox = MRIvoxelsInLabel(mri_zero, 255) ;
        fprintf(stderr, "writing %d low probability points to %s...\n", 
                nvox, fname) ;
        MRIwrite(mri_zero, fname) ;
        MRIfree(&mri_zero) ;
      }
    }
#endif
#define MIN_CHANGED 5000
    min_changed = restart ? 0 : MIN_CHANGED ;
    if (nchanged <= min_changed ||
        (restart && iter >= max_iter))
    {
      if (restart)
        iter = 0 ;
#if 0
      if (restart)
        break ;
#endif

      if (!restart)  /* examine whole volume next time */
      {
        for (x = 0 ; x < width ; x++)
          for (y = 0 ; y < height ; y++)
            for (z = 0 ; z < depth ; z++)
              MRIvox(mri_changed,x,y,z) = 1 ;
      }
      if (fixed && !restart)
      {
        printf("removing fixed flag...\n") ;
        if (mri_fixed)
          MRIclear(mri_fixed) ;
        fixed = 0 ;
      }
      else
      {
        PRIOR_FACTOR *= 2 ;
        if (PRIOR_FACTOR < MAX_PRIOR_FACTOR)
          fprintf(stderr, "setting PRIOR_FACTOR to %2.4f\n", PRIOR_FACTOR) ;
      }
      if (gca_write_iterations < 0)
      {
        char fname[STRLEN] ;
        static int fno = 0 ;

        sprintf(fname, "%s%03d.mgh", gca_write_fname, fno+1) ; fno++ ;
        printf("writing snapshot to %s...\n", fname) ;
        MRIwrite(mri_dst, fname) ;
      }
    }
    if ((gca_write_iterations > 0) && !(iter % gca_write_iterations))
    {
      char fname[STRLEN] ;
      sprintf(fname, "%s%03d.mgh", gca_write_fname, iter+1) ;
      printf("writing snapshot to %s...\n", fname) ;
      MRIwrite(mri_dst, fname) ;
    }
  } while ((nchanged > MIN_CHANGED || PRIOR_FACTOR < MAX_PRIOR_FACTOR) && 
           (iter++ < max_iter)) ;

#if 0
  {
    char  fname[STRLEN], *cp ;
    int   nzero, n_nonzero ;

    strcpy(fname, mri_inputs->fname) ;
    cp = strrchr(fname, '/') ;
    if (cp)
    {
      strcpy(cp+1, "indices") ;
      mri_probs = GCAcomputeProbabilities(mri_inputs, gca, mri_dst,NULL, transform) ;
      MRIorderIndices(mri_probs, x_indices, y_indices, z_indices) ;
      for (nzero = index = 0 ; index < nindices ; index++, nzero++)
      {
        x = x_indices[index] ; y = y_indices[index] ; z = z_indices[index] ;
        if (MRIvox(mri_probs, x, y, z) != 255)
          break ;
        MRIvox(mri_probs, x, y, z) = 0 ;
      }
      n_nonzero = nindices - nzero ;
      for ( ; index < nindices ; index++)
      {
        x = x_indices[index] ; y = y_indices[index] ; z = z_indices[index] ;
        if (MRIvox(mri_probs, x, y, z) == 255)
          MRIvox(mri_probs, x, y, z) = 0 ;
        else
        {
          MRIvox(mri_probs, x, y, z) = 
            100 * (float)(n_nonzero-index)/n_nonzero ;
        }
      }
      MRIwrite(mri_probs, fname) ;
      MRIfree(&mri_probs) ;
    }
  }
#endif


  free(x_indices) ; free(y_indices) ; free(z_indices) ;
  MRIfree(&mri_changed) ;

  return(mri_dst) ;
}
int
MRIcomputeVoxelPermutation(MRI *mri, short *x_indices, short *y_indices,
                           short *z_indices)
{
  int width, height, depth, tmp, nindices, i, index ;

  width = mri->width, height = mri->height ; depth = mri->depth ;
  nindices = width*height*depth ;

  for (i = 0 ; i < nindices ; i++)
  {
    x_indices[i] = i % width ;
    y_indices[i] = (i/width) % height ;
    z_indices[i] = (i / (width*height)) % depth ;
  }
  for (i = 0 ; i < nindices ; i++)
  {
    index = (int)randomNumber(0.0, (double)(nindices-0.0001)) ;

    tmp = x_indices[index] ; 
    x_indices[index] = x_indices[i] ; x_indices[i] = tmp ;

    tmp = y_indices[index] ; 
    y_indices[index] = y_indices[i] ; y_indices[i] = tmp ;

    tmp = z_indices[index] ; 
    z_indices[index] = z_indices[i] ; z_indices[i] = tmp ;
  }
  return(NO_ERROR) ;
}

#if 0
static int
gcaGibbsSort(GCA *gca, MRI *mri_labels, MRI *mri_inputs,
                           TRANSFORM *transform)
{
  int    x, y, z, width, depth, height ;
  double total_log_likelihood, log_likelihood ;
  MRI    *mri_probs ;

  width = mri_labels->width ; height = mri_labels->height ; 
  depth = mri_labels->depth ;
  mri_probs = MRIclone(mri_labels, NULL) ;
  
  for (total_log_likelihood = 0.0, x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        log_likelihood = 
          gcaVoxelGibbsLogLikelihood(gca, mri_labels, mri_inputs, x, y, z,transform,
                                     PRIOR_FACTOR);
        log_likelihood *= 20 ;
        if (log_likelihood > 255)
          log_likelihood = 255 ;
        MRIvox(mri_probs,x,y,z) = log_likelihood ;
        total_log_likelihood += log_likelihood ;
      }
    }
  }
  MRIorderIndices(mri_probs, x_indices, y_indices, z_indices) ;
  MRIfree(&mri_probs) ;
  return(NO_ERROR) ;
}
#endif

static double
gcaGibbsImageLogLikelihood(GCA *gca, MRI *mri_labels, MRI *mri_inputs,
                           TRANSFORM *transform)
{
  int    x, y, z, width, depth, height ;
  double total_log_likelihood, log_likelihood ;

  width = mri_labels->width ; height = mri_labels->height ; 
  depth = mri_labels->depth ;
  
  for (total_log_likelihood = 0.0, x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        log_likelihood = 
          gcaVoxelGibbsLogLikelihood(gca, mri_labels, mri_inputs, x, y, z,transform,
                                     PRIOR_FACTOR);
        if (check_finite("gcaGibbsImageLoglikelihood", log_likelihood) == 0)
					DiagBreak() ;
        total_log_likelihood += log_likelihood ;
				if (total_log_likelihood > 1e10)
					DiagBreak() ;
      }
    }
  }
  return(total_log_likelihood) ;
}
static double
gcaGibbsImpossibleConfiguration(GCA *gca, MRI *mri_labels, 
                                int x, int y, int z, TRANSFORM *transform)
{
  int       xn, yn, zn, xnbr, ynbr, znbr, nbr_label, label, i,j, n;
  GCA_NODE  *gcan ;
  GC1D      *gc ;

  label = MRIvox(mri_labels, x, y, z) ;

  /* find the node associated with this coordinate and classify */
  GCAsourceVoxelToNode(gca, mri_labels, transform, x, y, z, &xn, &yn, &zn) ;
  gcan = &gca->nodes[xn][yn][zn] ;

  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == label)
      break ;
  }
  if (n >= gcan->nlabels)
    return(1) ;  /* never occurred */

  gc = &gcan->gcs[n] ;
  
  for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
  {
    xnbr = mri_labels->xi[x+xnbr_offset[i]] ;
    ynbr = mri_labels->yi[y+ynbr_offset[i]] ;
    znbr = mri_labels->zi[z+znbr_offset[i]] ;
    nbr_label = MRIvox(mri_labels, xnbr, ynbr, znbr) ;
    for (j = 0 ; j < gc->nlabels[i] ; j++)
    {
      if (nbr_label == gc->labels[i][j])
        break ;
    }
    if (j < gc->nlabels[i])
    {
      if (FZERO(gc->label_priors[i][j]))
        return(1) ; /* never occurred (and this never should) */
    }
    else   /* never occurred - make it unlikely */
    {
      if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
        DiagBreak() ;
      if (FZERO(gc->label_priors[i][j]))
        return(1) ; /* never occurred (and this never should) */
    }
  }

  return(0) ;
}


static double
gcaNbhdGibbsLogLikelihood(GCA *gca, MRI *mri_labels, MRI *mri_inputs, int x, 
                      int y, int z, TRANSFORM *transform, double gibbs_coef)
{
  double total_log_likelihood/*, log_likelihood*/ ;
  /*  int    i, xnbr, ynbr, znbr ;*/


  total_log_likelihood = 
    gcaVoxelGibbsLogLikelihood(gca, mri_labels, mri_inputs, x, y, z, transform,
                               gibbs_coef) ;

#if 0
  for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
  {
    xnbr = mri_inputs->xi[x+xnbr_offset[i]] ;
    ynbr = mri_inputs->yi[y+ynbr_offset[i]] ;
    znbr = mri_inputs->zi[z+znbr_offset[i]] ;
    log_likelihood = 
      gcaVoxelGibbsLogLikelihood(gca, mri_labels, mri_inputs, xnbr, ynbr, 
                                 znbr, transform, gibbs_coef) ;
    total_log_likelihood += log_likelihood ;
  }
#endif

  return(total_log_likelihood) ;
}

static double
gcaVoxelGibbsLogLikelihood(GCA *gca, MRI *mri_labels, MRI *mri_inputs, int x, 
                      int y, int z, TRANSFORM *transform, double gibbs_coef)
{
  double    log_likelihood/*, dist*/, nbr_prior ;
  int       xn, yn, zn, xnbr, ynbr, znbr, nbr_label, label,
            i,j, n;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  GC1D      *gc ;
	float     vals[MAX_GCA_INPUTS] ;


	load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
  label = MRIvox(mri_labels, x, y, z) ;

  /* find the node associated with this coordinate and classify */
  GCAsourceVoxelToNode(gca, mri_inputs, transform, x, y, z, &xn, &yn, &zn) ;
  gcan = &gca->nodes[xn][yn][zn] ;
  gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;

  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == label)
      break ;
  }
  if (n >= gcan->nlabels)
  {
    if (gcan->total_training > 0)
      return(log(0.01f/((float)gcan->total_training*GIBBS_NEIGHBORS))) ; /* 10*GIBBS_NEIGHBORS*BIG_AND_NEGATIVE*/
    else
      return(log(VERY_UNLIKELY)) ;
  }

  gc = &gcan->gcs[n] ;
  
  /* compute 1-d Mahalanobis distance */
  log_likelihood = 
    GCAcomputeConditionalLogDensity(gc,vals,gca->ninputs, gcan->labels[n]);
	if (check_finite("gcaVoxelGibbsLogLikelihood: conditional log density", log_likelihood) == 0)
		DiagBreak() ;

  nbr_prior = 0.0 ;
  for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
  {
    xnbr = mri_labels->xi[x+xnbr_offset[i]] ;
    ynbr = mri_labels->yi[y+ynbr_offset[i]] ;
    znbr = mri_labels->zi[z+znbr_offset[i]] ;
    nbr_label = MRIvox(mri_labels, xnbr, ynbr, znbr) ;
    for (j = 0 ; j < gc->nlabels[i] ; j++)
    {
      if (nbr_label == gc->labels[i][j])
        break ;
    }
    if (j < gc->nlabels[i])
    {
      if (!FZERO(gc->label_priors[i][j]))
        nbr_prior += log(gc->label_priors[i][j]) ;
      else
        nbr_prior += log(0.1f/(float)gcan->total_training) ; /*BIG_AND_NEGATIVE */
      check_finite("gcaVoxelGibbsLogLikelihood: label_priors", nbr_prior) ;
    }
    else   /* never occurred - make it unlikely */
    {
      if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
        DiagBreak() ;
      nbr_prior += log(0.1f/(float)gcan->total_training) ; /*BIG_AND_NEGATIVE*/
    }
  }
  log_likelihood += (gibbs_coef * nbr_prior + log(getPrior(gcap, label))) ;
  if (check_finite("gcaVoxelGibbsLogLikelihood: final", log_likelihood)  == 0)
		DiagBreak() ;

  return(log_likelihood) ;
}
static int compare_sort_mri(const void *plp1, const void *plp2);
typedef struct
{
  unsigned char x, y, z, val ;
} SORT_VOXEL ;

static int
MRIorderIndices(MRI *mri, short *x_indices, short *y_indices, short *z_indices)
{
  int         width, height, depth, nindices, index, x, y, z ;
  SORT_VOXEL  *sort_voxels ;

  width = mri->width, height = mri->height ; depth = mri->depth ;
  nindices = width*height*depth ;

  sort_voxels = (SORT_VOXEL *)calloc(nindices, sizeof(SORT_VOXEL)) ;
  if (!sort_voxels)
    ErrorExit(ERROR_NOMEMORY,"MRIorderIndices: could not allocate sort table");

  for (index = x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++, index++)
      {
        sort_voxels[index].x = x ;
        sort_voxels[index].y = y ;
        sort_voxels[index].z = z ;
        sort_voxels[index].val = MRIvox(mri, x, y, z) ;
      }
    }
  }
  qsort(sort_voxels, nindices, sizeof(SORT_VOXEL), compare_sort_mri) ;

  for (index = 0 ; index < nindices ; index++)
  {
    x_indices[index] = sort_voxels[index].x ;
    y_indices[index] = sort_voxels[index].y ;
    z_indices[index] = sort_voxels[index].z ;
  }
  
  free(sort_voxels) ;
  return(NO_ERROR) ;
}

static int
compare_sort_mri(const void *psv1, const void *psv2)
{
  SORT_VOXEL  *sv1, *sv2 ;

  sv1 = (SORT_VOXEL *)psv1 ;
  sv2 = (SORT_VOXEL *)psv2 ;

  if (sv1->val > sv2->val)
    return(1) ;
  else if (sv1->val < sv2->val)
    return(-1) ;

  return(0) ;
}
MRI *
GCAbuildMostLikelyVolume(GCA *gca, MRI *mri)
{
  int       x,  y, z, xn, yn, zn, width, depth, height, n, xp, yp, zp, r ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;
  double    max_prior ;
  int       max_label ;
	GC1D      *gc_max ;

	if (mri->nframes != gca->ninputs)
		ErrorExit(ERROR_BADPARM, "GCAbuildMostLikelyVolume: mri->frames (%d) does not match gca->ninputs (%d)",
							mri->nframes, gca->ninputs) ;

  width = mri->width ; depth = mri->depth ; height = mri->height ;

  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        GCAvoxelToNode(gca, mri, x, y, z, &xn, &yn, &zn) ;
        GCAvoxelToPrior(gca, mri, x, y, z, &xp, &yp, &zp) ;
        gcan = &gca->nodes[xn][yn][zn] ;
        gcap = &gca->priors[xp][yp][zp] ;
        max_prior = gcap->priors[0] ; max_label = gcap->labels[0] ; gc_max = NULL ;
        for (n = 1 ; n < gcap->nlabels ; n++)
        {
          if (gcap->priors[n] > max_prior)
          {
            max_prior = gcap->priors[n] ; max_label = gcap->labels[n] ;
          }
        }
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          if (gcan->labels[n] == max_label)
            gc_max = &gcan->gcs[n] ;
        }

				if (!gc_max)
					continue ;
				for (r = 0 ; r < gca->ninputs ; r++)
				{
					switch (mri->type)
					{
					default:
						ErrorReturn(NULL,
												(ERROR_UNSUPPORTED, 
												 "GCAbuildMostLikelyVolume: unsupported image type %d", mri->type)) ;
						break ;
					case MRI_UCHAR:
						MRIseq_vox(mri, x, y, z, r) = nint(gc_max->means[r]) ;
						break ;
					case MRI_FLOAT:
						MRIFseq_vox(mri, x, y, z, r) = gc_max->means[r] ;
						break ;
					case MRI_SHORT:
						MRISseq_vox(mri, x, y, z, r) = gc_max->means[r] ;
						break ;
					}
				}
      }
    }
  }

  return(mri) ;
}

GC1D *
GCAfindPriorGC(GCA *gca, int xp, int yp, int zp,int label)
{
  int xn, yn, zn ;

  GCApriorToNode(gca, xp, yp, zp, &xn, &yn, &zn) ;
  return(GCAfindGC(gca, xn, yn, zn, label)) ;
}

#if 1
GC1D *
GCAfindGC(GCA *gca, int xn, int yn, int zn,int label)
{
  int        n ;
  GCA_NODE   *gcan  ;

  gcan = &gca->nodes[xn][yn][zn] ;

  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == label)
      return(&gcan->gcs[n]) ;
  }

  return(NULL) ;
}
#endif

#include "mrisegment.h"
/* each segment must be at least this much of total to be retained */
#define MIN_SEG_PCT  0.15   

static int gcaReclassifySegment(GCA *gca, MRI *mri_inputs, MRI *mri_labels,
                                MRI_SEGMENT *mseg, int old_label, TRANSFORM *transform);
static int gcaReclassifyVoxel(GCA *gca, MRI *mri_inputs, MRI *mri_labels,
                              int x, int y, int z, int old_label, TRANSFORM *transform);
MRI *
GCAconstrainLabelTopology(GCA *gca, MRI *mri_inputs,MRI *mri_src, MRI *mri_dst,
                          TRANSFORM *transform)
{
  int              i, j, nvox /*, x, y, z, width, height, depth*/ ;
  MRI_SEGMENTATION *mriseg ;

  mri_dst = MRIcopy(mri_src, mri_dst) ;

  for (i = 1 ; i <= MAX_CMA_LABEL ; i++)
  {
    if (!IS_BRAIN(i))
      continue ;
    nvox = MRIvoxelsInLabel(mri_dst, i) ;
    if (!nvox)
      continue ;
		if (LABEL_WITH_NO_TOPOLOGY_CONSTRAINT(i))
			continue ;
    /*    printf("label %03d: %d voxels\n", i, nvox) ;*/
    mriseg = MRIsegment(mri_src, (float)i, (float)i) ;
    if (!mriseg)
    {
      ErrorPrintf(Gerror,"GCAconstrainLabelTopology: label %s failed (%d vox)",
                  cma_label_to_name(i), nvox) ;
      continue ;
    }

    /*    printf("\t%d segments:\n", mriseg->nsegments) ;*/
    for (j = 0 ; j < mriseg->nsegments ; j++)
    {
      if (IS_LAT_VENT(i) && mriseg->segments[j].nvoxels > 20)
        continue ;
      /*      printf("\t\t%02d: %d voxels", j, mriseg->segments[j].nvoxels) ;*/
      if ((float)mriseg->segments[j].nvoxels / (float)nvox < MIN_SEG_PCT)
      {
        /*        printf(" - reclassifying...") ;*/
        gcaReclassifySegment(gca,mri_inputs,mri_dst, &mriseg->segments[j], i,
                             transform);
      }
      /*      printf("\n") ;*/
    }
    MRIsegmentFree(&mriseg) ;
  }

#if 0
  width = mri_dst->width ; height = mri_dst->height ; 
  depth  = mri_dst->depth ; 
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == 144 && y == 118 && z == 127)
          DiagBreak() ;
        if (MRIvox(mri_dst, x, y, z) == LABEL_UNDETERMINED)
          gcaReclassifyVoxel(gca, mri_inputs, mri_dst, 
                             x, y, z, LABEL_UNDETERMINED, transform) ;
      }
    }
  }
#endif

  return(mri_dst) ;
}

static int
gcaReclassifySegment(GCA *gca, MRI *mri_inputs, MRI *mri_labels,
                     MRI_SEGMENT *mseg, int old_label, TRANSFORM *transform)
{
  int   i ;

  for (i = 0 ; i < mseg->nvoxels ; i++)
  {
#if 1
    gcaReclassifyVoxel(gca, mri_inputs, mri_labels, 
                       mseg->voxels[i].x, mseg->voxels[i].y, mseg->voxels[i].z,
                       old_label, transform) ;
#else
    MRIvox(mri_labels,mseg->voxels[i].x,mseg->voxels[i].y,mseg->voxels[i].z) =
      LABEL_UNDETERMINED ;
#endif
  }

  return(NO_ERROR) ;
}

static int
gcaReclassifyVoxel(GCA *gca, MRI *mri_inputs, MRI *mri_labels,
                     int x, int y, int z, int old_label, TRANSFORM *transform)
{
  int     nbr_labels[255], xi, yi, zi, xk, yk, zk, i, new_label ;
  double  max_p, p ;

  if (x == 144 && y == 118 && z == 127)
    DiagBreak() ;
  memset(nbr_labels, 0, sizeof(nbr_labels)) ;
  for (zk = -1 ; zk <= 1 ; zk++)
  {
    zi = mri_labels->zi[z+zk] ;
    for (yk = -1 ; yk <= 1 ; yk++)
    {
      yi = mri_labels->yi[y+yk] ;
      for (xk = -1 ; xk <= 1 ; xk++)
      {
        xi = mri_labels->xi[x+xk] ;
        nbr_labels[MRIvox(mri_labels, xi, yi, zi)]++ ;
      }
    }
  }
  new_label = 0 ; max_p = 10*BIG_AND_NEGATIVE ;
  nbr_labels[old_label] = 0 ;
  for (i = 0 ; i <= 255 ; i++)
  {
    if (nbr_labels[i] > 0)
    {
      MRIvox(mri_labels, x, y, z) = i ;
      p = gcaVoxelGibbsLogLikelihood(gca, mri_labels, mri_inputs, x, y, z,transform,
                                     PRIOR_FACTOR);
      if (p > max_p)
      {
        max_p = p ;
        new_label = i ;
      }
    }
  }

  if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
  {
    printf("reclassifying (%d,%d,%d) from %s to %s in topology constraint\n",
           x, y, z, cma_label_to_name(MRIvox(mri_labels,x,y,z)),
           cma_label_to_name(new_label)) ;
    DiagBreak() ;
  }
  MRIvox(mri_labels, x, y, z) = new_label ;
  return(NO_ERROR) ;
}
#define MAX_VENTRICLE_ITERATIONS  10
MRI *
GCAexpandVentricle(GCA *gca, MRI *mri_inputs, MRI *mri_src,
                   MRI *mri_dst, TRANSFORM *transform, int target_label)
{
  int      nchanged, x, y, z, width, height, depth, xn, yn, zn, xi, yi, zi,
           xk, yk, zk, label, total_changed, i ;
  GCA_NODE *gcan ;
	GC1D     *gc, *gc_vent ;
  float    v_means[MAX_GCA_INPUTS], v_var, pv, plabel, vals[MAX_GCA_INPUTS] ;
  MRI      *mri_tmp ;

  /* compute label mean and variance */

  gcaComputeLabelStats(gca, target_label, &v_var, v_means) ;
  printf("ventricle intensity = ") ;
	for (i = 0 ; i < gca->ninputs ; i++)
		printf("%2.1f ", v_means[i]) ;
  printf(" +- %2.1f\n", sqrt(v_var)) ;
	

  if (mri_src != mri_dst)
    mri_dst = MRIcopy(mri_src, mri_dst) ;

  mri_tmp = MRIcopy(mri_dst, NULL) ;

  width = mri_src->width ; height = mri_src->height ; depth = mri_src->depth ;

  i = total_changed = 0 ;
  do
  {
    nchanged = 0 ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
            DiagBreak() ; 
          
					GCAsourceVoxelToNode(gca, mri_dst, transform, x, y, z, &xn, &yn, &zn) ;
					gc_vent = GCAfindGC(gca, xn, yn, zn, target_label) ;
					if (gc_vent == NULL)
						continue ;
          label = MRIvox(mri_dst, x, y, z) ;
          
          if (label == target_label)
          {
            for (zk = -1 ; zk <= 1 ; zk++)
            {
              for (yk = -1 ; yk <= 1 ; yk++)
              {
                for (xk = -1 ; xk <= 1 ; xk++)
                {
                  if (fabs(xk) + fabs(yk) + fabs(zk) > 1)
                    continue ;
                  xi = mri_src->xi[x+xk] ;
                  yi = mri_src->yi[y+yk] ;
                  zi = mri_src->zi[z+zk] ;
                  label = MRIvox(mri_dst, xi, yi, zi) ;
                  if (label != target_label) /* should it be changed? */
                  {
                    GCAsourceVoxelToNode(gca, mri_dst, transform, xi, yi, zi, 
                                         &xn, &yn, &zn) ;
                    gcan = &gca->nodes[xn][yn][zn] ;
            
                    if (xi == Ggca_x && yi == Ggca_y && zi == Ggca_z)
                      DiagBreak() ; 

										load_vals(mri_inputs, xi, yi, zi, vals, gca->ninputs) ;
										gc = GCAfindGC(gca, xn, yn, zn, label) ;
										if (gc == NULL)
											plabel = 0.0 ;
										else
											plabel = GCAcomputeConditionalDensity(gc, vals, gca->ninputs, label) ;
										gc = GCAfindGC(gca, xn, yn, zn, target_label) ;
										if (gc == NULL)   /* use neighboring gc to allow for variability in ventricles */
											gc = gc_vent ;
										pv = GCAcomputeConditionalDensity(gc, vals, gca->ninputs, target_label) ;
                    if (pv > 5*plabel)
                    {
                      if (xi == Ggca_x && yi == Ggca_y && zi == Ggca_z)
                      {
                        int olabel = MRIvox(mri_tmp, xi, yi, zi) ;

                        printf("voxel (%d, %d, %d) changed from %s (%d) "
                               "to %s (%d)\n", xi, yi, zi,
                               cma_label_to_name(olabel), olabel,
                               cma_label_to_name(target_label), target_label);
                      }
                      if (xi == 140 && yi == 79 && zi == 136)
                        DiagBreak() ;   /* v should be wm */
                      nchanged++ ;
                      MRIvox(mri_tmp, xi, yi, zi) = target_label ;
                    }
                  }
                }
              }
            }
          } 
        }
      }
    }
    MRIcopy(mri_tmp, mri_dst) ;
    total_changed += nchanged ;
    if (++i > MAX_VENTRICLE_ITERATIONS)
      break ;
  } while (nchanged > 0) ;

  MRIfree(&mri_tmp) ;
  printf("%d labels changed to %s...\n", total_changed, cma_label_to_name(target_label)) ;
  return(mri_dst) ;
}
#define MAX_CORTICAL_ITERATIONS  10
MRI *
GCAexpandCortex(GCA *gca, MRI *mri_inputs, MRI *mri_src,
                MRI *mri_dst, TRANSFORM *transform)
{
  int      nchanged, x, y, z, width, height, depth, xn, yn, zn, xi, yi, zi,
           xk, yk, zk, label, total_changed, i, wm_nbr, gray_nbr, left ;
  GCA_NODE *gcan ;
  float    wm_means[MAX_GCA_INPUTS], wm_var, gray_means[MAX_GCA_INPUTS], gray_var, ldist, wdist, gdist ;
  MRI      *mri_tmp ;
	float    vals[MAX_GCA_INPUTS] ;
	GC1D     *gc_wm, *gc_gm, *gc_label ;


  /* compute label mean and variance */

  gcaComputeLabelStats(gca, Left_Cerebral_White_Matter, &wm_var, wm_means) ;
  gcaComputeLabelStats(gca, Left_Cerebral_Cortex, &gray_var, gray_means) ;
  printf("cortex mean - gray ") ;
	for (i = 0 ; i < gca->ninputs ; i++)
		printf("%2.1f ", gray_means[i]) ;
	printf("+- %2.1f, white ", sqrt(gray_var)) ;
	for (i = 0 ; i < gca->ninputs ; i++)
		printf("%2.1f ", wm_means[i]) ;
	printf("+- %2.1f\n", sqrt(wm_var)) ;

  if (mri_src != mri_dst)
    mri_dst = MRIcopy(mri_src, mri_dst) ;

  mri_tmp = MRIcopy(mri_dst, NULL) ;

  width = mri_src->width ; height = mri_src->height ; depth = mri_src->depth ;

  i = total_changed = 0 ;
  do
  {
    nchanged = 0 ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
            DiagBreak() ; 
          
          label = MRIvox(mri_dst, x, y, z) ;

          if (label == Unknown ||
              label == Left_Cerebral_Cortex ||
              label == Right_Cerebral_Cortex ||
              label == Left_Cerebral_White_Matter ||
              label == Right_Cerebral_White_Matter
              )
          {
						if (label != Unknown)
							left = (label == Left_Cerebral_Cortex || label == Left_Cerebral_White_Matter) ;
						else
						{
							left = -1 ;
							for (zk = -1 ; left < 0 && zk <= 1 ; zk++)
							{
								for (yk = -1 ; left < 0 && yk <= 1 ; yk++)
								{
									for (xk = -1 ; left < 0 && xk <= 1 ; xk++)
									{
										xi = mri_src->xi[x+xk] ;
										yi = mri_src->yi[y+yk] ;
										zi = mri_src->zi[z+zk] ;
										label = MRIvox(mri_dst, xi, yi, zi) ;
										if (label == Left_Cerebral_Cortex || label == Left_Cerebral_White_Matter)
											left = 1 ;
										else if (label == Right_Cerebral_Cortex || label == Right_Cerebral_White_Matter)
											left = 0 ;
									}
								}
							}
						}

            gray_nbr = wm_nbr = 0 ;
						gc_wm = GCAfindSourceGC(gca, mri_src, transform, x, y, z, 
																		left ? Left_Cerebral_White_Matter : Right_Cerebral_White_Matter) ;
						gc_gm = GCAfindSourceGC(gca, mri_src, transform, x, y, z, 
																		left ? Left_Cerebral_Cortex : Right_Cerebral_Cortex) ;
						if (gc_gm)
							gray_nbr = left ? Left_Cerebral_Cortex : Right_Cerebral_Cortex ;
						if (gc_wm)
							wm_nbr = left ? Left_Cerebral_White_Matter : Right_Cerebral_White_Matter ;
            if (gc_gm == NULL || gc_wm == NULL) for (zk = -1 ; zk <= 1 ; zk++)
            {
              for (yk = -1 ; yk <= 1 ; yk++)
              {
                for (xk = -1 ; xk <= 1 ; xk++)
                {
                  if (fabs(xk) + fabs(yk) + fabs(zk) > 1)
                    continue ;
                  xi = mri_src->xi[x+xk] ;
                  yi = mri_src->yi[y+yk] ;
                  zi = mri_src->zi[z+zk] ;
                  label = MRIvox(mri_dst, xi, yi, zi) ;
                  if ((label ==  Left_Cerebral_Cortex ||
											 label ==  Right_Cerebral_Cortex) && (gc_gm == NULL))
									{
										gray_nbr = label ;
                    gc_gm = GCAfindSourceGC(gca, mri_src, transform, xi, yi, zi, label) ;
										if (!gc_gm)
											gray_nbr = 0 ;  /* shouldn't ever happen */
									}
                  else
                  if ((label ==  Left_Cerebral_White_Matter ||
											 label ==  Right_Cerebral_White_Matter) && (gc_wm == NULL))
									{
                    wm_nbr = label ;
                    gc_wm = GCAfindSourceGC(gca, mri_src, transform, xi, yi, zi, label) ;
										if (!gc_wm)
											wm_nbr = 0 ;  /* shouldn't ever happen */
									}
                }
              }
            }
            if (!wm_nbr && !gray_nbr)
              continue ;

						load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
            if (wm_nbr)
              wdist = sqrt(GCAmahDistIdentityCovariance(gc_wm, vals, gca->ninputs)) ;
            else
              wdist = 1e10 ;
            if (gray_nbr)
              gdist = sqrt(GCAmahDistIdentityCovariance(gc_gm, vals, gca->ninputs)) ;
            else
              gdist = 1e10 ;

            if (gc_wm == NULL || sqrt(GCAmahDist(gc_wm, vals, gca->ninputs)) > 1.5)
              wdist = 1e10 ; /* hack - don't label unlikely white */
            if (gc_gm == NULL || sqrt(GCAmahDist(gc_gm, vals, gca->ninputs)) > 1.5)
              gdist = 1e10 ;  /* hack - don't label unlikely gray */
            
            GCAsourceVoxelToNode(gca, mri_dst, transform, x, y, z,  &xn, &yn, &zn) ;
            gcan = &gca->nodes[xn][yn][zn] ;
						gc_label = GCAfindGC(gca, xn, yn, zn, label) ;
						if (gc_label)
							ldist = sqrt(GCAmahDistIdentityCovariance(gc_label, vals, gca->ninputs)) ;
						else
							ldist = 1e10 ;
            ldist *= .75 ;  /* bias towards retaining label */

            if ((wdist < gdist) && gc_wm)   /* might change to wm */
            {
              if (wdist < ldist && GCAisPossible(gca, mri_inputs, wm_nbr, transform, x, y, z))
              {
                if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
                {
                  int olabel = MRIvox(mri_tmp, x, y, z) ;
                  if (olabel != wm_nbr)
                    printf("voxel (%d, %d, %d) changed from %s (%d) "
                           "to %s (%d)\n", x, y, z,
                           cma_label_to_name(olabel), olabel,
                           cma_label_to_name(wm_nbr), wm_nbr);
                }
                nchanged++ ;
                MRIvox(mri_tmp, x, y, z) = wm_nbr ;
              }
            }
            else if (gc_gm)            /* might change to gm */
            {
              if (gdist < ldist && GCAisPossible(gca, mri_inputs, gray_nbr, transform, x, y, z))
              {
								int olabel = MRIvox(mri_tmp, x, y, z) ;
                if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
                {
                  if (olabel != gray_nbr)
                    printf("voxel (%d, %d, %d) changed from %s (%d) "
                           "to %s (%d)\n", x, y, z,
                           cma_label_to_name(olabel), olabel,
                           cma_label_to_name(gray_nbr), gray_nbr);
                }
								if (olabel != gray_nbr)
									nchanged++ ;

                MRIvox(mri_tmp, x, y, z) = gray_nbr ;
              }
            }
          }
        }
      }
    }
    MRIcopy(mri_tmp, mri_dst) ;
    total_changed += nchanged ;
    if (++i > MAX_CORTICAL_ITERATIONS)
      break ;
  } while (nchanged > 0) ;

  MRIfree(&mri_tmp) ;
  printf("%d labels changed to cortex...\n", total_changed) ;
  return(mri_dst) ;
}
MRI   *
GCAexpandLabelIntoWM(GCA *gca, MRI *mri_inputs, MRI *mri_src,
               MRI *mri_dst, TRANSFORM *transform, MRI *mri_fixed, int target_label)
{
  int      nchanged, x, y, z, width, height, depth, xn, yn, zn, xi, yi, zi,
           xk, yk, zk, nbr_label, n, label, total_changed, i ;
  GCA_NODE *gcan, *gcan_nbr ;
	GC1D     *gc_label, *gc_wm ;
  MRI      *mri_tmp ;
	float    vals[MAX_GCA_INPUTS] ;
	double   prior ;

  if (mri_src != mri_dst)
    mri_dst = MRIcopy(mri_src, mri_dst) ;

  mri_tmp = MRIcopy(mri_dst, NULL) ;

  width = mri_src->width ; height = mri_src->height ; depth = mri_src->depth ;

  i = total_changed = 0 ;
  do
  {
    nchanged = 0 ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (x == 140 && y == 111 && z == 139)
            DiagBreak() ;  /* should be wm */
          if (x == 138 && y == 103 && z == 139)
            DiagBreak() ;  /* should be pallidum */
          
          label = MRIvox(mri_dst, x, y, z) ;
          GCAsourceVoxelToNode(gca, mri_dst, transform, x, y, z, &xn, &yn, &zn) ;
          gcan = &gca->nodes[xn][yn][zn] ;
          
          if (label == target_label)
          {
						gc_label = GCAfindGC(gca, xn, yn, zn, label) ;
						gc_wm = NULL ;
            for (n = 0 ; n < gcan->nlabels ; n++)
            {
							if
                ((gcan->labels[n] == Left_Cerebral_White_Matter) ||
                 (gcan->labels[n] == Right_Cerebral_White_Matter))
								gc_wm = GCAfindGC(gca, xn, yn, zn, gcan->labels[n]) ;
            }
            
            for (zk = -1 ; zk <= 1 ; zk++)
            {
              for (yk = -1 ; yk <= 1 ; yk++)
              {
                for (xk = -1 ; xk <= 1 ; xk++)
                {
                  if (fabs(xk) + fabs(yk) + fabs(zk) > 1)
                    continue ;
                  xi = mri_src->xi[x+xk] ; yi = mri_src->yi[y+yk] ; zi = mri_src->zi[z+zk] ;
									if (xi == Ggca_x && yi == Ggca_y && zi == Ggca_z)
										DiagBreak() ;
                  nbr_label = MRIvox(mri_dst, xi, yi, zi) ;
                  if ((nbr_label == Right_Cerebral_White_Matter) ||
                      (nbr_label == Left_Cerebral_White_Matter))
                  {
										load_vals(mri_inputs, xi, yi, zi, vals, gca->ninputs) ;
										GCAsourceVoxelToNode(gca, mri_dst, transform, xi, yi, zi, &xn, &yn, &zn) ;
										gc_wm = GCAfindGC(gca, xn, yn, zn, nbr_label) ;
										gc_label = GCAfindGC(gca, xn, yn, zn, target_label) ;
										if (!gc_wm || !gc_label)
											continue ;
										if (GCAmahDistIdentityCovariance(gc_wm, vals, gca->ninputs) > 
												GCAmahDistIdentityCovariance(gc_label, vals, gca->ninputs))
                    {
                      gcan_nbr = &gca->nodes[xn][yn][zn] ;
                      for (prior = 0.0f, n = 0 ; n < gcan_nbr->nlabels ; n++)
                      {
                        if (gcan_nbr->labels[n] == target_label)
                        {
                          prior = get_node_prior(gca, target_label, xn, yn, zn) ;
                          break ;
                        }
                      }
#define PRIOR_THRESH 0.01
                      if (prior >= PRIOR_THRESH)  /* target is possible */
                      {
                        nchanged++ ;
                        MRIvox(mri_tmp, xi, yi, zi) = target_label ;
                        /*                        MRIvox(mri_fixed, xi, yi, zi) = 0 ;*/
                      }
                    }
                  }
                }
              }
            }
          } 
        }
      }
    }
    MRIcopy(mri_tmp, mri_dst) ;
    total_changed += nchanged ;
    if (++i >= 1)
      break ;
  } while (nchanged > 0) ;

  MRIfree(&mri_tmp) ;
  printf("%d labels changed to %s...\n", total_changed, cma_label_to_name(target_label)) ;
  return(mri_dst) ;
}


int
GCArankSamples(GCA *gca, GCA_SAMPLE *gcas, int nsamples, int *ordered_indices)
{
  LABEL_PROB  *label_probs ;
  int         i ;

  label_probs = (LABEL_PROB *)calloc(nsamples, sizeof(LABEL_PROB)) ;
  for (i = 0 ; i < nsamples ; i++)
  {
    label_probs[i].label = i ; label_probs[i].prob =  gcas[i].log_p ;
  }

  /* now sort the samples by probability */
  qsort(label_probs, nsamples, sizeof(LABEL_PROB), 
              compare_sort_probabilities) ;

  for (i = 0 ; i < nsamples ; i++)
  {
    ordered_indices[i] = label_probs[nsamples-(i+1)].label ;
  }

  free(label_probs) ;
  return(NO_ERROR) ;
}

#include "mrinorm.h"
MRI *
GCAnormalizeSamples(MRI *mri_in, GCA *gca, GCA_SAMPLE *gcas, int nsamples, 
                    TRANSFORM *transform, char *ctl_point_fname)
{
  MRI    *mri_dst, *mri_ctrl, *mri_bias ;
  int    xv, yv, zv, n, x, y, z, width, height, depth, xn, yn, zn, num, total, input,
         T1_index = 0 ;
  float   bias ;
  double  mean, sigma ;
	Real    val ;
 
	if (nsamples == 0)   /* only using control points from file */
	{
	  float      wm_means[MAX_GCA_INPUTS], tmp[MAX_GCA_INPUTS], max_wm ;
		int        r ;

	  GCAlabelMean(gca, Left_Cerebral_White_Matter, wm_means) ;
  	GCAlabelMean(gca, Left_Cerebral_White_Matter, tmp) ;
		max_wm = 0 ;
		for (r = 0 ; r < gca->ninputs ; r++)
		{
			wm_means[r] = (wm_means[r] + tmp[r]) / 2 ;
			if (wm_means[r] > max_wm)
			{
				T1_index = r ; max_wm = wm_means[r] ;
			}
		}
		printf("using volume %d as most T1-weighted for normalization\n",T1_index) ;
	}
  width = mri_in->width ; height = mri_in->height ; depth = mri_in->depth ;
  mri_dst = MRIclone(mri_in, NULL) ;
  mri_ctrl = MRIalloc(width, height, depth, MRI_UCHAR) ;
  mri_bias = MRIalloc(mri_in->width,mri_in->height,mri_in->depth,MRI_SHORT);
  if (!mri_bias)    
	ErrorExit(ERROR_NOMEMORY, 
            "GCAnormalizeSamples: could not allocate (%d,%d,%d,2) bias image",
             mri_in->width,mri_in->height,mri_in->depth) ;
              

#define MAX_BIAS 1250
#define NO_BIAS  1000
#define MIN_BIAS  750

  if (ctl_point_fname)
  {
    MRI3dUseFileControlPoints(mri_ctrl, ctl_point_fname) ;
    MRInormAddFileControlPoints(mri_ctrl, CONTROL_MARKED) ;
  }

  /* add control points from file */
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        MRISvox(mri_bias, x,y,z) = NO_BIAS ;  /* by default */
        if (MRIvox(mri_ctrl, x, y, z) != CONTROL_MARKED)  /* not read from file */
					continue ;

				if (nsamples == 0)   /* only using file control points */
				{
					MRIsampleVolumeFrame(mri_in, x, y, z, T1_index, &val) ;
					bias = NO_BIAS*DEFAULT_DESIRED_WHITE_MATTER_VALUE / val ;          
          MRISvox(mri_bias, x, y, z) = (short)nint(bias) ;
				}
				else    /* find atlas point this maps to */
        {
          int       n, max_n ;
          GC1D      *gc ;
          GCA_NODE  *gcan ;
          GCA_PRIOR *gcap ;
          double    max_p ;

          GCAsourceVoxelToNode(gca, mri_dst, transform,  x, y, z, &xn, &yn, &zn) ;
          gcan = &gca->nodes[xn][yn][zn] ;
          gcap = getGCAP(gca, mri_dst, transform, x, y, z) ;
          max_p = 0 ;
          for (max_n = -1, n = 0 ; n < gcan->nlabels ; n++)
          {
            if ((0 == IS_WM(gcan->labels[n])) &&
                (0 == IS_CEREBELLAR_WM(gcan->labels[n])) &&
								(gcan->labels[n] != Brain_Stem))
              continue ;
            gc = &gcan->gcs[n] ;
            if (getPrior(gcap, gcan->labels[n]) >= max_p)
            {
              max_p = getPrior(gcap, gcan->labels[n]) ;
              max_n = n ;
            }
          }
          if (max_n < 0)  /* couldn't find any valid label at this location */
            continue ;
          gc = &gcan->gcs[max_n] ;

					for (bias = 0.0, input = 0 ; input < gca->ninputs ; input++)
					{	
						MRIsampleVolumeFrame(mri_in, x, y, z, input, &val) ;
						if (FZERO(val))
							val = 1 ;
	          bias += (float)NO_BIAS*((float)gc->means[input]/val) ;
					}
					bias /= (float)gca->ninputs ;
          if (bias < 100 || bias > 5000)
            DiagBreak() ;
          if (bias < MIN_BIAS)
            bias = MIN_BIAS ;
          if (bias > MAX_BIAS)
            bias = MAX_BIAS ;
          
          MRISvox(mri_bias, x, y, z) = (short)nint(bias) ;
        }
      }
    }
  }

  TransformInvert(transform, mri_in) ;
  for (n = 0 ; n < nsamples ; n++)
  {
    if (gcas[n].xp == Ggca_x && gcas[n].yp == Ggca_y && gcas[n].zp == Ggca_z)
      DiagBreak() ;

    GCApriorToSourceVoxel(gca, mri_dst, transform, 
                          gcas[n].xp, gcas[n].yp, gcas[n].zp, &xv, &yv, &zv) ;

    if (xv == 181 && yv == 146 && zv == 128)
      DiagBreak() ;
    if (xv == Ggca_x && yv == Ggca_y && zv == Ggca_z)
      DiagBreak() ;
    if (gcas[n].label == 29 || gcas[n].label == 61)
    {
      gcas[n].label = 0 ;
      DiagBreak() ;
    }
    if (gcas[n].label > 0)
    {
      MRIvox(mri_ctrl, xv, yv, zv) = CONTROL_MARKED ;
	
			for (bias = 0.0, input = 0 ; input < gca->ninputs ; input++)
			{	
				MRIsampleVolumeFrame(mri_in, xv, yv, zv, input, &val) ;
				if (FZERO(val))
					val = 1 ;
        bias += (float)NO_BIAS*((float)gcas[n].means[input]/val) ;
			}
			bias /= (float)gca->ninputs ;
      if (bias < 100 || bias > 5000)
        DiagBreak() ;
#if 0
      if (bias < MIN_BIAS)
        bias = MIN_BIAS ;
      if (bias > MAX_BIAS)
        bias = MAX_BIAS ;
#endif

      MRISvox(mri_bias, xv, yv, zv) = (short)nint(bias) ;
    }
    else
      MRIvox(mri_ctrl, xv, yv, zv) = CONTROL_NONE ;
  }

  /* now check for and remove outliers */
  mean = sigma = 0.0 ;
  for (num = z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        if (MRIvox(mri_ctrl, x, y, z) == CONTROL_MARKED)  
        {
          num++ ;
          bias = (double)MRISvox(mri_bias, x, y, z) ;
          mean += bias ; sigma += (bias*bias) ;
        }
      }
    }
  }

  if (num > 0)
  {
    mean /= (double)num ;
    sigma  = sqrt(sigma / (double)num - mean*mean) ;
    printf("bias field = %2.3f +- %2.3f\n", mean/NO_BIAS, sigma/NO_BIAS) ;
  }

  /* now check for and remove outliers */
  for (total = num = z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        if (MRIvox(mri_ctrl, x, y, z) == CONTROL_MARKED)  
        {
          bias = (double)MRISvox(mri_bias, x, y, z) ;
          total++ ;
          if (fabs(bias-mean) > 4*sigma)
          {
            MRIvox(mri_ctrl, x, y, z) = CONTROL_NONE ;  
            num++ ;
            MRISvox(mri_bias, x, y, z) = NO_BIAS ;
          }
        }
      }
    }
  }

  printf("%d of %d control points discarded\n", num, total) ;

  MRIbuildVoronoiDiagram(mri_bias, mri_ctrl, mri_bias) ;
  /*  MRIwrite(mri_bias, "bias.mgh") ;*/
#if 1
  {
    MRI *mri_kernel, *mri_smooth, *mri_down ;
		float sigma = 16.0f ;

		mri_down = MRIdownsample2(mri_bias, NULL) ;
    mri_kernel = MRIgaussian1d(sigma, 100) ;
    mri_smooth = MRIconvolveGaussian(mri_down, NULL, mri_kernel) ;
		MRIfree(&mri_bias) ; MRIfree(&mri_kernel) ;
		mri_bias = MRIupsample2(mri_smooth, NULL) ;
		sigma = 2.0f ; MRIfree(&mri_down) ; MRIfree(&mri_smooth) ;
    mri_kernel = MRIgaussian1d(sigma, 100) ;
    mri_smooth = MRIconvolveGaussian(mri_bias, NULL, mri_kernel) ;
    MRIfree(&mri_bias) ; mri_bias = mri_smooth ; MRIfree(&mri_kernel) ;
  }
#else
  MRIsoapBubble(mri_bias, mri_ctrl, mri_bias, 10) ;
#endif
  /*  MRIwrite(mri_bias, "smooth_bias.mgh") ;*/


  width = mri_in->width ; height = mri_in->height ; depth = mri_in->depth ;
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        bias = (float)MRISvox(mri_bias, x, y, z)/NO_BIAS ;
        if (bias < 0)
          DiagBreak() ;
				for (input = 0 ; input < gca->ninputs ; input++)
				{
					MRIsampleVolumeFrame(mri_in, x, y, z, input, &val) ;
					val *= bias ;   /* corrected value */
					switch (mri_in->type)
					{
					case MRI_UCHAR: 
	  	      if (val < 0)
  	  	      val = 0 ;
        		else if (val > 255)
		          val = 255 ;
						MRIseq_vox(mri_dst, x, y, z, input) = (BUFTYPE)nint(val) ; 
						break ;
					case MRI_SHORT: 
						MRISseq_vox(mri_dst, x, y, z, input) = (short)nint(val) ; 
						break ;
					case MRI_FLOAT: 
						MRIFseq_vox(mri_dst, x, y, z, input) = val ; 
						break ;
					default:
						ErrorReturn(NULL,
												(ERROR_UNSUPPORTED, 
												"GCAnormalizeSamples: unsupported input type %d",mri_in->type));
						break ;
					}
				}
      }
    }
  }

  MRIfree(&mri_bias) ; MRIfree(&mri_ctrl) ;
  return(mri_dst) ;
}
MRI *
GCAnormalizeSamplesT1PD(MRI *mri_in, GCA *gca, GCA_SAMPLE *gcas, int nsamples, 
												TRANSFORM *transform, char *ctl_point_fname)
{
  MRI    *mri_dst, *mri_ctrl, *mri_bias ;
  int    xv, yv, zv, n, x, y, z, width, height, depth, out_val, xn, yn, zn, num, total ;
	Real   val ;
  float   bias ;
  double  mean, sigma ;

  mri_dst = MRIclone(mri_in, NULL) ;
  mri_ctrl = MRIalloc(mri_in->width,mri_in->height,mri_in->depth,MRI_UCHAR);
  mri_bias = MRIalloc(mri_in->width,mri_in->height,mri_in->depth,MRI_SHORT);
  if (!mri_bias)    ErrorExit(ERROR_NOMEMORY, 
              "GCAnormalize: could not allocate (%d,%d,%d,2) bias image",
              mri_in->width,mri_in->height,mri_in->depth) ;
              

#define MAX_BIAS 1250
#define NO_BIAS  1000
#define MIN_BIAS  750

  if (ctl_point_fname)
  {
    MRI3dUseFileControlPoints(mri_ctrl, ctl_point_fname) ;
    MRInormAddFileControlPoints(mri_ctrl, CONTROL_MARKED) ;
  }
  width = mri_in->width ; height = mri_in->height ; depth = mri_in->depth ;

  /* add control points from file */
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        MRISvox(mri_bias, x,y,z) = NO_BIAS ;  /* by default */
        if (MRIvox(mri_ctrl, x, y, z) == CONTROL_MARKED)  /* read from file */
        {
          int       n, max_n ;
          GC1D      *gc ;
          GCA_NODE  *gcan ;
          GCA_PRIOR *gcap ;
          double    max_p ;

          GCAsourceVoxelToNode(gca, mri_dst, transform,  x, y, z, &xn, &yn, &zn) ;
          gcan = &gca->nodes[xn][yn][zn] ;
          gcap = getGCAP(gca, mri_dst, transform, x, y, z) ;
          max_p = 0 ;
          for (max_n = -1, n = 0 ; n < gcan->nlabels ; n++)
          {
            if ((0 == IS_WM(gcan->labels[n])) &&
                (0 == IS_CEREBELLAR_WM(gcan->labels[n])))
              continue ;
            gc = &gcan->gcs[n] ;
            if (getPrior(gcap, gcan->labels[n]) >= max_p)
            {
              max_p = getPrior(gcap, gcan->labels[n]) ;
              max_n = n ;
            }
          }
          if (max_n < 0)  /* couldn't find any valid label at this location */
            continue ;
          gc = &gcan->gcs[max_n] ;

					MRIsampleVolumeFrameType(mri_in, x, y, z, 1, SAMPLE_NEAREST, &val) ;
					if (FZERO(val))
						val = 1 ;
          bias = (float)NO_BIAS*((float)gc->means[1]/val) ;
          if (bias < 100 || bias > 5000)
            DiagBreak() ;
          if (bias < MIN_BIAS)
            bias = MIN_BIAS ;
          if (bias > MAX_BIAS)
            bias = MAX_BIAS ;
          
          MRISvox(mri_bias, x, y, z) = (short)nint(bias) ;
        }
      }
    }
  }
  

  TransformInvert(transform, mri_in) ;
  for (n = 0 ; n < nsamples ; n++)
  {
    if (gcas[n].xp == Ggca_x && gcas[n].yp == Ggca_y && gcas[n].zp == Ggca_z)
      DiagBreak() ;

    GCApriorToSourceVoxel(gca, mri_dst, transform, 
                          gcas[n].xp, gcas[n].yp, gcas[n].zp, &xv, &yv, &zv) ;

    if (xv == 181 && yv == 146 && zv == 128)
      DiagBreak() ;
    if (xv == Ggca_x && yv == Ggca_y && zv == Ggca_z)
      DiagBreak() ;
    if (gcas[n].label == 29 || gcas[n].label == 61)
    {
      gcas[n].label = 0 ;
      DiagBreak() ;
    }
    if (gcas[n].label > 0)
    {
      MRIvox(mri_ctrl, xv, yv, zv) = CONTROL_MARKED ;
			MRIsampleVolumeFrameType(mri_in, xv, yv, zv, 1, SAMPLE_NEAREST, &val) ;
			if (FZERO(val))
				val = 1 ;
      bias = (float)NO_BIAS*((float)gcas[n].means[1]/val) ;
      if (bias < 100 || bias > 5000)
        DiagBreak() ;
#if 0
      if (bias < MIN_BIAS)
        bias = MIN_BIAS ;
      if (bias > MAX_BIAS)
        bias = MAX_BIAS ;
#endif

      MRISvox(mri_bias, xv, yv, zv) = (short)nint(bias) ;
    }
    else
      MRIvox(mri_ctrl, xv, yv, zv) = CONTROL_NONE ;
  }

  /* now check for and remove outliers */
  mean = sigma = 0.0 ;
  for (num = z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        if (MRIvox(mri_ctrl, x, y, z) == CONTROL_MARKED)  
        {
          num++ ;
          bias = (double)MRISvox(mri_bias, x, y, z) ;
          mean += bias ; sigma += (bias*bias) ;
        }
      }
    }
  }

  if (num > 0)
  {
    mean /= (double)num ;
    sigma  = sqrt(sigma / (double)num - mean*mean) ;
    printf("bias field = %2.3f +- %2.3f\n", mean/NO_BIAS, sigma/NO_BIAS) ;
  }

  /* now check for and remove outliers */
  for (total = num = z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        if (MRIvox(mri_ctrl, x, y, z) == CONTROL_MARKED)  
        {
          bias = (double)MRISvox(mri_bias, x, y, z) ;
          total++ ;
          if (fabs(bias-mean) > 4*sigma)
          {
            MRIvox(mri_ctrl, x, y, z) = CONTROL_NONE ;  
            num++ ;
            MRISvox(mri_bias, x, y, z) = NO_BIAS ;
          }
        }
      }
    }
  }

  printf("%d of %d control points discarded\n", num, total) ;

  MRIbuildVoronoiDiagram(mri_bias, mri_ctrl, mri_bias) ;
  /*  MRIwrite(mri_bias, "bias.mgh") ;*/
#if 0
  {
    MRI *mri_kernel, *mri_smooth ;

    mri_kernel = MRIgaussian1d(2.0f, 100) ;
    mri_smooth = MRIconvolveGaussian(mri_bias, NULL, mri_kernel) ;
    MRIfree(&mri_bias) ; mri_bias = mri_smooth ; MRIfree(&mri_kernel) ;
  }
#else
  MRIsoapBubble(mri_bias, mri_ctrl, mri_bias, 10) ;
#endif
  /*  MRIwrite(mri_bias, "smooth_bias.mgh") ;*/


  width = mri_in->width ; height = mri_in->height ; depth = mri_in->depth ;
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        bias = (float)MRISvox(mri_bias, x, y, z)/NO_BIAS ;
        if (bias < 0)
          DiagBreak() ;
				MRIsampleVolumeFrameType(mri_in, x, y, z, 1, SAMPLE_NEAREST, &val) ;
        out_val = nint((float)val*bias) ;
        MRISseq_vox(mri_dst, x, y, z, 1) = (short)nint(out_val) ;
      }
    }
  }

	MRIcopyFrame(mri_in, mri_dst, 0, 0) ;  /* copy over T1 frame */
  MRIfree(&mri_bias) ; MRIfree(&mri_ctrl) ;
  return(mri_dst) ;
}
int
GCAnodeToSourceVoxelFloat(GCA *gca, MRI *mri, TRANSFORM *transform, int xn, int yn, int zn, 
                    float *pxv, float *pyv, float *pzv)
{
  int   width, height, depth, xt, yt, zt ;
  float  xv, yv, zv ;

  width = mri->width ; height = mri->height ;  depth = mri->depth ;
  GCAnodeToVoxel(gca, mri, xn, yn, zn, &xt, &yt, &zt) ;
  TransformSampleInverse(transform, xt*mri->xsize, yt*mri->ysize, zt*mri->zsize, &xv, &yv, &zv) ;

  *pxv = xv ; *pyv = yv ; *pzv = zv ;
  return(NO_ERROR) ;
}

int
GCAnodeToSourceVoxel(GCA *gca, MRI *mri, TRANSFORM *transform, int xn, int yn, int zn, 
                    int *pxv, int *pyv, int *pzv)
{
  float  xf, yf, zf ;

  GCAnodeToSourceVoxelFloat(gca, mri, transform, xn, yn, zn, &xf, &yf, &zf) ;
  if (xf < 0) xf = 0 ;
  if (yf < 0) yf = 0 ;
  if (zf < 0) zf = 0 ;
  if (xf >= mri->width-1)  xf = mri->width-1 ;
  if (yf >= mri->height-1) yf = mri->height-1 ;
  if (zf >= mri->depth-1)  zf = mri->depth-1 ;
  *pxv = nint(xf) ; *pyv = nint(yf) ; *pzv = nint(zf) ;
  return(NO_ERROR) ;
}

int
GCApriorToSourceVoxelFloat(GCA *gca, MRI *mri, TRANSFORM *transform, int xp, int yp, int zp, 
                    float *pxv, float *pyv, float *pzv)
{
  int   width, height, depth, xt, yt, zt ;
  float  xv, yv, zv ;

  width = mri->width ; height = mri->height ;  depth = mri->depth ;
  GCApriorToVoxel(gca, mri, xp, yp, zp, &xt, &yt, &zt) ;
  TransformSampleInverse(transform, xt*mri->xsize, yt*mri->ysize, zt*mri->zsize, &xv, &yv, &zv) ;

  *pxv = xv ; *pyv = yv ; *pzv = zv ;
  return(NO_ERROR) ;
}

int
GCApriorToSourceVoxel(GCA *gca, MRI *mri, TRANSFORM *transform, int xp, int yp, int zp, 
                    int *pxv, int *pyv, int *pzv)
{
  float  xf, yf, zf ;

  GCApriorToSourceVoxelFloat(gca, mri, transform, xp, yp, zp, &xf, &yf, &zf) ;
  if (xf < 0) xf = 0 ;
  if (yf < 0) yf = 0 ;
  if (zf < 0) zf = 0 ;
  if (xf >= mri->width-1)  xf = mri->width-1 ;
  if (yf >= mri->height-1) yf = mri->height-1 ;
  if (zf >= mri->depth-1)  zf = mri->depth-1 ;
  *pxv = nint(xf) ; *pyv = nint(yf) ; *pzv = nint(zf) ;
  return(NO_ERROR) ;
}

float 
GCAlabelProbability(MRI *mri_src, GCA *gca, TRANSFORM *transform,
                          int x, int y, int z, int label)
{
  int      xn, yn, zn ;
  GCA_NODE *gcan ;
	GC1D     *gc ;
  float    plabel, vals[MAX_GCA_INPUTS] ;

  if (x == Gx && y == Gy && z == Gz)
    DiagBreak() ; 

  GCAsourceVoxelToNode(gca, mri_src, transform, x, y, z, &xn, &yn, &zn) ;
  gcan = &gca->nodes[xn][yn][zn] ;

	load_vals(mri_src, x, y, z, vals, gca->ninputs) ;
	gc = GCAfindGC(gca, xn, yn, zn, label) ;
	if (gc == NULL)
		plabel = 0.0 ;
	else
		plabel = GCAcomputeConditionalDensity(gc, vals, gca->ninputs, label) ;
  return(plabel) ;
}

static GCA_NODE *
findSourceGCAN(GCA *gca, MRI *mri_src, TRANSFORM *transform, int x, int y, int z)
{
  int      xn, yn, zn ;
  GCA_NODE *gcan ;

  GCAsourceVoxelToNode(gca, mri_src, transform, x, y, z, &xn, &yn, &zn) ;
  gcan = &gca->nodes[xn][yn][zn] ;
  return(gcan) ;
}
#if 0
static int
getLabelMean(GCA_NODE *gcan, int label, float *pvar, float *means, int ninputs)
{
  int   n, r ;
  float mean = -1.0f ;

  if (*pvar)
    *pvar = 0.0f ;
  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    if (gcan->labels[n] == label)
    {
			for (r = 0 ; r < ninputs ; r++)
				mean = gcan->gcs[n].means[r] ;
      if (pvar)
        *pvar = covariance_determinant(&gcan->gcs[n], ninputs) ; ;
      break ;
    }
  }
  return(NO_ERROR) ;
}
#endif
MRI   *
GCAmaxLikelihoodBorders(GCA *gca, MRI *mri_inputs, MRI *mri_src,
                        MRI *mri_dst, TRANSFORM *transform, int max_iter, float min_ratio)
{
  int      nchanged, x, y, z, width, height, depth, label, total_changed, i ;
  MRI      *mri_tmp ;

  if (mri_src != mri_dst)
    mri_dst = MRIcopy(mri_src, mri_dst) ;

  mri_tmp = MRIcopy(mri_dst, NULL) ;

  width = mri_src->width ; height = mri_src->height ; depth = mri_src->depth ;

  for (total_changed = i = 0 ; i < max_iter ; i++)
  {
    nchanged = 0 ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
						DiagBreak() ;
          if (x == 99 && y == 129 && z == 127)
            DiagBreak() ;  /* gray should be wm */
          if (x == 98 && y == 124 && z == 127)
            DiagBreak() ;  /* wm should be hippo */

          if (borderVoxel(mri_dst,x,y,z))
          {
            label = GCAmaxLikelihoodBorderLabel(gca, mri_inputs, mri_dst,transform,
                                                x, y, z, min_ratio) ;
            if (x == Ggca_x && y == Ggca_y && z == Ggca_z && 
                (label == Ggca_label || MRIvox(mri_tmp,x,y,z) == Ggca_label || 
                 Ggca_label < 0))
            {
              DiagBreak() ;
              
              if (label != MRIvox(mri_dst, x, y, z))
                printf(
                       "MLE (%d, %d, %d): old label %s (%d), new label %s (%d)\n",
                       x, y, z, cma_label_to_name(MRIvox(mri_tmp,x,y,z)),
                       MRIvox(mri_tmp,x,y,z), cma_label_to_name(label),
                       label) ;
            }
            if (label != MRIvox(mri_dst, x, y, z))
            {
              nchanged++ ;
              MRIvox(mri_tmp, x, y, z) = label ;
            }
          }
        }
      }
    }
    MRIcopy(mri_tmp, mri_dst) ;
    total_changed += nchanged ;
    if (!nchanged)
      break ;
  } 

  MRIfree(&mri_tmp) ;
  printf("%d border labels changed to MLE ...\n", total_changed) ;
  return(mri_dst) ;
}
static int
borderVoxel(MRI *mri, int x, int y, int z)
{
  int   xi, yi, zi, xk, yk, zk, label ;

  label = MRIvox(mri, x, y, z) ;

  for (xk = -1 ; xk <= 1 ; xk++)
  {
    xi = mri->xi[x+xk] ;
    for (yk = -1 ; yk <= 1 ; yk++)
    {
      for (zk = -1 ; zk <= 1 ; zk++)
      {
        if (abs(xk)+abs(yk)+abs(zk) != 1)
          continue ;
        yi = mri->yi[y+yk] ;
        zi = mri->zi[z+zk] ;
        if (MRIvox(mri, xi, yi, zi) != label)
          return(1) ;
      }
    }
  }
  return(0) ;
}

static int
GCAmaxLikelihoodBorderLabel(GCA *gca, MRI *mri_inputs, MRI *mri_labels, 
                       TRANSFORM *transform, int x, int y, int z, float min_ratio)
{
  float    p, max_p, vals[MAX_GCA_INPUTS] ;
  int      label, i, xi, yi, zi, best_label, orig_label, n ;
  GCA_NODE *gcan ;
	GC1D     *gc ;

  if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
    DiagBreak() ;  

	load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
  gcan = findSourceGCAN(gca, mri_inputs, transform, x, y, z) ;
  orig_label = best_label = MRIvox(mri_labels, x, y, z) ;
  gc = NULL ;
	for (n = 0 ; n < gcan->nlabels ; n++)
		if (gcan->labels[n] == best_label)
		{
			gc  = &gcan->gcs[n] ;
			break ;
		}

	if (gc == NULL)
	{
		ErrorPrintf(ERROR_BADPARM, "GCAmaxLikelihoodBorderLabel(%d, %d, %d): couldn't find gc for label %d", 
							x, y, z, best_label) ;
		max_p = 0.0 ;
	}
	else
		max_p = GCAcomputeConditionalDensity(gc, vals, gca->ninputs, best_label) ;
  for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
  {
    xi = mri_inputs->xi[x+xnbr_offset[i]] ;
    yi = mri_inputs->yi[y+ynbr_offset[i]] ;
    zi = mri_inputs->zi[z+znbr_offset[i]] ;
    gcan = findSourceGCAN(gca, mri_inputs, transform, xi, yi, zi) ;
    label = MRIvox(mri_labels, xi, yi, zi) ;
		gc = NULL ;
		for (n = 0 ; n < gcan->nlabels ; n++)
			if (gcan->labels[n] == label)
			{
				gc  = &gcan->gcs[n] ;
				break ;
			}
		if (gc == NULL)
			continue ;  /* label can't occur here */

    MRIvox(mri_labels, x, y, z) = label ;
    if (gcaGibbsImpossibleConfiguration(gca, mri_labels, x, y, z, transform))
      continue ;
    MRIvox(mri_labels, x, y, z) = orig_label ;

    p = GCAcomputeConditionalDensity(gc, vals, gca->ninputs, label) ;
    if ((best_label == orig_label && p > min_ratio*max_p) ||
        (best_label != orig_label && p > max_p))
    {
      max_p = p ;
      best_label = label ;
    }
  }

  /* test to make sure that it is not an impossible Gibbs configuration */
  if (best_label != MRIvox(mri_labels, x, y, z))
  {
    label = MRIvox(mri_labels, x, y, z) ;
    MRIvox(mri_labels, x, y, z) = best_label ;  /* test potential new label */
    if (gcaGibbsImpossibleConfiguration(gca, mri_labels, x, y,z, transform))
      best_label = label ;  /* revert back to old label */
    MRIvox(mri_labels, x, y, z) = label ;  /* caller will change it if needed */
  }
  return(best_label) ;
}


static int
gcaComputeLabelStats(GCA *gca, int target_label, float *pvar, float *means)
{
  int      x, y, z, n, r ;
  double   var, dof, total_dof ;
  GC1D     *gc ;
  GCA_NODE *gcan ;

  var = total_dof = 0.0 ;
	memset(means, 0, gca->ninputs*sizeof(float)) ;
  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          if (gcan->labels[n] == target_label)
          {
            gc = &gcan->gcs[n] ;
            dof = get_node_prior(gca, target_label, x, y, z) * gcan->total_training ;
						for (r = 0 ; r < gca->ninputs ; r++)
							means[r] += dof*gc->means[r] ;
            var += dof*covariance_determinant(gc, gca->ninputs) ;
            total_dof += dof ;
          }
        }
      }
    }
  }

  if (total_dof > 0.0)
  {
		for (r = 0 ; r < gca->ninputs ; r++)
			means[r] /= total_dof ;
    var /= total_dof ;
  }
  if (pvar)
    *pvar = var ;
  return(NO_ERROR) ;
}
int
GCAhistogramTissueStatistics(GCA *gca, MRI *mri_T1,MRI *mri_PD,
                              MRI *mri_labeled, TRANSFORM *transform, char *fname)
{
  int              x, y, z, n, label, biggest_label, T1, PD, xp, yp, zp ;
  GCA_NODE         *gcan ;
  GCA_TISSUE_PARMS *gca_tp ;
  VECTOR           *v_parc, *v_T1 ;
  static VECTOR *v_ras_cor = NULL, *v_ras_flash ;
  FILE             *fp ;
  float            xf, yf, zf ;
  
  fp = fopen(fname, "w") ;
  if (!fp)
    ErrorExit(ERROR_NOFILE, "GCAhistogramTissueStatistics: could not open %s",
              fname) ;
  
  v_parc = VectorAlloc(4, MATRIX_REAL) ;
  v_T1 = VectorAlloc(4, MATRIX_REAL) ;
  *MATRIX_RELT(v_parc, 4, 1) = 1.0 ;
  *MATRIX_RELT(v_T1, 4, 1) = 1.0 ;

  /* first build a list of all labels that exist */
  for (biggest_label = x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_height ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gca->tissue_parms[(int)gcan->labels[n]].label = gcan->labels[n] ;
          if (gcan->labels[n] > biggest_label)
            biggest_label = gcan->labels[n] ;
        }
      }
    }
  }

  for (label = 0 ; label <= biggest_label ; label++)
  {
    if (gca->tissue_parms[label].label <= 0)
      continue ;
    gca_tp = &gca->tissue_parms[label] ;
    
    for (z = 0 ; z < mri_T1->depth ; z++)
    {
      V3_Z(v_parc) = z ;
      for (y = 0 ; y < mri_T1->height ; y++)
      {
        V3_Y(v_parc) = y ;
        for (x = 0 ; x < mri_T1->width ; x++)
        {
          if (MRIvox(mri_labeled, x, y, z) != label)
            continue ;
          if (borderVoxel(mri_labeled, x, y, z))
            continue ;
          if (transform)
          {
            MATRIX *m_tmp ;
            V3_X(v_parc) = x ;

            TransformSample(transform, x*mri_T1->xsize, y*mri_T1->ysize, z*mri_T1->zsize, &xf, &yf, &zf) ;
            xp = nint(xf) ; yp = nint(zf) ; zp = nint(zf) ;
            V3_X(v_T1) = xp ; V3_Y(v_T1) = yp ; V3_Z(v_T1) = zp ;
            
            m_tmp = MRIgetVoxelToRasXform(mri_labeled) ;
            v_ras_cor = MatrixMultiply(m_tmp, v_parc, v_ras_cor);
            MatrixFree(&m_tmp) ;
            m_tmp = MRIgetVoxelToRasXform(mri_T1) ;
            v_ras_flash = MatrixMultiply(m_tmp, v_T1, v_ras_flash);
            MatrixFree(&m_tmp) ;
            if (!x && !y && !z && 0)
            {
              MatrixPrint(stdout, v_ras_cor) ;
              MatrixPrint(stdout, v_ras_flash) ;
            }

            if ((xp < 0 || xp >= mri_T1->width) ||
                (yp < 0 || yp >= mri_T1->height) ||
                (zp < 0 || zp >= mri_T1->depth))
              continue ;
          }
          else
          {
            xp = x ; yp = y ; zp = z ;
          }

          T1 = MRISvox(mri_T1, xp, yp, zp) ;
          PD = MRISvox(mri_PD, xp, yp, zp) ;
          fprintf(fp, "%d %d %d\n", label, T1, PD) ;
          gca_tp->total_training++ ;
          gca_tp->T1_mean += T1 ;
          gca_tp->T1_var += T1*T1 ;
          gca_tp->PD_mean += PD ;
          gca_tp->PD_var += PD*PD ;
        }
      }
    }
  }

  fclose(fp) ;
  return(NO_ERROR) ;
}

int
GCAnormalizeTissueStatistics(GCA *gca)
{
  int              n ;
  double           nsamples ;
  GCA_TISSUE_PARMS *gca_tp ;

  for (n = 0 ; n < MAX_GCA_LABELS ; n++)
  {
    gca_tp = &gca->tissue_parms[n] ;
    if (gca_tp->total_training <= 0)
      continue ;
    nsamples = gca_tp->total_training ;
    gca_tp->T1_mean /= nsamples ;
    gca_tp->PD_mean /= nsamples ;
    gca_tp->T2_mean /= nsamples ;
    gca_tp->T1_var = 
      gca_tp->T1_var / nsamples - gca_tp->T1_mean*gca_tp->T1_mean;
    gca_tp->PD_var = 
      gca_tp->PD_var / nsamples - gca_tp->PD_mean*gca_tp->PD_mean;
    printf("%-30.30s: T1=%4d +- %4d, PD=%4d +- %4d \n", 
           cma_label_to_name(n), 
           nint(gca_tp->T1_mean), nint(sqrt(gca_tp->T1_var)),
           nint(gca_tp->PD_mean), nint(sqrt(gca_tp->PD_var))) ;
  }

  return(NO_ERROR) ;
}


char *
cma_label_to_name(int label)
{
  static char name[STRLEN] ;

  if (label == Unknown)
    return("Unknown") ;
  if (label == Left_Cerebral_Exterior)
    return("Left_Cerebral_Exterior") ;
  if (label == Left_Cerebral_White_Matter)
    return("Left_Cerebral_White_Matter") ;
  if (label == Left_Cerebral_Cortex)
    return("Left_Cerebral_Cortex") ;
  if (label == Left_Lateral_Ventricle)
    return("Left_Lateral_Ventricle") ;
  if (label == Left_Inf_Lat_Vent)
    return("Left_Inf_Lat_Vent") ;
  if (label == Left_Cerebellum_Exterior)
    return("Left_Cerebellum_Exterior") ;
  if (label == Left_Cerebellum_White_Matter)
    return("Left_Cerebellum_White_Matter") ;
  if (label == Left_Cerebellum_Cortex)
    return("Left_Cerebellum_Cortex") ;
  if (label == Left_Thalamus)
    return("Left_Thalamus") ;
  if (label == Left_Thalamus_Proper)
    return("Left_Thalamus_Proper") ;
  if (label == Left_Caudate)
    return("Left_Caudate") ;
  if (label == Left_Putamen)
    return("Left_Putamen") ;
  if (label == Left_Pallidum)
    return("Left_Pallidum") ;
  if (label == Third_Ventricle)
    return("Third_Ventricle") ;
  if (label == Fourth_Ventricle)
    return("Fourth_Ventricle") ;
  if (label == Brain_Stem)
    return("Brain_Stem") ;
  if (label == Left_Hippocampus)
    return("Left_Hippocampus") ;
  if (label == Left_Amygdala)
    return("Left_Amygdala") ;
  if (label == Left_Insula)
    return("Left_Insula") ;
  if (label == Left_Operculum)
    return("Left_Operculum") ;
  if (label == Line_1)
    return("Line_1") ;
  if (label == Line_2)
    return("Line_2") ;
  if (label == Line_3)
    return("Line_3") ;
  if (label == CSF)
    return("CSF") ;
  if (label == Left_Lesion)
    return("Left_Lesion") ;
  if (label == Left_Accumbens_area)
    return("Left_Accumbens_area") ;
  if (label == Left_Substancia_Nigra)
    return("Left_Substancia_Nigra") ;
  if (label == Left_VentralDC)
    return("Left_VentralDC") ;
  if (label == Left_undetermined)
    return("Left_undetermined") ;
  if (label == Left_vessel)
    return("Left_vessel") ;
  if (label == Left_choroid_plexus)
    return("Left_choroid_plexus") ;
  if (label == Left_F3orb)
    return("Left_F3orb") ;
  if (label == Left_lOg)
    return("Left_lOg") ;
  if (label == Left_aOg)
    return("Left_aOg") ;
  if (label == Left_mOg)
    return("Left_mOg") ;
  if (label == Left_pOg)
    return("Left_pOg") ;
  if (label == Left_Stellate)
    return("Left_Stellate") ;
  if (label == Left_Porg)
    return("Left_Porg") ;
  if (label == Left_Aorg)
    return("Left_Aorg") ;
  if (label == Right_Cerebral_Exterior)
    return("Right_Cerebral_Exterior") ;
  if (label == Right_Cerebral_White_Matter)
    return("Right_Cerebral_White_Matter") ;
  if (label == Right_Cerebral_Cortex)
    return("Right_Cerebral_Cortex") ;
  if (label == Right_Lateral_Ventricle)
    return("Right_Lateral_Ventricle") ;
  if (label == Right_Inf_Lat_Vent)
    return("Right_Inf_Lat_Vent") ;
  if (label == Right_Cerebellum_Exterior)
    return("Right_Cerebellum_Exterior") ;
  if (label == Right_Cerebellum_White_Matter)
    return("Right_Cerebellum_White_Matter") ;
  if (label == Right_Cerebellum_Cortex)
    return("Right_Cerebellum_Cortex") ;
  if (label == Right_Thalamus)
    return("Right_Thalamus") ;
  if (label == Right_Thalamus_Proper)
    return("Right_Thalamus_Proper") ;
  if (label == Right_Caudate)
    return("Right_Caudate") ;
  if (label == Right_Putamen)
    return("Right_Putamen") ;
  if (label == Right_Pallidum)
    return("Right_Pallidum") ;
  if (label == Right_Hippocampus)
    return("Right_Hippocampus") ;
  if (label == Right_Amygdala)
    return("Right_Amygdala") ;
  if (label == Right_Insula)
    return("Right_Insula") ;
  if (label == Right_Operculum)
    return("Right_Operculum") ;
  if (label == Right_Lesion)
    return("Right_Lesion") ;
  if (label == Right_Accumbens_area)
    return("Right_Accumbens_area") ;
  if (label == Right_Substancia_Nigra)
    return("Right_Substancia_Nigra") ;
  if (label == Right_VentralDC)
    return("Right_VentralDC") ;
  if (label == Right_undetermined)
    return("Right_undetermined") ;
  if (label == Right_vessel)
    return("Right_vessel") ;
  if (label == Right_choroid_plexus)
    return("Right_choroid_plexus") ;
  if (label == Right_F3orb)
    return("Right_F3orb") ;
  if (label == Right_lOg)
    return("Right_lOg") ;
  if (label == Right_aOg)
    return("Right_aOg") ;
  if (label == Right_mOg)
    return("Right_mOg") ;
  if (label == Right_pOg)
    return("Right_pOg") ;
  if (label == Right_Stellate)
    return("Right_Stellate") ;
  if (label == Right_Porg)
    return("Right_Porg") ;
  if (label == Right_Aorg)
    return("Right_Aorg") ;
  if (label == Bone)
    return("Bone") ;
  if (label == Fat)
    return("Fat") ;
  if (label == Bright_Unknown)
    return("Bright Unknown") ;
  if (label == Dark_Unknown)
    return("Dark Unknown") ;

	if (label == Left_Interior)
		return("Left_Interior") ; 
	if (label == Right_Interior)
		return("Right_Interior") ; 
	if (label == Left_Lateral_Ventricles)
		return("Left_Lateral_Ventricles") ; 
	if (label == Right_Lateral_Ventricles)
		return("Right_Lateral_Ventricles") ; 
	if (label == WM_hypointensities)
		return("WM_hypointensities") ; 
	if (label == Left_WM_hypointensities)
		return("Left_WM_hypointensities") ; 
	if (label == Right_WM_hypointensities)
		return("Right_WM_hypointensities") ; 
	if (label == non_WM_hypointensities)
		return("non_WM_hypointensities") ; 
	if (label == Left_non_WM_hypointensities)
		return("Left_non_WM_hypointensities") ; 
	if (label == Right_non_WM_hypointensities)
		return("Right_non_WM_hypointensities") ; 
	if (label == Fifth_Ventricle)
		return("Fifth_Ventricle") ; 
	if (label == Optic_Chiasm)
		return("Optic_Chiasm") ; 
  return(name) ;
}
MRI *
GCArelabel_cortical_gray_and_white(GCA *gca, MRI *mri_inputs, 
                                   MRI *mri_src, MRI *mri_dst,TRANSFORM *transform)
{
  int      nchanged, x, y, z, width, height, depth, total_changed,
           label, xn, yn, zn, left, new_wm, new_gray ;
  MRI      *mri_tmp ;
  GCA_NODE *gcan ;
	GC1D     *gc_gray, *gc_wm ;
  float    vals[MAX_GCA_INPUTS], gray_dist, wm_dist ;

  if (mri_src != mri_dst)
    mri_dst = MRIcopy(mri_src, mri_dst) ;

  mri_tmp = MRIcopy(mri_dst, NULL) ;

  width = mri_src->width ; height = mri_src->height ; depth = mri_src->depth ;

  total_changed = new_wm = new_gray = 0 ;
  do
  {
    nchanged = 0 ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
            DiagBreak() ; 

          label = MRIvox(mri_src, x, y, z) ;
          if (label != Left_Cerebral_Cortex && 
              label != Left_Cerebral_White_Matter &&
              label != Right_Cerebral_Cortex && 
              label != Right_Cerebral_White_Matter)
            continue ;

					load_vals(mri_inputs, x, y, z, vals, gca->ninputs) ;
          GCAsourceVoxelToNode(gca, mri_dst, transform, x, y, z, &xn, &yn, &zn) ;
          gcan = &gca->nodes[xn][yn][zn] ;
          if (label == Left_Cerebral_Cortex || 
              label == Left_Cerebral_White_Matter)
          {
            left = 1 ;
						gc_gray = GCAfindGC(gca, xn, yn, zn, Left_Cerebral_Cortex) ;
						gc_wm = GCAfindGC(gca, xn, yn, zn, Left_Cerebral_White_Matter) ;
          }
          else
          {
						gc_gray = GCAfindGC(gca, xn, yn, zn, Right_Cerebral_Cortex) ;
						gc_wm = GCAfindGC(gca, xn, yn, zn, Right_Cerebral_White_Matter) ;
            left = 0 ;
          }

					if (!gc_wm || !gc_gray)
						continue ;
          gray_dist = sqrt(GCAmahDistIdentityCovariance(gc_gray, vals, gca->ninputs)) ;
          wm_dist = sqrt(GCAmahDistIdentityCovariance(gc_wm, vals, gca->ninputs)) ;
          if (gray_dist < wm_dist)
            label = left ? Left_Cerebral_Cortex : Right_Cerebral_Cortex ;
          else
            label = 
              left ? Left_Cerebral_White_Matter : Right_Cerebral_White_Matter ;
          if (label != MRIvox(mri_dst, x, y, z) && GCAisPossible(gca, mri_dst, label, transform, x, y, z))
          {
            if (x == Ggca_x && y == Ggca_y && z == Ggca_z && 
                (label == Ggca_label || Ggca_label < 0))
						{
							int in ;

              printf("voxel(%d,%d,%d), inputs=", x, y, z) ;
							for (in = 0 ;in < gca->ninputs ; in++)
								printf("%2.1f ", vals[in]) ;
              printf("label=%s (%d)\n",cma_label_to_name(label),label);
						}

            if (label == Left_Cerebral_Cortex ||label == Right_Cerebral_Cortex)
              new_gray++ ;
            else
              new_wm++ ;
            nchanged++ ;
            MRIvox(mri_tmp, x, y, z) = label ;
          }
        }
      }
    }
    MRIcopy(mri_tmp, mri_dst) ;
    total_changed += nchanged ;
  } while (nchanged > 0) ;

  MRIfree(&mri_tmp) ;
  printf("%d gm and wm labels changed (%%%2.0f to gray, %%%2.0f to white)\n", 
         total_changed, 100.0f*(float)new_gray/total_changed,
         100.0f*(float)new_wm/total_changed) ;
  return(mri_dst) ;
}
int
GCAdump(GCA *gca,MRI *mri,int x, int y, int z, TRANSFORM *transform, FILE *fp, int verbose)
{
  int       xn, yn, zn, xp, yp, zp ;
  GCA_NODE  *gcan ;
  GCA_PRIOR *gcap ;

  GCAsourceVoxelToNode(gca, mri, transform, x, y, z, &xn, &yn, &zn) ;
  GCAsourceVoxelToPrior(gca, mri, transform, x, y, z, &xp, &yp, &zp) ;
  printf("\nGCA node at voxel (%d, %d, %d) --> node (%d, %d, %d), prior (%d, %d, %d)\n",
         x, y, z, xn, yn, zn, xp, yp, zp) ;
  gcan = &gca->nodes[xn][yn][zn] ;
  gcap = getGCAP(gca, mri, transform, x, y, z) ;
  dump_gcan(gca, gcan, fp, verbose, gcap) ;
  return(NO_ERROR) ;
}

#if 0
static GCA_SAMPLE *
gcaExtractRegionLabelAsSamples(GCA *gca, MRI *mri_labeled, TRANSFORM *transform, 
                               int *pnsamples, int label, int xp, int yp, 
                               int zp, int wsize)
{
  int         i, nsamples, width, height, depth, x, y, z,
              xi, yi, zi, xk, yk, zk, whalf ;
  GCA_SAMPLE  *gcas ;
  GCA_PRIOR   *gcap ;
  GC1D        *gc ;

  width = mri_labeled->width ; 
  height = mri_labeled->height ; 
  depth = mri_labeled->depth ; 
  whalf = (wsize-1)/2 ;
  gcap = &gca->priors[xp][yp][zp] ;

  TransformInvert(transform, mri_labeled) ;
  GCApriorToSourceVoxel(gca, mri_labeled, transform, 
                       xp, yp, zp, &x, &y, &z) ;
  for (nsamples = 0, zk = -whalf  ; zk <= whalf ; zk++)
  {
    zi = mri_labeled->zi[z + zk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri_labeled->yi[y + yk] ;
      for (xk = -whalf ; xk <= whalf ; xk++)
      {
        xi = mri_labeled->xi[x + xk] ;
        if (MRIvox(mri_labeled, xi, yi, zi) != label)
          continue ;
        if (xi == Ggca_x && yi == Ggca_y && zi == Ggca_z)
          DiagBreak() ;
        nsamples++ ;
      }
    }
  }

  gcas = (GCA_SAMPLE *)calloc(nsamples, sizeof(GCA_SAMPLE)) ;
  if (!gcas)
    ErrorExit(ERROR_NOMEMORY, 
              "gcaExtractLabelAsSamples(%d): could not allocate %d samples\n",
              label, nsamples) ;


  /* go through region again and fill in samples */
  for (i = 0, zk = -whalf  ; zk <= whalf ; zk++)
  {
    zi = mri_labeled->zi[z + zk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri_labeled->yi[y + yk] ;
      for (xk = -whalf ; xk <= whalf ; xk++)
      {
        xi = mri_labeled->xi[x + xk] ;
        if (MRIvox(mri_labeled, xi, yi, zi) != label)
          continue ;
        if (xi == Ggca_x && yi == Ggca_y && zi == Ggca_z)
          DiagBreak() ;

        gcap = getGCAP(gca, mri_labeled, transform, xi, yi, zi) ;
        GCAsourceVoxelToPrior(gca, mri_labeled, transform, xi, yi, zi, &xp, &yp, &zp) ;
        gc = GCAfindPriorGC(gca, xp, yp, zp, label) ;
        if (!gc || !gcap)
        {
          nsamples-- ;   /* shouldn't happen */
          continue ;
        }

        gcas[i].label = label ;
        gcas[i].xp = xp ; gcas[i].yp = yp ; gcas[i].zp = zp ; 
        gcas[i].x = xi ;   gcas[i].y = yi ;   gcas[i].z = zi ; 
        gcas[i].var = gc->var ;
        gcas[i].mean = gc->mean ;
        gcas[i].prior = getPrior(gcap, label) ;
        if (FZERO(gcas[i].prior))
          DiagBreak() ;
        i++ ;
      }
    }
  }

  *pnsamples = nsamples ;
  return(gcas) ;
}
#endif

static GCA_SAMPLE *
gcaExtractThresholdedRegionLabelAsSamples(GCA *gca, MRI *mri_labeled, 
                                          TRANSFORM *transform, 
                                          int *pnsamples, int label, int xp, int yp, 
                                          int zp, int wsize, float pthresh)
{
  int         i, nsamples, width, height, depth, x, y, z,
              xi, yi, zi, xk, yk, zk, whalf, r, c, v ;
  GCA_SAMPLE  *gcas ;
  GCA_PRIOR   *gcap ;
  GC1D        *gc ;
  float       prior ;

  width = mri_labeled->width ; 
  height = mri_labeled->height ; 
  depth = mri_labeled->depth ; 
  whalf = (wsize-1)/2 ;
  gcap = &gca->priors[xp][yp][zp] ;

  TransformInvert(transform, mri_labeled) ;
  GCApriorToSourceVoxel(gca, mri_labeled, transform, 
                       xp, yp, zp, &x, &y, &z) ;
  for (nsamples = 0, zk = -whalf  ; zk <= whalf ; zk++)
  {
    zi = mri_labeled->zi[z + zk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri_labeled->yi[y + yk] ;
      for (xk = -whalf ; xk <= whalf ; xk++)
      {
        xi = mri_labeled->xi[x + xk] ;
        if (MRIvox(mri_labeled, xi, yi, zi) != label)
          continue ;
        if (xi == Ggca_x && yi == Ggca_y && zi == Ggca_z)
          DiagBreak() ;
        nsamples++ ;
      }
    }
  }

  gcas = (GCA_SAMPLE *)calloc(nsamples, sizeof(GCA_SAMPLE)) ;
  if (!gcas)
    ErrorExit(ERROR_NOMEMORY, 
              "gcaExtractLabelAsSamples(%d): could not allocate %d samples\n",
              label, nsamples) ;


  /* go through region again and fill in samples */
  for (i = 0, zk = -whalf  ; zk <= whalf ; zk++)
  {
    zi = mri_labeled->zi[z + zk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri_labeled->yi[y + yk] ;
      for (xk = -whalf ; xk <= whalf ; xk++)
      {
        xi = mri_labeled->xi[x + xk] ;
        if (MRIvox(mri_labeled, xi, yi, zi) != label)
          continue ;
        if (xi == Ggca_x && yi == Ggca_y && zi == Ggca_z)
          DiagBreak() ;

        gcap = getGCAP(gca, mri_labeled, transform, xi, yi, zi) ;
        GCAsourceVoxelToPrior(gca, mri_labeled, transform, xi, yi, zi, &xp, &yp, &zp) ;
        gc = GCAfindPriorGC(gca, xp, yp, zp, label) ;
        if (gcap)
          prior = getPrior(gcap, label) ;
        else
          prior = 0 ;
        if (!gc || !gcap || prior < pthresh)
        {
          nsamples-- ;   /* shouldn't happen */
          continue ;
        }

        gcas[i].label = label ;
        gcas[i].xp = xp ; gcas[i].yp = yp ; gcas[i].zp = zp ; 
        gcas[i].x = xi ;   gcas[i].y = yi ;   gcas[i].z = zi ; 
				gcas[i].means = (float *)calloc(gca->ninputs, sizeof(float)) ;
				gcas[i].covars = (float *)calloc((gca->ninputs*(gca->ninputs+1))/2, sizeof(float)) ;
				if (!gcas[i].means || !gcas[i].covars)
					ErrorExit(ERROR_NOMEMORY, "GCArenormalizeAdapative: could not allocate mean (%d) and covariance (%d) matrices",
										gca->ninputs, gca->ninputs*(gca->ninputs+1)/2) ;
				for (r = v = 0 ; r < gca->ninputs ; r++)
				{
					gcas[i].means[r] = gc->means[r] ;
					for (c = r ; c < gca->ninputs ; c++)
						gcas[i].covars[v] = gc->covars[v] ;
				}
        gcas[i].prior = prior ;
        if (FZERO(gcas[i].prior))
          DiagBreak() ;
        i++ ;
      }
    }
  }

  *pnsamples = nsamples ;
  return(gcas) ;
}

static GCA_SAMPLE *
gcaExtractLabelAsSamples(GCA *gca, MRI *mri_labeled, TRANSFORM *transform,
                         int *pnsamples, int label)
{
  int         i, nsamples, width, height, depth, x, y, z, xp, yp, zp, n, r, c, v ;
  GCA_SAMPLE  *gcas ;
  GCA_PRIOR    *gcap ;
  GC1D        *gc ;

  width = mri_labeled->width ; 
  height = mri_labeled->height ; 
  depth = mri_labeled->depth ; 

  for (nsamples = z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (MRIvox(mri_labeled, x, y, z) != label)
          continue ;
        nsamples++ ;
      }
    }
  }

  gcas = (GCA_SAMPLE *)calloc(nsamples, sizeof(GCA_SAMPLE)) ;
  if (!gcas)
    ErrorExit(ERROR_NOMEMORY, 
              "gcaExtractLabelAsSamples(%d): could not allocate %d samples\n",
              label, nsamples) ;


  for (i = z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (MRIvox(mri_labeled, x, y, z) != label)
          continue ;

        GCAsourceVoxelToPrior(gca, mri_labeled, transform, x, y, z, &xp, &yp, &zp) ;
        gcap = &gca->priors[xp][yp][zp] ;
        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          if (gcap->labels[n] == label)
            break ;
        }

        gc = GCAfindPriorGC(gca, xp, yp, zp, label) ;
        if (n >= gcap->nlabels || !gc)
        {
          nsamples-- ;   /* doesn't exist at this location */
          continue ;   /* ?? */
        }
        gcas[i].label = label ;
        gcas[i].xp = xp ; gcas[i].yp = yp ; gcas[i].zp = zp ; 
        gcas[i].x = x ;   gcas[i].y = y ;   gcas[i].z = z ; 
				for (r = v = 0 ; r < gca->ninputs ; r++)
				{
					gcas[i].means[r] = gc->means[r] ;
					for (c = r ; c < gca->ninputs ; c++)
						gcas[i].covars[v] = gc->covars[v] ;
				}
        gcas[i].prior = getPrior(gcap, label) ;
        if (FZERO(gcas[i].prior))
          DiagBreak() ;
        i++ ;
      }
    }
  }

  *pnsamples = nsamples ;
  return(gcas) ;
}

#define MIN_MEAN_SAMPLES      10
#define SAMPLE_PCT       0.20

int
GCArenormalize(MRI *mri_in, MRI *mri_labeled, GCA *gca, TRANSFORM *transform)
{
  int              x, y, z, n, label, biggest_label, nsamples, val,
                   *ordered_indices, i, index, width, height, depth ;
  float            mean, var, *means, *stds, *gca_means ;
  GCA_NODE         *gcan ;
  GCA_SAMPLE       *gcas ;
  GC1D             *gc ;
  
  if (gca->ninputs > 1)
		ErrorExit(ERROR_UNSUPPORTED, "GCArenormalize: can only renormalize scalars") ;

  /* first build a list of all labels that exist */
  for (nsamples = biggest_label = x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_height ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          if (gcan->labels[n] > biggest_label)
            biggest_label = gcan->labels[n] ;
        }
      }
    }
  }

  gca_means = (float *)calloc(biggest_label+1, sizeof(float)) ;
  means = (float *)calloc(biggest_label+1, sizeof(float)) ;
  stds = (float *)calloc(biggest_label+1, sizeof(float)) ;
  if (!gca_means || !means || !stds)
    ErrorExit(ERROR_NOMEMORY, "%s: could not allocated %d vector",
              Progname, biggest_label+1) ;

  /* do unknown labels separately */
  for (mean = var = 0.0, nsamples = x = 0 ; x < mri_in->width ; x++)
  {
    for (y = 0 ; y < mri_in->height ; y++)
    {
      for (z = 0 ; z < mri_in->height ; z++)
      {
        if (MRIvox(mri_labeled, x, y, z) == Unknown)
        {
          nsamples++ ;
          val = (float)MRIvox(mri_in, x, y, z) ;
          mean += val ; var += (val*val) ;
        }
      }
    }
  }

  if (DIAG_VERBOSE_ON && 0)
  {
    HISTOGRAM *histo ;
    char  fname[STRLEN] ;

    label = Right_Hippocampus ;
    histo = MRIhistogramLabel(mri_in, mri_labeled, label, 256) ;
    sprintf(fname, "%s.plt", cma_label_to_name(label)) ;
    HISTOplot(histo, fname) ;
    HISTOfree(&histo) ;
  }

#if 1
  label = Unknown ;
  if (!FZERO(nsamples))
  {
    mean /= nsamples ;
    means[label] = mean ;
    stds[label] = sqrt(var/nsamples - mean*mean) ;
    GCAlabelMean(gca, label, &gca_means[label]) ;
    printf("scaling label %s by %2.2f (%2.2f / %2.2f) (%d samples, std=%2.1f)\n", 
           cma_label_to_name(label),
           means[label] / gca_means[label],
           means[label], gca_means[label], nsamples, stds[label]) ;
  }
  else
  {
    gca_means[label] = stds[label] = means[label] = 1.0 ;
  }
  
  gca_means[label] = stds[label] = means[label] = 1.0 ;
#endif

  for (label = 1 ; label <= biggest_label ; label++)
  {
    gcas = gcaExtractLabelAsSamples(gca, mri_labeled,transform,&nsamples,label);
    if (!nsamples)
      continue ;
    if (nsamples < MIN_MEAN_SAMPLES)  /* not enough sample to estimate mean */
    {
      free(gcas) ;
      continue ;
    }
    ordered_indices = (int *)calloc(nsamples, sizeof(int)) ;
    GCAcomputeLogSampleProbability(gca, gcas, mri_in, transform, nsamples) ;
    GCArankSamples(gca, gcas, nsamples, ordered_indices) ;

    if (nint(nsamples*SAMPLE_PCT) < MIN_MEAN_SAMPLES)
      nsamples = MIN_MEAN_SAMPLES ;
    else
      nsamples = nint(SAMPLE_PCT * (float)nsamples) ;
    

    /* compute mean and variance of image intensities in this label */
    for (var = mean = 0.0f, i = 0 ; i < nsamples ; i++)
    {
      index = ordered_indices[i] ;
      val = (float)MRIvox(mri_in, gcas[index].x, gcas[index].y, gcas[index].z);
      mean += val ;
      var += val*val ;
    }
    mean /= (float)nsamples ; var = var / nsamples - mean*mean ; 
    var = sqrt(var);
#if 0
    printf("label %s: using %d samples to estimate mean = %2.1f +- %2.1f\n",
           cma_label_to_name(label), nsamples, mean, var) ;
#endif
    means[label] = mean ; stds[label] = var ;
    GCAlabelMean(gca, label, &gca_means[label]) ;
    free(gcas) ; free(ordered_indices) ;
    printf("scaling label %s by %2.2f (%2.2f / %2.2f) (%d samples, std=%2.1f)\n", 
           cma_label_to_name(label),
           means[label] / gca_means[label],
           means[label], gca_means[label], nsamples, stds[label]) ;
    if (FZERO(gca_means[label]))
      DiagBreak() ;
  }

  width = gca->node_width ; height = gca->node_height ; depth = gca->node_depth ;
  for (x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          label = gcan->labels[n] ;
          gc = &gcan->gcs[n] ;
          mean = gc->means[0] * means[label] / gca_means[label] ;
          gc->means[0] = mean ;
        }
      }
    }
  }

  free(means) ; free(stds) ; free(gca_means) ;
  return(NO_ERROR) ;
}
int
GCArenormalizeAdaptive(MRI *mri_in, MRI *mri_labeled, GCA *gca, TRANSFORM *transform,
                       int wsize, float pthresh)
{
  int              x, y, z, n, label, nsamples, xp,yp, zp,
                   peak, orig_wsize, frame ;
#if 0
	int              i, index, *ordered_indices ;
  float            mean, var ;
	Real             val ;
#endif
  GCA_NODE         *gcan ;
  GCA_SAMPLE       *gcas ;
  GC1D             *gc ;
  HISTOGRAM        *histo, *hsmooth ;
	float            fmin, fmax ;

  orig_wsize = wsize ;
	MRIvalRange(mri_in, &fmin, &fmax) ;
  histo = HISTOalloc((int)(fmax-fmin+1)) ; hsmooth = HISTOalloc((int)(fmax-fmin+1)) ;

  /* go through GCA renormalizing each entry */
  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          label = gcan->labels[n] ;
          if (label == Unknown)
            continue ;
          gcaNodeToPrior(gca, x, y, z, &xp, &yp, &zp) ;
					gc = &gcan->gcs[n] ;

#define MIN_SAMPLES 20
          wsize = orig_wsize ;
          if (label == Ggca_label)
            DiagBreak() ;
          do
          {
            gcas = gcaExtractThresholdedRegionLabelAsSamples(gca, mri_labeled,transform,
                                                             &nsamples,label,
                                                             xp, yp, zp, wsize,pthresh);
          
            if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
              DiagBreak() ;
            wsize += 2;  
            if (gcas && nsamples < MIN_SAMPLES)
							GCAfreeSamples(&gcas, nsamples) ;
          } while ((nsamples < MIN_SAMPLES) && (wsize < 2*orig_wsize));

          if (nsamples < MIN_SAMPLES) /* couldn't find any in this nbhd */
            continue ;

					for (frame = 0 ; frame < gca->ninputs ; frame++)
					{
						gcaHistogramSamples(gca, gcas, mri_in, transform, nsamples, histo, frame);
						HISTOsmooth(histo, hsmooth, 2) ;
						if (IS_WM(label) && gca->ninputs == 1)
							peak = HISTOfindLastPeakRelative(hsmooth, HISTO_WINDOW_SIZE,.3);
						else if (IS_LAT_VENT(label) && gca->ninputs == 1)
							peak = HISTOfindFirstPeakRelative(hsmooth, HISTO_WINDOW_SIZE,.3);
						else
							peak = HISTOfindHighestPeakInRegion(hsmooth, fmin, fmax) ;
						if (peak < 0)
							continue ;
#if 0
						ordered_indices = (int *)calloc(nsamples, sizeof(int)) ;
						GCAcomputeLogSampleProbability(gca, gcas, mri_in, transform, nsamples) ;
						GCArankSamples(gca, gcas, nsamples, ordered_indices) ;
						
						if (nsamples > MIN_SAMPLES)
						{
							if (nint(nsamples*SAMPLE_PCT) < MIN_SAMPLES)
								nsamples = MIN_SAMPLES ;
							else
								nsamples = nint(SAMPLE_PCT * (float)nsamples) ;
						}
						
						
						/* compute mean and variance of image intensities in this label */
						for (var = mean = 0.0f, i = 0 ; i < nsamples ; i++)
						{
							index = ordered_indices[i] ;
							MRIsampleVolumeFrame(mri_in, gcas[index].x, gcas[index].y, gcas[index].z,
								frame, &val);
							mean += val ;
							var += val*val ;
							if (gcas[index].x == Ggca_x &&  gcas[index].y == Ggca_y &&
									gcas[index].z == Ggca_z)
								DiagBreak() ;
						}
						mean /= (float)nsamples ; var = var / nsamples - mean*mean ; 
						var = sqrt(var);
						free(ordered_indices) ;
						gc->means[0] = mean ;
#endif
						gc->means[frame] = (float)peak ;
					}
					GCAfreeSamples(&gcas, nsamples) ; 
        }
      }
    }
  }

  HISTOfree(&histo) ; HISTOfree(&hsmooth) ;
  return(NO_ERROR) ;
}


#define MEAN_RESOLUTION  8

int
GCArenormalizeLabels(MRI *mri_in, MRI *mri_labeled, GCA *gca, TRANSFORM *transform)
{
  int              x, y, z, n, label, biggest_label, nsamples, xv, yv, zv,
                   *ordered_indices, i, index, width, height, depth ;
  float            mean, var, val ;
  GCA_NODE         *gcan ;
  GCA_SAMPLE       *gcas ;
  GC1D             *gc ;
  MRI              *mri_means, *mri_control, *mri_tmp ;
  char             fname[STRLEN] ;

  if (gca->ninputs > 1)
		ErrorExit(ERROR_UNSUPPORTED, "GCArenormalizeLabels: can only renormalize scalars") ;

  /* first build a list of all labels that exist */
  for (biggest_label = x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_height ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          if (gcan->labels[n] > biggest_label)
            biggest_label = gcan->labels[n] ;
        }
      }
    }
  }

  for (label = 1 ; label <= biggest_label ; label++)
  {
    gcas = gcaExtractLabelAsSamples(gca, mri_labeled,transform,&nsamples,label);
    if (!nsamples)
      continue ;
    if (nsamples < MIN_SAMPLES/SAMPLE_PCT)
    {
      free(gcas) ;
      continue ;
    }
    ordered_indices = (int *)calloc(nsamples, sizeof(int)) ;
    GCAcomputeLogSampleProbability(gca, gcas, mri_in, transform, nsamples) ;
    GCArankSamples(gca, gcas, nsamples, ordered_indices) ;

    if (nint(nsamples*SAMPLE_PCT) < MIN_SAMPLES)
      nsamples = MIN_SAMPLES ;
    else
      nsamples = nint(SAMPLE_PCT * (float)nsamples) ;
    

    for (var = mean = 0.0f, i = 0 ; i < nsamples ; i++)
    {
      index = ordered_indices[i] ;
      val = (float)MRIvox(mri_in, gcas[index].x, gcas[index].y, gcas[index].z);
      mean += val ;
      var += val*val ;
    }
    mean /= (float)nsamples ; var = var / nsamples - mean*mean ; 
    var = sqrt(var);
    printf("label %s: using %d samples to estimate mean = %2.1f +- %2.1f\n",
           cma_label_to_name(label), nsamples, mean, var) ;
    
    width = nint(mri_in->width / MEAN_RESOLUTION) ;
    height = nint(mri_in->height / MEAN_RESOLUTION) ;
    depth = nint(mri_in->depth / MEAN_RESOLUTION) ;
    mri_means = MRIalloc(width, height, depth, MRI_FLOAT) ;
    mri_control = MRIalloc(width, height, depth, MRI_SHORT) ;
    MRIsetResolution(mri_means, 
                     MEAN_RESOLUTION,MEAN_RESOLUTION,MEAN_RESOLUTION) ;
    MRIsetResolution(mri_control, 
                     MEAN_RESOLUTION,MEAN_RESOLUTION,MEAN_RESOLUTION) ;

    GCAlabelMri(gca, mri_means, label, transform) ;
    if (DIAG_VERBOSE_ON && Gdiag & DIAG_WRITE)
    {
      sprintf(fname, "%s_label.mgh", cma_label_to_name(label)) ;
      MRIwrite(mri_means, fname) ;
    }

    for (mean = 0.0f, n = z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (MRIFvox(mri_means, x, y, z) > 1)
          {
            n++ ;
            mean += MRIFvox(mri_means, x, y, z) ;
          }
        }
      }
    }
    mean /= (float)n ;
    printf("mean GCA value %2.1f\n", mean) ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (MRIFvox(mri_means, x, y, z) < 1)
          {
            MRIFvox(mri_means, x, y, z) = mean ;
          }
        }
      }
    }
          
    TransformInvert(transform, mri_in) ;
    for (i = 0 ; i < nsamples ; i++)
    {
      index = ordered_indices[i] ;
      GCApriorToSourceVoxel(gca, mri_means, transform, 
                           gcas[index].xp, gcas[index].yp, gcas[index].zp, 
                           &x, &y, &z) ;
      val = (float)MRIvox(mri_in, gcas[index].x, gcas[index].y, gcas[index].z);
      if (x == 19 && y == 14 && z == 15)
        DiagBreak() ;
      if (MRISvox(mri_control, x, y, z) == 0)
        MRIFvox(mri_means, x, y, z) = val ;
      else
        MRIFvox(mri_means, x, y, z) += val ;
      MRISvox(mri_control, x, y, z)++ ;
    }
        
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (x == 19 && y == 14 && z == 15)
            DiagBreak() ;
          if (MRISvox(mri_control, x, y, z) > 0)
          {
            MRIFvox(mri_means,x,y,z) = 
              nint((float)MRIFvox(mri_means,x,y,z)/
                   (float)MRISvox(mri_control,x,y,z)) ;
            MRISvox(mri_control, x, y, z) = 1 ;
          }
        }
      }
    }

    if (DIAG_VERBOSE_ON && Gdiag & DIAG_WRITE)
    {
      sprintf(fname, "%s_means.mgh", cma_label_to_name(label)) ;
      MRIwrite(mri_means, fname) ;
    }

    mri_tmp = MRIalloc(width, height, depth, MRI_SHORT) ;
    MRIcopy(mri_means, mri_tmp) ;
    MRIfree(&mri_means) ; mri_means = mri_tmp ;

    mri_tmp = MRIalloc(width, height, depth, MRI_UCHAR) ;
    MRIcopy(mri_control, mri_tmp) ;
    MRIfree(&mri_control) ; mri_control = mri_tmp ;

    MRIsoapBubble(mri_means, mri_control, mri_means, 10) ;
    MRIclear(mri_control) ;
    MRIsoapBubble(mri_means, mri_control, mri_means, 1) ;
    
    if (DIAG_VERBOSE_ON && Gdiag & DIAG_WRITE)
    {
      sprintf(fname, "%s_soap.mgh", cma_label_to_name(label)) ;
      MRIwrite(mri_means, fname) ;

      sprintf(fname, "%s_control.mgh", cma_label_to_name(label)) ;
      MRIwrite(mri_control, fname) ;
    }


    width = gca->node_width ; height = gca->node_height ; depth = gca->node_depth ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        for (x = 0 ; x < width ; x++)
        {
          if (x == Gx && y == Gy && z == Gz)
            DiagBreak() ;
          gcan = &gca->nodes[x][y][z] ;
          for (n = 0 ; n < gcan->nlabels ; n++)
          {
            if (gcan->labels[n] == label)
            {
              if (x == Gx && y == Gy && z == Gz)
                DiagBreak() ;
              gc = &gcan->gcs[n] ;
              GCAnodeToVoxel(gca, mri_means, x, y, z, &xv, &yv, &zv) ;
              gc->means[0] = (float)MRISvox(mri_means, xv, yv, zv);
              break ;
            }
          }
        }
      }
    }

    MRIfree(&mri_means) ; MRIfree(&mri_control) ;
    free(gcas) ; free(ordered_indices) ;
  }

  return(NO_ERROR) ;
}

int
GCArenormalizeIntensities(GCA *gca, int *labels, float *intensities, int num)
{
  float     wm_mean, scale_factor, gray_mean, prior ;
  int       xn, yn, zn, n, i, label ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;
  double    *means, *wts ;

	if (gca->ninputs > 1)
		ErrorExit(ERROR_UNSUPPORTED, "GCArenormalizeIntensities: can only renormalize scalars") ;

  means = (double *)calloc(num, sizeof(double)) ;
  wts = (double *)calloc(num, sizeof(double)) ;
  if (!means || !wts)
    ErrorExit(ERROR_NOMEMORY, "%s: could not allocate %d wt and mean vectors",
              Progname, num) ;

  /* compute overall white matter mean to use as anchor for rescaling */
  for (zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[xn][yn][zn] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          /* find index in lookup table for this label */
          label = gcan->labels[n] ;
          for (i = 0 ; i < num ; i++)
            if (label == labels[i])
              break ;

          if (i >= num)
            continue ;  /* shouldn't happen */

          gc = &gcan->gcs[n] ;
          prior = get_node_prior(gca, label, xn, yn, zn) ;
          wts[i] += prior ;
          means[i] += gc->means[0]*prior ;
                           
        }
      }
    }
  }
  gray_mean = scale_factor = wm_mean = -1 ;
  for (i = 0 ; i < num ; i++)
  {
    if (!FZERO(wts[i]))
      means[i] /= wts[i] ;
    if (labels[i] == Left_Cerebral_White_Matter)
    {
      wm_mean = means[i] ;
      scale_factor = wm_mean / intensities[i] ;
    }
    if (labels[i] == Left_Cerebral_Cortex)
      gray_mean = means[i] ;
  }
  if (wm_mean < 0)
    ErrorReturn(ERROR_BADPARM, 
                (ERROR_BADPARM, "GCArenormalizeIntensities: "
                 "could not find white matter in intensity table")) ;
  scale_factor = 1 ;
  printf("mean wm intensity = %2.1f, scaling gray to %2.2f (from %2.2f)\n", 
         wm_mean, scale_factor*gray_mean, gray_mean) ;

  /* now go through each label and scale it by gca wm to norm wm ratio */
  /* compute overall white matter mean to use as anchor for rescaling */
  for (zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
        if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z)
          DiagBreak() ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          label = gcan->labels[n] ;
          if (label == Gdiag_no)
            DiagBreak() ;

          /* find index in lookup table for this label */
          for (i = 0 ; i < num ; i++)
            if (label == labels[i])
              break ;
          if (i >= num)
            continue ;
          
          gc = &gcan->gcs[n] ;
          gc->means[0] = scale_factor*intensities[i]*gc->means[0]/means[i] ;
        }
      }
    }
  }

  free(wts) ;
  free(means) ;
  return(NO_ERROR) ;
}

int
GCAunifyVariance(GCA *gca)
{
  int        xn, yn, zn, n, r, c, v ;
  GCA_NODE   *gcan ;
  GC1D       *gc ;
 
  for (zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
					for (r = v = 0 ; r < gca->ninputs ; r++)
					{
						for (c = r ; c < gca->ninputs ; c++, v++)
						{
							if (r == c)
								gc->covars[v] = MIN_VAR ;
							else
								gc->covars[v] = 0.0 ;
						}
					}
        }
      }
    }
  }

  return(NO_ERROR) ;
}


int
GCAlabelMean(GCA *gca, int label, float *means)
{
  int       xn, yn, zn, n, r ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;
  double    wt ;
  float     prior ;

  /* compute overall white matter mean to use as anchor for rescaling */
	memset(means, 0, gca->ninputs*sizeof(float)) ;
  for (wt = 0.0, zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          /* find index in lookup table for this label */
          if (gcan->labels[n] != label)
            continue ;
          gc = &gcan->gcs[n] ;
          prior = get_node_prior(gca, label, xn, yn, zn) ;
          wt += prior ;
					for (r = 0 ; r < gca->ninputs ; r++)
					{
						means[r] += gc->means[r]*prior ;
						if (!finite(gc->means[r]))
							DiagBreak() ;
					}
                           
        }
      }
    }
  }
	if (FZERO(wt))
		return(NO_ERROR) ;
	for (r = 0 ; r < gca->ninputs ; r++)
		means[r] /= wt ;
  return(NO_ERROR) ;
}
int
GCAregularizeConditionalDensities(GCA *gca, float smooth)
{
  int       xn, yn, zn, n, i, label, max_label, r ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;
  double    **means, *wts ;
  float     prior ;

  means = (double **)calloc(gca->ninputs, sizeof(double *)) ;
  wts = (double *)calloc(MAX_GCA_LABELS, sizeof(double)) ;
  if (!means || !wts)
    ErrorExit(ERROR_NOMEMORY, "%s: could not allocate %d wt and mean vectors",
              Progname, MAX_GCA_LABELS) ;
	for (r = 0 ; r < gca->ninputs ; r++)
	{
		means[r] = (double *)calloc(MAX_GCA_LABELS, sizeof(double)) ;
		if (!means[r])
			ErrorExit(ERROR_NOMEMORY, "%s: could not allocate %d wt and mean vectors",
								Progname, MAX_GCA_LABELS) ;
	}

  /* compute overall mean for each class */
  for (max_label = 1, zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          /* find index in lookup table for this label */
          label = gcan->labels[n] ;

          gc = &gcan->gcs[n] ;
          prior = get_node_prior(gca, label, xn, yn, zn) ;
          wts[label] += prior ;
					for (r = 0 ; r < gca->ninputs ; r++)
						means[r][label] += gc->means[r]*prior ;
          if (label > max_label)
            max_label = label ;
        }
      }
    }
  }

  for (i = 0 ; i <= max_label ; i++)
  {
    if (!FZERO(wts[i]))
		{
			for (r = 0 ; r < gca->ninputs ; r++)
				means[r][i] /= wts[i] ;
		}
  }

  /* now impose regularization */
  for (zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          /* find index in lookup table for this label */
          label = gcan->labels[n] ;
          if (label <= 0)
            continue ;

          gc = &gcan->gcs[n] ;
					for (r = 0 ; r < gca->ninputs ; r++)
						gc->means[r] = means[r][label]*smooth + gc->means[r]*(1.0f-smooth) ;
        }
      }
    }
  }

	for (r = 0 ; r < gca->ninputs ; r++)
		free(means[r]) ;
  free(wts) ; free(means) ;
  return(NO_ERROR) ;
}
int
GCArenormalizeToFlash(GCA *gca, char *tissue_parms_fname, MRI *mri)
{
  FILE     *fp ;
  char     *cp, line[STRLEN] ;
  int      labels[MAX_GCA_LABELS], nlabels ;
  float   intensities[MAX_GCA_LABELS], TR, alpha, T1, PD ;
  
  TR = mri->tr ; alpha = mri->flip_angle ;

  fp = fopen(tissue_parms_fname, "r") ;
  if (!fp)
    ErrorReturn(ERROR_NOFILE,
                (ERROR_NOFILE, 
                 "GCArenormalizeToFlash: could not open tissue parms file %s",
                 tissue_parms_fname)) ;

  cp = fgetl(line, STRLEN-1, fp) ; nlabels = 0 ;
  while (cp)
  {
    if (sscanf(cp, "%d %f %f", &labels[nlabels], &T1, &PD)
        != 3)
      ErrorReturn(ERROR_BADFILE,
                (ERROR_BADFILE, 
                 "GCArenormalizeToFlash: could not parse %dth line %s in %s",
                 nlabels+1, cp, tissue_parms_fname)) ;
                
    intensities[nlabels] = FLASHforwardModel(T1, PD, TR, alpha, 3) ;

    nlabels++ ;
    cp = fgetl(line, STRLEN-1, fp) ;
  }
  fclose(fp) ;
  GCArenormalizeIntensities(gca, labels, intensities, nlabels) ;
  return(NO_ERROR) ;
}

int
GCAmeanFilterConditionalDensities(GCA *gca, float navgs)
{
	/* won't work for covariances */
  int       xn, yn, zn, xn1, yn1, zn1, n, label, max_label, niter, f ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;
  double    means[MAX_GCA_INPUTS], wt ;
  MRI       *mri_means ;
  float     prior ;

  mri_means = MRIallocSequence(gca->node_width, gca->node_height, gca->node_depth, MRI_FLOAT, gca->ninputs) ;

  /* compute overall mean for each class */
  for (max_label = 1, zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
				if (xn == Gx && yn == Gy && zn == Gz)
					DiagBreak() ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          /* find index in lookup table for this label */
          label = gcan->labels[n] ;

          gc = &gcan->gcs[n] ;
          if (label > max_label)
            max_label = label ;
        }
      }
    }
  }

  /* now impose regularization */
  for (niter = 0 ; niter < navgs ; niter++)
  {
    for (label = 0 ; label <= max_label ; label++)
    {
			if (label == Gdiag_no)
				DiagBreak() ;
      if (IS_UNKNOWN(label))
        continue ;
      for (zn = 0 ; zn < gca->node_depth ; zn++)
      {
        for (yn = 0 ; yn < gca->node_height ; yn++)
        {
          for (xn = 0 ; xn < gca->node_width ; xn++)
          {
						if (xn == Gx && yn == Gy && zn == Gz)
							DiagBreak() ;
						wt = 0.0 ;
						for (f = 0 ; f < gca->ninputs ; f++)
							means[f] = 0 ;
            for (xn1 = xn-1 ; xn1 <= xn+1 ; xn1++)
            {
              if (xn1 < 0 || xn1 >= gca->node_width)
                continue ;
              for (yn1 = yn-1 ; yn1 <= yn+1 ; yn1++)
              {
                if (yn1 < 0 || yn1 >= gca->node_height)
                  continue ;
                for (zn1 = zn-1 ; zn1 <= zn+1 ; zn1++)
                {
                  if (zn1 < 0 || zn1 >= gca->node_depth)
                    continue ;
                  if (xn1 == xn && yn1 == yn && zn1 == zn)
                    DiagBreak() ;
                  gcan = &gca->nodes[xn1][yn1][zn1] ;
                  for (n = 0 ; n < gcan->nlabels ; n++)
                  {
                    /* find index in lookup table for this label */
                    if (gcan->labels[n] != label)
                      continue ;
                  
                    gc = &gcan->gcs[n] ;
                    prior = get_node_prior(gca, label, xn1, yn1, zn1) ;
										for (f = 0 ; f < gca->ninputs ; f++)
										{
											means[f] += prior*gc->means[f] ;
											if (!finite(means[f] / wt))
												DiagBreak() ;
										}
                    wt += prior ;
                    break ;
                  }
                }
              }
            }
            if (FZERO(wt))
              continue ;   /* label didn't occur here */
						for (f = 0 ; f < gca->ninputs ; f++)
						{
							if (!finite(means[f] / wt))
								DiagBreak() ;
							MRIFseq_vox(mri_means, xn, yn, zn, f) = means[f] / wt ;
							if (!finite(MRIFseq_vox(mri_means,xn,yn,zn,f)))
								DiagBreak() ;
						}
          }
        }
      }
      
      for (zn = 0 ; zn < gca->node_depth ; zn++)
      {
        for (yn = 0 ; yn < gca->node_height ; yn++)
        {
          for (xn = 0 ; xn < gca->node_width ; xn++)
          {
            gcan = &gca->nodes[xn][yn][zn] ;
            for (n = 0 ; n < gcan->nlabels ; n++)
            {
              if (gcan->labels[n] != label)
                continue ;
              gc = &gcan->gcs[n] ;
							if (xn == Gx && yn == Gy && zn == Gz)
								DiagBreak() ;
							for (f = 0 ; f < gca->ninputs ; f++)
							{
								gc->means[f] = MRIFseq_vox(mri_means, xn, yn, zn, f) ;
								if (!finite(gc->means[f]))
									DiagBreak() ;
							}
              break ;
            }
          }
        }
      }
    }
  }

  MRIfree(&mri_means) ;
  return(NO_ERROR) ;
}

#define OFFSET_SIZE  25
double BOX_SIZE =   60;   /* mm */
double HALF_BOX=   (60/2);
int
GCAhistoScaleImageIntensities(GCA *gca, MRI *mri)
{
  float      x0, y0, z0, fmin, fmax, min_real_val ;
  int        mri_peak, r, max_T1_weighted_image = 0, min_real_bin ;
  float      wm_means[MAX_GCA_INPUTS], tmp[MAX_GCA_INPUTS], scales[MAX_GCA_INPUTS], max_wm, scale ;
  HISTOGRAM *h_mri, *h_smooth ;
  MRI_REGION box ;
	MRI        *mri_frame ;

  GCAlabelMean(gca, Left_Cerebral_White_Matter, wm_means) ;
  GCAlabelMean(gca, Left_Cerebral_White_Matter, tmp) ;
	max_wm = 0 ;
	for (r = 0 ; r < gca->ninputs ; r++)
	{
		wm_means[r] = (wm_means[r] + tmp[r]) / 2 ;
		if (wm_means[r] > max_wm)
		{
			max_T1_weighted_image = r ; max_wm = wm_means[r] ;
		}
	}

	mri_frame = MRIcopyFrame(mri, NULL, max_T1_weighted_image, 0) ;
	MRIvalRange(mri_frame, &fmin, &fmax) ;
  h_mri = MRIhistogram(mri_frame, nint(fmax-fmin+1)) ; h_mri->counts[0] = 0 ; /* ignore background */
  h_smooth = HISTOsmooth(h_mri, NULL, 2) ;
#if 0
  mri_peak = HISTOfindHighestPeakInRegion(h_smooth, 0, h_smooth->nbins/3) ;
#else
  mri_peak = HISTOfindFirstPeak(h_smooth, 5, .1) ;
#endif
  min_real_bin = HISTOfindEndOfPeak(h_smooth, mri_peak, .25) ;
  min_real_val = h_smooth->bins[min_real_bin] ;	
	if (min_real_val > 0.25*fmax)   /* for skull-stripped images */
		min_real_val = 0.25*fmax ;
	printf("using real data threshold=%2.1f\n", min_real_val) ;

  MRIfindApproximateSkullBoundingBox(mri_frame, min_real_val, &box) ;
	MRIfree(&mri_frame) ; HISTOfree(&h_mri) ; HISTOfree(&h_smooth) ;
  x0 = box.x+box.dx/3 ; y0 = box.y+box.dy/3 ; z0 = box.z+box.dz/2 ;
  printf("using (%.0f, %.0f, %.0f) as brain centroid...\n",x0, y0, z0) ;
#if 0
  box.x = x0 - HALF_BOX*mri->xsize ; box.dx = BOX_SIZE*mri->xsize ;
  box.y = y0 - HALF_BOX*mri->ysize ; box.dy = BOX_SIZE*mri->ysize ;
  box.z = z0 - HALF_BOX*mri->zsize ; box.dz = BOX_SIZE*mri->zsize ;
#else
  box.dx /= 4 ; box.x = x0 - box.dx/2;
  box.dy /= 4 ; box.y = y0 - box.dy/2;
  box.dz /= 4 ; box.z = z0 - box.dz/2;
#endif
  for (r = 0 ; r < gca->ninputs ; r++)
	{
		mri_frame = MRIcopyFrame(mri, NULL, r, 0) ;
		printf("mean wm in atlas = %2.0f, using box (%d,%d,%d) --> (%d, %d,%d) "
					 "to find MRI wm\n", wm_means[r], box.x, box.y, box.z, 
					 box.x+box.dx-1,box.y+box.dy-1, box.z+box.dz-1) ;

		h_mri = MRIhistogramRegion(mri_frame, 0, NULL, &box) ; 
		if (gca->ninputs == 1)
			HISTOclearBins(h_mri, h_mri, 0, min_real_val) ;
		if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
			HISTOplot(h_mri, "mri.histo") ;
		if (gca->ninputs == 1)   /* assume it is T1-weighted */
			mri_peak = HISTOfindLastPeak(h_mri, HISTO_WINDOW_SIZE,MIN_HISTO_PCT);
		else
			mri_peak = HISTOfindHighestPeakInRegion(h_mri, 1, h_mri->nbins * h_mri->bin_size);
		mri_peak = h_mri->bins[mri_peak] ;
		printf("before smoothing, mri peak at %d\n", mri_peak) ;
		h_smooth = HISTOsmooth(h_mri, NULL, 2) ;
		if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
			HISTOplot(h_smooth, "mri_smooth.histo") ;
		if (gca->ninputs == 1)   /* assume it is T1-weighted */
			mri_peak = HISTOfindLastPeak(h_smooth, HISTO_WINDOW_SIZE,MIN_HISTO_PCT);
		else
			mri_peak = HISTOfindHighestPeakInRegion(h_mri, 1, h_mri->nbins * h_mri->bin_size);
		mri_peak = h_mri->bins[mri_peak] ;
		printf("after smoothing, mri peak at %d, scaling input intensities "
					 "by %2.3f\n", mri_peak, wm_means[r]/mri_peak) ;
#if 0
		MRIscalarMul(mri_frame, mri_frame, wm_means[r]/mri_peak) ;
		MRIcopyFrame(mri_frame, mri, 0, r) ;   /* put it back in multi-frame image */
#endif
		if (mri_peak == 0)
			ErrorExit(ERROR_BADPARM, "GCAhistoScaleImageIntensities: could not find wm peak") ;
		scales[r] = wm_means[r]/mri_peak ;
		MRIfree(&mri_frame) ;
		HISTOfree(&h_smooth) ; HISTOfree(&h_mri) ;
	}
	for (scale = 0.0, r = 0 ; r < gca->ninputs ; r++)
		scale += scales[r] ;
	scale /= (double)gca->ninputs ;
	printf("scaling all frames by %2.3f\n", scale) ;
	MRIscalarMul(mri, mri, scale) ;
  return(NO_ERROR) ;
}

int
GCAhisto(GCA *gca, int nbins, int **pcounts)
{
  int   *counts, x, y, z ;
  GCA_NODE *gcan ;

  *pcounts = counts = (int *)calloc(nbins+1, sizeof(int)) ;

  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        if (gcan->nlabels == 0 || (gcan->nlabels == 1 && 
                                   GCAfindGC(gca,x,y,z,Unknown) != NULL))
          continue ;
        counts[gcan->nlabels]++ ;
      }
    }
  }

  return(NO_ERROR) ;
}

int
GCArenormalizeToExample(GCA *gca, MRI *mri_seg, MRI *mri_T1)
{
  float     intensities[MAX_CMA_LABEL+1] ;
  int       x, y, z, label, labels[MAX_CMA_LABEL+1], width, height, depth,
            counts[MAX_CMA_LABEL+1] ;

  for (label = 0 ; label <= MAX_CMA_LABEL ; label++)
    labels[label] = label ;
  memset(intensities, 0, MAX_CMA_LABEL*sizeof(float)) ;
  memset(counts, 0, MAX_CMA_LABEL*sizeof(int)) ;

  width = mri_seg->width ; height = mri_seg->height ; depth = mri_seg->height;
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        label = MRIvox(mri_seg, x, y, z) ;
        if (label == Gdiag_no)
          DiagBreak() ;
        if (label > MAX_CMA_LABEL)
        {
          ErrorPrintf(ERROR_BADPARM, 
                      "GCArenormalizeToExample: bad label %d", label) ;
          continue ;
        }
        intensities[label] += (float)MRIvox(mri_T1, x, y, z) ;
        counts[label]++ ;
      }
    }
  }

  for (label = 0 ; label <= MAX_CMA_LABEL ; label++)
  {
    if (counts[label] <= 0)
      continue ;
    intensities[label] /= (float)counts[label] ;
  }
  GCArenormalizeIntensities(gca, labels, intensities, MAX_CMA_LABEL) ;

  return(NO_ERROR) ;
}

static GC1D *
findClosestValidGC(GCA *gca, int x0, int y0, int z0, int label, int check_var)
{
  GC1D       *gc, *gc_min ;
  int        x, y, z, n ;
  double     dist, min_dist, det ;
  GCA_NODE   *gcan ;
	MATRIX     *m_cov_inv ;

  min_dist = gca->node_width+gca->node_height+gca->node_depth ;
  gc_min = NULL ;
#define WSIZE 3
  for (x = x0-WSIZE ; x <= x0+WSIZE ; x++)
  {
    if (x < 0 || x >= gca->node_width)
      continue ;
    for (y = y0-WSIZE ; y <= y0+WSIZE ; y++)
    {
      if (y < 0 || y >= gca->node_height)
        continue ;
      for (z = z0-WSIZE ; z <= z0+WSIZE  ; z++)
      {
        if (z < 0 || z >= gca->node_depth)
          continue ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels && min_dist > 1 ; n++)
        {
          if (gcan->labels[n] != label)
            continue ;
          gc = &gcan->gcs[n] ;
					det = covariance_determinant(gc, gca->ninputs) ;
					m_cov_inv = load_inverse_covariance_matrix(gc, NULL, gca->ninputs) ;
					if (m_cov_inv == NULL)
						det = -1 ;
					else
						MatrixFree(&m_cov_inv) ;
					if (check_var && det <= MIN_DET)
						continue ;
          dist = sqrt(SQR(x-x0)+SQR(y-y0)+SQR(z-z0)) ;
          if (dist < min_dist)
          {
            gc_min = gc ;
            min_dist = dist ;
          }
        }
      }
    }
  }
  if (gc_min)   /* found one in immediate nbhd */
    return(gc_min) ;

  /* couldn't find one close - search everywhere */
  for (x = 0 ; x < gca->node_width && min_dist > 1 ; x++)
  {
    for (y = 0 ; y < gca->node_height && min_dist > 1 ; y++)
    {
      for (z = 0 ; z < gca->node_depth && min_dist > 1 ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels && min_dist > 1 ; n++)
        {
          if (gcan->labels[n] != label)
            continue ;
          gc = &gcan->gcs[n] ;
					det = covariance_determinant(gc, gca->ninputs) ;
					if (check_var && det <= 0)
						continue ;
          dist = sqrt(SQR(x-x0)+SQR(y-y0)+SQR(z-z0)) ;
          if (dist < min_dist)
          {
            gc_min = gc ;
            min_dist = dist ;
          }
        }
      }
    }
  }
  return(gc_min) ;
}

static int
gcaCheck(GCA *gca)
{
  int x, y, z, ret = NO_ERROR, n, r, c, v ;
  GCA_NODE *gcan ;
  GC1D     *gc ;

  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
					for (v = r = 0 ; r < gca->ninputs ; r++)
					{
						for (c = r ; c < gca->ninputs ; c++, v++)
							if (!finite(gc->means[r]) || !finite(gc->covars[v]))
							{
								ret = ERROR_BADPARM ;
								DiagBreak() ;
							}
					}
        }
      }
    }
  }
  return(ret) ;
}
int
GCAcomputeVoxelLikelihoods(GCA *gca, MRI *mri_in, int x, int y, int z, TRANSFORM *transform, int *labels, double *likelihoods)
{
  GCA_NODE  *gcan ;
  int       xn, yn, zn, n ;
	float     vals[MAX_GCA_INPUTS] ;
  
  GCAsourceVoxelToNode(gca, mri_in, transform, x, y, z, &xn, &yn, &zn) ;
	load_vals(mri_in, x, y, z, vals, gca->ninputs) ;
  gcan = &gca->nodes[xn][yn][zn] ;
  for (n = 0 ; n < gcan->nlabels ; n++)
  {
    labels[n] = gcan->labels[n] ;
    likelihoods[n] = 
      GCAcomputeConditionalDensity(&gcan->gcs[n],vals, gca->ninputs, labels[n]);
  }
  
  return(gcan->nlabels) ;
}

double
GCAcomputeConditionalDensity(GC1D *gc, float *vals, int ninputs, int label)
{
  double  p, dist ;

  /* compute 1-d Mahalanobis distance */
#if 0
  if (label == Unknown)  /* uniform distribution */
  {
    p = 1.0/256.0f ;
  }
  else   /* Gaussian distribution */
#endif
  {
    dist = GCAmahDist(gc, vals, ninputs) ;
		p = (1.0 / (pow(2*M_PI,ninputs/2.0)*sqrt(covariance_determinant(gc, ninputs)))) * exp(-dist) ;
  }
  return(p) ;
}
double
GCAcomputeNormalizedConditionalDensity(GCA *gca, int xp, int yp, int zp, float *vals, int label)
{
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  int        n ;
  double      p, total, plabel ;

  gcap = &gca->priors[xp][yp][zp] ;
  if (gcap->nlabels <= 1)
		return(1.0) ;

  for (plabel = total = 0.0, n = 0 ; n < gcap->nlabels ; n++)
  {
    gc = GCAfindPriorGC(gca, xp, yp, zp, gcap->labels[n]) ; 
    if (!gc)
      continue ;
    p = GCAcomputeConditionalDensity(gc,vals,gca->ninputs,gcap->labels[n]);
		if (gcap->labels[n] == label)
			plabel = p ;
		total += p ;
  }

	if (!FZERO(total))
		plabel /= total ;
  return(plabel) ;
}

static double 
gcaComputeLogDensity(GC1D *gc, float *vals, int ninputs, float prior, int label)
{
  double log_p ;

  log_p = GCAcomputeConditionalLogDensity(gc, vals, ninputs, label) ;
  log_p += log(prior) ;
  return(log_p) ;
}

double
GCAcomputeConditionalLogDensity(GC1D *gc, float *vals, int ninputs, int label)
{
  double  log_p, det ;


  /* compute 1-d Mahalanobis distance */
#if 0
  if (label == Unknown)  /* uniform distribution */
  {
    log_p = log(1.0/256.0f) ;
  }
  else   /* Gaussian distribution */
#endif
  {
		det = covariance_determinant(gc, ninputs) ;
		log_p = -log(sqrt(det)) - .5*GCAmahDist(gc, vals, ninputs) ;
  }
  return(log_p) ;
}
GCA_SAMPLE *
GCAfindAllSamples(GCA *gca, int *pnsamples, int *exclude_list)
{
  GCA_SAMPLE *gcas ;
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  int        x, y, z, width, height, depth, n, label, i,
             max_n, max_label, nsamples, r, c, v ;
  float      max_p, prior ;

  width = gca->prior_width ; height = gca->prior_height ; depth = gca->prior_depth ;

  /* find the # of places in the atlas that the highest prior class is
     other than unknown.
  */
  for (nsamples = x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        gcap = &gca->priors[x][y][z] ;
        max_p = 0 ;  max_n = -1 ; max_label = 0 ;
				if (gcap->nlabels == 0)
					continue ;

        if (x*gca->prior_spacing == Gx && y*gca->prior_spacing == Gy && 
            z*gca->prior_spacing == Gz)
          DiagBreak() ;
        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          label = gcap->labels[n] ;
          if (label == Gdiag_no)
            DiagBreak() ;
          prior = gcap->priors[n] ;
          if (prior > max_p)
          {
            max_n = n ;
            max_p = prior ;
            max_label = gcap->labels[n] ;
          }
        }
				if (exclude_list && exclude_list[max_label] > 0)
					continue ;
        if (IS_UNKNOWN(max_label) &&
            (different_nbr_max_labels(gca, x, y, z, 1, 0) == 0))
          continue ;
        if (max_label == Gdiag_no)
          DiagBreak() ;
        nsamples++ ;
      }
    }
  }

  gcas = calloc(nsamples, sizeof(GCA_SAMPLE )) ;

  for (i = x = 0 ; x < width ; x++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (z = 0 ; z < depth ; z++)
      {
        gcap = &gca->priors[x][y][z] ;
        max_p = 0 ;  max_n = -1 ; max_label = 0 ;
				if (gcap->nlabels == 0)
					continue ;

        for (n = 0 ; n < gcap->nlabels ; n++)
        {
          label = gcap->labels[n] ;
          prior = gcap->priors[n] ;
          if (label == Gdiag_no)
            DiagBreak() ;
          if (prior > max_p)
          {
            max_n = n ;
            max_p = prior ;
            max_label = gcap->labels[n] ;
          }
        }
				if (exclude_list && exclude_list[max_label] > 0)
					continue ;
        if (IS_UNKNOWN(max_label) &&
            (different_nbr_max_labels(gca, x, y, z, 1, 0) == 0))
          continue ;
        if (max_label == Gdiag_no)
          DiagBreak() ;
        gcas[i].xp = x ; gcas[i].yp = y ; gcas[i].zp = z ;
        gcas[i].x = x*gca->prior_spacing ; 
        gcas[i].y = y*gca->prior_spacing ; 
        gcas[i].z = z*gca->prior_spacing ;
        gcas[i].label = max_label ;
				if (FZERO(max_p))
				{
					max_p = 0.1 ;
					DiagBreak() ;
				}
        gcas[i].prior = max_p ;
				gcas[i].means = (float *)calloc(gca->ninputs, sizeof(float)) ;
				gcas[i].covars = (float *)calloc((gca->ninputs*(gca->ninputs+1))/2, sizeof(float)) ;
				if (!gcas[i].means || !gcas[i].covars)
					ErrorExit(ERROR_NOMEMORY, "GCAfindStableSamples: could not allocate mean (%d) and covariance (%d) matrices",
								gca->ninputs, gca->ninputs*(gca->ninputs+1)/2) ;
        gc = GCAfindPriorGC(gca, x, y, z, max_label) ;
        if (gc)
        {
					for (v = r = 0 ; r < gca->ninputs ; r++)
					{
						gcas[i].means[r] = gc->means[r] ;
						for (c = r ; c < gca->ninputs ; c++, v++)
							gcas[i].covars[v] = gc->covars[v] ;
					}
        }
        else
        {
					for (v = r = 0 ; r < gca->ninputs ; r++)
					{
						gcas[i].means[r] = 0.0 ;
						for (c = r ; c < gca->ninputs ; c++, v++)
						{
							if (c == r)
								gcas[i].covars[v] = 1.0 ;
							else
								gcas[i].covars[v] = 0.0 ;
						}
					}
        }
        gcas[i].log_p = 0 ;
        i++ ;
      }
    }
  }

  *pnsamples = nsamples ;
  return(gcas) ;
}
int
GCAcomputeMAPlabelAtLocation(GCA *gca, int xp, int yp, int zp, float *vals,
                             int *pmax_n, float *plog_p)
{
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  int        n, max_n, max_label ;
  float      log_p, max_log_p ;

  gcap = &gca->priors[xp][yp][zp] ;
  if (gcap->nlabels == 0)
  {
    if (plog_p)
      *plog_p = 0.0 ;
    if (pmax_n)
      *pmax_n = -1 ;
    return(Unknown) ;
  }

  max_label = gcap->labels[0] ; max_n = 0 ;
  gc = GCAfindPriorGC(gca, xp, yp, zp, gcap->labels[0]) ; 
  if (gc)
    max_log_p = gcaComputeLogDensity(gc, vals, gca->ninputs, gcap->priors[0], max_label) ;
  else
    max_log_p = -100000 ;
  for (n = 1 ; n < gcap->nlabels ; n++)
  {
    gc = GCAfindPriorGC(gca, xp, yp, zp, gcap->labels[n]) ; 
    if (!gc)
      continue ;
    log_p = gcaComputeLogDensity(gc,vals,gca->ninputs,gcap->labels[n], gcap->priors[n]);
    if (log_p > max_log_p)
    {
      max_log_p = log_p ;
      max_n = n ;
      max_label = gcap->labels[n] ;
    }
  }

  if (plog_p)
    *plog_p = max_log_p ;
  if (pmax_n)
    *pmax_n = max_n ;
  return(max_label) ;
}
int
GCAcomputeMLElabelAtLocation(GCA *gca, int xp, int yp, int zp, float *vals, 
                             int *pmax_n, float *plog_p)
{
  GCA_PRIOR  *gcap ;
  GC1D       *gc ;
  int        n, max_n, max_label ;
  float      log_p, max_log_p ;

  gcap = &gca->priors[xp][yp][zp] ;
  if (gcap->nlabels == 0)
  {
    if (plog_p)
      *plog_p = 0.0 ;
    if (pmax_n)
      *pmax_n = -1 ;
    return(Unknown) ;
  }

  max_label = gcap->labels[0] ; max_n = 0 ;
  gc = GCAfindPriorGC(gca, xp, yp, zp, gcap->labels[0]) ; 
  if (gc)
    max_log_p = GCAcomputeConditionalLogDensity(gc, vals, gca->ninputs, max_label) ;
  else
    max_log_p = -100000 ;
  for (n = 1 ; n < gcap->nlabels ; n++)
  {
    gc = GCAfindPriorGC(gca, xp, yp, zp, gcap->labels[n]) ; 
    if (!gc)
      continue ;
    log_p = GCAcomputeConditionalLogDensity(gc,vals,gca->ninputs,gcap->labels[n]);
    if (log_p > max_log_p)
    {
      max_log_p = log_p ;
      max_n = n ;
      max_label = gcap->labels[n] ;
    }
  }

  if (plog_p)
    *plog_p = max_log_p ;
  if (pmax_n)
    *pmax_n = max_n ;
  return(max_label) ;
}
static GC1D *
alloc_gcs(int nlabels, int flags, int ninputs)
{
  GC1D  *gcs ;
  int   i ;

  gcs = (GC1D *)calloc(nlabels, sizeof(GC1D)) ;
  if (gcs == NULL)
    ErrorExit(ERROR_NOMEMORY, "alloc_gcs(%d, %x): could not allocated %d gcs",
              nlabels, flags, nlabels) ;
  for (i = 0 ; i < nlabels ; i++)
  {
    gcs[i].means = (float *)calloc(ninputs, sizeof(float)) ;
    gcs[i].covars = (float *)calloc((ninputs*(ninputs+1))/2, sizeof(float)) ;
    if (!gcs[i].means || !gcs[i].covars)
      ErrorExit(ERROR_NOMEMORY, "could not allocate mean (%d) and covariance (%d) matrices",
	ninputs, ninputs*(ninputs+1)/2) ;
  }
  if (flags & GCA_NO_MRF)
    return(gcs) ;

  for (i = 0 ; i < nlabels ; i++)
  {
    gcs[i].nlabels = (short *)calloc(GIBBS_NEIGHBORHOOD, sizeof(short)) ;
    gcs[i].labels = (unsigned char **)calloc(GIBBS_NEIGHBORHOOD, sizeof(unsigned char *)) ;
    gcs[i].label_priors = (float **)calloc(GIBBS_NEIGHBORHOOD, sizeof(float)) ;
    if (!gcs[i].nlabels || !gcs[i].labels || !gcs[i].label_priors)
      ErrorExit(ERROR_NOMEMORY, "alloc_gcs(%d, %x): could not allocated %d gcs(%d)",
                nlabels, flags, nlabels, i) ;
  }
  return(gcs) ;
}
static int
free_gcs(GC1D *gcs, int nlabels, int ninputs)
{
  int   i, j ;

  for (i = 0 ; i < nlabels ; i++)
  {
		free(gcs[i].means) ;
		free(gcs[i].covars) ;
    if (gcs[i].nlabels)  /* gibbs stuff allocated */
    {
      for (j = 0 ; j < GIBBS_NEIGHBORHOOD ; j++)
      {
        if (gcs[i].labels[j])
          free(gcs[i].labels[j]) ;
        if (gcs[i].label_priors[j])
          free(gcs[i].label_priors[j]) ;
      }
      free(gcs[i].nlabels) ;
      free(gcs[i].labels) ;
      free(gcs[i].label_priors) ;
    }
  }
		
  free(gcs) ;
  return(NO_ERROR) ;
}

static int
copy_gcs(int nlabels, GC1D *gcs_src, GC1D *gcs_dst, int ninputs)
{
  int   i, j, k, r, c,v ;

  for (i = 0 ; i < nlabels ; i++)
  {
		for (v = r = 0 ; r < ninputs ; r++)
		{
			gcs_dst[i].means[r] = gcs_src[i].means[r] ;
			for (c = r ; c < ninputs ; c++, v++)
				gcs_dst[i].covars[v] = gcs_src[i].covars[v] ;
		}
    gcs_dst[i].ntraining = gcs_src[i].ntraining ;
    if (gcs_dst[i].nlabels == NULL)   /* NO_MRF flag must be set */
      continue ;
    for (j = 0 ; j < GIBBS_NEIGHBORHOOD ; j++)
    {
      gcs_dst[i].nlabels[j] = gcs_src[i].nlabels[j] ;
      if (!gcs_dst[i].label_priors[j])
      {
        gcs_dst[i].label_priors[j] = (float *)calloc(gcs_src[i].nlabels[j],sizeof(float)) ;
        gcs_dst[i].labels[j] = (unsigned char *)calloc(gcs_src[i].nlabels[j],sizeof(unsigned char)) ;
        if (!gcs_dst[i].label_priors[j] || !gcs_dst[i].labels[j])
          ErrorExit(ERROR_NOMEMORY, "copy_gcs(%d): i=%d, j=%d, could not gibbs\n",
                    nlabels, i, j) ;
      }
      for (k = 0 ; k < gcs_src[i].nlabels[j] ; k++)
      {
        gcs_dst[i].label_priors[j][k] = gcs_src[i].label_priors[j][k] ;
        gcs_dst[i].labels[j][k] = gcs_src[i].labels[j][k] ;
      }
    }
  }
  return(NO_ERROR) ;
}

int
GCAfreeGibbs(GCA *gca)
{
  int       x, y, z,n, i ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;

  if (gca->flags & GCA_NO_MRF)
    return(NO_ERROR) ;  /* already done */

  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          gc = &gcan->gcs[n] ;
          for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
          {
            free(gc->label_priors[i]) ;
            free(gc->labels[i]) ;
          }
          free(gc->nlabels) ;
          free(gc->labels) ;
          free(gc->label_priors) ;
        }
      }
    }
  }
  gca->flags |= GCA_NO_MRF ;
  return(NO_ERROR) ;
}

int
GCAcomputeSampleCoords(GCA *gca, MRI *mri, GCA_SAMPLE *gcas, 
                       int nsamples,TRANSFORM *transform)
{
  int    n, x, y, z ;

  TransformInvert(transform, mri) ;
  for (n = 0 ; n < nsamples ; n++)
  {
    if (gcas[n].label == Gdiag_no)
      DiagBreak() ;
    GCApriorToSourceVoxel(gca, mri, transform, gcas[n].xp, gcas[n].yp, gcas[n].zp, &x, &y, &z) ;
    gcas[n].x = x ; gcas[n].y = y ; gcas[n].z = z ;
    if (DIAG_VERBOSE_ON && Gdiag & DIAG_SHOW)
      printf("label %d: (%d, %d, %d) <-- (%d, %d, %d)\n",
             gcas[n].label,gcas[n].xp,gcas[n].yp,gcas[n].zp, x, y, z) ;
  }
  return(NO_ERROR) ;
}

static HISTOGRAM *
gcaHistogramSamples(GCA *gca, GCA_SAMPLE *gcas, MRI *mri, 
                    TRANSFORM *transform, int nsamples, HISTOGRAM *histo, int frame)
{
  int    i ;
  double mean, var ;
	Real   val ;
	float  fmin, fmax ;

  if (!histo)
	{
		MRIvalRangeFrame(mri, &fmin, &fmax, frame) ;
    histo = HISTOalloc(nint(fmax-fmin+1)) ;
	}
  else
    HISTOclear(histo, histo) ;

  for (mean = var = 0.0, i = 0 ; i < nsamples ; i++)
  {
		MRIsampleVolumeFrame(mri, gcas[i].x,gcas[i].y,gcas[i].z, frame, &val) ;
    histo->counts[(int)val]++ ;
    mean += val ;
    var += (val*val) ;
  }

  mean /= (double)nsamples ;
  var = var / (double)nsamples - mean*mean ;
  return(histo) ;
}
int
GCArenormalizeFromAtlas(GCA *gca, GCA *gca_template)
{
  int   xs, ys, zs, xt, yt, zt, ns, label, v ;
  float scale ;
  GCA_NODE *gcan ;
  GC1D      *gc, *gct ;

  scale = (float)gca_template->node_width / (float)gca->node_width ;

  for (xs = 0 ; xs < gca->node_width ; xs++)
  {
    xt = (int)(scale*xs) ;
    for (ys = 0 ; ys < gca->node_width ; ys++)
    {
      yt = (int)(scale*ys) ;
      for (zs = 0 ; zs < gca->node_width ; zs++)
      {
        zt = (int)(scale*zs) ;
        if (xs == Ggca_x && ys == Ggca_y && zs == Ggca_z)
          DiagBreak() ;
        if (xt == Ggca_x && yt == Ggca_y && zt == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[xs][ys][zs] ;
        for (ns = 0 ; ns < gcan->nlabels ; ns++)
        {
          label = gcan->labels[ns] ;
          gc = &gcan->gcs[ns] ;
          gct = GCAfindGC(gca_template, xt, yt, zt, label) ;
          if (gct == NULL)
            continue ;    /* label not in template GCA */
					for (v = 0 ; v < gca->ninputs ; v++)
						gc->means[v] = gct->means[v] ;
        }
      }
    }
  }
  return(NO_ERROR) ;
}


void
load_vals(MRI *mri_inputs, float x, float y, float z, float *vals, int ninputs)
{
	int  n ;
	Real val ;

	for (n = 0 ; n < ninputs ; n++)
	{
		MRIsampleVolumeFrame(mri_inputs, x, y, z, n, &val) ;
		vals[n] = val ;
	}
}

double
GCAmahDist(GC1D *gc, float *vals, int ninputs)
{
	static VECTOR *v_means = NULL, *v_vals = NULL ;
	static MATRIX *m_cov = NULL, *m_cov_inv ;
	int    i ;
	double dsq ;

	if (ninputs == 1)
	{
		float  v ;
		v = vals[0] - gc->means[0] ;
		dsq = v*v / gc->covars[0] ;
		return(dsq) ;
	}

	if (v_vals && ninputs != v_vals->rows)
		VectorFree(&v_vals) ;
	if (v_means && ninputs != v_means->rows)
		VectorFree(&v_means) ;
	if (m_cov && (ninputs != m_cov->rows || ninputs != m_cov->cols))
	{
		MatrixFree(&m_cov) ;
		MatrixFree(&m_cov_inv) ;
	}
	v_means = load_mean_vector(gc, v_means, ninputs) ;
	m_cov = load_covariance_matrix(gc, m_cov, ninputs) ;
	if (v_vals == NULL)
		v_vals = VectorClone(v_means) ;
	for (i = 0 ; i < ninputs ; i++)
		VECTOR_ELT(v_vals, i+1) = vals[i] ;

	VectorSubtract(v_means, v_vals, v_vals) ;  /* v_vals now has mean removed */
	m_cov_inv = MatrixInverse(m_cov, m_cov_inv) ;
	if (!m_cov_inv)
		ErrorExit(ERROR_BADPARM, "singular covariance matrix!") ;
	MatrixSVDInverse(m_cov, m_cov_inv) ;

	MatrixMultiply(m_cov_inv, v_vals, v_means) ;  /* v_means is now inverse(cov) * v_vals */
	dsq = VectorDot(v_vals, v_means) ;

	return(dsq);
}
double
GCAmahDistIdentityCovariance(GC1D *gc, float *vals, int ninputs)
{
	static VECTOR *v_means = NULL, *v_vals = NULL ;
	int    i ;
	double dsq ;

	if (v_vals && ninputs != v_vals->rows)
		VectorFree(&v_vals) ;
	if (v_means && ninputs != v_means->rows)
		VectorFree(&v_means) ;
	v_means = load_mean_vector(gc, v_means, ninputs) ;
	if (v_vals == NULL)
		v_vals = VectorClone(v_means) ;
	for (i = 0 ; i < ninputs ; i++)
		VECTOR_ELT(v_vals, i+1) = vals[i] ;

	VectorSubtract(v_means, v_vals, v_vals) ;  /* v_vals now has mean removed */
	dsq = VectorDot(v_vals, v_vals) ;

	return(dsq);
}

double
GCAcomputePosteriorDensity(GCA_PRIOR *gcap, GCA_NODE *gcan, int n, float *vals, int ninputs)
{
	GC1D *gc ;
	double p ;

	gc = &gcan->gcs[n] ;

	p = GCAcomputeConditionalDensity(gc, vals, ninputs, gcan->labels[n]) ;
	p *= getPrior(gcap, gcan->labels[n]) ;
	return(p) ;
}

VECTOR *
load_mean_vector(GC1D *gc, VECTOR *v_means, int ninputs)
{
	int n ;

	if (v_means == NULL)
		v_means = VectorAlloc(ninputs, MATRIX_REAL) ;

	for (n = 0 ; n < ninputs ; n++)
		VECTOR_ELT(v_means, n+1) = gc->means[n] ;

	return(v_means) ;
}
MATRIX *
load_covariance_matrix(GC1D *gc, MATRIX *m_cov, int ninputs)
{
  int n, m, i ;

  if (m_cov == NULL)
    m_cov = MatrixAlloc(ninputs, ninputs, MATRIX_REAL) ;
  
  for (i = n = 0 ; n < ninputs ; n++)
    for (m = n ; m < ninputs ; m++, i++)
      {
	*MATRIX_RELT(m_cov, n+1, m+1) = gc->covars[i] ;
	if (m != n)
	  *MATRIX_RELT(m_cov, m+1, n+1) = gc->covars[i] ;
      }
  
  return(m_cov) ;
}
MATRIX *
load_inverse_covariance_matrix(GC1D *gc, MATRIX *m_inv_cov, int ninputs)
{
	static MATRIX *m_cov = NULL ;

	if (m_cov && (m_cov->rows != ninputs || m_cov->cols != ninputs))
		MatrixFree(&m_cov) ;
	m_cov = load_covariance_matrix(gc, m_cov, ninputs) ;
	m_inv_cov = MatrixInverse(m_cov, m_inv_cov) ;
#if 0
	if (m_inv_cov == NULL)
		ErrorPrintf(ERROR_BADPARM, "load_inverse_covariance_matrix: singular covariance matrix!") ;
#endif
	return(m_inv_cov) ;
}

double
covariance_determinant(GC1D *gc, int ninputs)
{
	double det ;
	static MATRIX *m_cov = NULL ;

	if (ninputs == 1)
		return(gc->covars[0]) ;
	if (m_cov && (m_cov->rows != ninputs || m_cov->cols != ninputs))
		MatrixFree(&m_cov) ;
	m_cov = load_covariance_matrix(gc, m_cov, ninputs) ;
	det = MatrixDeterminant(m_cov) ;
#if 0
	if (det <= 0)
		det = 0.001 ;
#endif
	return(det) ;
}

static double
gcaComputeSampleLogDensity(GCA_SAMPLE *gcas, float *vals, int ninputs)
{
  double log_p ;

  log_p = gcaComputeSampleConditionalLogDensity(gcas, vals, ninputs, gcas->label) ;
  log_p += log(gcas->prior) ;
  return(log_p) ;
}

static double
gcaComputeSampleConditionalLogDensity(GCA_SAMPLE *gcas, float *vals, int ninputs, int label)
{
  double  log_p, det ;


#if 0
  if (label == Unknown)  /* uniform distribution */
  {
    log_p = log(1.0/256.0f) ;
  }
  else   /* Gaussian distribution */
#endif
  {
		det = sample_covariance_determinant(gcas, ninputs) ;
		log_p = -log(sqrt(det)) - .5*GCAsampleMahDist(gcas, vals, ninputs) ;
  }
  return(log_p) ;
}
static VECTOR *
load_sample_mean_vector(GCA_SAMPLE *gcas, VECTOR *v_means, int ninputs)
{
	int n ;

	if (v_means == NULL)
		v_means = VectorAlloc(ninputs, MATRIX_REAL) ;

	for (n = 0 ; n < ninputs ; n++)
		VECTOR_ELT(v_means, n+1) = gcas->means[n] ;

	return(v_means) ;
}
static MATRIX *
load_sample_covariance_matrix(GCA_SAMPLE *gcas, MATRIX *m_cov, int ninputs)
{
	int n, m, i ;

	if (m_cov == NULL)
		m_cov = MatrixAlloc(ninputs, ninputs, MATRIX_REAL) ;

	for (i = n = 0 ; n < ninputs ; n++)
		for (m = n ; m < ninputs ; m++, i++)
		{
			*MATRIX_RELT(m_cov, n+1, m+1) = gcas->covars[i] ;
		}

	return(m_cov) ;
}

static double
sample_covariance_determinant(GCA_SAMPLE *gcas, int ninputs)
{
	double det ;
	static MATRIX *m_cov = NULL ;

	if (ninputs == 1)
		return(gcas->covars[0]) ;
	if (m_cov && (m_cov->rows != ninputs || m_cov->cols != ninputs))
		MatrixFree(&m_cov) ;

	m_cov = load_sample_covariance_matrix(gcas, m_cov, ninputs) ;
	det = MatrixDeterminant(m_cov) ;
	return(det) ;
}
double 
GCAsampleMahDist(GCA_SAMPLE *gcas, float *vals, int ninputs)
{
	static VECTOR *v_means = NULL, *v_vals = NULL ;
	static MATRIX *m_cov = NULL, *m_cov_inv ;
	int    i ;
	double dsq ;

	if (ninputs == 1)
	{
		float  v ;
		v = vals[0] - gcas->means[0] ;
		dsq = v*v / gcas->covars[0] ;
		return(dsq) ;
	}

	if (v_vals && ninputs != v_vals->rows)
		VectorFree(&v_vals) ;
	if (v_means && ninputs != v_means->rows)
		VectorFree(&v_means) ;
	if (m_cov && (ninputs != m_cov->rows || ninputs != m_cov->cols))
	{
		MatrixFree(&m_cov) ;
		MatrixFree(&m_cov_inv) ;
	}

	v_means = load_sample_mean_vector(gcas, v_means, ninputs) ;
	m_cov = load_sample_covariance_matrix(gcas, m_cov, ninputs) ;

	if (v_vals == NULL)
		v_vals = VectorClone(v_means) ;
	for (i = 0 ; i < ninputs ; i++)
		VECTOR_ELT(v_vals, i+1) = vals[i] ;

	VectorSubtract(v_means, v_vals, v_vals) ;  /* v_vals now has mean removed */
	m_cov_inv = MatrixInverse(m_cov, m_cov_inv) ;
	if (!m_cov_inv)
		ErrorExit(ERROR_BADPARM, "singular covariance matrix!") ;

	MatrixMultiply(m_cov_inv, v_vals, v_means) ;  /* v_means is now inverse(cov) * v_vals */
	dsq = VectorDot(v_vals, v_means) ;

	return(dsq);
}
#if 0
static double
gcaComputeSampleConditionalDensity(GCA_SAMPLE *gcas, float *vals, int ninputs, int label)
{
  double  p, dist ;

  /* compute 1-d Mahalanobis distance */
#if 0
  if (label == Unknown)  /* uniform distribution */
  {
    p = 1.0/256.0f ;
  }
  else   /* Gaussian distribution */
#endif
  {
    dist = GCAsampleMahDist(gcas, vals, ninputs) ;
		p = 1 / sqrt(sample_covariance_determinant(gcas, ninputs)) * exp(-dist) ;
  }
  return(p) ;
}
#endif
int
GCAlabelExists(GCA *gca, MRI *mri, TRANSFORM *transform, int x, int y, int z, int label)
{
#if 0
	int  xn, yn, zn ;

	GCAsourceVoxelToNode(gca, mri, transform, x, y, z, &xn, &yn, &zn) ;
	if (GCAfindGC(gca, xn, yn, zn, label) == NULL)
		return(0) ;
	return(1) ;
#else
	return(GCAisPossible(gca, mri, label, transform, x, y, z)) ;
#endif
}

GC1D *
GCAfindSourceGC(GCA *gca, MRI *mri, TRANSFORM *transform, int x, int y, int z, int label)
{
	int  xn, yn, zn ;
	GC1D *gc ;

	GCAsourceVoxelToNode(gca, mri, transform, x, y, z, &xn, &yn, &zn) ;
	gc = GCAfindGC(gca, xn, yn, zn, label) ;
	return(gc) ;
}
int
GCAfreeSamples(GCA_SAMPLE **pgcas, int nsamples)
{
	GCA_SAMPLE *gcas ;
	int         i ;
	
	gcas = *pgcas ; *pgcas = NULL ;

	if (gcas == NULL)
		return(NO_ERROR) ;

	for (i = 0 ; i < nsamples ; i++)
	{
		if (gcas[i].means)
			free(gcas[i].means) ;
		if (gcas[i].covars)
			free(gcas[i].covars) ;
	}
	free(gcas) ;
	return(NO_ERROR) ;
}
int
GCAnormalizePD(GCA *gca, MRI *mri_inputs, TRANSFORM *transform)
{
	double     mri_PD  = 0.0, gca_PD = 0.0 ;
	int        n, x, y, z, brain_node, nvals, label ;
	GCA_PRIOR  *gcap ;
	GC1D       *gc ;
	Real      val ;

	if (mri_inputs->nframes != 2 || gca->ninputs != 2)
		ErrorReturn(ERROR_BADPARM,
								(ERROR_BADPARM, "GCAnormalizePD: invalid input size (%d) or gca size (%d)",
								 mri_inputs->nframes, gca->ninputs)) ;

	for (nvals = x = 0 ;  x < mri_inputs->width ; x++)
	{
		for (y = 0 ; y < mri_inputs->height ; y++)
		{
			for (z = 0 ; z < mri_inputs->depth ; z++)
			{
				gcap = getGCAP(gca, mri_inputs, transform, x, y, z) ;
				for (label = brain_node = 0, n = 0 ; !brain_node && n < gcap->nlabels ; n++)
					if (IS_WM(gcap->labels[n]) && gcap->priors[n] > 0.5)
					{
						brain_node = 1 ; 
						label = gcap->labels[n] ;
					}
				if (!brain_node)
					continue ;   /* no valid data here */
				gc = GCAfindSourceGC(gca, mri_inputs, transform, x, y, z, label) ;
				MRIsampleVolumeFrameType(mri_inputs, x, y, z, 1, SAMPLE_NEAREST, &val) ;
				nvals++ ;
				mri_PD += val ;
				gca_PD += gc->means[1] ;
			}
		}
	} 
	mri_PD /= (double)nvals ; gca_PD /= (double)nvals ;
	printf("mean PD in volume %2.1f, mean GCA PD %2.1f - scaling by %2.3f\n",
				 mri_PD, gca_PD, gca_PD / mri_PD) ;
	MRIscalarMulFrame(mri_inputs, mri_inputs, gca_PD / mri_PD, 1) ;

	return(NO_ERROR) ;
}
GCA *
GCAcreateFlashGCAfromParameterGCA(GCA *gca_T1PD, double *TR, double *fa, double *TE, int nflash)
{
	GCA        *gca_flash ;
	GCA_PRIOR  *gcap_src, *gcap_dst ;
	GCA_NODE   *gcan_src, *gcan_dst ;
	GC1D       *gc_src, *gc_dst ;
	MATRIX     *m_jacobian, *m_cov_src = NULL, *m_cov_dst = NULL, *m_jacobian_T = NULL, *m_tmp = NULL ;
	int        n, x, y, z, i, j, v, label_count = 0 ;
	double     T1, PD, label_means[MAX_GCA_INPUTS] ;

	if (gca_T1PD->ninputs != 2)
		ErrorExit(ERROR_BADPARM, 
							"GCAcreateFlashGCAfromParameterGCA: input gca must be T1/PD (ninputs=%d, should be 2", 
							gca_T1PD->ninputs) ;
	gca_flash = GCAalloc(nflash, gca_T1PD->prior_spacing, gca_T1PD->node_spacing,
											 gca_T1PD->node_width*gca_T1PD->node_spacing,
											 gca_T1PD->node_height*gca_T1PD->node_spacing,
											 gca_T1PD->node_depth*gca_T1PD->node_spacing, GCA_NO_FLAGS) ;

	m_jacobian = MatrixAlloc(gca_flash->ninputs, gca_T1PD->ninputs, MATRIX_REAL) ;

	/* first copy over priors */
	for (x = 0 ; x < gca_flash->prior_width ; x++)
	{
		for (y = 0 ; y < gca_flash->prior_height ; y++)
		{
			for (z = 0 ; z < gca_flash->prior_depth ; z++)
			{
				gcap_src = &gca_T1PD->priors[x][y][z] ;
				gcap_dst = &gca_flash->priors[x][y][z] ;
				gcap_dst->nlabels = gcap_src->nlabels ;
				if (gcap_src->nlabels > gcap_dst->max_labels)
				{
					free(gcap_dst->priors) ;
					free(gcap_dst->labels) ;

          gcap_dst->labels = (unsigned char *)calloc(gcap_src->nlabels, sizeof(unsigned char)) ;
          if (!gcap_dst->labels)
            ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromParameterGCA: couldn't allocate %d labels",
                      gcap_src->nlabels) ;

					gcap_dst->priors = (float *)calloc(gcap_src->nlabels, sizeof(float)) ;
          if (!gcap_dst->priors)
            ErrorExit(ERROR_NOMEMORY,
                      "GCAcreateFlashGCAfromParameterGCA: couldn't allocate %d priors", 
											 gcap_src->nlabels) ;
					gcap_dst->max_labels = gcap_dst->nlabels ;
				}
				gcap_dst->total_training = gcap_src->total_training ;
				for (n = 0 ; n < gcap_src->nlabels ; n++)
				{
					gcap_dst->labels[n] = gcap_src->labels[n] ;
					gcap_dst->priors[n] = gcap_src->priors[n] ;
				}
			}
		}
	}

	/* now copy over classifiers and Markov stuff, using Jacobian to map to new image space */
	for (x = 0 ; x < gca_flash->node_width ; x++)
	{
		for (y = 0 ; y < gca_flash->node_height ; y++)
		{
			for (z = 0 ; z < gca_flash->node_depth ; z++)
			{
				if (x == Gx && y == Gy && z == Gz)
					DiagBreak()  ;
				gcan_src = &gca_T1PD->nodes[x][y][z] ; gcan_dst = &gca_flash->nodes[x][y][z] ;
				gcan_dst->nlabels = gcan_src->nlabels ;
				gcan_dst->total_training = gcan_src->total_training ;
				if (gcan_src->nlabels > gcan_dst->max_labels)
				{
					free(gcan_dst->labels) ;
					for (n = 0 ; n < gcan_dst->max_labels ; n++)
					{
						gc_dst = &gcan_dst->gcs[n] ;
						for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
						{
							if (gc_dst->label_priors[i])
								free(gc_dst->label_priors[i]) ;
							if (gc_dst->labels[i])
								free(gc_dst->labels[i]) ;
						}
						if (gc_dst->nlabels)
							free(gc_dst->nlabels) ;
						if (gc_dst->labels)
							free(gc_dst->labels) ;
						if (gc_dst->label_priors)
							free(gc_dst->label_priors) ;
					}

          gcan_dst->labels = (unsigned char *)calloc(gcan_src->nlabels, sizeof(unsigned char)) ;
          if (!gcan_dst->labels)
            ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromParameterGCA: couldn't allocate %d labels",
                      gcan_src->nlabels) ;

					gcan_dst->gcs = alloc_gcs(gcan_src->nlabels, GCA_NO_FLAGS, nflash) ;
				}
				for (n = 0 ; n < gcan_src->nlabels ; n++)
				{
					gcan_dst->labels[n] = gcan_src->labels[n] ;
					gc_src = &gcan_src->gcs[n] ; gc_dst = &gcan_dst->gcs[n] ;
					gc_dst->ntraining = gc_src->ntraining ;
					gc_dst->n_just_priors = gc_src->n_just_priors ;
					for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
					{
						gc_dst->nlabels[i] = gc_src->nlabels[i] ;
						gc_dst->label_priors[i] = (float *)calloc(gc_src->nlabels[i],sizeof(float));
						if (!gc_dst->label_priors[i])
							ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromParameterGCA: to %d",gc_src->nlabels[i]);
						gc_dst->labels[i] = (unsigned char *)calloc(gc_src->nlabels[i], sizeof(unsigned char)) ;
						if (!gc_dst->labels)
							ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromParameterGCA: to %d",gc_src->nlabels[i]);
						for (j = 0 ; j < gc_src->nlabels[i] ; j++)
						{
							gc_dst->label_priors[i][j]  = gc_src->label_priors[i][j] ;
							gc_dst->labels[i][j]  = gc_src->labels[i][j] ;
						}
					}

					/* now map intensity and covariance info over */
					T1 = gc_src->means[0] ; PD = gc_src->means[1] ;
					if (Ggca_label == gcan_dst->labels[n])
						label_count++ ; 
					for (i = 0 ; i < gca_flash->ninputs ; i++)
					{
						gc_dst->means[i] = 
							FLASHforwardModel(T1, PD, TR[i], fa[i], TE[i]) ;
						if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
								(Ggca_label < 0 || Ggca_label == gcan_src->labels[n]))
							printf("gcan(%d, %d, %d) %s: image[%d] (fa=%2.1f) predicted mean "
										 "(%2.1f,%2.1f) --> %2.1f\n",
										 x, y, z, cma_label_to_name(gcan_dst->labels[n]), i, DEGREES(fa[i]),
										 T1, PD, gc_dst->means[i]) ;
						*MATRIX_RELT(m_jacobian, i+1, 1) = dFlash_dT1(T1, PD, TR[i], fa[i], TE[i]) ;
						*MATRIX_RELT(m_jacobian, i+1, 2) = dFlash_dPD(T1, PD, TR[i], fa[i], TE[i]) ;
						if (gcan_dst->labels[n] == Ggca_label)
						{
							label_means[i] += gc_dst->means[i] ;
						}
					}
#define MIN_T1 50
					if (T1 < MIN_T1)
						m_cov_dst = MatrixIdentity(gca_flash->ninputs, m_cov_dst) ;
					else
					{
						m_cov_src = load_covariance_matrix(gc_src, m_cov_src, gca_T1PD->ninputs) ;
						m_jacobian_T = MatrixTranspose(m_jacobian, m_jacobian_T) ;
						m_tmp = MatrixMultiply(m_cov_src, m_jacobian_T, m_tmp) ;
						m_cov_dst = MatrixMultiply(m_jacobian, m_tmp, m_cov_dst) ;
					}
					for (v = i = 0 ; i < gca_flash->ninputs ; i++)
					{
						for (j = i ; j < gca_flash->ninputs ; j++, v++)
							gc_dst->covars[v] = *MATRIX_RELT(m_cov_dst, i+1, j+1) ;
					}
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label < 0 || Ggca_label == gcan_src->labels[n]))
					{
						printf("predicted covariance matrix:\n") ; MatrixPrint(stdout, m_cov_dst)  ;
					}
				}
			}
		}
	}

	if (Ggca_label >= 0)
	{
		printf("label %s (%d): means = ", cma_label_to_name(Ggca_label), Ggca_label) ;
		for (i = 0 ; i < gca_flash->ninputs ; i++)
		{
			label_means[i] /= (double)label_count ;
			printf("%2.1f ", label_means[i]) ;
		}
		printf("\n") ;
	}

	/* check and fix singular covariance matrixces */
	GCAfixSingularCovarianceMatrices(gca_flash) ;
	MatrixFree(&m_jacobian) ; MatrixFree(&m_cov_src) ; MatrixFree(&m_cov_dst) ;
	MatrixFree(&m_jacobian_T) ;

	return(gca_flash) ;
}

int
GCAfixSingularCovarianceMatrices(GCA *gca)
{
	int       x, y, z, fixed = 0, i, r, c, n, num, nparams, regularized = 0 ;
	GCA_NODE  *gcan ;
	GC1D      *gc, *gc_nbr ; 
	double    det, vars[MAX_GCA_INPUTS], min_det ;
	MATRIX    *m_cov_inv, *m_cov = NULL ;

	nparams = (gca->ninputs * (gca->ninputs+1))/2 + gca->ninputs ; /* covariance matrix and means */

	memset(vars, 0, sizeof(vars)) ;
	
  for (num = 0, x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						DiagBreak() ;
          gc = &gcan->gcs[n] ;
					det = covariance_determinant(gc, gca->ninputs) ;
					if ((gc->ntraining == 0 && det > 1) || (gc->ntraining*gca->ninputs > 2.5*nparams))  /* enough to estimate parameters */
					{
						m_cov = load_covariance_matrix(gc, m_cov, gca->ninputs) ;
						for (r = 0 ; r < gca->ninputs ; r++)
						{
							vars[r] += *MATRIX_RELT(m_cov,r+1, r+1) ;
						}
						num++ ;
					}
				}
			}
		}
	}
	if (m_cov)
		MatrixFree(&m_cov) ;
	if (num >= 1)
	{
		printf("average std = ") ;
		for (min_det = 1.0, r = 0 ; r < gca->ninputs ; r++)
		{
			vars[r] /= (float)num ;
			printf("%2.1f ", sqrt(vars[r])) ;
			min_det *= vars[r] ;
		}
		min_det = min_det / pow(10.0, gca->ninputs) ;
		printf("  using min determinant for regularization = %2.1f\n", min_det) ;
	}
	else
		min_det = MIN_DET ;

  for (regularized = fixed = x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						DiagBreak() ;
          gc = &gcan->gcs[n] ;
					det = covariance_determinant(gc, gca->ninputs) ;
					m_cov_inv = load_inverse_covariance_matrix(gc, NULL, gca->ninputs) ;
					
					if (det <= 0 || m_cov_inv == NULL)
					{
            fixed++ ;
            gc_nbr = findClosestValidGC(gca, x, y, z, gcan->labels[n], 1) ;
            if (!gc_nbr || 1)     /* always do this this - just to regularization */ 
            {
							for (i = r = 0 ; r < gca->ninputs ; r++)
							{
								for (c = r ; c < gca->ninputs ; c++, i++)
								{
									if (r == c)
										gc->covars[i] += vars[r] ;  /* mean of other variances at this location */
								}
							}
            }   /* found another valid gc for this label - use it's covariance matrix */
						else
						{
							for (i = r = 0 ; r < gca->ninputs ; r++)
							{
								gc->means[r] = gc_nbr->means[r] ; 
								for (c = r ; c < gca->ninputs ; c++, i++)
									gc->covars[i] = gc_nbr->covars[i] ;
							}
						}
						if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
								(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						{ 
							MATRIX *m ;
							printf("fixing singular covariance matrix for %s @ (%d, %d, %d):\n", 
										 cma_label_to_name(gcan->labels[n]), x, y, z) ; 
							m = load_covariance_matrix(gc, NULL, gca->ninputs) ;
							MatrixPrint(stdout, m) ;
							MatrixFree(&m) ;
						}
          }
					else   /* not singular - check if it is ill-conditioned */
					{
						if ((gc->ntraining*gca->ninputs < 2*nparams && det < 0.1) || (det < min_det))
						{
							regularized++ ;
							for (i = r = 0 ; r < gca->ninputs ; r++)
							{
								for (c = r ; c < gca->ninputs ; c++, i++)
								{
									if (r == c)
										gc->covars[i] += vars[r] ;  /* mean of overall variance in this image */
								}
							}
							if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
									(Ggca_label == gcan->labels[n] || Ggca_label < 0))
							{ 
								MATRIX *m ;
								printf("fixing ill-conditioned covariance matrix for %s @ (%d, %d, %d):\n", 
											 cma_label_to_name(gcan->labels[n]), x, y, z) ; 
								m = load_covariance_matrix(gc, NULL, gca->ninputs) ;
								MatrixPrint(stdout, m) ;
								MatrixFree(&m) ;
							}
						}
					}

					if (m_cov_inv)
						MatrixFree(&m_cov_inv) ;
					det = covariance_determinant(gc, gca->ninputs) ;
					m_cov_inv = load_inverse_covariance_matrix(gc, NULL, gca->ninputs) ;
					if (det <= min_det || m_cov_inv == NULL)
					{
						printf("warning: regularization of node (%d, %d, %d) label %s failed\n",
									 x, y, z, cma_label_to_name(gcan->labels[n])) ;
						DiagBreak() ;
						
					}
					if (m_cov_inv)
						MatrixFree(&m_cov_inv) ;
        }
      }
    }
  }

	printf("%d singular and %d ill-conditioned covariance matrices regularized\n", fixed, regularized) ;
	return(NO_ERROR) ;
}
int
GCAsetFlashParameters(GCA *gca, double *TRs, double *FAs, double *TEs)
{
	int n ;

	printf("setting GCA flash parameters to:\n") ;
	for (n = 0 ; n < gca->ninputs ; n++)
	{
		gca->TRs[n] = TRs[n] ;
		gca->FAs[n] = FAs[n] ;
		gca->TEs[n] = TEs[n] ;
		printf("TR=%2.1f msec, FA=%2.1f deg, TE=%2.1f msec\n", TRs[n], DEGREES(FAs[n]), TEs[n]) ;
	}
	gca->type = GCA_FLASH ;
	return(NO_ERROR) ;
}

GCA *
GCAcreateFlashGCAfromFlashGCA(GCA *gca_flash_src, double *TR, double *fa, double *TE, int nflash)
{
	GCA        *gca_flash_dst ;
	GCA_PRIOR  *gcap_src, *gcap_dst ;
	GCA_NODE   *gcan_src, *gcan_dst ;
	GC1D       *gc_src, *gc_dst ;
	MATRIX     *m_jac_dst, *m_jac_src, *m_cov_src, *m_cov_dst, *m_pinv_src, *m_cov_T1PD ;
	int        n, x, y, z, i, j, v, label_count = 0 ;
	double     T1, PD, label_means[MAX_GCA_INPUTS] ;

	m_cov_T1PD = m_cov_dst = m_pinv_src = m_cov_src = NULL ;
	FlashBuildLookupTables(gca_flash_src->ninputs, gca_flash_src->TRs, gca_flash_src->FAs, gca_flash_src->TEs) ;

	gca_flash_dst = GCAalloc(nflash, gca_flash_src->prior_spacing, gca_flash_src->node_spacing,
											 gca_flash_src->node_width*gca_flash_src->node_spacing,
											 gca_flash_src->node_height*gca_flash_src->node_spacing,
											 gca_flash_src->node_depth*gca_flash_src->node_spacing, GCA_NO_FLAGS) ;

	for (n = 0 ; n < nflash ; n++)
	{
		gca_flash_dst->TRs[n] = TR[n] ;
		gca_flash_dst->FAs[n] = fa[n] ;
		gca_flash_dst->TEs[n] = TE[n] ;
	}

	m_jac_src = MatrixAlloc(gca_flash_src->ninputs, 2, MATRIX_REAL) ;  /* T1/PD --> src jacobian */
	m_jac_dst = MatrixAlloc(gca_flash_dst->ninputs, 2, MATRIX_REAL) ;  /* T1/PD --> dst jacobian */

	/* first copy over priors */
	for (x = 0 ; x < gca_flash_dst->prior_width ; x++)
	{
		for (y = 0 ; y < gca_flash_dst->prior_height ; y++)
		{
			for (z = 0 ; z < gca_flash_dst->prior_depth ; z++)
			{
				gcap_src = &gca_flash_src->priors[x][y][z] ;
				gcap_dst = &gca_flash_dst->priors[x][y][z] ;
				gcap_dst->nlabels = gcap_src->nlabels ;
				if (gcap_src->nlabels > gcap_dst->max_labels)
				{
					free(gcap_dst->priors) ;
					free(gcap_dst->labels) ;

          gcap_dst->labels = (unsigned char *)calloc(gcap_src->nlabels, sizeof(unsigned char)) ;
          if (!gcap_dst->labels)
            ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromFlashGCA: couldn't allocate %d labels",
                      gcap_src->nlabels) ;

					gcap_dst->priors = (float *)calloc(gcap_src->nlabels, sizeof(float)) ;
          if (!gcap_dst->priors)
            ErrorExit(ERROR_NOMEMORY,
                      "GCAcreateFlashGCAfromFlashGCA: couldn't allocate %d priors", 
											 gcap_src->nlabels) ;
					gcap_dst->max_labels = gcap_dst->nlabels ;
				}
				gcap_dst->total_training = gcap_src->total_training ;
				for (n = 0 ; n < gcap_src->nlabels ; n++)
				{
					gcap_dst->labels[n] = gcap_src->labels[n] ;
					gcap_dst->priors[n] = gcap_src->priors[n] ;
				}
			}
		}
	}

	/* now copy over classifiers and Markov stuff, using Jacobian to map to new image space */
	for (x = 0 ; x < gca_flash_dst->node_width ; x++)
	{
		for (y = 0 ; y < gca_flash_dst->node_height ; y++)
		{
			for (z = 0 ; z < gca_flash_dst->node_depth ; z++)
			{
				if (x == Gx && y == Gy && z == Gz)
					DiagBreak()  ;
				gcan_src = &gca_flash_src->nodes[x][y][z] ; gcan_dst = &gca_flash_dst->nodes[x][y][z] ;
				gcan_dst->nlabels = gcan_src->nlabels ;
				gcan_dst->total_training = gcan_src->total_training ;
				if (gcan_src->nlabels > gcan_dst->max_labels)
				{
					free(gcan_dst->labels) ;
					for (n = 0 ; n < gcan_dst->max_labels ; n++)
					{
						gc_dst = &gcan_dst->gcs[n] ;
						for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
						{
							if (gc_dst->label_priors[i])
								free(gc_dst->label_priors[i]) ;
							if (gc_dst->labels[i])
								free(gc_dst->labels[i]) ;
						}
						if (gc_dst->nlabels)
							free(gc_dst->nlabels) ;
						if (gc_dst->labels)
							free(gc_dst->labels) ;
						if (gc_dst->label_priors)
							free(gc_dst->label_priors) ;
					}

          gcan_dst->labels = (unsigned char *)calloc(gcan_src->nlabels, sizeof(unsigned char)) ;
          if (!gcan_dst->labels)
            ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromFlashGCA: couldn't allocate %d labels",
                      gcan_src->nlabels) ;

					gcan_dst->gcs = alloc_gcs(gcan_src->nlabels, GCA_NO_FLAGS, nflash) ;
				}
				for (n = 0 ; n < gcan_src->nlabels ; n++)
				{
					gcan_dst->labels[n] = gcan_src->labels[n] ;
					gc_src = &gcan_src->gcs[n] ; gc_dst = &gcan_dst->gcs[n] ;
					gc_dst->ntraining = gc_src->ntraining ;
					gc_dst->n_just_priors = gc_src->n_just_priors ;
					for (i = 0 ; i < GIBBS_NEIGHBORS ; i++)
					{
						gc_dst->nlabels[i] = gc_src->nlabels[i] ;
						gc_dst->label_priors[i] = (float *)calloc(gc_src->nlabels[i],sizeof(float));
						if (!gc_dst->label_priors[i])
							ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromFlashGCA: to %d",gc_src->nlabels[i]);
						gc_dst->labels[i] = (unsigned char *)calloc(gc_src->nlabels[i], sizeof(unsigned char)) ;
						if (!gc_dst->labels)
							ErrorExit(ERROR_NOMEMORY, "GCAcreateFlashGCAfromFlashGCA: to %d",gc_src->nlabels[i]);
						for (j = 0 ; j < gc_src->nlabels[i] ; j++)
						{
							gc_dst->label_priors[i][j]  = gc_src->label_priors[i][j] ;
							gc_dst->labels[i][j]  = gc_src->labels[i][j] ;
						}
					}

					/* now map intensity and covariance info over */
					compute_T1_PD(gca_flash_src->ninputs, gc_src->means, gca_flash_src->TRs, gca_flash_src->FAs, gca_flash_src->TEs, 
												&T1, &PD) ;
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label < 0 || Ggca_label == gcan_src->labels[n]))
					{
						int j ;
						printf("gcan(%d, %d, %d) %s: means=[", x, y, z, cma_label_to_name(gcan_src->labels[n])) ;
						for (j = 0 ; j < gca_flash_src->ninputs ; j++)
							printf(" %2.1f", gc_src->means[j]) ;
						printf("] T1/PD=%2.1f/%2.1f\n", T1, PD) ;
					}
					if (Ggca_label == gcan_dst->labels[n])
						label_count++ ; 
					for (i = 0 ; i < gca_flash_dst->ninputs ; i++)
					{
						gc_dst->means[i] = 
							FLASHforwardModel(T1, PD, TR[i], fa[i], TE[i]) ;
						if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
								(Ggca_label < 0 || Ggca_label == gcan_src->labels[n]))
							printf("gcan(%d, %d, %d) %s: image[%d] (fa=%2.1f) predicted mean "
										 "(%2.1f,%2.1f) --> %2.1f\n",
										 x, y, z, cma_label_to_name(gcan_dst->labels[n]), i, DEGREES(fa[i]),
										 T1, PD, gc_dst->means[i]) ;
						*MATRIX_RELT(m_jac_dst, i+1, 1) = 
							dFlash_dT1(T1, PD, gca_flash_dst->TRs[i], gca_flash_dst->FAs[i], gca_flash_dst->TEs[i]) ;
						*MATRIX_RELT(m_jac_dst, i+1, 2) = 
							dFlash_dPD(T1, PD, gca_flash_dst->TRs[i], gca_flash_dst->FAs[i], gca_flash_dst->TEs[i]) ;

						*MATRIX_RELT(m_jac_src, i+1, 1) = 
							dFlash_dT1(T1, PD, gca_flash_src->TRs[i], gca_flash_src->FAs[i], gca_flash_src->TEs[i]) ;
						*MATRIX_RELT(m_jac_src, i+1, 2) = 
							dFlash_dPD(T1, PD, gca_flash_src->TRs[i], gca_flash_src->FAs[i], gca_flash_src->TEs[i]) ;
						if (gcan_dst->labels[n] == Ggca_label)
						{
							label_means[i] += gc_dst->means[i] ;
						}
					}
#define MIN_T1 50
					if (x == Gx && y == Gy && z == Gz)
						DiagBreak()  ;
					if (T1 < MIN_T1)
						m_cov_dst = MatrixIdentity(gca_flash_dst->ninputs, m_cov_dst) ;
					else
					{
						m_cov_src = load_covariance_matrix(gc_src, m_cov_src, gca_flash_src->ninputs) ;
						m_pinv_src = MatrixPseudoInverse(m_jac_src, m_pinv_src) ;
						m_cov_T1PD = MatrixSimilarityTransform(m_cov_src, m_pinv_src, m_cov_T1PD) ;
						m_cov_dst = MatrixSimilarityTransform(m_cov_T1PD, m_jac_dst, m_cov_dst) ;
					}
					for (v = i = 0 ; i < gca_flash_dst->ninputs ; i++)
					{
						for (j = i ; j < gca_flash_dst->ninputs ; j++, v++)
							gc_dst->covars[v] = *MATRIX_RELT(m_cov_dst, i+1, j+1) ;
					}
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label < 0 || Ggca_label == gcan_src->labels[n]))
					{
						printf("predicted covariance matrix:\n") ; MatrixPrint(stdout, m_cov_dst)  ;
					}
				}
			}
		}
	}

	if (Ggca_label >= 0)
	{
		printf("label %s (%d): means = ", cma_label_to_name(Ggca_label), Ggca_label) ;
		for (i = 0 ; i < gca_flash_dst->ninputs ; i++)
		{
			label_means[i] /= (double)label_count ;
			printf("%2.1f ", label_means[i]) ;
		}
		printf("\n") ;
	}

	/* check and fix singular covariance matrixces */
	GCAfixSingularCovarianceMatrices(gca_flash_dst) ;
	MatrixFree(&m_jac_dst) ; 
	MatrixFree(&m_jac_src) ; 
	if (m_cov_src)
		MatrixFree(&m_cov_src) ; 
	MatrixFree(&m_cov_dst) ;
	if (m_pinv_src)
		MatrixFree(&m_pinv_src) ;
	if (m_cov_T1PD)
		MatrixFree(&m_cov_T1PD) ;

	return(gca_flash_dst) ;
}
		 
int
GCAnormalizeMeans(GCA *gca, float target)
{
	int       x, y, z, frame, n ;
	double    norm ;
	Real      val ;
	GCA_NODE  *gcan ;
	GC1D      *gc ;

	for (x = 0 ; x < gca->node_width ; x++)
	{
		for (y = 0 ; y < gca->node_height ; y++)
		{
			for (z = 0 ; z < gca->node_depth ; z++)
			{
				gcan = &gca->nodes[x][y][z] ;
				for (n = 0 ; n < gcan->nlabels ; n++)
				{
					gc = &gcan->gcs[n] ;
					for (frame = 0, norm = 0 ; frame < gca->ninputs ; frame++)
					{
						val = gc->means[frame] ;
						norm += (val*val) ;
					}
					norm = sqrt(norm) / target ;
					if (FZERO(norm))
						norm = 1 ;				
					for (frame = 0 ; frame < gca->ninputs ; frame++)
						gc->means[frame] /= norm ;
				}
			}
		}
	}

	return(NO_ERROR) ;
}
int
GCAregularizeCovariance(GCA *gca, float regularize)
{
	int       x, y, z, i, r, c, n, num, nparams ;
	GCA_NODE  *gcan ;
	GC1D      *gc ; 
	double    det, vars[MAX_GCA_INPUTS] ;
	MATRIX    *m_cov = NULL ;

	nparams = (gca->ninputs * (gca->ninputs+1))/2 + gca->ninputs ; /* covariance matrix and means */
	memset(vars, 0, sizeof(vars)) ;
  for (num = 0, x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						DiagBreak() ;
          gc = &gcan->gcs[n] ;
					det = covariance_determinant(gc, gca->ninputs) ;
					if ((gc->ntraining == 0 && det > 1) || (gc->ntraining*gca->ninputs > 2.5*nparams))  /* enough to estimate parameters */
					{
						m_cov = load_covariance_matrix(gc, m_cov, gca->ninputs) ;
						for (r = 0 ; r < gca->ninputs ; r++)
						{
							vars[r] += *MATRIX_RELT(m_cov,r+1, r+1) ;
						}
						num++ ;
					}
				}
			}
		}
	}
	if (m_cov)
		MatrixFree(&m_cov) ;
	if (num >= 1)
	{
		for (r = 0 ; r < gca->ninputs ; r++)
		{
			vars[r] /= (float)num ;
			printf("average std[%d] = %2.1f\n", r, sqrt(vars[r])) ;
		}
	}
  for (x = 0 ; x < gca->node_width ; x++)
  {
    for (y = 0 ; y < gca->node_height ; y++)
    {
      for (z = 0 ; z < gca->node_depth ; z++)
      {
        if (x == Ggca_x && y == Ggca_y && z == Ggca_z)
          DiagBreak() ;
        gcan = &gca->nodes[x][y][z] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
						DiagBreak() ;
          gc = &gcan->gcs[n] ;
					for (i = r = 0 ; r < gca->ninputs ; r++)
					{
						for (c = r ; c < gca->ninputs ; c++, i++)
						{
							gc->covars[i] = (1-regularize)*gc->covars[i] ;
							if (r == c)
								gc->covars[i] += regularize*vars[r] ;  
						}
					}
					if (x == Ggca_x && y == Ggca_y && z == Ggca_z &&
							(Ggca_label == gcan->labels[n] || Ggca_label < 0))
					{ 
						MATRIX *m ;
						printf("regularizing covariance matrix for %s @ (%d, %d, %d):\n", 
									 cma_label_to_name(gcan->labels[n]), x, y, z) ; 
						m = load_covariance_matrix(gc, NULL, gca->ninputs) ;
						MatrixPrint(stdout, m) ;
						MatrixFree(&m) ;
					}
        }
      }
    }
  }
	return(NO_ERROR) ;
}

MATRIX  *
GCAlabelCovariance(GCA *gca, int label, MATRIX *m_total)
{
  int       xn, yn, zn, n ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;
  double    wt ;
  float     prior ;
	MATRIX    *m_cov = NULL ;

  /* compute overall white matter mean to use as anchor for rescaling */
  for (wt = 0.0, zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          /* find index in lookup table for this label */
          if (gcan->labels[n] != label)
            continue ;
          gc = &gcan->gcs[n] ;
          prior = get_node_prior(gca, label, xn, yn, zn) ;
          wt += prior ;
					m_cov = load_covariance_matrix(gc, m_cov, gca->ninputs) ;
					MatrixScalarMul(m_cov, prior, m_cov) ;
					if (m_total == NULL)
						m_total = MatrixCopy(m_cov, NULL) ;
					else
						MatrixAdd(m_cov, m_total, m_total) ;
        }
      }
    }
  }

	if (m_total == NULL)
		return(NULL) ;

	if (FZERO(wt))
		wt = 1 ;
	MatrixScalarMul(m_total, 1/wt, m_total) ;
	MatrixFree(&m_cov) ;
  return(m_total) ;
}

int
GCAlabelMeanFromImage(GCA *gca, TRANSFORM *transform, MRI *mri, int label, float *means)
{
  int       xn, yn, zn, n, r, xv, yv, zv ;
  GCA_NODE  *gcan ;
  GC1D      *gc ;
  double    wt, val ;
  float     prior ;
 double MIN_MEAN_PRIOR = 0.5 ;

  /* compute overall white matter mean to use as anchor for rescaling */
	memset(means, 0, gca->ninputs*sizeof(float)) ;
  for (wt = 0.0, zn = 0 ; zn < gca->node_depth ; zn++)
  {
    for (yn = 0 ; yn < gca->node_height ; yn++)
    {
      for (xn = 0 ; xn < gca->node_width ; xn++)
      {
        gcan = &gca->nodes[xn][yn][zn] ;
				GCAnodeToSourceVoxel(gca, mri, transform, xn, yn, zn, &xv, &yv, &zv) ;
        for (n = 0 ; n < gcan->nlabels ; n++)
        {
          /* find index in lookup table for this label */
          if (gcan->labels[n] != label)
            continue ;
          gc = &gcan->gcs[n] ;
          prior = get_node_prior(gca, label, xn, yn, zn) ;
					if (prior < MIN_MEAN_PRIOR)
						continue ;
          wt += prior ;
					for (r = 0 ; r < gca->ninputs ; r++)
					{
						MRIsampleVolumeFrame(mri, xv, yv, zv, r, &val) ;
						means[r] += val*prior ;
						if (!finite(gc->means[r]))
							DiagBreak() ;
					}
                           
        }
      }
    }
  }
	for (r = 0 ; r < gca->ninputs ; r++)
		means[r] /= wt ;
  return(NO_ERROR) ;
}

static double pthresh = 0.5 ;
int
GCAmapRenormalize(GCA *gca, MRI *mri, TRANSFORM *transform)
{
	HISTOGRAM *h, *hsmooth ;
	int       l, xp, yp, zp, x, y, z, nbins, i, xn, yn, zn, num, frame, bin ;
	float     fmin, fmax, prior, label_scales[MAX_GCA_LABELS], label_means[MAX_GCA_LABELS], 
            means[MAX_GCA_INPUTS],std, peak, smooth_peak ;
	Real      val/*, scale*/ ;
	GCA_PRIOR *gcap ;
	GCA_NODE  *gcan ;
	GC1D      *gc ;
	MATRIX    *m_cov ;

	/* for each class, build a histogram of values (weighted by priors) to determine
		 p(I|u,c) p(c).
	*/


#if 0
	if (gca->ninputs > 1)
		ErrorReturn(ERROR_UNSUPPORTED, 
								(ERROR_UNSUPPORTED, "GCAmapRenormalize: not implemented for ninputs > 1")) ;
#endif

	for (frame = 0 ; frame < mri->nframes ; frame++)
	{
		printf("renormalizing input #%d\n", frame) ;
		MRIvalRangeFrame(mri, &fmin, &fmax, frame) ;
		nbins = 256 ; h = HISTOalloc(nbins) ;
		
		hsmooth = HISTOcopy(h, NULL) ;
		for (l = 0 ; l <= MAX_GCA_LABELS ; l++)
		{
			label_scales[l] = 1 ;  /* mark it as unusable */
			GCAlabelMean(gca, l, means) ;
			m_cov = GCAlabelCovariance(gca, l, NULL) ;
			if (m_cov == NULL)
				continue ;
			std = 4*sqrt(*MATRIX_RELT(m_cov, frame+1,frame+1)) ;
			MatrixFree(&m_cov) ;
			label_means[l] = means[frame] ;
			printf("label %s (%d): mean = %2.3f +- %2.1f\n", cma_label_to_name(l), l, label_means[l], std) ;
			if (l == Gdiag_no)
				DiagBreak() ;
			if (FZERO(label_means[l]))
				continue ;
			HISTOclear(h, h) ;
			h->bin_size = (fmax-fmin)/255.0 ;
			if (h->bin_size < 1 && (mri->type == MRI_UCHAR || mri->type == MRI_SHORT))
				h->bin_size = 1 ;
			for (i = 0 ; i < nbins ; i++)
				h->bins[i] = (i+1)*h->bin_size ;

			for (num = xp = 0 ; xp < gca->prior_width ; xp++)
			{
				for (yp = 0 ; yp < gca->prior_height ; yp++)
				{
					for (zp = 0 ; zp < gca->prior_depth ; zp++)
					{
						if (xp == Gx && yp == Gy && zp == Gz)
							DiagBreak() ;
						gcap = &gca->priors[xp][yp][zp] ;
						prior = getPrior(gcap, l) ;
						if (prior < pthresh)
							continue ;
						GCApriorToSourceVoxel(gca, mri, transform, xp, yp, zp, &x, &y, &z) ;
						MRIsampleVolumeFrame(mri, x, y, z, frame, &val) ;
						bin = nint((val - fmin)/h->bin_size) ;
						if (bin >= h->nbins)
							bin = h->nbins-1 ;
						else if (bin < 0)
							bin = 0 ;
							
						h->counts[bin] += prior ; num++ ;
					}
				}
			}
			if (num <= 50)  /* not enough to reliably estimate density */
				continue ;
			HISTOfillHoles(h) ;
			if (l == Gdiag_no)
				DiagBreak() ;
			HISTOsmooth(h, hsmooth, 1) ;
			peak = h->bins[HISTOfindHighestPeakInRegion(h, 0, h->nbins)] ;
			smooth_peak = hsmooth->bins[HISTOfindHighestPeakInRegion(hsmooth, 0, hsmooth->nbins)] ;
			label_scales[l] = (float)smooth_peak / label_means[l] ;
			printf("%s (%d): peak at %2.2f, smooth at %2.2f (%d voxels), scaling by %2.2f\n", 
						 cma_label_to_name(l), l, peak, smooth_peak, num,
						 label_scales[l]) ;
			bin = nint((means[frame] - fmin)/hsmooth->bin_size) ;
			bin = HISTOfindCurrentPeak(hsmooth, bin, 11, .2) ;
			smooth_peak = hsmooth->bins[bin] ;
			if (bin < 0 || smooth_peak <= 0)
				continue ;
			label_scales[l] = (float)smooth_peak / label_means[l] ;
			printf("%s (%d): AFTER PRIOR: peak at %2.2f, smooth at %2.2f (%d voxels), scaling by %2.2f\n", 
						 cma_label_to_name(l), l, peak, smooth_peak, num,
						 label_scales[l]) ;

			if (l == Gdiag_no)
				DiagBreak() ;
			if (l >100)
				break ;
		}

		for (xn = 0 ; xn < gca->node_width ; xn++)
		{
			double     means_before[100], means_after[100], scales[100] ;
			int        labels[100], niter ;
			LABEL_PROB ranks_before[100], ranks_after[100] ;

			for (yn = 0 ; yn < gca->node_height ; yn++)
			{
				for (zn = 0 ; zn < gca->node_depth ; zn++)
				{
					if (xn == Ggca_x && yn == Ggca_y && zn == Ggca_z)
						DiagBreak() ;
					gcan = &gca->nodes[xn][yn][zn] ;
					if (gcan->nlabels <= 0)
						continue ;

					for (i = 0 ; i < gcan->nlabels ; i++)
					{
						gc = &gcan->gcs[i] ;
						l = gcan->labels[i] ;
						labels[i] = l ;
						scales[i] = label_scales[l] ;
						means_before[i] = gc->means[frame] ;
						ranks_before[i].label = l ;
						ranks_before[i].prob = means_before[i] ;
						ranks_before[i].index = i ;
					}
					qsort(ranks_before, gcan->nlabels, sizeof(LABEL_PROB), compare_sort_probabilities) ;
					niter = 0 ;
					for (i = 0 ; i < gcan->nlabels ; i++)
					{
						gc = &gcan->gcs[i] ;
						l = gcan->labels[i] ;
						means_after[i] = means_before[i]*scales[i] ;
						ranks_after[i].label = l ;
						ranks_after[i].prob = means_after[i] ;
						ranks_after[i].index = i ;
					}
					qsort(ranks_after, gcan->nlabels, sizeof(LABEL_PROB), compare_sort_probabilities) ;
					for (i = 0 ; i < gcan->nlabels ; i++)
					{
						if (ranks_before[i].label != ranks_after[i].label)
						{
							double diff, avg ;
							int    j ;

							/* two have swapped position - put them back in the right order */
							for (j = 0 ; j < gcan->nlabels ; j++)
								if (ranks_after[j].label == ranks_before[i].label)
									break ;
							diff = means_before[ranks_after[i].index] - means_before[ranks_before[i].index];
							avg = (means_after[ranks_after[i].index] + means_after[ranks_before[i].index]) / 2 ;
							ranks_after[i].prob = means_after[ranks_after[i].index] = avg+diff/4 ;
							ranks_after[j].prob = means_after[ranks_after[j].index] = avg-diff/4 ;
							qsort(ranks_after, gcan->nlabels, sizeof(LABEL_PROB), compare_sort_probabilities) ;
							i = -1 ;   /* start loop over */
							if (niter++ > 9)
							{
								DiagBreak() ;
								break ;
							}
							continue ;
						}
					}

					for (i = 0 ; i < gcan->nlabels ; i++)
					{
						if (FZERO(label_scales[gcan->labels[i]]))
							continue ;
						gc = &gcan->gcs[i] ;
						if ((xn == Ggca_x && yn == Ggca_y && zn == Ggca_z) &&
								(Ggca_label == gcan->labels[i] || Ggca_label < 0))
						{
							printf("scaling gc for label %s at (%d, %d, %d) from %2.1f to %2.1f\n",
										 cma_label_to_name(gcan->labels[i]), xn, yn, zn,
										 means_before[i], means_after[i]) ;
							DiagBreak() ;
						}
						gc->means[frame] = means_after[i] ;
					}
				}
			}
		}
	}

	return(NO_ERROR) ;
}

