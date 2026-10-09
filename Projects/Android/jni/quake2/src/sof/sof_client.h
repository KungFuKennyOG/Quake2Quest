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
} sofmesh_t;

typedef struct sofdraw_s
{
	int nummeshes;
	const sofmesh_t *meshes;
} sofdraw_t;

/* return the meshes of an entity / of a client's view weapon at the current game time;
 * the data stays valid until the next call for the same entity */
typedef const sofdraw_t *(*sof_entitydraw_t)(int entnum);
typedef const sofdraw_t *(*sof_viewweapondraw_t)(int clientnum);

void CL_SoF_RegisterHooks(sof_entitydraw_t entity, sof_viewweapondraw_t viewweapon);

#ifdef __cplusplus
}
#endif
#endif
