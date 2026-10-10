/*
 * sofbridge.h - plain C interface between the two halves of the SoF game adapter.
 *
 * The adapter is a Quake 2 game module (q2side.c, built with the engine's headers) that
 * hosts the Soldier of Fortune game module (sofside.cpp, built with the SoF SDK headers).
 * The two halves cannot share headers (trace_t, cvar_t, edict_t... differ), so they talk
 * through this file using only plain types. Entities are referred to by number; the Q2
 * side keeps a "mirror" edict per SoF edict, which is what the engine links, traces
 * against and sends to clients.
 */
#ifndef SOF_BRIDGE_H
#define SOF_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- provided by q2side.c */

typedef struct
{
	int allsolid, startsolid;
	float fraction;
	float endpos[3];
	float normal[3];
	float dist;
	int planetype, signbits;
	const void *surface;  /* unique per texinfo or NULL */
	const char *surfname;
	int surfflags, surfvalue;
	int contents;
	int entnum;           /* -1 = nothing */
} q2b_trace_t;

typedef struct
{
	int inuse;
	float origin[3], angles[3], old_origin[3];
	int modelindex, frame, skinnum, renderfx, effects, sound, event;
	int solid, svflags, clipmask, owner;   /* owner: entity number or -1 */
	float mins[3], maxs[3];
} q2b_ent_t;

typedef struct
{
	float absmin[3], absmax[3], size[3];
	int linkcount;
	int num_clusters;
	int clusternums[16];
	int headnode;
	int areanum, areanum2;
	int s_solid;
} q2b_linkout_t;

/* view/player state sent to the client */
typedef struct
{
	int pm_type;               /* Q2 numbering */
	float origin[3], velocity[3];
	int pm_flags, pm_time;
	float gravity;
	float delta_angles[3];
	float viewangles[3], viewoffset[3], kick_angles[3];
	float blend[4];
	float fov;
	int rdflags;
	short stats[32];
} q2b_ps_t;

typedef struct
{
	int pm_type;
	float origin[3], velocity[3];
	int pm_flags, pm_time;
	float gravity;
	float delta_angles[3];
	int msec, buttons;
	short angles[3];
	float forwardmove, sidemove, upmove;
	int snapinitial;
	int passent;
	int numtouch;
	int touchents[32];
	float viewangles[3];
	float viewheight;
	float mins[3], maxs[3];
	int groundentity;
	int watertype, waterlevel;
} q2b_pmove_t;

/* world */
void  q2b_trace(const float *start, const float *mins, const float *maxs, const float *end, int passent, int mask, q2b_trace_t *out);
int   q2b_pointcontents(const float *p);
int   q2b_inlinebounds(int modelnum, float *mins, float *maxs);
void  q2b_setent(int num, const q2b_ent_t *e);
void  q2b_link(int num, const q2b_ent_t *e, q2b_linkout_t *out);
void  q2b_unlink(int num);
int   q2b_boxedicts(const float *mins, const float *maxs, int *list, int maxcount, int areatype);
int   q2b_inpvs(const float *a, const float *b);
int   q2b_inphs(const float *a, const float *b);
void  q2b_setareaportalstate(int portal, int open);
int   q2b_areasconnected(int a, int b);
void  q2b_pmove(q2b_pmove_t *pm);
void  q2b_setps(int num, const q2b_ps_t *ps);
void  q2b_setnumedicts(int n);

/* configstrings / resources */
int   q2b_modelindex(const char *name);
int   q2b_soundindex(const char *name);
int   q2b_imageindex(const char *name);
void  q2b_configstring(int q2index, const char *s);
int   q2b_cs_models(void);
int   q2b_cs_sounds(void);
int   q2b_cs_images(void);
int   q2b_cs_lights(void);
int   q2b_max_sounds(void);
void  q2b_sound(int ent, int channel, int soundindex, float volume, float attenuation, float timeofs);
void  q2b_positioned_sound(const float *origin, int ent, int channel, int soundindex, float volume, float attenuation, float timeofs);

/* text */
void  q2b_bprint(int level, const char *s);
void  q2b_dprint(const char *s);
void  q2b_cprint(int ent, int level, const char *s);
void  q2b_centerprint(int ent, const char *s);
void  q2b_error(const char *s);

/* cvars (opaque handle = engine cvar) */
void *q2b_cvar(const char *name, const char *value, int flags);
void  q2b_cvar_set(const char *name, const char *value);
void  q2b_cvar_forceset(const char *name, const char *value);
const char *q2b_cvar_string(void *cv);
float q2b_cvar_value(void *cv);

/* commands */
int   q2b_argc(void);
const char *q2b_argv(int n);
const char *q2b_args(void);
void  q2b_addcommandstring(const char *text);

/* memory */
void *q2b_tagmalloc(int size, int tag);
void  q2b_tagfree(void *p);
void  q2b_freetags(int tag);

/* files (engine filesystem, pak aware) */
int   q2b_loadfile(const char *path, void **buf);
void  q2b_freefile(void *buf);
void  q2b_listfiles(const char *dir, const char *ext, void (*cb)(const char *name, void *ctx), void *ctx);
const char *q2b_gamedir(void);

/* VR (Quake2Quest) */
void  q2b_getvrorigins(float *weaponoffset, float *weaponangles, float *hmdposition);
void  q2b_vibrate(float duration, int channel, float intensity);

/* ---------------------------------------------------------------- provided by sofside.cpp */

typedef struct
{
	short angles[3];
	float forwardmove, sidemove, upmove;
	int msec, buttons, lightlevel;
} sofb_usercmd_t;

int   sofb_init(int maxclients);           /* load the SoF modules, call Init; returns 0 on failure */
void  sofb_shutdown(void);
void  sofb_spawnentities(const char *mapname, const char *entities, const char *spawnpoint);
int   sofb_clientconnect(int num, char *userinfo);
void  sofb_clientbegin(int num);
void  sofb_clientuserinfochanged(int num, char *userinfo);
void  sofb_clientdisconnect(int num);
void  sofb_clientcommand(int num);
void  sofb_clientthink(int num, const sofb_usercmd_t *cmd);
void  sofb_runframe(void);
void  sofb_syncmirrors(void);              /* copy SoF entity / player state into the mirrors */
int   sofb_numedicts(void);
void  sofb_writegame(const char *filename, int autosave);
void  sofb_readgame(const char *filename);
void  sofb_writelevel(const char *filename);
void  sofb_readlevel(const char *filename);

#ifdef __cplusplus
}
#endif
#endif
