/*
  Name:    vol2roi.c
  Author:  Douglas N. Greve 
  email:   analysis-bugs@nmr.mgh.harvard.edu
  Date:    1/2/00
  Purpose: averages the voxels within an ROI. The ROI
           can be constrained structurally (with a label file)
           and/or functionally (with a volumetric mask)
  $Id: mri_vol2roi.c,v 1.13 2003/04/16 18:58:57 kteich Exp $
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <string.h>

#include "MRIio_old.h"
#include "mri.h"
#include "macros.h"
#include "error.h"
#include "diag.h"
#include "proto.h"
#include "label.h"

#include "bfileio.h"
#include "registerio.h"
#include "resample.h"
#include "corio.h"
#include "selxavgio.h"
#include "mri2.h"
#include "version.h"

LABEL   *LabelReadFile(char *labelfile);

static int  parse_commandline(int argc, char **argv);
static void check_options(void);
static void print_usage(void) ;
static void usage_exit(void);
static void print_help(void) ;
static void print_version(void) ;
static void argnerr(char *option, int n);
static void dump_options(FILE *fp);
static int  singledash(char *flag);
static int  check_format(char *fmt);
/*static int  isoptionflag(char *flag);*/

int CompleteResFOVDim(float **trgres, float **trgfov, int **trgdim);
int CountLabelHits(MRI *SrcVol, MATRIX *Qsrc, MATRIX *Fsrc, 
       MATRIX *Wsrc, MATRIX *Dsrc, 
       MATRIX *Msrc2lbl, LABEL *Label, float labelfillthresh,
       int float2int);

int main(int argc, char *argv[]) ;

static char vcid[] = "$Id: mri_vol2roi.c,v 1.13 2003/04/16 18:58:57 kteich Exp $";
char *Progname = NULL;

char *roifile    = NULL;
char *roifmt     = "bvolume";
char *roitxtfile = NULL;
int  oldtxtstyle = 0;
int  plaintxtstyle = 0;

char *srcvolid   = NULL;
char *srcfmt     = NULL;
char *srcregfile = NULL;
char *srcwarp    = NULL;
int   srcoldreg  = 0;

char *labelfile   = NULL;
char *src2lblregfile = NULL;

char *mskvolid   = NULL;
char *mskfmt     = NULL;
char *mskregfile = NULL;
char *msk2srcregfile = NULL;
int   msksamesrc  = 1;

float  mskthresh = 0.5;
char  *msktail = "abs";
int    mskinvert = 0;
int    mskframe = 0;

char  *finalmskvolid = NULL;

LABEL *Label;

char  *float2int_string = NULL;
int    float2int = -1;

int debug = 0;

MATRIX *Dsrc, *Wsrc, *Fsrc, *Qsrc;
MATRIX *Dmsk, *Wmsk, *Fmsk, *Qmsk;
MATRIX *Msrc2lbl;
MATRIX *Mmsk2src;

SXADAT *sxa;

char *SUBJECTS_DIR = NULL;
char *FS_TALAIRACH_SUBJECT = NULL;
char *srcsubject, *msksubject;
char *regfile = "register.dat";
MRI *mSrcVol, *mROI, *mMskVol, *mSrcMskVol, *mFinalMskVol;
FILE *fp;
int nmskhits, nlabelhits, nfinalhits;

char tmpstr[2000];
int float2int_src, float2int_msk;
float labelfillthresh = .0000001;

int main(int argc, char **argv)
{
  int n,err, f, nhits, r,c,s;
  int nrows_src, ncols_src, nslcs_src, nfrms, endian, srctype;
  int nrows_msk, ncols_msk, nslcs_msk, msktype;
  int roitype;
  float ipr, bpr, intensity;
  float colres_src, rowres_src, slcres_src;
  float colres_msk, rowres_msk, slcres_msk;
  float *framepower=NULL, val;
  LTA *lta;
  int nargs;

  /* rkt: check for and handle version tag */
  nargs = handle_version_option (argc, argv, "$Id: mri_vol2roi.c,v 1.13 2003/04/16 18:58:57 kteich Exp $");
  if (nargs && argc - nargs == 1)
    exit (0);
  argc -= nargs;

  Progname = argv[0] ;
  argc --;
  argv++;
  ErrorInit(NULL, NULL, NULL) ;
  DiagInit(NULL, NULL, NULL) ;

  if(argc == 0) usage_exit();

  parse_commandline(argc, argv);
  check_options();

  printf("--------------------------------------------------------\n");
  getcwd(tmpstr,2000);
  printf("%s\n",tmpstr);
  printf("%s\n",Progname);
  for(n=0;n<argc;n++) printf(" %s",argv[n]);
  printf("\n");
  printf("version %s\n",vcid);
  printf("--------------------------------------------------------\n");

  dump_options(stdout);

  /* ------------ get info about the source volume ------------------*/
  err = bf_getvoldim(srcvolid,&nrows_src,&ncols_src,
         &nslcs_src,&nfrms,&endian,&srctype);
  if(err) exit(1);
  /* Dsrc: read the source registration file */
  if(srcregfile != NULL){
    err = regio_read_register(srcregfile, &srcsubject, &ipr, &bpr, 
            &intensity, &Dsrc, &float2int_src);
    if(err) exit(1);
    colres_src = ipr; /* in-plane resolution */
    rowres_src = ipr; /* in-plane resolution */
    slcres_src = bpr; /* between-plane resolution */
  }
  else{
    Dsrc = NULL;
    colres_src = 1; /* in-plane resolution */
    rowres_src = 1; /* in-plane resolution */
    slcres_src = 1; /* between-plane resolution */
  }
  /* Wsrc: Get the source warping Transform */
  Wsrc = NULL;
  /* Fsrc: Get the source FOV registration matrix */
  Fsrc = NULL;
  /* Qsrc: Compute the quantization matrix for src volume */
  Qsrc = FOVQuantMatrix(ncols_src,  nrows_src,  nslcs_src, 
      colres_src, rowres_src, slcres_src); 

  /* ----------- load in the label ----------------- */
  if(labelfile != NULL){
    Label = LabelReadFile(labelfile);
    if(Label == NULL) exit(1);
    /* load in the source-to-label registration */
    if(src2lblregfile != NULL){
      //err = regio_read_xfm(src2lblregfile, &Msrc2lbl);
      //if(err) exit(1);
      lta = LTAread(src2lblregfile);
      if(lta->type == LINEAR_VOX_TO_VOX){
  printf("INFO: converting LTA to RAS\n");
  LTAvoxelTransformToCoronalRasTransform(lta);
      }
      Msrc2lbl = lta->xforms[0].m_L;
      printf("-- Source2Label %s \n---- \n",src2lblregfile);
      MatrixPrint(stdout,Msrc2lbl);
      printf("-------------------------------\n");
    }
    else Msrc2lbl = NULL;
  }
  else{
    Label = NULL;
    Msrc2lbl = NULL;
  }

  /* -------------- load mask volume stuff -----------------------------*/
  if(mskvolid != NULL){
    /* get mask volume info */
    err = bf_getvoldim(mskvolid,&nrows_msk,&ncols_msk,
           &nslcs_msk,&nfrms,&endian,&msktype);
    if(err) exit(1);

    /* get the mask registration info */
    /* xyzFOV = Dmsk*xyzAnat (in mask space) */
    if(mskregfile != NULL){
      err = regio_read_register(mskregfile, &msksubject, &ipr, &bpr, 
        &intensity, &Dmsk, &float2int_msk);
      if(err) exit(1);
      colres_msk = ipr; /* in-plane resolution */
      rowres_msk = ipr; /* in-plane resolution */
      slcres_msk = bpr; /* between-plane resolution */
    }
    else if(msksamesrc){
      Dmsk = MatrixCopy(Dsrc,NULL);
      colres_msk = colres_src; /* in-plane resolution */
      rowres_msk = rowres_src; /* in-plane resolution */
      slcres_msk = slcres_src; /* between-plane resolution */
    }
    else{
      Dmsk = NULL;
      colres_msk = 1; /* in-plane resolution */
      rowres_msk = 1; /* in-plane resolution */
      slcres_msk = 1; /* between-plane resolution */
    }

    /* Qmsk: Compute the quantization matrix for msk volume */
    /* crsFOV = Qmsk*xyzFOV */
    Qmsk = FOVQuantMatrix(ncols_msk,  nrows_msk,  nslcs_msk, 
        colres_msk, rowres_msk, slcres_msk); 

    /* get the mask2source registration information */
    /* xyzSrc = Mmsk2src * xyzMsk */
    if(msk2srcregfile != NULL){
      err = regio_read_mincxfm(msk2srcregfile, &Mmsk2src);
      if(err) exit(1);
    }
    else Mmsk2src = NULL;

    /* load the mask volume (single frame) */
    mMskVol = mri_load_bvolume_frame(mskvolid, mskframe);
    if(mMskVol == NULL) exit(1);

    /* convert from Mask Anatomical to Src FOV */
    if(!msksamesrc){
      mSrcMskVol = vol2vol_linear(mMskVol, Qmsk, NULL, NULL, Dmsk, 
          Qsrc, Fsrc, Wsrc, Dsrc, 
          nrows_src, ncols_src, nslcs_src, 
          Mmsk2src, INTERP_NEAREST, float2int);
      if(mSrcMskVol == NULL) exit(1);
    }
    else mSrcMskVol = mMskVol;

    /* binarize the mask volume */
    mri_binarize(mSrcMskVol, mskthresh, msktail, mskinvert,
     mSrcMskVol, &nmskhits);
  }
  else {mSrcMskVol = NULL; nmskhits = 0;}
  /*-------------- Done loading mask stuff -------------------------*/

  /* --------- load in the (possibly 4-D) source volume --------------*/
  printf("Loading volume %s ...",srcvolid); fflush(stdout);
  mSrcVol = mri_load_bvolume(srcvolid);
  mSrcVol->xsize = colres_src;
  mSrcVol->ysize = rowres_src;
  mSrcVol->zsize = slcres_src;
  
  if(mSrcVol == NULL) exit(1);
  printf("done\n");
  /* If this is a statistical volume, raise each frame to it's appropriate
     power (eg, stddev needs to be squared)*/
  if(is_sxa_volume(srcvolid)){
    printf("INFO: Source volume detected as selxavg format\n");
    sxa = ld_sxadat_from_stem(srcvolid);
    if(sxa == NULL) exit(1);
    framepower = sxa_framepower(sxa,&f);
    if(f != mSrcVol->nframes){
      fprintf(stderr," number of frames is incorrect (%d,%d)\n",
        f,mSrcVol->nframes);
      exit(1);
    }
    printf("INFO: Adjusting Frame Power\n");  fflush(stdout);
    mri_framepower(mSrcVol,framepower);
  }

  /*--------- Prepare the final mask ------------------------*/
  if(Label != NULL){
    mFinalMskVol = label2mask_linear(mSrcVol, Qsrc, Fsrc, Wsrc, 
             Dsrc, mSrcMskVol,
             Msrc2lbl, Label, labelfillthresh, float2int, 
             &nlabelhits, &nfinalhits);

    if(mFinalMskVol == NULL) exit(1);
  }
  else {
    mFinalMskVol = mSrcMskVol;
    nfinalhits = nmskhits;
  }
  
  if(!oldtxtstyle){
    /* count the number of functional voxels = 1 in the mask */
    nfinalhits = 0;
    for(r=0;r<mFinalMskVol->height;r++){
      for(c=0;c<mFinalMskVol->width;c++){
  for(s=0;s<mFinalMskVol->depth;s++){
    val = MRIFseq_vox(mFinalMskVol,c,r,s,0); 
          if(val > 0.5) nfinalhits ++;
  }
      }
    }    
    if(Label != NULL)
      nlabelhits = CountLabelHits(mSrcVol, Qsrc, Fsrc, 
          Wsrc, Dsrc, Msrc2lbl, 
          Label, labelfillthresh,float2int);
    else  nlabelhits = 0;
  }

  /*-------------------------------------------------------*/
  /*--------- Map the volume into the ROI -----------------*/
  printf("Averging over ROI\n");  fflush(stdout);
  mROI = vol2maskavg(mSrcVol, mFinalMskVol,&nhits);
  if(mROI == NULL) exit(1);
  printf("Done averging over ROI (nhits = %d)\n",nhits);
  /*-------------------------------------------------------*/

  /* ------- Save the final mask ------------------ */
  if(finalmskvolid != 0){
    mri_save_as_bvolume(mFinalMskVol,finalmskvolid,endian,BF_FLOAT); 
  }

  /* If this is a statistical volume, lower each frame to it's appropriate
     power (eg, variance needs to be sqrt'ed) */
  if(is_sxa_volume(srcvolid)){
    printf("INFO: Readjusting Frame Power\n");  fflush(stdout);
    for(f=0; f < mROI->nframes; f++) framepower[f] = 1.0/framepower[f];
    mri_framepower(mROI,framepower);
  }

  /* save the target volume in an appropriate format */
  roitype = srctype;
  if(!strcasecmp(roifmt,"bshort") || !strcasecmp(roifmt,"bfloat") || 
     !strcasecmp(roifmt,"bfile")  || !strcasecmp(roifmt,"bvolume") ){
    /*-------------- bvolume --------------*/
    if(!strcasecmp(roifmt,"bfile")  || !strcasecmp(roifmt,"bvolume") )
      roitype = srctype;
    else if(!strcasecmp(roifmt,"bshort")) roitype = BF_SHORT;
    else if(!strcasecmp(roifmt,"bfloat")) roitype = BF_FLOAT;

    printf("Saving ROI to %s as bvolume\n",roifile); fflush(stdout);
    mri_save_as_bvolume(mROI,roifile,endian,roitype); 

    /* for a stat volume, save the .dat file */
    if(is_sxa_volume(srcvolid)) {
      sxa->nrows = 1;
      sxa->ncols = 1;
      sv_sxadat_by_stem(sxa,roifile);
    }
  }

  /* save as text */
  if(roitxtfile != NULL){
    fp = fopen(roitxtfile,"w");
    if(fp==NULL){
      fprintf(stderr,"ERROR: cannot open %s\n",roitxtfile);
      exit(1);
    }
    if(oldtxtstyle){
      printf("INFO: saving as old style txt\n");
      fprintf(fp,"%d \n",nmskhits);
    }
    if(! plaintxtstyle ){
      fprintf(fp,"%d \n",nlabelhits);
      fprintf(fp,"%d \n",nfinalhits);
    }
    for(f=0; f < mROI->nframes; f++) 
      fprintf(fp,"%9.4f\n",MRIFseq_vox(mROI,0,0,0,f));
    fclose(fp);
  }

  return(0);
}
/*--------------------------------------------------------------------*/
/* ------ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^------------ */
/*--------------------------------------------------------------------*/

/* ------------------------------------------------------------------ */
static int parse_commandline(int argc, char **argv)
{
  int  nargc , nargsused;
  char **pargv, *option ;

  if(argc < 1) usage_exit();

  nargc   = argc;
  pargv = argv;
  while(nargc > 0){

    option = pargv[0];
    if(debug) printf("%d %s\n",nargc,option);
    nargc -= 1;
    pargv += 1;

    nargsused = 0;

    if (!strcasecmp(option, "--help"))  print_help() ;

    else if (!strcasecmp(option, "--version")) print_version() ;

    else if (!strcasecmp(option, "--debug"))   debug = 1;

    else if (!strcasecmp(option, "--oldtxtstyle"))    oldtxtstyle = 1;
    else if (!strcasecmp(option, "--plaintxtstyle"))  plaintxtstyle = 1;
    else if (!strcasecmp(option, "--mskinvert"))  mskinvert = 1;

    /* -------- ROI output file ------ */
    else if (!strcmp(option, "--roiavgtxt")){
      if(nargc < 1) argnerr(option,1);
      roitxtfile = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--roiavg")){
      if(nargc < 1) argnerr(option,1);
      roifile = pargv[0];
      nargsused = 1;
    }

    /* -------- source volume inputs ------ */
    else if (!strcmp(option, "--srcvol")){
      if(nargc < 1) argnerr(option,1);
      srcvolid = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--srcfmt")){
      if(nargc < 1) argnerr(option,1);
      srcfmt = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--srcreg")){
      if(nargc < 1) argnerr(option,1);
      srcregfile = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--srcoldreg")){
      srcoldreg = 1;
    }
    else if (!strcmp(option, "--srcwarp")){
      if(nargc < 1) argnerr(option,1);
      srcwarp = pargv[0];
      nargsused = 1;
    }

    /* -------- label inputs ------ */
    else if(!strcmp(option, "--labelfile") || 
      !strcmp(option, "--label")){
      if(nargc < 1) argnerr(option,1);
      labelfile = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--labelreg")){
      if(nargc < 1) argnerr(option,1);
      src2lblregfile = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--labelfillthresh")){
      if(nargc < 1) argnerr(option,1);
      sscanf(pargv[0],"%f",&labelfillthresh);
      nargsused = 1;
    }

    /* -------- mask volume inputs ------ */
    else if (!strcmp(option, "--mskvol")){
      if(nargc < 1) argnerr(option,1);
      mskvolid = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--mskfmt")){
      if(nargc < 1) argnerr(option,1);
      mskfmt = pargv[0];
      nargsused = 1;
    }
    else if (!strcmp(option, "--mskreg")){
      if(nargc < 1) argnerr(option,1);
      mskregfile = pargv[0];
      nargsused = 1;
      msksamesrc = 0;
    }
    else if (!strcmp(option, "--msksamesrc")){
      msksamesrc = 1;
      nargsused = 0;
    }
    else if (!strcmp(option, "--msktail")){
      if(nargc < 1) argnerr(option,1);
      msktail = pargv[0];
      nargsused = 1;
      if(strncasecmp(msktail,"abs",3) &&
   strncasecmp(msktail,"pos",3) &&
   strncasecmp(msktail,"neg",3)){
  fprintf(stderr,"ERROR: msk tail = %s, must be abs, pos, or neg\n",
    msktail);
  exit(1);
      }
    }

    else if (!strcmp(option, "--mskthresh")){
      if(nargc < 1) argnerr(option,1);
      sscanf(pargv[0],"%f",&mskthresh);
      nargsused = 1;
    }

    else if (!strcmp(option, "--mskframe")){
      if(nargc < 1) argnerr(option,1);
      sscanf(pargv[0],"%d",&mskframe);
      nargsused = 1;
    }

    else if (!strcmp(option, "--finalmskvol")){
      if(nargc < 1) argnerr(option,1);
      finalmskvolid = pargv[0];
      nargsused = 1;
    }

    else if (!strcmp(option, "--float2int")){
      if(nargc < 1) argnerr(option,1);
      float2int_string = pargv[0];
      nargsused = 1;
    }
    else{
      fprintf(stderr,"ERROR: Option %s unknown\n",option);
      if(singledash(option))
  fprintf(stderr,"       Did you really mean -%s ?\n",option);
      printf("Match %d\n",strcmp(option, "--roiavgtxt"));
      exit(-1);
    }
    nargc -= nargsused;
    pargv += nargsused;
  }
  return(0);
}
/* ------------------------------------------------------ */
static void usage_exit(void)
{
  print_usage() ;
  exit(1) ;
}
/* --------------------------------------------- */
static void print_usage(void)
{
  fprintf(stderr, "USAGE: %s \n",Progname) ;
  fprintf(stderr, "\n");
  fprintf(stderr, "   --roiavg    output path for ROI average\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "   --srcvol    input volume path \n");
  fprintf(stderr, "   --srcfmt    input volume format \n");
  fprintf(stderr, "   --srcreg    source registration (SrcXYZ = R*AnatXYZ) \n");
  fprintf(stderr, "   --srcoldreg interpret srcreg as old-style reg.dat \n");
  fprintf(stderr, "   --srcwarp   source scanner warp table\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "   --label     path to label file \n");
  fprintf(stderr, "   --labelreg  label registration (LabelXYZ = L*AnatXYZ) \n");
  fprintf(stderr, "   --labelfillthresh thresh : fraction of voxel\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "   --mskvol     mask volume path \n");
  fprintf(stderr, "   --mskfmt     mask volume format \n");
  fprintf(stderr, "   --mskreg     mask registration  (MaskXYZ = M*AnatXYZ)\n");
  //fprintf(stderr, "   --msksamesrc mask volume has same FOV as source \n");
  fprintf(stderr, "\n");
  fprintf(stderr, "   --mskthresh threshold (0.5) mask threshold\n");
  fprintf(stderr, "   --msktail   <abs>, pos, or neg (mask tail) \n");
  fprintf(stderr, "   --mskframe  0-based mask frame <0> \n");
  fprintf(stderr, "   --mskinvert : invert the mask \n");
  fprintf(stderr, "\n");
  fprintf(stderr, "   --finalmskvol path in which to save final mask\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "   --float2int float-to-int conversion method (<round>, floor, or tkreg )\n");
  fprintf(stderr, "\n");
}
/* --------------------------------------------- */
static void print_help(void)
{
  print_usage() ;
  printf("\nThis program will resample one volume into an ROI. \n") ;
  exit(1) ;
}
/* --------------------------------------------- */
static void print_version(void)
{
  fprintf(stderr, "%s\n", vcid) ;
  exit(1) ;
}
/* --------------------------------------------- */
static void argnerr(char *option, int n)
{
  if(n==1)
    fprintf(stderr,"ERROR: %s flag needs %d argument\n",option,n);
  else
    fprintf(stderr,"ERROR: %s flag needs %d arguments\n",option,n);
  exit(-1);
}
/* --------------------------------------------- */
static void check_options(void)
{
  if(srcvolid == NULL){
    fprintf(stderr,"A source volume path must be supplied\n");
    exit(1);
  }
  if(roifile == NULL){
    fprintf(stderr,"A ROI output path must be supplied\n");
    exit(1);
  }

  if(msksamesrc && mskregfile != NULL){
    fprintf(stderr,"ERROR: cannot specify both --mskreg and --msksamesrc\n");
    exit(1);
  }

  if(msksamesrc) mskregfile = srcregfile;

  if(float2int_string == NULL) float2int_string = "round";
  float2int = float2int_code(float2int_string);
  if(float2int == -1){
    fprintf(stderr,"ERROR: float2int = %s\n",float2int_string);
    fprintf(stderr,"  must be either round, floor, or tkreg\n");
    exit(1);
  }

  if(srcfmt == NULL) srcfmt = "bvolume";
  check_format(srcfmt);

  return;
}

/* --------------------------------------------- */
int check_format(char *trgfmt)
{
  if( strcasecmp(trgfmt,"bvolume") != 0 &&
      strcasecmp(trgfmt,"bfile") != 0 &&
      strcasecmp(trgfmt,"bshort") != 0 &&
      strcasecmp(trgfmt,"bfloat") != 0 &&
      strcasecmp(trgfmt,"cor") != 0 ){
    fprintf(stderr,"ERROR: format %s unrecoginized\n",trgfmt);
    fprintf(stderr,"Legal values are: bvolume, bfile, bshort, bfloat, and cor\n");
    exit(1);
  }
  return(0);
}

/* --------------------------------------------- */
static void dump_options(FILE *fp)
{
  fprintf(fp,"roifile = %s\n",roifile);
  if(roitxtfile != NULL){
    fprintf(fp,"roitxtfile = %s\n",roitxtfile);
    fprintf(fp,"oldtxtstyle = %d\n",oldtxtstyle);
  }

  fprintf(fp,"srcvol = %s\n",srcvolid);
  if(srcfmt != NULL) fprintf(fp,"srcfmt = %s\n",srcfmt);
  else                  fprintf(fp,"srcfmt unspecified\n");
  if(srcregfile != NULL) fprintf(fp,"srcreg = %s\n",srcregfile);
  else                  fprintf(fp,"srcreg unspecified\n");
  fprintf(fp,"srcregold = %d\n",srcoldreg);
  if(srcwarp != NULL) fprintf(fp,"srcwarp = %s\n",srcwarp);
  else                   fprintf(fp,"srcwarp unspecified\n");

  if(labelfile != NULL){
    fprintf(fp,"label file = %s\n",labelfile);
    if(src2lblregfile != NULL) fprintf(fp,"labelreg = %s\n",src2lblregfile);
    else                     fprintf(fp,"labelreg unspecified\n");
    fprintf(fp,"label fill thresh = %g\n",labelfillthresh);
  }

  if(mskvolid != NULL) fprintf(fp,"mskvol = %s\n",mskvolid);
  else                  fprintf(fp,"mskvol unspecified\n");
  if(mskvolid != NULL) {
    if(mskfmt != NULL) fprintf(fp,"mskfmt = %s\n",mskfmt);
    else               fprintf(fp,"mskfmt unspecified\n");
    if(!msksamesrc){
      if(mskregfile != NULL) fprintf(fp,"mskreg = %s\n",mskregfile);
      else               fprintf(fp,"mskreg unspecified\n");
    }
    else fprintf(fp,"msk volume same as source\n");
    fprintf(fp,"msk tail = %s\n",msktail); 
    fprintf(fp,"msk threshold = %f\n",mskthresh); 
    fprintf(fp,"msk invert = %d\n",mskinvert); 
    fprintf(fp,"msk frame = %d\n",mskframe); 
  }

  if(float2int_string != NULL) fprintf(fp,"float2int = %s\n",float2int_string);
  else                  fprintf(fp,"float2int unspecified\n");

  return;
}
/*---------------------------------------------------------------*/
static int singledash(char *flag)
{
  int len;
  len = strlen(flag);
  if(len < 2) return(0);

  if(flag[0] == '-' && flag[1] != '-') return(1);
  return(0);
}
#if 0
/*---------------------------------------------------------------*/
static int isoptionflag(char *flag)
{
  int len;
  len = strlen(flag);
  if(len < 3) return(0);

  if(flag[0] != '-' || flag[1] != '-') return(0);
  return(1);
}
#endif

/*---------------------------------------------------------------*/
/*---------------------------------------------------------------*/
/*---------------------------------------------------------------*/
LABEL   *LabelReadFile(char *labelfile)
{
  LABEL  *area ;
  char   *fname, line[STRLEN], *cp;
  FILE   *fp ;
  int    vno, nlines ;
  float  x, y, z, stat ;

  fname = labelfile;

  area = (LABEL *)calloc(1, sizeof(LABEL)) ;
  if (!area)
    ErrorExit(ERROR_NOMEMORY,"%s: could not allocate LABEL struct.",Progname);

  /* read in the file */
  fp = fopen(fname, "r") ;
  if (!fp)
    ErrorReturn(NULL, (ERROR_NOFILE, "%s: could not open label file %s",
                       Progname, fname)) ;

  cp = fgetl(line, 199, fp) ;
  if (!cp)
    ErrorReturn(NULL,
                (ERROR_BADFILE, "%s: empty label file %s", Progname, fname)) ;
  if (!sscanf(cp, "%d", &area->n_points))
    ErrorReturn(NULL,
                (ERROR_BADFILE, "%s: could not scan # of lines from %s",
                 Progname, fname)) ;
  area->max_points = area->n_points ;
  area->lv = (LABEL_VERTEX *)calloc(area->n_points, sizeof(LABEL_VERTEX)) ;
  if (!area->lv)
    ErrorExit(ERROR_NOMEMORY, 
              "%s: LabelRead(%s) could not allocate %d-sized vector",
              Progname, labelfile, sizeof(LV)*area->n_points) ;
  nlines = 0 ;
  while ((cp = fgetl(line, 199, fp)) != NULL)
  {
    if (sscanf(cp, "%d %f %f %f %f", &vno, &x, &y, &z, &stat) != 5)
      ErrorReturn(NULL, (ERROR_BADFILE, "%s: could not parse %dth line in %s",
                Progname, area->n_points, fname)) ;
    area->lv[nlines].x = x ;
    area->lv[nlines].y = y ;
    area->lv[nlines].z = z ;
    area->lv[nlines].stat = stat ;
    area->lv[nlines].vno = vno ;
    nlines++ ;
  }

  fclose(fp) ;
  if (!nlines)
    ErrorReturn(NULL,
                (ERROR_BADFILE, 
                 "%s: no data in label file %s", Progname, fname));
  return(area) ;
}

/*------------------------------------------------------------
  int CountLabelHits(): This constructs a mask only from the
  label as a way to count the number of functional voxels in
  the label itself.
  ------------------------------------------------------------*/
int CountLabelHits(MRI *SrcVol, MATRIX *Qsrc, MATRIX *Fsrc, 
       MATRIX *Wsrc, MATRIX *Dsrc, 
       MATRIX *Msrc2lbl, LABEL *Label, float labelfillthresh,
       int float2int)
{
  MRI * LabelMskVol;
  int nlabelhits, nfinalhits;
  int r,c,s;
  float val;

  LabelMskVol = label2mask_linear(mSrcVol, Qsrc, Fsrc, Wsrc, 
          Dsrc, NULL, Msrc2lbl,
          Label, labelfillthresh, float2int, 
          &nlabelhits, &nfinalhits);

  nlabelhits = 0;
  for(r=0;r<LabelMskVol->height;r++){
    for(c=0;c<LabelMskVol->width;c++){
      for(s=0;s<LabelMskVol->depth;s++){
  val = MRIFseq_vox(LabelMskVol,c,r,s,0); 
  if(val > 0.5) nlabelhits ++;
      }
    }
  }    
  MRIfree(&LabelMskVol);
  return(nlabelhits);
}
