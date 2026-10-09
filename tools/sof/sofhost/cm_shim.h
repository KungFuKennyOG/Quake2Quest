/* cm_shim.h - plain-C view of the Yamagi collision model, so code built against the
 * SoF headers (different trace_t/cvar_t layouts) can use it without type clashes. */
#ifndef CM_SHIM_H
#define CM_SHIM_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	int allsolid, startsolid;
	float fraction;
	float endpos[3];
	float normal[3];
	float dist;
	int planetype, signbits;
	const void *surface;     /* unique per texinfo, NULL if none */
	const char *surfname;
	int surfflags, surfvalue;
	int contents;
} cms_trace_t;

typedef int (*cms_loadfile_t)(const char *path, void **buf);
typedef void (*cms_freefile_t)(void *buf);
void cms_setfs(cms_loadfile_t lf, cms_freefile_t ff);

int  cms_load(const char *name, unsigned *checksum);
const char *cms_entities(void);
int  cms_numinline(void);
int  cms_inline(const char *name, float mins[3], float maxs[3], float origin[3]);  /* returns headnode */
int  cms_numclusters(void);
void cms_boxtrace(const float *s, const float *e, const float *mins, const float *maxs, int headnode, int mask, cms_trace_t *t);
void cms_transformedtrace(const float *s, const float *e, const float *mins, const float *maxs, int headnode, int mask,
                          const float *origin, const float *angles, cms_trace_t *t);
int  cms_pointcontents(const float *p, int headnode);
int  cms_transformedpointcontents(const float *p, int headnode, const float *origin, const float *angles);
int  cms_headnodeforbox(const float *mins, const float *maxs);
int  cms_boxleafnums(const float *mins, const float *maxs, int *list, int listsize, int *topnode);
int  cms_pointleafnum(const float *p);
int  cms_leafcluster(int leaf);
int  cms_leafarea(int leaf);
int  cms_areasconnected(int a, int b);
void cms_setportal(int portal, int open);
const unsigned char *cms_clusterpvs(int cluster);
const unsigned char *cms_clusterphs(int cluster);
int  cms_headnodevisible(int headnode, const unsigned char *vis);

#ifdef __cplusplus
}
#endif
#endif
