/* cm_shim.c - wraps Yamagi's collision.c (built with the Q2 headers) for the SoF host. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "common/header/common.h"
#include "cm_shim.h"

/* --- what collision.c needs from the rest of the engine --- */
static cms_loadfile_t s_lf;
static cms_freefile_t s_ff;
static cvar_t s_entfile_cv;
cvar_t *sv_entfile = &s_entfile_cv;
void Com_Printf(char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
void Com_DPrintf(char *f, ...) { (void)f; }
void Com_Error(int c, char *f, ...) { va_list a; (void)c; va_start(a, f); printf("CM ERROR: "); vprintf(f, a); va_end(a); printf("\n"); exit(2); }
void Sys_Error(char *f, ...) { va_list a; va_start(a, f); printf("SYS ERROR: "); vprintf(f, a); va_end(a); printf("\n"); exit(2); }
cvar_t *Cvar_Get(char *n, char *v, int fl)
{
	cvar_t *c = (cvar_t *)calloc(1, sizeof(cvar_t));
	(void)fl;
	c->name = n; c->string = v; c->value = (float)atof(v);
	return c;
}
float Cvar_VariableValue(char *n) { (void)n; return 0; }
int FS_LoadFile(char *n, void **buf) { return s_lf ? s_lf(n, buf) : -1; }
void FS_FreeFile(void *b) { if (s_ff) s_ff(b); }
int FS_Read(void *b, int l, fileHandle_t f) { (void)b; (void)l; (void)f; return 0; }

void cms_setfs(cms_loadfile_t lf, cms_freefile_t ff) { s_lf = lf; s_ff = ff; }

int cms_load(const char *name, unsigned *checksum)
{
	static int swapped = 0;
	if (!swapped) { Swap_Init(); swapped = 1; }
	return CM_LoadMap((char *)name, false, checksum) != NULL;
}
const char *cms_entities(void) { return CM_EntityString(); }
int cms_numinline(void) { return CM_NumInlineModels(); }
int cms_inline(const char *name, float mins[3], float maxs[3], float origin[3])
{
	cmodel_t *m = CM_InlineModel((char *)name);
	VectorCopy(m->mins, mins); VectorCopy(m->maxs, maxs); VectorCopy(m->origin, origin);
	return m->headnode;
}
int cms_numclusters(void) { return CM_NumClusters(); }

static void conv(trace_t *s, cms_trace_t *t)
{
	t->allsolid = s->allsolid; t->startsolid = s->startsolid; t->fraction = s->fraction;
	VectorCopy(s->endpos, t->endpos);
	VectorCopy(s->plane.normal, t->normal);
	t->dist = s->plane.dist; t->planetype = s->plane.type; t->signbits = s->plane.signbits;
	t->surface = s->surface;
	t->surfname = s->surface ? s->surface->name : "";
	t->surfflags = s->surface ? s->surface->flags : 0;
	t->surfvalue = s->surface ? s->surface->value : 0;
	t->contents = s->contents;
}
void cms_boxtrace(const float *s, const float *e, const float *mins, const float *maxs, int headnode, int mask, cms_trace_t *t)
{
	trace_t r = CM_BoxTrace((float *)s, (float *)e, (float *)mins, (float *)maxs, headnode, mask);
	conv(&r, t);
}
void cms_transformedtrace(const float *s, const float *e, const float *mins, const float *maxs, int headnode, int mask,
                          const float *origin, const float *angles, cms_trace_t *t)
{
	trace_t r = CM_TransformedBoxTrace((float *)s, (float *)e, (float *)mins, (float *)maxs, headnode, mask,
	                                   (float *)origin, (float *)angles);
	conv(&r, t);
}
int cms_pointcontents(const float *p, int headnode) { return CM_PointContents((float *)p, headnode); }
int cms_transformedpointcontents(const float *p, int headnode, const float *origin, const float *angles)
{ return CM_TransformedPointContents((float *)p, headnode, (float *)origin, (float *)angles); }
int cms_headnodeforbox(const float *mins, const float *maxs) { return CM_HeadnodeForBox((float *)mins, (float *)maxs); }
int cms_boxleafnums(const float *mins, const float *maxs, int *list, int listsize, int *topnode)
{ return CM_BoxLeafnums((float *)mins, (float *)maxs, list, listsize, topnode); }
int cms_pointleafnum(const float *p) { return CM_PointLeafnum((float *)p); }
int cms_leafcluster(int leaf) { return CM_LeafCluster(leaf); }
int cms_leafarea(int leaf) { return CM_LeafArea(leaf); }
int cms_areasconnected(int a, int b) { return CM_AreasConnected(a, b); }
void cms_setportal(int portal, int open) { CM_SetAreaPortalState(portal, open); }
const unsigned char *cms_clusterpvs(int cluster) { return CM_ClusterPVS(cluster); }
const unsigned char *cms_clusterphs(int cluster) { return CM_ClusterPHS(cluster); }
int cms_headnodevisible(int headnode, const unsigned char *vis) { return CM_HeadnodeVisible(headnode, (byte *)vis); }
