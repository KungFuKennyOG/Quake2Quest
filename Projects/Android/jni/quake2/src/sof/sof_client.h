/*
 * sof_client.h - plain C interface used by the client and the renderer to draw
 * Soldier of Fortune GHOUL models. The meshes are produced by the SoF game adapter
 * (which owns the GHOUL instances) and registered through CL_SoF_RegisterHooks().
 */
#ifndef SOF_CLIENT_H
#define SOF_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/* renderfx bit the adapter sets on GHOUL entities */
#define RF_SOFGHOUL 0x00400000

typedef struct
{
	int numverts;
	const float *xyz;          /* 3 per vertex, model (entity) space */
	const float *st;           /* 2 per vertex */
	const float *normal;       /* 3 per vertex */
	int numindices;
	const unsigned short *indices;
	const char *skin;          /* texture path for the renderer, e.g. "ghoul/enemy/meso/b_basic.tga" */
	float rgba[4];
	/* effects: per-vertex colours (4 per vertex, drawn unlit) and blending */
	const unsigned char *colors;
	int blend;                 /* SOFBLEND_* */
	int nodepth;               /* draw on top of everything */
} sofmesh_t;

#define SOFBLEND_NONE        0 /* opaque with alpha-tested cut-outs, lit (models) */
#define SOFBLEND_ALPHA       1
#define SOFBLEND_ADD         2
#define SOFBLEND_SUBTRACT    3

typedef struct sofdraw_s
{
	int nummeshes;
	const sofmesh_t *meshes;
} sofdraw_t;

/* return the meshes of an entity / of a client's view weapon at the current game time;
 * the data stays valid until the next call for the same entity */
/* Quake 2 sends the view offset as signed bytes in 1/4 units (-32..31.75); SoF's eye
   height is 35. The server side subtracts this before sending, the client adds it back. */
#define SOF_VIEWOFFSET_BIAS 16.0f

/* lerpfrac: the client's interpolation fraction between the last two server frames */
typedef const sofdraw_t *(*sof_entitydraw_t)(int entnum, float lerpfrac);
typedef const sofdraw_t *(*sof_viewweapondraw_t)(int clientnum, float lerpfrac);

void CL_SoF_RegisterHooks(sof_entitydraw_t entity, sof_viewweapondraw_t viewweapon);

/* ---- SoF client effects (muzzle flashes, smoke, sparks, blood, effect sounds and lights) */
typedef struct
{
	float origin[3];
	float radius;
	float color[3];
} soffxlight_t;

typedef struct
{
	const char *name;          /* "weapons/dpistol/fire.wav" */
	float origin[3];
	int entnum;
	float volume, attenuation;
	int local;                 /* follows the player (view weapon) */
} soffxsound_t;

typedef struct
{
	sofdraw_t draw;            /* world-space quads, one mesh per texture/blend batch */
	int numlights;
	const soffxlight_t *lights;
	int numsounds;
	const soffxsound_t *sounds;
} soffxframe_t;

/* time in seconds; the view weapon pose is the one the client drew it with (gunvalid 0 = none) */
typedef const soffxframe_t *(*sof_fxframe_t)(float time, const float *vieworg, const float *viewangles,
                                             int gunvalid, const float *gunorigin, const float *gunangles, float gunscale);

void CL_SoF_RegisterFxHook(sof_fxframe_t fx);

#ifdef __cplusplus
}
#endif
#endif
