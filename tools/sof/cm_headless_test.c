#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "common/header/common.h"
cvar_t *sv_entfile; 
static cvar_t cv; 
void Com_Printf(char *f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);}
void Com_DPrintf(char *f,...){}
void Com_Error(int c,char *f,...){va_list a;va_start(a,f);printf("ERR: ");vprintf(f,a);va_end(a);printf("\n");exit(2);}
void Sys_Error(char *f,...){va_list a;va_start(a,f);printf("SYSERR: ");vprintf(f,a);va_end(a);printf("\n");exit(2);}
cvar_t *Cvar_Get(char *n,char *v,int fl){static cvar_t c[16];static int k;cvar_t*r=&c[k++%16];r->name=n;r->string=v;r->value=atof(v);return r;}
float Cvar_VariableValue(char *n){return 0;}
int FS_LoadFile(char *n,void **buf){FILE*f=fopen(n,"rb");if(!f)return -1;fseek(f,0,SEEK_END);int l=ftell(f);rewind(f);void*b=malloc(l+1);fread(b,1,l,f);((char*)b)[l]=0;if(buf)*buf=b;else free(b);return l;}
void FS_FreeFile(void*b){free(b);}
int FS_Read(void*b,int l,fileHandle_t f){return 0;}
int main(int c,char**v){
 int cs; Swap_Init(); sv_entfile=&cv; cv.value=0;
 cmodel_t *m=CM_LoadMap(v[1],false,(unsigned*)&cs);
 printf("loaded: checksum %u, numclusters %d, numentities-text %d bytes, models %d\n",cs,CM_NumClusters(),(int)strlen(CM_EntityString()),CM_NumInlineModels());
 vec3_t o={0,0,0};
 /* trace down from the sky at several spots, count solid hits */
 float mn[3]={-16,-16,-24},mx[3]={16,16,32};
 int hits=0,tot=0,inl=0;
 for(float x=-3000;x<3000;x+=250)for(float y=-3000;y<3000;y+=250){
   vec3_t s={x,y,4000},e={x,y,-4000};
   trace_t t=CM_BoxTrace(s,e,mn,mx,0,MASK_SOLID);tot++;
   if(t.fraction<1&&!t.startsolid){hits++;}
   int ct=CM_PointContents(s,0); if(ct&CONTENTS_SOLID)inl++;
 }
 printf("traces: %d/%d hit world geometry, %d start points inside solid\n",hits,tot,inl);
 {vec3_t p={-3596,0,24};printf("player start contents: 0x%x (0=air)\n",CM_PointContents(p,0));
  vec3_t e={-3596,0,-500};trace_t t=CM_BoxTrace(p,e,mn,mx,0,MASK_SOLID);printf("down trace from start: fraction %.3f startsolid %d endz %.1f\n",t.fraction,t.startsolid,t.endpos[2]);
  vec3_t e2={-3596-2000,0,24};t=CM_BoxTrace(p,e2,mn,mx,0,MASK_SOLID);printf("horizontal trace: fraction %.3f endx %.1f\n",t.fraction,t.endpos[0]);}
 {vec3_t p={-3596,0,24},e={-3596,0,-500},z={0,0,0};trace_t t=CM_BoxTrace(p,e,z,z,0,MASK_SOLID);printf("point trace down: fraction %.3f startsolid %d floor z=%.1f\n",t.fraction,t.startsolid,t.endpos[2]);
  float m2[3]={-16,-16,-24},x2[3]={16,16,32};vec3_t p2={-3596,0,40};t=CM_BoxTrace(p2,e,m2,x2,0,MASK_SOLID);printf("box from z=40: startsolid %d endz %.1f\n",t.startsolid,t.endpos[2]);}
 return 0;}
