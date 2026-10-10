/*
 * q2side.c - Quake 2 game module face of the Soldier of Fortune adapter.
 *
 * The engine loads this like any game module. It owns the edict array the engine sees
 * ("mirrors"), forwards the game_export_t calls to the SoF game (sofside.cpp) and gives
 * the SoF side access to the engine through sofbridge.h.
 *
 * Built with the engine's own headers (game/header/game.h).
 */
#include "../../common/header/shared.h"
#include "../../game/header/game.h"
#include "sofbridge.h"
#include "../sof_client.h"

#define TAG_GAME 765  /* same values as the Q2 game (g_local.h) */
#define TAG_LEVEL 766

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* engine functions used directly (the adapter is linked against the engine) */
int   FS_LoadFile(char *path, void **buffer);
void  FS_FreeFile(void *buffer);
char **FS_ListFiles2(char *findname, int *numfiles, unsigned musthave, unsigned canthave);
void  FS_FreeList(char **list, int nfiles);
char *FS_Gamedir(void);
cmodel_t *CM_InlineModel(char *name);

static game_import_t gi;
static game_export_t ge;

#define MIRROR_MAX 1024
static edict_t *mirrors;
static gclient_t *mirrorClients;
static int maxclients;
static int sounds_used;

#define M(n) (&mirrors[(n)])
static int NUM(const edict_t *e) { return e ? (int)(e - mirrors) : -1; }

/* ---------------------------------------------------------------- world */

void q2b_trace(const float *start, const float *mins, const float *maxs, const float *end, int passent, int mask, q2b_trace_t *out)
{
	static vec3_t zero = { 0, 0, 0 };
	trace_t t = gi.trace((float *)start, mins ? (float *)mins : zero, maxs ? (float *)maxs : zero, (float *)end,
	                     passent >= 0 ? M(passent) : NULL, mask);
	out->allsolid = t.allsolid;
	out->startsolid = t.startsolid;
	out->fraction = t.fraction;
	VectorCopy(t.endpos, out->endpos);
	VectorCopy(t.plane.normal, out->normal);
	out->dist = t.plane.dist;
	out->planetype = t.plane.type;
	out->signbits = t.plane.signbits;
	out->surface = t.surface;
	out->surfname = t.surface ? t.surface->name : "";
	out->surfflags = t.surface ? t.surface->flags : 0;
	out->surfvalue = t.surface ? t.surface->value : 0;
	out->contents = t.contents;
	out->entnum = t.ent ? NUM(t.ent) : -1;
}

int q2b_inlinebounds(int num, float *mins, float *maxs)
{
	char name[16];
	cmodel_t *m;
	snprintf(name, sizeof(name), "*%d", num);
	m = CM_InlineModel(name);
	if (!m) return 0;
	VectorCopy(m->mins, mins);
	VectorCopy(m->maxs, maxs);
	return 1;
}

int q2b_pointcontents(const float *p) { return gi.pointcontents((float *)p); }

void q2b_setent(int num, const q2b_ent_t *e)
{
	edict_t *m;
	if (num < 0 || num >= MIRROR_MAX) return;
	m = M(num);
	m->inuse = e->inuse;
	m->s.number = num;
	VectorCopy(e->origin, m->s.origin);
	VectorCopy(e->angles, m->s.angles);
	VectorCopy(e->old_origin, m->s.old_origin);
	m->s.modelindex = e->modelindex;
	m->s.frame = e->frame;
	m->s.skinnum = e->skinnum;
	m->s.renderfx = e->renderfx;
	m->s.effects = e->effects;
	m->s.sound = e->sound;
	m->s.event = e->event;
	m->solid = (solid_t)e->solid;
	m->svflags = e->svflags;
	m->clipmask = e->clipmask;
	m->owner = (e->owner >= 0 && e->owner < MIRROR_MAX) ? M(e->owner) : NULL;
	VectorCopy(e->mins, m->mins);
	VectorCopy(e->maxs, m->maxs);
}

void q2b_link(int num, const q2b_ent_t *e, q2b_linkout_t *out)
{
	edict_t *m;
	int i;
	if (num < 0 || num >= MIRROR_MAX) return;
	q2b_setent(num, e);
	m = M(num);
	gi.linkentity(m);
	VectorCopy(m->absmin, out->absmin);
	VectorCopy(m->absmax, out->absmax);
	VectorCopy(m->size, out->size);
	out->linkcount = m->linkcount;
	out->num_clusters = m->num_clusters;
	for (i = 0; i < 16 && i < MAX_ENT_CLUSTERS; i++) out->clusternums[i] = m->clusternums[i];
	out->headnode = m->headnode;
	out->areanum = m->areanum;
	out->areanum2 = m->areanum2;
	out->s_solid = m->s.solid;
}

void q2b_unlink(int num)
{
	if (num < 0 || num >= MIRROR_MAX) return;
	gi.unlinkentity(M(num));
}

int q2b_boxedicts(const float *mins, const float *maxs, int *list, int maxcount, int areatype)
{
	edict_t *tmp[MIRROR_MAX];
	int i, n;
	if (maxcount > MIRROR_MAX) maxcount = MIRROR_MAX;
	n = gi.BoxEdicts((float *)mins, (float *)maxs, tmp, maxcount, areatype);
	for (i = 0; i < n; i++) list[i] = NUM(tmp[i]);
	return n;
}

int q2b_inpvs(const float *a, const float *b) { return gi.inPVS((float *)a, (float *)b); }
int q2b_inphs(const float *a, const float *b) { return gi.inPHS((float *)a, (float *)b); }
void q2b_setareaportalstate(int portal, int open) { gi.SetAreaPortalState(portal, open); }
int q2b_areasconnected(int a, int b) { return gi.AreasConnected(a, b); }

static int pm_passent, pm_alive;
static trace_t pm_trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
	edict_t *pass = pm_passent >= 0 ? M(pm_passent) : NULL;
	if (pm_alive)
		return gi.trace(start, mins, maxs, end, pass, MASK_PLAYERSOLID);
	return gi.trace(start, mins, maxs, end, pass, MASK_DEADSOLID);
}

void q2b_pmove(q2b_pmove_t *p)
{
	pmove_t pm;
	int i;
	memset(&pm, 0, sizeof(pm));
	pm.s.pm_type = (pmtype_t)p->pm_type;
	for (i = 0; i < 3; i++)
	{
		pm.s.origin[i] = p->origin[i];
		pm.s.velocity[i] = p->velocity[i];
		pm.s.delta_angles[i] = p->delta_angles[i];
		pm.cmd.angles[i] = p->angles[i];
	}
	pm.s.pm_flags = (byte)p->pm_flags;
	pm.s.pm_time = (byte)p->pm_time;
	pm.s.gravity = p->gravity;
	pm.cmd.msec = (byte)p->msec;
	pm.cmd.buttons = (byte)p->buttons;
	pm.cmd.forwardmove = p->forwardmove;
	pm.cmd.sidemove = p->sidemove;
	pm.cmd.upmove = p->upmove;
	pm.snapinitial = p->snapinitial;
	pm_passent = p->passent;
	pm_alive = !(p->pm_type == PM_DEAD || p->pm_type == PM_GIB);
	pm.trace = pm_trace;
	pm.pointcontents = gi.pointcontents;
	gi.Pmove(&pm);
	for (i = 0; i < 3; i++)
	{
		p->origin[i] = pm.s.origin[i];
		p->velocity[i] = pm.s.velocity[i];
		p->delta_angles[i] = pm.s.delta_angles[i];
		p->viewangles[i] = pm.viewangles[i];
		p->mins[i] = pm.mins[i];
		p->maxs[i] = pm.maxs[i];
	}
	p->pm_flags = pm.s.pm_flags;
	p->pm_time = pm.s.pm_time;
	p->numtouch = pm.numtouch;
	for (i = 0; i < pm.numtouch && i < 32; i++) p->touchents[i] = NUM(pm.touchents[i]);
	p->viewheight = pm.viewheight;
	p->groundentity = pm.groundentity ? NUM(pm.groundentity) : -1;
	p->watertype = pm.watertype;
	p->waterlevel = pm.waterlevel;
}

void q2b_setps(int num, const q2b_ps_t *s)
{
	player_state_t *ps;
	int i;
	if (num < 1 || num > maxclients) return;
	ps = &mirrorClients[num - 1].ps;
	ps->pmove.pm_type = (pmtype_t)s->pm_type;
	for (i = 0; i < 3; i++)
	{
		ps->pmove.origin[i] = s->origin[i];
		ps->pmove.velocity[i] = s->velocity[i];
		ps->pmove.delta_angles[i] = s->delta_angles[i];
		ps->viewangles[i] = s->viewangles[i];
		ps->viewoffset[i] = s->viewoffset[i];
		if (i == 2)
		{
			/* fits the network encoding, see SOF_VIEWOFFSET_BIAS */
			float v = s->viewoffset[2] - SOF_VIEWOFFSET_BIAS;
			ps->viewoffset[2] = v < -32.0f ? -32.0f : v > 31.75f ? 31.75f : v;
		}
		ps->kick_angles[i] = s->kick_angles[i];
	}
	ps->pmove.pm_flags = (byte)s->pm_flags;
	ps->pmove.pm_time = (byte)s->pm_time;
	ps->pmove.gravity = s->gravity;
	for (i = 0; i < 4; i++) ps->blend[i] = s->blend[i];
	ps->fov = s->fov;
	ps->rdflags = s->rdflags;
	for (i = 0; i < MAX_STATS && i < 32; i++) ps->stats[i] = s->stats[i];
	ps->gunindex = 0;
}

void q2b_setnumedicts(int n)
{
	if (n > MIRROR_MAX) n = MIRROR_MAX;
	if (n > ge.num_edicts) ge.num_edicts = n;
}

/* ---------------------------------------------------------------- resources */

int q2b_modelindex(const char *name) { return gi.modelindex((char *)name); }
int q2b_soundindex(const char *name)
{
	/* the Q2 protocol has 256 sound slots, SoF uses up to 356: keep the overflow local */
	if (sounds_used >= MAX_SOUNDS - 2) return 0;
	sounds_used++;
	return gi.soundindex((char *)name);
}
int q2b_imageindex(const char *name) { return gi.imageindex((char *)name); }
void q2b_configstring(int q2index, const char *s)
{
	if (q2index < 0 || q2index >= MAX_CONFIGSTRINGS) return;
	gi.configstring(q2index, (char *)s);
}
int q2b_cs_models(void) { return CS_MODELS; }
int q2b_cs_sounds(void) { return CS_SOUNDS; }
int q2b_cs_images(void) { return CS_IMAGES; }
int q2b_cs_lights(void) { return CS_LIGHTS; }
int q2b_max_sounds(void) { return MAX_SOUNDS; }
void q2b_sound(int ent, int channel, int soundindex, float volume, float attenuation, float timeofs)
{
	if (soundindex <= 0 || soundindex >= MAX_SOUNDS || ent < 0 || ent >= MIRROR_MAX) return;
	gi.sound(M(ent), channel, soundindex, volume, attenuation, timeofs);
}
void q2b_positioned_sound(const float *origin, int ent, int channel, int soundindex, float volume, float attenuation, float timeofs)
{
	if (soundindex <= 0 || soundindex >= MAX_SOUNDS) return;
	gi.positioned_sound((float *)origin, ent >= 0 && ent < MIRROR_MAX ? M(ent) : M(0), channel, soundindex, volume, attenuation, timeofs);
}

/* ---------------------------------------------------------------- text */

void q2b_bprint(int level, const char *s) { gi.bprintf(level, "%s", s); }
void q2b_dprint(const char *s) { gi.dprintf("%s", s); }
void q2b_cprint(int ent, int level, const char *s) { gi.cprintf(ent > 0 && ent < MIRROR_MAX ? M(ent) : NULL, level, "%s", s); }
void q2b_centerprint(int ent, const char *s) { if (ent > 0 && ent < MIRROR_MAX && M(ent)->client) gi.centerprintf(M(ent), "%s", s); }
void q2b_error(const char *s) { gi.error("%s", s); }

/* ---------------------------------------------------------------- cvars / commands / memory / files */

void *q2b_cvar(const char *name, const char *value, int flags) { return gi.cvar((char *)name, (char *)value, flags); }
void q2b_cvar_set(const char *name, const char *value) { gi.cvar_set((char *)name, (char *)value); }
void q2b_cvar_forceset(const char *name, const char *value) { gi.cvar_forceset((char *)name, (char *)value); }
const char *q2b_cvar_string(void *cv) { return cv ? ((cvar_t *)cv)->string : ""; }
float q2b_cvar_value(void *cv) { return cv ? ((cvar_t *)cv)->value : 0.0f; }
int q2b_argc(void) { return gi.argc(); }
const char *q2b_argv(int n) { return gi.argv(n); }
const char *q2b_args(void) { return gi.args(); }
void q2b_addcommandstring(const char *text) { gi.AddCommandString((char *)text); }
void *q2b_tagmalloc(int size, int tag) { return gi.TagMalloc(size, tag); }
void q2b_tagfree(void *p) { gi.TagFree(p); }
void q2b_freetags(int tag) { gi.FreeTags(tag); }
int q2b_loadfile(const char *path, void **buf) { return FS_LoadFile((char *)path, buf); }
void q2b_freefile(void *buf) { FS_FreeFile(buf); }
void q2b_listfiles(const char *dir, const char *ext, void (*cb)(const char *name, void *ctx), void *ctx)
{
	char pattern[256];
	int n = 0, i;
	char **list;
	snprintf(pattern, sizeof(pattern), "%s/*%s", dir, ext);
	list = FS_ListFiles2(pattern, &n, 0, 0);
	if (!list) return;
	for (i = 0; i < n - 1; i++) /* the last entry is NULL */
	{
		const char *p;
		if (!list[i]) continue;
		p = strrchr(list[i], '/');
		cb(p ? p + 1 : list[i], ctx);
	}
	FS_FreeList(list, n);
}
const char *q2b_gamedir(void) { return FS_Gamedir(); }

void q2b_getvrorigins(float *weaponoffset, float *weaponangles, float *hmdposition)
{
	if (gi.getVROrigins) gi.getVROrigins(weaponoffset, weaponangles, hmdposition);
	else { VectorClear(weaponoffset); VectorClear(weaponangles); VectorClear(hmdposition); }
}
void q2b_vibrate(float duration, int channel, float intensity)
{
	if (gi.HapticVibrate) gi.HapticVibrate(duration, channel, intensity);
}

/* ---------------------------------------------------------------- game_export_t */

static void G_Init(void)
{
	cvar_t *mc = gi.cvar("maxclients", "1", CVAR_SERVERINFO | CVAR_LATCH);
	int i;
	maxclients = (int)mc->value;
	if (maxclients < 1) maxclients = 1;
	mirrors = gi.TagMalloc(MIRROR_MAX * sizeof(edict_t), TAG_GAME);
	mirrorClients = gi.TagMalloc(maxclients * sizeof(gclient_t), TAG_GAME);
	memset(mirrors, 0, MIRROR_MAX * sizeof(edict_t));
	memset(mirrorClients, 0, maxclients * sizeof(gclient_t));
	ge.edicts = mirrors;
	ge.edict_size = sizeof(edict_t);
	ge.max_edicts = MIRROR_MAX;
	ge.num_edicts = maxclients + 1;
	for (i = 0; i < maxclients; i++)
		mirrors[i + 1].client = &mirrorClients[i];
	if (!sofb_init(maxclients))
		gi.error("Soldier of Fortune game module could not be started");
}

static void G_Shutdown(void)
{
	sofb_shutdown();
	gi.FreeTags(TAG_LEVEL);
	gi.FreeTags(TAG_GAME);
}

static void G_SpawnEntities(char *mapname, char *entities, char *spawnpoint)
{
	int i;
	for (i = 0; i < MIRROR_MAX; i++)
	{
		gclient_t *c = mirrors[i].client;
		memset(&mirrors[i], 0, sizeof(edict_t));
		mirrors[i].client = c;
	}
	sounds_used = 0;
	ge.num_edicts = maxclients + 1;
	sofb_spawnentities(mapname, entities, spawnpoint);
	sofb_syncmirrors();
}

static void G_WriteGame(char *filename, qboolean autosave) { (void)filename; (void)autosave; }
static void G_ReadGame(char *filename) { (void)filename; }
static void G_WriteLevel(char *filename) { (void)filename; }
static void G_ReadLevel(char *filename) { (void)filename; }

static qboolean G_ClientConnect(edict_t *ent, char *userinfo)
{
	int ok = sofb_clientconnect(NUM(ent), userinfo);
	if (ok) ent->inuse = true;
	return ok;
}
static void G_ClientBegin(edict_t *ent) { sofb_clientbegin(NUM(ent)); sofb_syncmirrors(); }
static void G_ClientUserinfoChanged(edict_t *ent, char *userinfo) { sofb_clientuserinfochanged(NUM(ent), userinfo); }
static void G_ClientDisconnect(edict_t *ent) { sofb_clientdisconnect(NUM(ent)); ent->inuse = false; sofb_syncmirrors(); }
static void G_ClientCommand(edict_t *ent) { sofb_clientcommand(NUM(ent)); }
static void G_ClientThink(edict_t *ent, usercmd_t *cmd)
{
	sofb_usercmd_t c;
	int i;
	for (i = 0; i < 3; i++) c.angles[i] = cmd->angles[i];
	c.forwardmove = cmd->forwardmove;
	c.sidemove = cmd->sidemove;
	c.upmove = cmd->upmove;
	c.msec = cmd->msec;
	c.buttons = cmd->buttons;
	c.lightlevel = cmd->lightlevel;
	sofb_clientthink(NUM(ent), &c);
	sofb_syncmirrors();
}
static void G_RunFrame(void)
{
	sofb_runframe();
	sofb_syncmirrors();
}
static void G_ServerCommand(void) {}

game_export_t *GetGameAPI(game_import_t *import)
{
	gi = *import;
	memset(&ge, 0, sizeof(ge));
	ge.apiversion = GAME_API_VERSION;
	ge.Init = G_Init;
	ge.Shutdown = G_Shutdown;
	ge.SpawnEntities = G_SpawnEntities;
	ge.WriteGame = G_WriteGame;
	ge.ReadGame = G_ReadGame;
	ge.WriteLevel = G_WriteLevel;
	ge.ReadLevel = G_ReadLevel;
	ge.ClientConnect = G_ClientConnect;
	ge.ClientBegin = G_ClientBegin;
	ge.ClientUserinfoChanged = G_ClientUserinfoChanged;
	ge.ClientDisconnect = G_ClientDisconnect;
	ge.ClientCommand = G_ClientCommand;
	ge.ClientThink = G_ClientThink;
	ge.RunFrame = G_RunFrame;
	ge.ServerCommand = G_ServerCommand;
	ge.edict_size = sizeof(edict_t);
	return &ge;
}
