/*
 * sofside.cpp - Soldier of Fortune face of the adapter: loads the SoF game module
 * (gamex64.so / libsofgame.so) and the player module, implements the SoF game_import_t
 * on top of the engine (through sofbridge.h) and keeps the engine's mirror edicts in sync.
 *
 * Built with the SoF SDK headers (not part of this repository), see docs/sof/BUILDING.md.
 */
#include "q_shared.h"
#include "../qcommon/mathlib.h"
#include "../qcommon/pmove.h"
#include "../qcommon/configstring.h"
#include "../qcommon/ef_flags.h"
#include "game.h"
#include "../ghoul/ighoul.h"
#include "ghoul_engine.h"
#include "ghb_model.h"
#include "sofbridge.h"
#include "../sof_client.h"
#include "../fx/sof_fx.h"
#include "q_sh_fx.h"
/* the effect flags are redefined below as enums (sfx::EFF_*, EFAT_*) */
#undef EFF_SCALE
#undef EFF_NUMELEMS
#undef EFF_POS2
#undef EFF_DIR
#undef EFF_MIN
#undef EFF_MAX
#undef EFF_LIFETIME
#undef EFF_RADIUS
#undef EFAT_POS
#undef EFAT_ENT
#undef EFAT_BOLT
#undef EFAT_BOLTANDINST
#undef EFAT_POSTOWALL
#undef EFAT_ALTAXIS
#undef EFAT_HASFLAGS

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <map>
#include <set>
#include <string>
#include <vector>

static void registerClientHooks(void);
static void clearDrawCache(void);
static void fxRegister(void);
static void fxUnregister(void);
extern "C" void Pmove_SetSoFHeights(int sof) __attribute__((weak));
extern "C" void CL_SoF_RegisterHooks(sof_entitydraw_t, sof_viewweapondraw_t) __attribute__((weak));

static game_export_t *sge;          /* the SoF game */
static game_import_t sgi;
static void *g_gameLib, *g_playerLib;
static int g_maxclients = 1;
static int g_thinkClient = -1;
static int g_frame;

#define EDICT_NUM(n) ((edict_t *)((char *)sge->edicts + sge->edict_size * (n)))
#define NUM_FOR_EDICT(e) ((int)(((char *)(e) - (char *)sge->edicts) / sge->edict_size))
static int numOf(edict_t *e) { return e ? NUM_FOR_EDICT(e) : -1; }
static edict_t *edictOf(int n) { return (n >= 0 && sge && n < sge->max_edicts) ? EDICT_NUM(n) : 0; }

static std::string vfmt(const char *fmt, va_list ap)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	return buf;
}

/* ------------------------------------------------------------------ printing */

static void I_bprintf(int level, char *fmt, ...) { va_list a; va_start(a, fmt); std::string s = vfmt(fmt, a); va_end(a); q2b_bprint(level, s.c_str()); }
static void I_dprintf(char *fmt, ...) { va_list a; va_start(a, fmt); std::string s = vfmt(fmt, a); va_end(a); q2b_dprint(s.c_str()); }
static void I_cprintf(edict_t *e, int level, char *fmt, ...) { va_list a; va_start(a, fmt); std::string s = vfmt(fmt, a); va_end(a); q2b_cprint(numOf(e), level, s.c_str()); }
static void I_clprintf(edict_t *e, edict_t *, int, char *fmt, ...) { va_list a; va_start(a, fmt); std::string s = vfmt(fmt, a); va_end(a); q2b_cprint(numOf(e), 2, s.c_str()); }
static void I_welcomeprint(edict_t *) {}
static void I_centerprintf(edict_t *e, char *fmt, ...) { va_list a; va_start(a, fmt); std::string s = vfmt(fmt, a); va_end(a); q2b_centerprint(numOf(e), s.c_str()); }
static void I_cinprintf(edict_t *e, int, int, int, char *text) { q2b_centerprint(numOf(e), text); }
static void I_Con_ClearNotify(void) {}
static void I_error(char *fmt, ...) { va_list a; va_start(a, fmt); std::string s = vfmt(fmt, a); va_end(a); q2b_error(s.c_str()); }
static void I_Sys_ConsoleOutput(char *s) { q2b_dprint(s); }

/* ------------------------------------------------------------------ string packages (strip/*.sp) */

struct SPString { std::string text; bool centered; };
static std::map<unsigned short, SPString> g_strings;
static std::set<std::string> g_packages;

static void I_SP_Register(const char *pkg)
{
	std::string key(pkg);
	for (size_t i = 0; i < key.size(); i++) key[i] = (char)tolower((unsigned char)key[i]);
	if (g_packages.count(key)) return;
	g_packages.insert(key);
	char *buf = 0;
	int len = q2b_loadfile((std::string("strip/") + pkg + ".sp").c_str(), (void **)&buf);
	if (len < 0 || !buf) return;
	std::string t(buf, (size_t)len);
	q2b_freefile(buf);
	int id = 0, index = -1;
	bool centered = false;
	size_t p = 0;
	while (p < t.size())
	{
		size_t e = t.find('\n', p);
		if (e == std::string::npos) e = t.size();
		std::string line = t.substr(p, e - p);
		p = e + 1;
		while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) line.erase(line.size() - 1);
		size_t s = 0;
		while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) s++;
		line = line.substr(s);
		if (!line.compare(0, 3, "ID ")) id = atoi(line.c_str() + 3);
		else if (!line.compare(0, 6, "INDEX ")) { index = atoi(line.c_str() + 6); centered = false; }
		else if (!line.compare(0, 6, "FLAGS ")) centered = line.find("CENTERED") != std::string::npos;
		else if (!line.compare(0, 13, "TEXT_ENGLISH ") && index >= 0)
		{
			size_t q1 = line.find('"'), q2 = line.find_last_of('"');
			std::string raw = q1 != std::string::npos && q2 > q1 ? line.substr(q1 + 1, q2 - q1 - 1) : std::string();
			std::string o;
			for (size_t i = 0; i < raw.size(); i++)
			{
				if (raw[i] == '\\' && i + 1 < raw.size()) { char c = raw[++i]; o += c == 'n' ? '\n' : (c == 't' ? '\t' : c); }
				else if (raw[i] == '$' && !raw.compare(i, 3, "$P_"))
				{
					/* $P_<COLOR> colour codes: dropped (the Q2 console has no colours) */
					size_t j = i + 3;
					while (j < raw.size() && raw[j] >= 'A' && raw[j] <= 'Z') j++;
					/* colour names are upper case; keep the text that follows */
					static const char *colors[] = { "CYAN", "GREEN", "RED", "WHITE", "YELLOW", "BLUE", "ORANGE", "GREY", "PURPLE", "BLACK", 0 };
					size_t best = i + 3;
					for (int c = 0; colors[c]; c++)
						if (!raw.compare(i + 3, strlen(colors[c]), colors[c])) { best = i + 3 + strlen(colors[c]); break; }
					i = best - 1;
				}
				else o += raw[i];
			}
			SPString sp;
			sp.text = o;
			sp.centered = centered;
			g_strings[(unsigned short)((id << 8) | index)] = sp;
		}
	}
}

static const char *spText(unsigned short id)
{
	std::map<unsigned short, SPString>::iterator it = g_strings.find(id);
	return it == g_strings.end() ? "" : it->second.text.c_str();
}
static void spOut(edict_t *ent, unsigned short ID, va_list ap)
{
	std::map<unsigned short, SPString>::iterator it = g_strings.find(ID);
	if (it == g_strings.end()) return;
	std::string s = vfmt(it->second.text.c_str(), ap);
	if (it->second.centered) q2b_centerprint(ent ? numOf(ent) : 1, s.c_str());
	else if (ent) q2b_cprint(numOf(ent), 2, s.c_str());
	else q2b_bprint(2, s.c_str());
}
static void I_SP_Print(edict_t *ent, unsigned short ID, ...) { va_list a; va_start(a, ID); spOut(ent, ID, a); va_end(a); }
static void I_SP_Print_Obit(edict_t *ent, unsigned short ID, ...) { va_list a; va_start(a, ID); spOut(ent, ID, a); va_end(a); }
static int I_SP_SPrint(char *buffer, int size, unsigned short ID, ...)
{
	va_list a;
	va_start(a, ID);
	vsnprintf(buffer, (size_t)size, spText(ID), a);
	va_end(a);
	return (int)strlen(buffer);
}
static const char *I_SP_GetStringText(unsigned short ID) { return spText(ID); }
static void I_bcaption(int, unsigned short id) { q2b_centerprint(1, spText(id)); }
static void I_captionprintf(edict_t *e, unsigned short id) { q2b_centerprint(numOf(e), spText(id)); }

/* ------------------------------------------------------------------ cvars (SoF layout shadows of engine cvars) */

struct CvarShadow { cvar_t cv; void *engine; std::string last; };
static std::map<std::string, CvarShadow *> g_cvars;

static std::string lowerStr(const char *s)
{
	std::string r(s ? s : "");
	for (size_t i = 0; i < r.size(); i++) r[i] = (char)tolower((unsigned char)r[i]);
	return r;
}
static void refreshCvar(CvarShadow *c)
{
	const char *s = q2b_cvar_string(c->engine);
	if (c->last == s) return;
	c->last = s;
	free(c->cv.string);
	c->cv.string = strdup(s);
	c->cv.value = q2b_cvar_value(c->engine);
	c->cv.modified = true;
}
static cvar_t *I_cvar(const char *name, const char *value, int flags, cvarcommand_t command)
{
	std::string k = lowerStr(name);
	std::map<std::string, CvarShadow *>::iterator it = g_cvars.find(k);
	if (it != g_cvars.end()) { refreshCvar(it->second); return &it->second->cv; }
	CvarShadow *c = new CvarShadow;
	memset(&c->cv, 0, sizeof(c->cv));
	c->engine = q2b_cvar(name, value ? value : "", flags & 0x1f);
	c->cv.name = strdup(name);
	c->cv.string = strdup("");
	c->cv.flags = flags;
	c->cv.command = command;
	c->last = "\x01";
	refreshCvar(c);
	g_cvars[k] = c;
	return &c->cv;
}
static cvar_t *I_cvar_set(const char *name, const char *value)
{
	q2b_cvar_set(name, value);
	return I_cvar(name, value, 0, 0);
}
static void I_cvar_setvalue(const char *name, float v)
{
	char b[64];
	if (v == (float)(int)v) snprintf(b, sizeof(b), "%d", (int)v); else snprintf(b, sizeof(b), "%f", v);
	I_cvar_set(name, b);
}
static cvar_t *I_cvar_forceset(const char *name, const char *value)
{
	q2b_cvar_forceset(name, value);
	return I_cvar(name, value, 0, 0);
}
static char *I_cvar_info(int flag)
{
	static std::string s;
	s.clear();
	for (std::map<std::string, CvarShadow *>::iterator it = g_cvars.begin(); it != g_cvars.end(); ++it)
		if (it->second->cv.flags & flag) s += std::string("\\") + it->second->cv.name + "\\" + it->second->cv.string;
	return (char *)s.c_str();
}
static float I_cvar_variablevalue(const char *name) { return I_cvar(name, "0", 0, 0)->value; }
static void refreshAllCvars(void)
{
	for (std::map<std::string, CvarShadow *>::iterator it = g_cvars.begin(); it != g_cvars.end(); ++it)
		refreshCvar(it->second);
}

/* ------------------------------------------------------------------ commands, memory, files */

static int I_argc(void) { return q2b_argc(); }
static char *I_argv(int n) { return (char *)q2b_argv(n); }
static char *I_args(void) { return (char *)q2b_args(); }
static bool saveExists(const char *slot)
{
	std::string path = std::string(q2b_gamedir()) + "/save/" + slot + "/server.ssv";
	FILE *f = fopen(path.c_str(), "rb");
	if (f) fclose(f);
	return f != 0;
}

static void I_AddCommandString(const char *text)
{
	if (getenv("SOF_DEBUG")) { char b[300]; snprintf(b, sizeof(b), "[sof] AddCommandString: %s\n", text); q2b_dprint(b); }
	if (text && !strncmp(text, "respawn", 7) && (text[7] == 0 || text[7] == '\n' || text[7] == ' ' || text[7] == ';'))
	{
		/* SoF's engine restarted from the last save when you die in single player;
		   use the quick save if there is one, else the save made entering the level */
		static int lastRespawnFrame = -1000;
		if (g_frame - lastRespawnFrame < 20) return; /* one load per death (presses repeat it) */
		lastRespawnFrame = g_frame;
		q2b_addcommandstring(saveExists("quick") ? "load quick\n" : "load save0\n");
		return;
	}
	q2b_addcommandstring(text);
}
static void *I_TagMalloc(int size, int tag) { void *p = q2b_tagmalloc(size, tag); memset(p, 0, (size_t)size); return p; }
static void I_TagFree(void *p) { if (p) q2b_tagfree(p); }
static void I_FreeTags(int tag) { q2b_freetags(tag); }
static int I_FS_LoadFile(char *name, void **buf, bool) { return q2b_loadfile(name, buf); }
static void I_FS_FreeFile(void *buf) { q2b_freefile(buf); }
static char *I_FS_Userdir(void) { return (char *)q2b_gamedir(); }
static void I_FS_CreatePath(char *) {}
static int I_FS_FileExists(char *path) { void *b = 0; int n = q2b_loadfile(path, &b); if (b) q2b_freefile(b); return n >= 0; }

static int ghoulLoad(const char *p, void **b) { return q2b_loadfile(p, b); }
static void ghoulFree(void *b) { q2b_freefile(b); }
static void listCb(const char *name, void *ctx) { ((std::vector<std::string> *)ctx)->push_back(name); }
static void ghoulList(const char *dir, const char *ext, std::vector<std::string> &out) { q2b_listfiles(dir, ext, listCb, &out); }
static void ghoulPrintf(const char *fmt, ...) { va_list a; va_start(a, fmt); std::string s = vfmt(fmt, a); va_end(a); q2b_dprint(s.c_str()); }

/* ------------------------------------------------------------------ resources */

static std::vector<std::string> g_effects;
static int I_modelindex(const char *n) { return n && n[0] ? q2b_modelindex(n) : 0; }
static int I_soundindex(const char *n) { return n && n[0] ? q2b_soundindex(n) : 0; }
static int I_imageindex(const char *n) { return n && n[0] ? q2b_imageindex(n) : 0; }
static int I_effectindex(const char *n)
{
	if (!n || !n[0]) return 0;
	for (size_t i = 0; i < g_effects.size(); i++) if (!strcasecmp(g_effects[i].c_str(), n)) return (int)i + 1;
	if (g_effects.size() >= MAX_EFPACKS - 1) return 0;
	g_effects.push_back(n);
	return (int)g_effects.size();
}
static void I_unload_sound(const char *) {}
static qboolean I_FilterPacket(char *) { return false; }
static void I_CreateGhoulConfigStrings(void) {}

/* SoF and Q2 configstring layouts differ; forward what the Q2 client understands */
static void I_configstring(int num, char *s)
{
	if (num < CS_MODELS)
	{
		if (num <= CS_SKYROTATE) q2b_configstring(num, s); /* name, cdtrack, sky, skyaxis, skyrotate */
		return;
	}
	if (num >= CS_MODELS && num < CS_MODELS + MAX_MODELS) { q2b_configstring(q2b_cs_models() + (num - CS_MODELS), s); return; }
	if (num >= CS_SOUNDS && num < CS_SOUNDS + MAX_SOUNDS)
	{
		if (num - CS_SOUNDS < q2b_max_sounds()) q2b_configstring(q2b_cs_sounds() + (num - CS_SOUNDS), s);
		return;
	}
	if (num >= CS_IMAGES && num < CS_IMAGES + MAX_IMAGES) { q2b_configstring(q2b_cs_images() + (num - CS_IMAGES), s); return; }
	if (num >= CS_LIGHTS && num < CS_LIGHTS + MAX_LIGHTSTYLES) { q2b_configstring(q2b_cs_lights() + (num - CS_LIGHTS), s); return; }
}

/* ------------------------------------------------------------------ world */

static int g_ghoulModelIndex; /* "#sofghoul": marks GHOUL entities so the server sends them */
static void entToBridge(edict_t *e, q2b_ent_t *b)
{
	memset(b, 0, sizeof(*b));
	b->inuse = e->inuse;
	VectorCopy(e->s.origin, b->origin);
	VectorCopy(e->s.angles, b->angles);
	b->modelindex = e->s.modelindex;
	b->frame = e->s.frame;
	b->skinnum = e->s.skinnum;
	b->renderfx = e->s.renderfx & 0x0000027f;
	if (e->ghoulInst || (e->s.renderfx & RF_GHOUL))
	{
		b->renderfx |= 0x00400000; /* RF_SOFGHOUL on the client */
		if (!b->modelindex && e->ghoulInst) b->modelindex = g_ghoulModelIndex;
	}
	b->sound = e->s.sound;
	b->solid = e->solid;
	b->svflags = e->svflags & 0x7;
	b->clipmask = e->clipmask;
	b->owner = numOf(e->owner);
	VectorCopy(e->mins, b->mins);
	VectorCopy(e->maxs, b->maxs);
}

static void I_linkentity(edict_t *ent)
{
	q2b_ent_t b;
	q2b_linkout_t o;
	entToBridge(ent, &b);
	VectorCopy(ent->s.origin, b.old_origin);
	q2b_link(numOf(ent), &b, &o);
	VectorCopy(o.absmin, ent->absmin);
	VectorCopy(o.absmax, ent->absmax);
	VectorCopy(o.size, ent->size);
	ent->linkcount = o.linkcount;
	ent->num_clusters = o.num_clusters;
	for (int i = 0; i < 16 && i < MAX_ENT_CLUSTERS; i++) ent->clusternums[i] = (short)o.clusternums[i];
	ent->headnode = o.headnode;
	ent->areanum = o.areanum;
	ent->areanum2 = o.areanum2;
	ent->s.solid = o.s_solid;
	ent->area.prev = ent->area.next = &ent->area; /* marks "linked" for the game's own checks */
}
static void I_unlinkentity(edict_t *ent)
{
	q2b_unlink(numOf(ent));
	ent->area.prev = ent->area.next = 0;
}

static int I_BoxEdicts(vec3_t mins, vec3_t maxs, edict_t **list, int maxcount, int areatype)
{
	std::vector<int> nums((size_t)(maxcount > 0 ? maxcount : 1));
	int n = q2b_boxedicts(mins, maxs, &nums[0], maxcount, areatype), k = 0;
	for (int i = 0; i < n; i++) { edict_t *e = edictOf(nums[(size_t)i]); if (e) list[k++] = e; }
	return k;
}

/* SoF game code reads trace.surface->flags and ->textureinfo->name */
struct AdapterTexInfo { char name[64]; int surfaceType; };
static std::map<const void *, mtexinfo_t *> g_surfs;
static mtexinfo_t *surfFor(const q2b_trace_t &t)
{
	static mtexinfo_t none;
	static AdapterTexInfo noneInfo;
	if (!t.surface) { none.textureinfo = (struct ctextureinfo_s *)&noneInfo; return &none; }
	std::map<const void *, mtexinfo_t *>::iterator it = g_surfs.find(t.surface);
	if (it != g_surfs.end()) return it->second;
	mtexinfo_t *m = (mtexinfo_t *)calloc(1, sizeof(mtexinfo_t));
	AdapterTexInfo *ti = (AdapterTexInfo *)calloc(1, sizeof(AdapterTexInfo));
	strncpy(ti->name, t.surfname, sizeof(ti->name) - 1);
	ti->surfaceType = (int)((unsigned)t.surfflags >> 24);
	m->flags = t.surfflags;
	m->value = t.surfvalue;
	m->textureinfo = m->origtextureinfo = (struct ctextureinfo_s *)ti;
	g_surfs[t.surface] = m;
	return m;
}

static void toTrace(const q2b_trace_t &c, trace_t *t)
{
	memset(t, 0, sizeof(*t));
	t->allsolid = c.allsolid != 0;
	t->startsolid = c.startsolid != 0;
	t->fraction = c.fraction;
	for (int k = 0; k < 3; k++) { t->endpos[k] = c.endpos[k]; t->plane.normal[k] = c.normal[k]; }
	t->plane.dist = c.dist;
	t->plane.type = (byte)c.planetype;
	t->plane.signbits = (byte)c.signbits;
	t->surface = surfFor(c);
	t->contents = c.contents;
	t->ent = edictOf(c.entnum);
}

static qboolean I_trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *passent, int contentmask, trace_t *tr)
{
	q2b_trace_t c;
	q2b_trace(start, mins, maxs, end, numOf(passent), contentmask, &c);
	toTrace(c, tr);
	return tr->fraction < 1.0f;
}

/* polyTrace: like trace, but entities whose model the ray misses (PolyHitFunc returns 0)
 * are passed through. Implemented by temporarily making them non-solid. */
static qboolean I_polyTrace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *passent, int contentmask, trace_t *tr,
                            int (*PolyHitFunc)(trace_t *tr, vec3_t start, vec3_t end, int clipMask))
{
	std::vector<std::pair<edict_t *, int> > hidden;
	for (int iter = 0; iter < 16; iter++)
	{
		I_trace(start, mins, maxs, end, passent, contentmask, tr);
		if (tr->fraction >= 1.0f || !tr->ent || !PolyHitFunc) break;
		if (PolyHitFunc(tr, start, end, contentmask)) break;
		if (tr->ent == EDICT_NUM(0)) { tr->fraction = 1.0f; VectorCopy(end, tr->endpos); tr->ent = 0; break; }
		hidden.push_back(std::make_pair(tr->ent, (int)tr->ent->solid));
		q2b_ent_t b;
		entToBridge(tr->ent, &b);
		b.solid = SOLID_NOT;
		q2b_setent(numOf(tr->ent), &b);
	}
	for (size_t i = 0; i < hidden.size(); i++)
	{
		q2b_ent_t b;
		entToBridge(hidden[i].first, &b);
		q2b_setent(numOf(hidden[i].first), &b);
	}
	return tr->fraction < 1.0f;
}

static int I_pointcontents(vec3_t p) { return q2b_pointcontents(p); }
static float I_RegionDistance(vec3_t) { return 0; }
static qboolean I_inPVS(vec3_t a, vec3_t b) { return q2b_inpvs(a, b) != 0; }
static qboolean I_inPHS(vec3_t a, vec3_t b) { return q2b_inphs(a, b) != 0; }
static void I_SetAreaPortalState(int p, qboolean open) { q2b_setareaportalstate(p, open ? 1 : 0); }
static qboolean I_AreasConnected(int a, int b) { return q2b_areasconnected(a, b) != 0; }

static void I_setmodel(edict_t *ent, char *name)
{
	if (!name) { I_error((char *)"setmodel: NULL"); return; }
	ent->s.modelindex = I_modelindex(name);
	if (name[0] == '*')
	{
		/* the engine knows the inline model bounds: link once through the mirror to read them */
		q2b_ent_t b;
		entToBridge(ent, &b);
		q2b_setent(numOf(ent), &b);
		q2b_inlinebounds(atoi(name + 1), ent->mins, ent->maxs);
		I_linkentity(ent);
	}
}
static void I_setrendermodel(edict_t *ent, char *name) { if (name) ent->s.renderindex = I_modelindex(name); }

/* ------------------------------------------------------------------ sound and network output */

static void I_sound(edict_t *ent, int channel, int idx, float vol, float attn, float ofs, int) { q2b_sound(numOf(ent), channel, idx, vol, attn, ofs); }
static void I_positioned_sound(vec3_t origin, edict_t *ent, int channel, int idx, float vol, float attn, float ofs, int)
{ q2b_positioned_sound(origin, numOf(ent), channel, idx, vol, attn, ofs); }
static void I_DebugGraph(float, int) {}
static bool I_DamageTexture(struct mtexinfo_s *, int) { return false; }
static int I_SurfaceTypeList(byte *mat_list, int max_size) { if (mat_list && max_size > 0) memset(mat_list, 0, (size_t)max_size); return 0; }
static void I_Update(float, bool) {}
/* SoF's own network messages: the game writes them with gi.Write*() and sends them with
 * multicast/unicast. This is a local game, so they are decoded here and the effects are
 * run by the client-side effects system (sof/fx) directly; others are dropped. */
static std::vector<unsigned char> g_msg;
static void msgPut(const void *p, size_t n) { const unsigned char *b = (const unsigned char *)p; g_msg.insert(g_msg.end(), b, b + n); }
static void handleMessage(void);
static void I_multicast(vec3_t, multicast_t) { handleMessage(); }
static void I_multicastignore(vec3_t, multicast_t, int) { handleMessage(); }
static void I_unicast(edict_t *, qboolean) { handleMessage(); }
static void I_WriteChar(int c) { signed char b = (signed char)c; msgPut(&b, 1); }
static void I_WriteByte(int c) { unsigned char b = (unsigned char)c; msgPut(&b, 1); }
static void I_WriteShort(int c) { short v = (short)c; msgPut(&v, 2); }
static void I_WriteLong(int c) { int v = c; msgPut(&v, 4); }
static void I_WriteFloat(float f) { msgPut(&f, 4); }
static void I_WriteString(char *s) { if (!s) s = (char *)""; msgPut(s, strlen(s) + 1); }
/* positions and directions travel as plain floats (both ends are in this process) */
static void I_WritePosition(vec3_t v) { msgPut(v, 12); }
static void I_WriteDir(vec3_t v) { msgPut(v, 12); }
static void I_WriteAngle(float f) { msgPut(&f, 4); }
static void SZ_Write(sizebuf_t *buf, const void *data, int length)
{
	if (!buf || !buf->data) return;
	if (buf->cursize + length > buf->maxsize) { buf->overflowed = true; buf->cursize = 0; return; }
	memcpy(buf->data + buf->cursize, data, (size_t)length);
	buf->cursize += length;
}
static void I_WriteByteSizebuf(sizebuf_t *sz, int c) { byte b = (byte)c; SZ_Write(sz, &b, 1); }
static void I_WriteShortSizebuf(sizebuf_t *sz, int c) { short s = (short)c; SZ_Write(sz, &s, 2); }
static void I_WriteLongSizebuf(sizebuf_t *sz, int c) { SZ_Write(sz, &c, 4); }
static void I_ReliableWriteByteToClient(byte, int) {}
static void I_ReliableWriteDataToClient(const void *, int, int) {}
static int I_GetNearestByteNormal(vec3_t dir)
{
	int best = 0;
	float bd = -2;
	for (int i = 0; i < 256; i++)
	{
		float d = dir[0] * ghb::kDirTable[i][0] + dir[1] * ghb::kDirTable[i][1] + dir[2] * ghb::kDirTable[i][2];
		if (d > bd) { bd = d; best = i; }
	}
	return best;
}
static void I_sendPlayernameColors(edict_t *, int, int, byte *) {}
static void I_SZ_Init(sizebuf_t *buf, byte *data, int length) { memset(buf, 0, sizeof(*buf)); buf->data = data; buf->maxsize = length; }
static void I_SZ_Clear(sizebuf_t *buf) { buf->cursize = 0; buf->overflowed = false; }

/* ------------------------------------------------------------------ player module, misc */

class AdapterModelInfo : public IPlayerModelInfoC {};
static IPlayerModelInfoC *I_NewPlayerModelInfo(char *) { return new AdapterModelInfo; }

/* ---- VR aiming --------------------------------------------------------------
 * SoF fires from origin + ps.viewoffset along ps.viewangles. In VR the gun is in
 * the player's hand, so for the duration of each shot those are replaced with the
 * controller's position and orientation (like Quake2Quest's SV_SetWeapon_Client6DOF).
 * The player entity itself never moves, so collision and AI are unaffected.
 * The game hands its fire callbacks to the player module through Sys_GetPlayerAPI;
 * they are wrapped there. player_sv_import_t starts with:
 *   GetGhoul, levelTime, FireHelper, AltfireHelper (references are pointers in the ABI). */
typedef void (*sof_fire_fn)(void *sh, edict_t *ent, void *inven);
struct PlayerSvImportHead { void *(*GetGhoul)(); float *levelTime; sof_fire_fn FireHelper, AltfireHelper; };
static sof_fire_fn g_origFire, g_origAltfire;

struct VRAimSave { vec3_t viewoffset, viewangles; bool active; };
static bool vrMuzzle(float *out); /* world position of the drawn view weapon's muzzle */

static void vrAimBegin(edict_t *ent, VRAimSave &save)
{
	save.active = false;
	if (!ent || !ent->client) return;
	static void *cv_aim, *cv_scale, *cv_adjust;
	if (!cv_aim) cv_aim = q2b_cvar("sof_vraim", "1", 0);
	if (!cv_scale) cv_scale = q2b_cvar("vr_worldscale", "36", 0);
	if (!cv_adjust) cv_adjust = q2b_cvar("vr_height_adjust", "0", 0);
	if (cv_aim && q2b_cvar_value(cv_aim) == 0) return;

	float off[3], ang[3], hmd[3];
	q2b_getvrorigins(off, ang, hmd);
	if (!off[0] && !off[1] && !off[2] && !ang[0] && !ang[1] && !ang[2]) return; /* no VR (desktop) */

	player_state_t *ps = (player_state_t *)ent->client;
	/* looking through the sniper scope (the game narrows the fov and hides the gun):
	   the client magnifies the head's view, so shots go where the head looks */
	if (ps->fov > 0 && ps->fov < 90) return;
	float ws = cv_scale ? q2b_cvar_value(cv_scale) : 36.0f;
	float adj = cv_adjust ? q2b_cvar_value(cv_adjust) : 0.0f;
	VectorCopy(ps->viewoffset, save.viewoffset);
	VectorCopy(ps->viewangles, save.viewangles);
	save.active = true;

	/* eye as the client renders it, plus the controller's offset from the head
	   (VR axes x, y up, z -> Quake forward = -z, left = x, up = y) */
	float eyez = ps->viewoffset[2] - 1.57f * ws + (hmd[1] + adj) * ws;
	ps->viewoffset[0] = -off[2] * ws;
	ps->viewoffset[1] = off[0] * ws;
	ps->viewoffset[2] = eyez + off[1] * ws;
	VectorCopy(ang, ps->viewangles);
	/* the shot leaves the drawn gun's muzzle (along the controller, which the barrel is
	   fitted to): from the hand it would pass well below the barrel at short range.
	   Not through a wall the muzzle pokes into. */
	{
		float muzzle[3];
		if (vrMuzzle(muzzle))
		{
			float hand[3] = { ent->s.origin[0] + ps->viewoffset[0], ent->s.origin[1] + ps->viewoffset[1],
			                  ent->s.origin[2] + ps->viewoffset[2] };
			q2b_trace_t tr;
			q2b_trace(hand, 0, 0, muzzle, NUM_FOR_EDICT(ent), 1 /* CONTENTS_SOLID */, &tr);
			if (tr.fraction >= 1)
				for (int k = 0; k < 3; k++) ps->viewoffset[k] = muzzle[k] - ent->s.origin[k];
		}
	}
	if (getenv("SOF_DEBUG"))
	{
		char b[256];
		snprintf(b, sizeof(b), "[sof] VR shot from %.1f %.1f %.1f (eye offset %.1f) angles %.1f %.1f %.1f\n",
			ent->s.origin[0] + ps->viewoffset[0], ent->s.origin[1] + ps->viewoffset[1], ent->s.origin[2] + ps->viewoffset[2],
			save.viewoffset[2], ang[0], ang[1], ang[2]);
		q2b_dprint(b);
	}
}
static void vrAimEnd(edict_t *ent, VRAimSave &save)
{
	if (!save.active) return;
	player_state_t *ps = (player_state_t *)ent->client;
	VectorCopy(save.viewoffset, ps->viewoffset);
	VectorCopy(save.viewangles, ps->viewangles);
}
/* With the VR scope the sniper rifle is always looking through its scope: SoF throws
   unzoomed rifle shots up to 4.5 degrees off (fov above 60), so fire it as zoomed. */
static bool vrScopedRifle(edict_t *ent);
static void vrFire(void *sh, edict_t *ent, void *inven)
{
	VRAimSave save;
	vrAimBegin(ent, save);
	player_state_t *ps = ent && ent->client ? (player_state_t *)ent->client : 0;
	float fov = ps ? ps->fov : 0;
	bool scoped = ps && vrScopedRifle(ent) && fov > 59;
	if (scoped) ps->fov = 59;
	g_origFire(sh, ent, inven);
	if (scoped) ps->fov = fov;
	vrAimEnd(ent, save);
}
static void vrAltfire(void *sh, edict_t *ent, void *inven)
{
	VRAimSave save;
	vrAimBegin(ent, save);
	g_origAltfire(sh, ent, inven);
	vrAimEnd(ent, save);
}

static void *I_Sys_GetPlayerAPI(void *parmscom, void *parmscl, void *parmssv, int isClient)
{
	typedef void *(*api2_t)(void *, void *);
	if (!g_playerLib) return 0;
	if (!isClient && parmssv)
	{
		PlayerSvImportHead *h = (PlayerSvImportHead *)parmssv;
		if (h->FireHelper && h->FireHelper != vrFire) { g_origFire = h->FireHelper; h->FireHelper = vrFire; }
		if (h->AltfireHelper && h->AltfireHelper != vrAltfire) { g_origAltfire = h->AltfireHelper; h->AltfireHelper = vrAltfire; }
	}
	api2_t f = (api2_t)dlsym(g_playerLib, isClient ? "GetPlayerClientAPI" : "GetPlayerServerAPI");
	return f ? f(parmscom, isClient ? parmscl : parmssv) : 0;
}
static void I_Sys_UnloadPlayer(int) {}
static float I_flrand(float min, float max) { return min + (max - min) * ((float)rand() / (float)RAND_MAX); }
static int I_irand(int min, int max) { if (max <= min) return min; return min + rand() % (max - min + 1); }
/* ---- save games -----------------------------------------------------------
 * SoF's game writes saves as a sequence of chunks (4 character id, length, data)
 * and reads them back in the same order. They are kept in memory and written to /
 * read from the files the Quake 2 server names (WriteGame/WriteLevel...). */
static std::vector<unsigned char> g_save;
static size_t g_savePos;

static qboolean I_AppendToSavegame(unsigned long chid, void *data, int length)
{
	uint32_t id = (uint32_t)chid;
	int32_t len = length > 0 ? length : 0;
	const unsigned char *h = (const unsigned char *)&id, *l = (const unsigned char *)&len;
	g_save.insert(g_save.end(), h, h + 4);
	g_save.insert(g_save.end(), l, l + 4);
	if (len && data) g_save.insert(g_save.end(), (const unsigned char *)data, (const unsigned char *)data + len);
	else if (len) g_save.insert(g_save.end(), (size_t)len, 0);
	return true;
}

static void chunkName(uint32_t id, char out[5])
{
	/* multi-character constants: 'GAME' is 0x47414D45 */
	for (int i = 0; i < 4; i++) { char c = (char)((id >> (24 - 8 * i)) & 0xff); out[i] = c >= 32 && c < 127 ? c : '?'; }
	out[4] = 0;
}

static int I_ReadFromSavegame(unsigned long chid, void *address, int length, void **addressptr)
{
	if (addressptr) *addressptr = 0;
	if (g_savePos + 8 > g_save.size())
	{
		char want[5];
		chunkName((uint32_t)chid, want);
		q2b_error((std::string("Save game is truncated (reading ") + want + ")").c_str());
		return 0;
	}
	uint32_t id;
	int32_t len;
	memcpy(&id, &g_save[g_savePos], 4);
	memcpy(&len, &g_save[g_savePos + 4], 4);
	if (id != (uint32_t)chid || len < 0 || g_savePos + 8 + (size_t)len > g_save.size())
	{
		char want[5], got[5];
		chunkName((uint32_t)chid, want);
		chunkName(id, got);
		q2b_error((std::string("Save game does not match this version (expected ") + want + ", found " + got + ")").c_str());
		return 0;
	}
	const unsigned char *src = &g_save[g_savePos + 8];
	g_savePos += 8 + (size_t)len;
	if (addressptr)
	{
		/* the game frees these with TagFree; some (strings) live for the whole game */
		void *p = I_TagMalloc(len + 1, 765 /* TAG_GAME */);
		memcpy(p, src, (size_t)len);
		*addressptr = p;
	}
	if (address)
	{
		/* length 0 means "the whole chunk" (e.g. GHOUL instance states of unknown size) */
		int n = length <= 0 ? len : (len < length ? len : length);
		memcpy(address, src, (size_t)n);
		if (length > n) memset((char *)address + n, 0, (size_t)(length - n));
	}
	return len;
}

static bool saveToFile(const char *filename)
{
	FILE *f = fopen(filename, "wb");
	if (!f) { q2b_dprint((std::string("SoF: can't write save ") + filename + "\n").c_str()); return false; }
	bool ok = g_save.empty() || fwrite(&g_save[0], 1, g_save.size(), f) == g_save.size();
	fclose(f);
	return ok;
}

static bool loadFromFile(const char *filename)
{
	g_save.clear();
	g_savePos = 0;
	FILE *f = fopen(filename, "rb");
	if (!f) { q2b_dprint((std::string("SoF: can't read save ") + filename + "\n").c_str()); return false; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0)
	{
		g_save.resize((size_t)n);
		if (fread(&g_save[0], 1, (size_t)n, f) != (size_t)n) g_save.clear();
	}
	fclose(f);
	return true;
}

extern "C" void sofb_writegame(const char *filename, int autosave)
{
	g_save.clear();
	sge->WriteGame(autosave != 0);
	saveToFile(filename);
	g_save.clear();
}
extern "C" void sofb_readgame(const char *filename)
{
	if (!loadFromFile(filename)) return;
	sge->ReadGame(false);
	g_save.clear();
}
extern "C" void sofb_writelevel(const char *filename)
{
	g_save.clear();
	sge->WriteLevel();
	saveToFile(filename);
	g_save.clear();
}
extern "C" void sofb_readlevel(const char *filename)
{
	if (!loadFromFile(filename)) return;
	sge->ReadLevel();
	g_save.clear();
	q2b_setnumedicts(sge->num_edicts);
	sofb_syncmirrors();
}
static void *I_GetGhoul(void) { return Ghoul_Get(false, false); }
static int g_isClientVal = 0;
static int *g_isClientPtr = &g_isClientVal;

static void I_Pmove(pmove_t *pm)
{
	q2b_pmove_t p;
	memset(&p, 0, sizeof(p));
	switch (pm->s.pm_type)
	{
	case PM_NORMAL: p.pm_type = 0; break;
	case PM_NOCLIP: case PM_SPECTATOR: p.pm_type = 1; break;
	case PM_DEAD: p.pm_type = 2; break;
	case PM_GIB: p.pm_type = 3; break;
	default: p.pm_type = 4; break;
	}
	for (int i = 0; i < 3; i++)
	{
		p.origin[i] = pm->s.origin[i];
		p.velocity[i] = pm->s.velocity[i];
		p.delta_angles[i] = pm->s.delta_angles[i];
		p.angles[i] = pm->cmd.angles[i];
	}
	p.pm_flags = (pm->s.pm_flags & 63) | ((pm->s.pm_flags & PMF_NO_PREDICTION) ? 64 : 0);
	p.pm_time = pm->s.pm_time;
	p.gravity = pm->s.gravity;
	p.msec = pm->cmd.msec;
	p.buttons = pm->cmd.buttons & 3;
	p.forwardmove = pm->cmd.forwardmove;
	p.sidemove = pm->cmd.sidemove;
	p.upmove = pm->cmd.upmove;
	p.snapinitial = pm->snapinitial;
	p.passent = g_thinkClient;
	q2b_pmove(&p);
	for (int i = 0; i < 3; i++)
	{
		pm->s.origin[i] = (short)lrintf(p.origin[i]);
		pm->s.velocity[i] = (short)lrintf(p.velocity[i]);
		pm->s.delta_angles[i] = (short)lrintf(p.delta_angles[i]);
		pm->viewangles[i] = p.viewangles[i];
		pm->mins[i] = p.mins[i];
		pm->maxs[i] = p.maxs[i];
	}
	pm->s.pm_flags = (byte)((p.pm_flags & 63) | ((p.pm_flags & 64) ? PMF_NO_PREDICTION : 0) | (pm->s.pm_flags & PMF_FATIGUED));
	pm->s.pm_time = (byte)p.pm_time;
	pm->numtouch = 0;
	for (int i = 0; i < p.numtouch && i < MAXTOUCH; i++) { edict_t *t = edictOf(p.touchents[i]); if (t) pm->touchents[pm->numtouch++] = t; }
	pm->viewheight = p.viewheight;
	pm->groundentity = edictOf(p.groundentity);
	pm->watertype = p.watertype;
	pm->waterlevel = p.waterlevel;
}

static void buildImports(void)
{
	memset(&sgi, 0, sizeof(sgi));
	sgi.modelindex = I_modelindex;
	sgi.soundindex = I_soundindex;
	sgi.effectindex = I_effectindex;
	sgi.imageindex = I_imageindex;
	sgi.unload_sound = I_unload_sound;
	sgi.FilterPacket = I_FilterPacket;
	sgi.CreateGhoulConfigStrings = I_CreateGhoulConfigStrings;
	sgi.setmodel = I_setmodel;
	sgi.setrendermodel = I_setrendermodel;
	sgi.argc = I_argc;
	sgi.argv = I_argv;
	sgi.args = I_args;
	sgi.bprintf = I_bprintf;
	sgi.dprintf = I_dprintf;
	sgi.cprintf = I_cprintf;
	sgi.clprintf = I_clprintf;
	sgi.welcomeprint = I_welcomeprint;
	sgi.centerprintf = I_centerprintf;
	sgi.cinprintf = I_cinprintf;
	sgi.bcaption = I_bcaption;
	sgi.captionprintf = I_captionprintf;
	sgi.Con_ClearNotify = I_Con_ClearNotify;
	sgi.sound = I_sound;
	sgi.positioned_sound = I_positioned_sound;
	sgi.DebugGraph = I_DebugGraph;
	sgi.DamageTexture = I_DamageTexture;
	sgi.SurfaceTypeList = I_SurfaceTypeList;
	sgi.Update = I_Update;
	sgi.multicast = I_multicast;
	sgi.multicastignore = I_multicastignore;
	sgi.unicast = I_unicast;
	sgi.WriteChar = I_WriteChar;
	sgi.WriteByte = I_WriteByte;
	sgi.WriteShort = I_WriteShort;
	sgi.WriteLong = I_WriteLong;
	sgi.WriteFloat = I_WriteFloat;
	sgi.WriteString = I_WriteString;
	sgi.WritePosition = I_WritePosition;
	sgi.WriteDir = I_WriteDir;
	sgi.WriteAngle = I_WriteAngle;
	sgi.WriteByteSizebuf = I_WriteByteSizebuf;
	sgi.WriteShortSizebuf = I_WriteShortSizebuf;
	sgi.WriteLongSizebuf = I_WriteLongSizebuf;
	sgi.ReliableWriteByteToClient = I_ReliableWriteByteToClient;
	sgi.ReliableWriteDataToClient = I_ReliableWriteDataToClient;
	sgi.GetNearestByteNormal = I_GetNearestByteNormal;
	sgi.sendPlayernameColors = I_sendPlayernameColors;
	sgi.SP_Register = I_SP_Register;
	sgi.SP_Print = I_SP_Print;
	sgi.SP_Print_Obit = I_SP_Print_Obit;
	sgi.SP_SPrint = I_SP_SPrint;
	sgi.SP_GetStringText = I_SP_GetStringText;
	sgi.trace = I_trace;
	sgi.polyTrace = I_polyTrace;
	sgi.pointcontents = I_pointcontents;
	sgi.RegionDistance = I_RegionDistance;
	sgi.inPVS = I_inPVS;
	sgi.inPHS = I_inPHS;
	sgi.SetAreaPortalState = I_SetAreaPortalState;
	sgi.AreasConnected = I_AreasConnected;
	sgi.GetGhoul = I_GetGhoul;
	sgi.NewPlayerModelInfo = I_NewPlayerModelInfo;
	sgi.FindGSQFile = GSQ_FindFile;
	sgi.ReadGsqEntry = GSQ_ReadEntry;
	sgi.PrecacheGSQFile = GSQ_Precache;
	sgi.RegisterGSQSequences = GSQ_RegisterSequences;
	sgi.TurnOffPartsFromGSQFile = GSQ_TurnOffParts;
	sgi.isClient = &g_isClientPtr;
	sgi.configstring = I_configstring;
	sgi.SZ_Init = I_SZ_Init;
	sgi.SZ_Clear = I_SZ_Clear;
	sgi.SZ_Write = SZ_Write;
	sgi.error = I_error;
	sgi.Sys_ConsoleOutput = I_Sys_ConsoleOutput;
	sgi.Sys_GetPlayerAPI = I_Sys_GetPlayerAPI;
	sgi.Sys_UnloadPlayer = I_Sys_UnloadPlayer;
	sgi.flrand = I_flrand;
	sgi.irand = I_irand;
	sgi.linkentity = I_linkentity;
	sgi.unlinkentity = I_unlinkentity;
	sgi.BoxEdicts = I_BoxEdicts;
	sgi.Pmove = I_Pmove;
	sgi.TagMalloc = I_TagMalloc;
	sgi.TagFree = I_TagFree;
	sgi.FreeTags = I_FreeTags;
	sgi.AppendToSavegame = I_AppendToSavegame;
	sgi.ReadFromSavegame = I_ReadFromSavegame;
	sgi.cvar = I_cvar;
	sgi.cvar_set = I_cvar_set;
	sgi.cvar_setvalue = I_cvar_setvalue;
	sgi.cvar_forceset = I_cvar_forceset;
	sgi.cvar_info = I_cvar_info;
	sgi.cvar_variablevalue = I_cvar_variablevalue;
	sgi.FS_LoadFile = I_FS_LoadFile;
	sgi.FS_FreeFile = I_FS_FreeFile;
	sgi.FS_Userdir = I_FS_Userdir;
	sgi.FS_CreatePath = I_FS_CreatePath;
	sgi.FS_FileExists = I_FS_FileExists;
	sgi.AddCommandString = I_AddCommandString;
}

/* ------------------------------------------------------------------ module loading */

static std::string myDir(void)
{
	Dl_info info;
	if (dladdr((void *)&myDir, &info) && info.dli_fname)
	{
		std::string p(info.dli_fname);
		size_t s = p.find_last_of('/');
		if (s != std::string::npos) return p.substr(0, s + 1);
	}
	return "./";
}

static void *openLib(const char *cvarName, const char *const *names)
{
	void *cv = q2b_cvar(cvarName, "", 0);
	const char *forced = q2b_cvar_string(cv);
	std::vector<std::string> tries;
	if (forced && forced[0]) tries.push_back(forced);
	for (int i = 0; names[i]; i++)
	{
		tries.push_back(myDir() + names[i]);
		tries.push_back(names[i]);
	}
	for (size_t i = 0; i < tries.size(); i++)
	{
		void *h = dlopen(tries[i].c_str(), RTLD_NOW | RTLD_LOCAL);
		if (h) { q2b_dprint(("SoF: loaded " + tries[i] + "\n").c_str()); return h; }
	}
	q2b_dprint((std::string("SoF: could not load module: ") + dlerror() + "\n").c_str());
	return 0;
}

/* ------------------------------------------------------------------ bridge entry points */

extern "C" int sofb_init(int maxclients)
{
	static const char *gameNames[] = { "libsofgame.so", "gamex64.so", "gamesof.so", 0 };
	static const char *playerNames[] = { "libsofplayer.so", "player.so", "playersof.so", 0 };
	g_maxclients = maxclients;
	GhoulEngineImports gimp = { ghoulLoad, ghoulFree, ghoulList, ghoulPrintf };
	Ghoul_Init(gimp);
	g_playerLib = openLib("sof_playerlib", playerNames);
	g_gameLib = openLib("sof_gamelib", gameNames);
	if (!g_gameLib || !g_playerLib) return 0;
	typedef game_export_t *(*getapi_t)(game_import_t *);
	getapi_t getapi = (getapi_t)dlsym(g_gameLib, "GetGameAPI");
	if (!getapi) return 0;
	buildImports();
	sge = getapi(&sgi);
	if (!sge || sge->apiversion != GAME_API_VERSION) return 0;
	/* SoF's default key bindings use quicksave / quickload */
	q2b_addcommandstring("alias quicksave \"save quick\"\nalias quickload \"load quick\"\n");

	/* single player: SoF's chat flood protection only ever fires on stray commands */
	q2b_cvar_forceset("flood_msgs", "0");

	/* VR scale: SoF people are 64 units tall with eyes 59 units above the floor
	   (Quake 2's marine: 56 / 46), about 36 units per metre. Quake2Quest's default
	   is Quake 2's 26.2467, which makes you feel short in SoF; switch it unless the
	   player has set their own value. */
	{
		void *ws = q2b_cvar("vr_worldscale", "36", 0);
		float v = ws ? q2b_cvar_value(ws) : 0;
		if (ws && v > 26.24f && v < 26.25f) q2b_cvar_forceset("vr_worldscale", "36");
	}
	/* Quake2Quest's weapon/inventory wheels list Quake 2 items; with them on, the
	   stick doesn't switch weapons. SoF uses stick up/down (weapprev/weapnext). */
	q2b_cvar_forceset("vr_use_wheels", "0");
	sge->Init();
	registerClientHooks();
	return 1;
}

extern "C" void sofb_shutdown(void)
{
	if (CL_SoF_RegisterHooks) CL_SoF_RegisterHooks(0, 0);
	fxUnregister();
	if (Pmove_SetSoFHeights) Pmove_SetSoFHeights(0);
	clearDrawCache();
	if (sge) sge->Shutdown();
	sge = 0;
	Ghoul_Shutdown();
	if (g_gameLib) dlclose(g_gameLib);
	g_gameLib = 0;
}

extern "C" void sofb_spawnentities(const char *mapname, const char *entities, const char *spawnpoint)
{
	Ghoul_SetLevelName(mapname);
	g_effects.clear();
	sfx::Clear();
	g_ghoulModelIndex = q2b_modelindex("#sofghoul");
	refreshAllCvars();
	sge->SpawnEntities((char *)mapname, (char *)entities, (char *)spawnpoint);
}

/* SoF's own client predicts the view weapon and tells the server when it fired; the
   server then only fires on those "fire events". The Quake 2 client never sends them, so
   the userinfo always says "predicting 0" and the server fires from the attack button. */
static void sofUserinfo(const char *in, std::string &out)
{
	out.clear();
	const char *p = in ? in : "";
	while (*p == '\\')
	{
		const char *k = p + 1, *v = strchr(k, '\\');
		if (!v) break;
		const char *e = strchr(v + 1, '\\');
		if (!e) e = v + 1 + strlen(v + 1);
		if (!((size_t)(v - k) == 10 && !strncasecmp(k, "predicting", 10))) out.append(p, (size_t)(e - p));
		p = e;
	}
	out += "\\predicting\\0";
}
static void copyBack(char *userinfo, const std::string &info)
{
	/* the game may add a "rejmsg" for the engine; Quake 2's buffer is 512 bytes */
	if (userinfo && info.size() < 512) memcpy(userinfo, info.c_str(), info.size() + 1);
}
extern "C" int sofb_clientconnect(int num, char *userinfo)
{
	refreshAllCvars();
	std::string info;
	sofUserinfo(userinfo, info);
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", info.c_str());
	int ok = sge->ClientConnect(edictOf(num), buf);
	copyBack(userinfo, buf);
	return ok;
}
extern "C" void sofb_clientbegin(int num)
{
	sge->ClientBegin(edictOf(num));
	/* sof_giveall 1: hand out every weapon on spawn (SoF's own "elbow" cheat) so they
	   can be tried without playing the tutorial; set it in commandline.txt */
	static void *cv_giveall;
	if (!cv_giveall) cv_giveall = q2b_cvar("sof_giveall", "0", 0);
	if (num == 1 && q2b_cvar_value(cv_giveall) != 0) q2b_addcommandstring("cmd elbow\n");
}
extern "C" void sofb_clientuserinfochanged(int num, char *userinfo)
{
	std::string info;
	sofUserinfo(userinfo, info);
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", info.c_str());
	sge->ClientUserinfoChanged(edictOf(num), buf, true);
}
extern "C" void sofb_clientdisconnect(int num) { sge->ClientDisconnect(edictOf(num)); }
extern "C" void sofb_clientcommand(int num)
{
	if (getenv("SOF_DEBUG")) { char b[300]; snprintf(b, sizeof(b), "[sof] ClientCommand %d: %s %s\n", num, q2b_argv(0), q2b_args()); q2b_dprint(b); }
	sge->ClientCommand(edictOf(num));
}

extern "C" void sofb_clientthink(int num, const sofb_usercmd_t *c)
{
	usercmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.msec = (byte)c->msec;
	/* Q2 buttons: 1 attack, 2 use, 4 alt attack (SoF builds), 128 any. Q2 running is already folded into the move speeds. */
	cmd.buttons = (byte)(c->buttons & (BUTTON_ATTACK | BUTTON_USE | BUTTON_ALTATTACK | BUTTON_WEAP3 | BUTTON_WEAP4 | BUTTON_ANY));
	/* VR scope on the sniper rifle: the grip (alternate fire) changes the scope's
	   magnification instead of SoF's scope view, which hides the rifle */
	if (vrScopedRifle(edictOf(num)))
	{
		static bool wasDown;
		bool down = (cmd.buttons & BUTTON_ALTATTACK) != 0;
		if (down && !wasDown)
		{
			static void *cv_mag;
			if (!cv_mag) cv_mag = q2b_cvar("vr_scope_mag", "4", 0);
			float m = q2b_cvar_value(cv_mag);
			const char *next = m < 3 ? "4" : m < 6 ? "8" : m < 12 ? "16" : "2";
			q2b_cvar_set("vr_scope_mag", next);
			char b[64];
			snprintf(b, sizeof(b), "Scope %sx\n", next);
			q2b_centerprint(num, b);
		}
		wasDown = down;
		cmd.buttons &= ~BUTTON_ALTATTACK;
	}
	cmd.lightlevel = (byte)c->lightlevel;
	for (int i = 0; i < 3; i++) cmd.angles[i] = c->angles[i];
	cmd.forwardmove = (short)c->forwardmove;
	cmd.sidemove = (short)c->sidemove;
	cmd.upmove = (short)c->upmove;
	g_thinkClient = num;
	sge->ClientThink(edictOf(num), &cmd);
	g_thinkClient = -1;
	static const char *dbg = getenv("SOF_DEBUG");
	static int n;
	if (dbg && (n++ % (atoi(dbg) > 1 ? 1 : 100)) == 0)
	{
		edict_t *e = edictOf(num);
		player_state_t *ps = e && e->client ? (player_state_t *)e->client : 0;
		char b[256];
		snprintf(b, sizeof(b), "[sofdbg] think %d: fwd %d side %d up %d yaw %d btn %d msec %d -> pm_type %d origin %.0f %.0f %.0f\n",
		         num, cmd.forwardmove, cmd.sidemove, cmd.upmove, cmd.angles[1], cmd.buttons, cmd.msec, ps ? ps->pmove.pm_type : -1,
		         e ? e->s.origin[0] : 0, e ? e->s.origin[1] : 0, e ? e->s.origin[2] : 0);
		q2b_dprint(b);
	}
}

static void fxEntityEvents(void);
extern "C" void sofb_runframe(void)
{
	refreshAllCvars();
	sge->RunFrame(g_frame++);
	fxEntityEvents();
}

extern "C" int sofb_numedicts(void) { return sge ? sge->num_edicts : 0; }

static std::vector<float> g_prevOrigin;

extern "C" void sofb_syncmirrors(void)
{
	if (!sge) return;
	if (g_prevOrigin.size() < (size_t)sge->max_edicts * 3) g_prevOrigin.assign((size_t)sge->max_edicts * 3, 0.0f);
	q2b_setnumedicts(sge->num_edicts);
	for (int i = 0; i < sge->num_edicts; i++)
	{
		edict_t *e = EDICT_NUM(i);
		q2b_ent_t b;
		entToBridge(e, &b);
		for (int k = 0; k < 3; k++) { b.old_origin[k] = g_prevOrigin[(size_t)i * 3 + k]; g_prevOrigin[(size_t)i * 3 + k] = e->s.origin[k]; }
		q2b_setent(i, &b);
	}
	for (int c = 1; c <= g_maxclients; c++)
	{
		edict_t *e = EDICT_NUM(c);
		if (!e->client) continue;
		player_state_t *ps = (player_state_t *)e->client; /* gclient_t starts with player_state_t */
		q2b_ps_t o;
		memset(&o, 0, sizeof(o));
		switch (ps->pmove.pm_type)
		{
		case PM_NORMAL: o.pm_type = 0; break;
		case PM_NOCLIP: case PM_SPECTATOR: o.pm_type = 1; break;
		case PM_DEAD: o.pm_type = 2; break;
		case PM_GIB: o.pm_type = 3; break;
		default: o.pm_type = 4; break;
		}
		for (int k = 0; k < 3; k++)
		{
			o.origin[k] = ps->pmove.origin[k];
			o.velocity[k] = ps->pmove.velocity[k];
			o.delta_angles[k] = ps->pmove.delta_angles[k];
			o.viewangles[k] = ps->viewangles[k];
			o.viewoffset[k] = ps->viewoffset[k];
			o.kick_angles[k] = ps->kick_angles[k] + ps->weaponkick_angles[k];
		}
		o.pm_flags = (ps->pmove.pm_flags & 63) | ((ps->pmove.pm_flags & PMF_NO_PREDICTION) ? 64 : 0);
		o.pm_time = ps->pmove.pm_time;
		o.gravity = ps->pmove.gravity;
		/* remote camera views (cinematics): show the camera */
		if (ps->remote_type > REMOTE_TYPE_TPS && ps->remote_id >= 0)
		{
			for (int k = 0; k < 3; k++)
			{
				o.origin[k] = ps->remote_vieworigin[k]; /* SoF stores it in 1/8 units already, like pmove.origin */
				o.viewangles[k] = ps->remote_viewangles[k];
				o.viewoffset[k] = 0;
				o.velocity[k] = 0;
			}
			o.pm_type = 4;
		}
		for (int k = 0; k < 4; k++) o.blend[k] = ps->blend[k];
		for (int k = 0; k < MAX_STATS && k < 32; k++) o.stats[k] = ps->stats[k];
		o.fov = ps->fov > 0 ? ps->fov : 90.0f;
		o.rdflags = ps->rdflags;
		q2b_setps(c, &o);
	}
}

/* ------------------------------------------------------------------ client-side hooks */

/* skin name -> texture path, resolved once ("ghoul/<dir>/<skin>.tga", .m32, or ghoul/comskin) */
static std::map<std::string, std::string> g_skinPaths;
static bool fileExists(const std::string &p)
{
	void *b = 0;
	int n = q2b_loadfile(p.c_str(), &b);
	if (b) q2b_freefile(b);
	return n >= 0;
}
/* first entry of an image file list (.ifl), without extension */
static std::string iflFirst(const std::string &path)
{
	char *b = 0;
	int n = q2b_loadfile(path.c_str(), (void **)&b);
	if (n <= 0 || !b) return std::string();
	std::string t(b, (size_t)n), out;
	q2b_freefile(b);
	size_t p = 0;
	while (p < t.size() && out.empty())
	{
		size_t e = t.find_first_of("\r\n", p);
		if (e == std::string::npos) e = t.size();
		std::string l = t.substr(p, e - p);
		p = e + 1;
		while (!l.empty() && isspace((unsigned char)l[l.size() - 1])) l.erase(l.size() - 1);
		while (!l.empty() && isspace((unsigned char)l[0])) l.erase(0, 1);
		if (l.empty()) continue;
		size_t dot = l.find_last_of('.');
		out = dot == std::string::npos ? l : l.substr(0, dot);
	}
	return out;
}
static const char *skinPath(const std::string &dir, const std::string &skin)
{
	if (skin.empty()) return "";
	std::string key = dir + "|" + skin;
	std::map<std::string, std::string>::iterator it = g_skinPaths.find(key);
	if (it != g_skinPaths.end()) return it->second.c_str();
	static const char *exts[] = { ".tga", ".m32", 0 };
	std::string dirs[2] = { "ghoul/" + dir + "/", "ghoul/comskin/" };
	std::string found;
	for (int pass = 0; pass < 2 && found.empty(); pass++)
	{
		std::string name = skin;
		for (int d = 0; d < 2 && found.empty(); d++)
		{
			if (pass == 1)
			{
				/* the name may be an image list: use its first image */
				name = iflFirst(dirs[d] + skin + ".ifl");
				if (name.empty()) continue;
			}
			for (int dd = 0; dd < 2 && found.empty(); dd++)
				for (int e = 0; exts[e] && found.empty(); e++)
					if (fileExists(dirs[dd] + name + exts[e])) found = dirs[dd] + name + exts[e];
		}
	}
	return (g_skinPaths[key] = found).c_str();
}

static float g_drawLerp;   /* the client's last interpolation fraction */

struct DrawCache
{
	std::vector<GhoulDrawSurface> surfs;
	std::vector<sofmesh_t> meshes;
	sofdraw_t draw;
};
static std::map<int, DrawCache> g_drawCache;
static void clearDrawCache(void) { g_drawCache.clear(); }

static const sofdraw_t *buildDraw(int key, IGhoulInst *inst, float lerpfrac)
{
	if (!inst) return 0;
	DrawCache &c = g_drawCache[key];
	c.surfs.clear();
	/* The client draws between the last two server frames (cl.lerpfrac); pose the models
	   at that same moment so animation is smooth and in step with entity movement.
	   g_frame * 0.1 is the time of the newest server frame. */
	if (lerpfrac < 0) lerpfrac = 0; else if (lerpfrac > 1) lerpfrac = 1;
	g_drawLerp = lerpfrac;
	Ghoul_BuildDrawList(inst, ((float)g_frame - 1.0f + lerpfrac) * 0.1f, c.surfs);
	c.meshes.resize(c.surfs.size());
	for (size_t i = 0; i < c.surfs.size(); i++)
	{
		GhoulDrawSurface &s = c.surfs[i];
		sofmesh_t &m = c.meshes[i];
		m.numverts = (int)(s.xyz.size() / 3);
		m.xyz = s.xyz.empty() ? 0 : &s.xyz[0];
		m.st = s.st.empty() ? 0 : &s.st[0];
		m.normal = s.normal.empty() ? 0 : &s.normal[0];
		m.numindices = (int)s.indices.size();
		m.indices = s.indices.empty() ? 0 : &s.indices[0];
		m.skin = skinPath(s.objectDir, s.skin);
		for (int k = 0; k < 4; k++) m.rgba[k] = s.tint[k];
		m.colors = 0;
		m.blend = SOFBLEND_NONE;
		m.nodepth = 0;
	}
	c.draw.nummeshes = (int)c.meshes.size();
	c.draw.meshes = c.meshes.empty() ? 0 : &c.meshes[0];
	c.draw.hasscope = 0;
	return &c.draw;
}

static const sofdraw_t *hookEntityDraw(int num, float lerpfrac)
{
	edict_t *e = edictOf(num);
	if (!e || !e->inuse || !e->ghoulInst) return 0;
	return buildDraw(num, (IGhoulInst *)e->ghoulInst, lerpfrac);
}
/* ---- holding the view weapon in VR
 * SoF's view weapons are made for a flat screen: the model's origin is the camera and the
 * gun sits out in front, lower right, angled in at the crosshair, with both arms reaching
 * in from the screen edges. On a controller that hangs the whole arm off the hand. So each
 * weapon is moved into the hand: the right hand's grip goes to the controller and the
 * barrel (the muzzle "flash" bolt's forward axis, which the shots also follow) points the
 * way the controller aims. The sleeves, which only made sense cut off by the screen edge,
 * are hidden. The fit is taken from the weapon's idle poses, so firing, reloading and
 * other animations still move the gun in the hand. */
static void cp3(const float *a, float *b);
static void normalize3(float *v);
struct GunFit
{
	bool valid;
	bool measured;                         /* heading taken from the muzzle area */
	float grip[3], fwd[3], left[3], up[3]; /* in the weapon's entity space */
	float k, sx;                           /* size: hand to a real hand's size, gun stretched to its real length */
};
static std::map<const void *, GunFit> g_gunAxes; /* model -> orientation, measured once */
static std::map<std::pair<const void *, std::string>, GunFit> g_gunFits; /* (model, idle sequence) */
static std::map<const void *, GunFit> g_gunFitLast;                       /* model -> fit in use */
static GunFit g_gunFitNow;                                                 /* this frame's fit */

static bool partIs(const std::string &part, const char *a, const char *b)
{
	std::string p = part;
	for (size_t i = 0; i < p.size(); i++) p[i] = (char)toupper((unsigned char)p[i]);
	return p.find(a) != std::string::npos && (!b || p.find(b) != std::string::npos);
}

/* Length of the weapon as the characters carry it (their models are made at about 4/3 of
   real size): the in-hand view models are drawn far shorter, since a flat screen only shows
   them end-on. Longest extent of ghoul/enemy/bolt/w_<weapon>.ghb at its first frame. */
static float worldGunLength(const std::string &objectDir)
{
	static std::map<std::string, float> cache;
	std::map<std::string, float>::iterator it = cache.find(objectDir);
	if (it != cache.end()) return it->second;
	static const char *const map[][2] = {
		{ "pistol1", "w_pistol1" }, { "pistol2", "w_pistol2" }, { "shotgun", "w_shotgun" },
		{ "sniperrifle", "w_sniperrifle" }, { "assaultrifle", "w_assault_rifle" }, { "machinegun", "w_machinegun" },
		{ "mpistol", "w_machinepistol" }, { "autoshotgun", "w_autoshotgun" }, { "flamegun", "w_flamethrower" },
		{ "mpg", "w_mpg" }, { "rocket", "w_rocket" }, { "knife", "w_knife" },
	};
	float len = 0;
	std::string base = objectDir.substr(objectDir.rfind('/') + 1);
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]) && !len; i++)
	{
		if (strcasecmp(base.c_str(), map[i][0])) continue;
		std::string path = std::string("ghoul/enemy/bolt/") + map[i][1] + ".ghb";
		void *buf = 0;
		int n = q2b_loadfile(path.c_str(), &buf);
		if (n <= 0 || !buf) break;
		ghb::Model m;
		std::string err;
		if (ghb::Load((const uint8_t *)buf, (size_t)n, m, err))
		{
			ghb::FrameCache fc;
			const float *ap = 0, *an = 0;
			if (m.numAnimPos) fc.Get(m, 0, &ap, &an);
			std::vector<int> tris;
			ghb::BuildTriangles(m, tris);
			float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
			for (size_t t = 0; t + 3 < tris.size(); t += 4)
				for (int k = 1; k <= 3; k++)
				{
					const ghb::Corner &c = m.corners[(size_t)tris[t + (size_t)k]];
					const float *p = c.pos < 0 ? &m.staticPos[(size_t)(~c.pos) * 3] : (ap ? ap + (size_t)c.pos * 3 : 0);
					if (!p) continue;
					for (int a = 0; a < 3; a++) { if (p[a] < lo[a]) lo[a] = p[a]; if (p[a] > hi[a]) hi[a] = p[a]; }
				}
			for (int a = 0; a < 3; a++) if (hi[a] - lo[a] > len) len = hi[a] - lo[a];
		}
		q2b_freefile(buf);
	}
	return cache[objectDir] = len;
}

static bool fitGun(IGhoulInst *gun, float time, const std::vector<GhoulDrawSurface> &surfs, GunFit &fit)
{
	fit.valid = false;
	fit.measured = false;
	fit.k = 0;
	fit.sx = 1;
	IGhoulObj *obj = gun->GetGhoulObject();
	if (!obj) return false;

	/* the grip: middle of the right hand */
	double sum[3] = { 0, 0, 0 };
	size_t n = 0;
	for (int pass = 0; pass < 2 && !n; pass++)
		for (size_t i = 0; i < surfs.size(); i++)
		{
			if (!(pass == 0 ? partIs(surfs[i].part, "_R_", "HAND") : partIs(surfs[i].part, "HAND", 0))) continue;
			const std::vector<float> &x = surfs[i].xyz;
			for (size_t k = 0; k + 2 < x.size(); k += 3) { sum[0] += x[k]; sum[1] += x[k + 1]; sum[2] += x[k + 2]; n++; }
		}
	if (!n) return false;
	for (int k = 0; k < 3; k++) fit.grip[k] = (float)(sum[k] / (double)n);

	if (const char *dump = getenv("SOF_GUNDUMP"))
	{
		std::string fn = std::string(dump) + "_" + (surfs.empty() ? std::string("?") : surfs[0].objectDir) + "_" + Ghoul_PlayingSequenceName(gun) + ".obj";
		for (size_t i = strlen(dump); i < fn.size(); i++) if (fn[i] == '/') fn[i] = '_';
		if (FILE *df = fopen(fn.c_str(), "w"))
		{
			size_t base = 1;
			for (size_t i = 0; i < surfs.size(); i++)
			{
				fprintf(df, "o %s\nusemtl %s/%s\n", surfs[i].part.c_str(), surfs[i].objectDir.c_str(), surfs[i].skin.c_str());
				const std::vector<float> &x = surfs[i].xyz;
				for (size_t k = 0; k + 2 < x.size(); k += 3) fprintf(df, "v %f %f %f\n", x[k], x[k + 1], x[k + 2]);
				for (size_t k = 0; k + 1 < surfs[i].st.size(); k += 2) fprintf(df, "vt %f %f\n", surfs[i].st[k], surfs[i].st[k + 1]);
				for (size_t k = 0; k + 2 < surfs[i].indices.size(); k += 3)
					fprintf(df, "f %zu/%zu %zu/%zu %zu/%zu\n", base + surfs[i].indices[k], base + surfs[i].indices[k], base + surfs[i].indices[k + 1],
					        base + surfs[i].indices[k + 1], base + surfs[i].indices[k + 2], base + surfs[i].indices[k + 2]);
				base += x.size() / 3;
			}
			fclose(df);
		}
	}
	/* the barrel: the long axis of the gun around its muzzle (the gun's vertices without
	   hands and arms, weighted by closeness to the muzzle bolt - the flat models stretch
	   grips and magazines far down past the screen edge), pointing at the muzzle; the
	   gun's up is from the hand that holds it toward the muzzle. Without a muzzle bolt
	   (knife) the whole model's long axis is used, pointing forward. */
	float f[3] = { 1, 0, 0 }, u[3] = { 0, 0, 1 };
	{
		float muzzle[3] = { 0, 0, 0 };
		GhoulID flash = obj->FindPart("flash");
		if (flash)
		{
			Matrix4 m;
			gun->GetBoltMatrix(time, m, flash, IGhoulInst::MatrixType::Entity, true);
			Vect3 r3;
			m.GetRow(3, r3);
			muzzle[0] = r3.x(); muzzle[1] = r3.y(); muzzle[2] = r3.z();
		}
		std::vector<const float *> pts;
		for (size_t i = 0; i < surfs.size(); i++)
		{
			const std::string &pn = surfs[i].part;
			if (partIs(pn, "HAND", 0) || partIs(pn, "ARM", 0) || partIs(pn, "SLEEVE", 0)) continue;
			const std::vector<float> &x = surfs[i].xyz;
			for (size_t k = 0; k + 2 < x.size(); k += 3) pts.push_back(&x[k]);
		}
		if (pts.size() > 8)
		{
			double sigma2 = 1e12;
			if (flash)
			{
				double far = 0;
				for (size_t i = 0; i < pts.size(); i++)
				{
					double d = 0;
					for (int a = 0; a < 3; a++) d += (pts[i][a] - muzzle[a]) * (pts[i][a] - muzzle[a]);
					if (d > far) far = d;
				}
				sigma2 = far * 0.0625; /* sigma: a quarter of the gun's reach from the muzzle */
				if (sigma2 < 1) sigma2 = 1;
			}
			double wsum = 0, mean[3] = { 0, 0, 0 }, cov[3][3] = { { 0 } };
			std::vector<double> w(pts.size());
			for (size_t i = 0; i < pts.size(); i++)
			{
				double d = 0;
				if (flash) for (int a = 0; a < 3; a++) d += (pts[i][a] - muzzle[a]) * (pts[i][a] - muzzle[a]);
				w[i] = exp(-d / sigma2);
				wsum += w[i];
				for (int a = 0; a < 3; a++) mean[a] += w[i] * pts[i][a];
			}
			for (int a = 0; a < 3; a++) mean[a] /= wsum;
			for (size_t i = 0; i < pts.size(); i++)
			{
				double d[3] = { pts[i][0] - mean[0], pts[i][1] - mean[1], pts[i][2] - mean[2] };
				for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) cov[a][b] += w[i] * d[a] * d[b];
			}
			/* the flat weapons are modelled level and upright (only their grips run long),
			   so keep that and take just the barrel's heading from the muzzle area */
			double v[2] = { 1, 0.05 };
			for (int it = 0; it < 64; it++)
			{
				double nv[2] = { cov[0][0] * v[0] + cov[0][1] * v[1], cov[1][0] * v[0] + cov[1][1] * v[1] };
				double l = sqrt(nv[0] * nv[0] + nv[1] * nv[1]);
				if (l <= 0) break;
				v[0] = nv[0] / l; v[1] = nv[1] / l;
			}
			if (v[0] < 0) { v[0] = -v[0]; v[1] = -v[1]; }
			/* a heading more than 45 degrees off forward is not a barrel (round muzzle areas) */
			if (!flash || v[0] < 0.7071) { v[0] = 1; v[1] = 0; }
			else fit.measured = true;
			f[0] = (float)v[0]; f[1] = (float)v[1]; f[2] = 0;
			u[0] = 0; u[1] = 0; u[2] = 1;
			if (!flash)
			{
				/* blades (knife): the flat view holds them upright; point the blade the way
				   the controller points, along the model's long axis away from the hand */
				double w3[3] = { 0.3, 0.3, 1 };
				for (int it = 0; it < 64; it++)
				{
					double nv[3];
					for (int a = 0; a < 3; a++) nv[a] = cov[a][0] * w3[0] + cov[a][1] * w3[1] + cov[a][2] * w3[2];
					double l = sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
					if (l <= 0) break;
					for (int a = 0; a < 3; a++) w3[a] = nv[a] / l;
				}
				double sgn = 0;
				for (int a = 0; a < 3; a++) sgn += (mean[a] - fit.grip[a]) * w3[a];
				for (int a = 0; a < 3; a++) f[a] = (float)(sgn < 0 ? -w3[a] : w3[a]);
				/* keep the model's sideways axis; up follows */
				float l0[3] = { 0, 1, 0 };
				float lf = l0[0] * f[0] + l0[1] * f[1] + l0[2] * f[2];
				for (int a = 0; a < 3; a++) l0[a] -= lf * f[a];
				normalize3(l0);
				u[0] = f[1] * l0[2] - f[2] * l0[1];
				u[1] = f[2] * l0[0] - f[0] * l0[2];
				u[2] = f[0] * l0[1] - f[1] * l0[0];
				fit.measured = true;
			}
			if (getenv("SOF_DEBUG"))
			{
				char b[200];
				snprintf(b, sizeof(b), "[sof] gun axis: %zu points, muzzle %.1f %.1f %.1f sigma %.1f\n", pts.size(),
				         muzzle[0], muzzle[1], muzzle[2], sqrt(sigma2));
				q2b_dprint(b);
			}
		}
	}
	normalize3(f);
	float d = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
	for (int k = 0; k < 3; k++) u[k] -= d * f[k];
	normalize3(u);
	cp3(f, fit.fwd);
	cp3(u, fit.up);
	/* left = up x forward */
	fit.left[0] = u[1] * f[2] - u[2] * f[1];
	fit.left[1] = u[2] * f[0] - u[0] * f[2];
	fit.left[2] = u[0] * f[1] - u[1] * f[0];
	fit.valid = true;
	if (getenv("SOF_DEBUG"))
	{
		char b[300];
		snprintf(b, sizeof(b), "[sof] gun fit %s (%s): grip %.1f %.1f %.1f fwd %.2f %.2f %.2f up %.2f %.2f %.2f%s\n",
		         Ghoul_PlayingSequenceName(gun), surfs.empty() ? "?" : surfs[0].objectDir.c_str(),
		         fit.grip[0], fit.grip[1], fit.grip[2], f[0], f[1], f[2], u[0], u[1], u[2], obj->FindPart("flash") ? "" : " (no flash bolt)");
		q2b_dprint(b);
	}
	return true;
}

/* entity space of the flat weapon -> the hand's space (grip at the origin, barrel along x) */
static void gunFitPoint(const GunFit &g, const float *p, float *o)
{
	float d[3] = { p[0] - g.grip[0], p[1] - g.grip[1], p[2] - g.grip[2] };
	o[0] = d[0] * g.fwd[0] + d[1] * g.fwd[1] + d[2] * g.fwd[2];
	o[1] = d[0] * g.left[0] + d[1] * g.left[1] + d[2] * g.left[2];
	o[2] = d[0] * g.up[0] + d[1] * g.up[1] + d[2] * g.up[2];
}
/* the weapon's size (uniform) and length (along the barrel) - for points on the gun */
static void gunFitSize(const GunFit &g, float *o)
{
	float k = g.k > 0 ? g.k : 1;
	o[0] *= k * g.sx;
	o[1] *= k;
	o[2] *= k;
}
static void gunFitDir(const GunFit &g, const float *p, float *o)
{
	float d[3] = { p[0], p[1], p[2] };
	o[0] = d[0] * g.fwd[0] + d[1] * g.fwd[1] + d[2] * g.fwd[2];
	o[1] = d[0] * g.left[0] + d[1] * g.left[1] + d[2] * g.left[2];
	o[2] = d[0] * g.up[0] + d[1] * g.up[1] + d[2] * g.up[2];
}

/* the idle sequence that is the weapon's rest pose */
static std::string restSequence(IGhoulInst *gun)
{
	std::vector<std::string> names;
	Ghoul_SequenceNames(gun, names);
	const char *prefer[] = { "idle_a", "idle" };
	for (int p = 0; p < 2; p++)
		for (size_t i = 0; i < names.size(); i++)
			if (!strcasecmp(names[i].c_str(), prefer[p])) return names[i];
	for (size_t i = 0; i < names.size(); i++)
		if (!strncasecmp(names[i].c_str(), "idle", 4) && names[i].find("_to_") == std::string::npos) return names[i];
	return std::string();
}

static bool partIsGunBody(const std::string &pn)
{
	return !partIs(pn, "HAND", 0) && !partIs(pn, "ARM", 0) && !partIs(pn, "SLEEVE", 0);
}

/* orientation, grip and size of a weapon, from one pose of it */
static void measureGun(IGhoulInst *gun, const std::string &seq, GunFit &fit)
{
	fit.valid = false;
	bool posed = !seq.empty() && Ghoul_SetPose(gun, seq.c_str());
	std::vector<GhoulDrawSurface> s;
	float t = posed ? 0.0f : ((float)g_frame - 1.0f + g_drawLerp) * 0.1f;
	Ghoul_BuildDrawList(gun, t, s);
	fitGun(gun, t, s, fit);
	if (posed) Ghoul_RestorePose(gun);
	if (!fit.valid) return;

	/* size: the right hand to a real hand's size (12 cm at vr_worldscale 36); the gun
	   stretched along the barrel to its real length (the characters' model of the same
	   weapon, at 3/4 - those are made a third larger than life) */
	bool hasHigh = false;
	for (size_t i = 0; i < s.size(); i++) if (partIs(s[i].part, "HIGH_RES", 0)) hasHigh = true;
	float hlo[3] = { 1e9f, 1e9f, 1e9f }, hhi[3] = { -1e9f, -1e9f, -1e9f }, gx0 = 1e9f, gx1 = -1e9f;
	for (size_t i = 0; i < s.size(); i++)
	{
		const std::string &pn = s[i].part;
		if (hasHigh && (partIs(pn, "MEDIUM_RES", 0) || partIs(pn, "LOW_RES", 0))) continue;
		bool rhand = partIs(pn, "_R_", "HAND"), body = partIsGunBody(pn);
		if (!rhand && !body) continue;
		for (size_t v = 0; v + 2 < s[i].xyz.size(); v += 3)
		{
			float o[3];
			gunFitPoint(fit, &s[i].xyz[v], o);
			if (rhand) for (int a = 0; a < 3; a++) { if (o[a] < hlo[a]) hlo[a] = o[a]; if (o[a] > hhi[a]) hhi[a] = o[a]; }
			else { if (o[0] < gx0) gx0 = o[0]; if (o[0] > gx1) gx1 = o[0]; }
		}
	}
	float hand = 0;
	for (int a = 0; a < 3; a++) if (hhi[a] - hlo[a] > hand) hand = hhi[a] - hlo[a];
	static void *cv_ws;
	if (!cv_ws) cv_ws = q2b_cvar("vr_weaponscale", "0.56", 0);
	float ws = q2b_cvar_value(cv_ws);
	if (ws <= 0.01f) ws = 1;
	fit.k = hand > 0.01f ? (4.3f / ws) / hand : 1.0f;
	fit.sx = 1;
	std::string dir = s.empty() ? std::string() : s[0].objectDir;
	float wl = worldGunLength(dir);
	if (wl > 0 && gx1 > gx0)
	{
		fit.sx = (0.75f * wl / ws) / ((gx1 - gx0) * fit.k);
		if (fit.sx < 0.8f) fit.sx = 0.8f;
		if (fit.sx > 3.0f) fit.sx = 3.0f;
	}
	if (getenv("SOF_DEBUG"))
	{
		char b[240];
		snprintf(b, sizeof(b), "[sof] gun size %s (%s): hand %.1f -> scale %.2f, length %.1f (characters' %.1f) -> stretch %.2f\n",
		         dir.c_str(), seq.c_str(), hand, fit.k, gx1 - gx0, wl, fit.sx);
		q2b_dprint(b);
	}
}

/* where the right hand holds the weapon in one of its poses */
static bool measureGrip(IGhoulInst *gun, const char *seq, float *grip)
{
	if (!Ghoul_SetPose(gun, seq)) return false;
	std::vector<GhoulDrawSurface> s;
	Ghoul_BuildDrawList(gun, 0, s);
	Ghoul_RestorePose(gun);
	double sum[3] = { 0, 0, 0 };
	size_t n = 0;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (!partIs(s[i].part, "_R_", "HAND")) continue;
		for (size_t v = 0; v + 2 < s[i].xyz.size(); v += 3) { sum[0] += s[i].xyz[v]; sum[1] += s[i].xyz[v + 1]; sum[2] += s[i].xyz[v + 2]; n++; }
	}
	if (!n) return false;
	for (int a = 0; a < 3; a++) grip[a] = (float)(sum[a] / (double)n);
	return true;
}

static const sofdraw_t *hookViewWeaponDraw(int client, float lerpfrac)
{
	edict_t *e = edictOf(client);
	if (!e || !e->client) return 0;
	IGhoulInst *gun = ((player_state_t *)e->client)->gun;
	const sofdraw_t *d = buildDraw(-client, gun, lerpfrac);
	g_gunFitNow.valid = false;
	if (!d || !gun || !gun->GetGhoulObject()) return d;

	static void *cv_fit;
	if (!cv_fit) cv_fit = q2b_cvar("sof_vrgunfit", "1", 0);
	if (q2b_cvar_value(cv_fit) == 0) return d;

	DrawCache &c = g_drawCache[-client];
	const void *obj = gun->GetGhoulObject();
	float time = ((float)g_frame - 1.0f + g_drawLerp) * 0.1f;
	std::string seq = Ghoul_PlayingSequenceName(gun);
	for (size_t i = 0; i < seq.size(); i++) seq[i] = (char)tolower((unsigned char)seq[i]);
	bool idle = seq.compare(0, 4, "idle") == 0 && seq.find("_to_") == std::string::npos;

	/* the fit is measured on the weapon at rest - the first frame of its idle sequence -
	   whatever is playing, so a raise or reload animation never skews it */
	GunFit fit;
	fit.valid = false;
	{
		std::map<const void *, GunFit>::iterator ax = g_gunAxes.find(obj);
		if (ax == g_gunAxes.end())
		{
			GunFit a;
			measureGun(gun, restSequence(gun), a);
			ax = g_gunAxes.insert(std::make_pair(obj, a)).first;
		}
		if (!ax->second.valid) return d;
		fit = ax->second;
		/* the grip per idle pose (two-handed holds bring the gun in), held through the
		   animations that follow it */
		if (idle)
		{
			std::pair<const void *, std::string> key(obj, seq);
			std::map<std::pair<const void *, std::string>, GunFit>::iterator it = g_gunFits.find(key);
			if (it == g_gunFits.end())
			{
				GunFit gp = fit;
				float grip[3];
				if (measureGrip(gun, seq.c_str(), grip)) cp3(grip, gp.grip);
				it = g_gunFits.insert(std::make_pair(key, gp)).first;
			}
			cp3(it->second.grip, fit.grip);
			g_gunFitLast[obj] = fit;
		}
		else
		{
			std::map<const void *, GunFit>::iterator it = g_gunFitLast.find(obj);
			if (it != g_gunFitLast.end()) cp3(it->second.grip, fit.grip);
		}
	}
	(void)time;
	g_gunFitNow = fit;

	/* which surfaces to draw: no sleeves (they ran off the screen edge), one set of hands
	   (the model carries three levels of detail) */
	bool hasHigh = false;
	for (size_t i = 0; i < c.surfs.size(); i++) if (partIs(c.surfs[i].part, "HIGH_RES", 0)) hasHigh = true;
	std::vector<char> keep(c.surfs.size(), 1);
	for (size_t i = 0; i < c.surfs.size(); i++)
	{
		const std::string &pn = c.surfs[i].part;
		if (partIs(pn, "SLEEVE", 0)) keep[i] = 0;
		if (hasHigh && (partIs(pn, "MEDIUM_RES", 0) || partIs(pn, "LOW_RES", 0))) keep[i] = 0;
	}

	/* into the hand */
	for (size_t i = 0; i < c.surfs.size(); i++)
	{
		if (!keep[i]) continue;
		GhoulDrawSurface &s = c.surfs[i];
		for (size_t k = 0; k + 2 < s.xyz.size(); k += 3) { float o[3]; gunFitPoint(fit, &s.xyz[k], o); cp3(o, &s.xyz[k]); }
		for (size_t k = 0; k + 2 < s.normal.size(); k += 3) { float o[3]; gunFitDir(fit, &s.normal[k], o); cp3(o, &s.normal[k]); }
	}

	/* hand and gun extents (now in the hand's space: x along the barrel, z up) */
	float rhLo = 1e9f, rhHi = -1e9f, gunLo = 1e9f, gunReach = 0;
	for (size_t i = 0; i < c.surfs.size(); i++)
	{
		if (!keep[i]) continue;
		const std::string &pn = c.surfs[i].part;
		const std::vector<float> &x = c.surfs[i].xyz;
		bool rhand = partIs(pn, "_R_", "HAND"), hand = partIs(pn, "HAND", 0) || partIs(pn, "ARM", 0);
		for (size_t k = 0; k + 2 < x.size(); k += 3)
		{
			if (rhand) { if (x[k + 2] < rhLo) rhLo = x[k + 2]; if (x[k + 2] > rhHi) rhHi = x[k + 2]; }
			else if (!hand)
			{
				if (x[k + 2] < gunLo) gunLo = x[k + 2];
				float r = sqrtf(x[k] * x[k] + x[k + 1] * x[k + 1] + x[k + 2] * x[k + 2]);
				if (r > gunReach) gunReach = r;
			}
		}
	}

	/* the flat models run grips and magazines far down past the screen edge: shorten
	   whatever hangs more than half a hand below the hand */
	if (rhHi > rhLo && gunLo < rhLo)
	{
		float allow = 0.5f * (rhHi - rhLo), ext = rhLo - gunLo;
		if (ext > allow)
		{
			float k = allow / ext;
			for (size_t i = 0; i < c.surfs.size(); i++)
			{
				if (!keep[i] || partIs(c.surfs[i].part, "HAND", 0)) continue;
				std::vector<float> &x = c.surfs[i].xyz;
				for (size_t v = 0; v + 2 < x.size(); v += 3)
					if (x[v + 2] < rhLo) x[v + 2] = rhLo + (x[v + 2] - rhLo) * k;
			}
		}
	}

	/* the left hand only shows when it is on the weapon (two-handed grips, reloads); in
	   one-handed poses it hangs in mid air where the flat screen cut it off */
	{
		double sum[3] = { 0, 0, 0 };
		size_t n = 0;
		for (size_t i = 0; i < c.surfs.size(); i++)
		{
			if (!keep[i] || !partIs(c.surfs[i].part, "_L_", "HAND")) continue;
			const std::vector<float> &x = c.surfs[i].xyz;
			for (size_t k = 0; k + 2 < x.size(); k += 3) { sum[0] += x[k]; sum[1] += x[k + 1]; sum[2] += x[k + 2]; n++; }
		}
		if (n)
		{
			float lh[3] = { (float)(sum[0] / n), (float)(sum[1] / n), (float)(sum[2] / n) };
			float best = 1e18f;
			for (size_t i = 0; i < c.surfs.size(); i++)
			{
				const std::string &pn = c.surfs[i].part;
				if (!keep[i] || partIs(pn, "HAND", 0) || partIs(pn, "ARM", 0)) continue;
				const std::vector<float> &x = c.surfs[i].xyz;
				for (size_t k = 0; k + 2 < x.size(); k += 3)
				{
					float dx = x[k] - lh[0], dy = x[k + 1] - lh[1], dz = x[k + 2] - lh[2];
					float dd = dx * dx + dy * dy + dz * dz;
					if (dd < best) best = dd;
				}
			}
			float handSize = rhHi > rhLo ? rhHi - rhLo : 4.0f;
			/* off the weapon, or hanging below the gun hand (not holding anything) */
			if (sqrtf(best) > handSize || lh[2] < rhLo)
				for (size_t i = 0; i < c.surfs.size(); i++)
					if (partIs(c.surfs[i].part, "_L_", "HAND")) keep[i] = 0;
		}
	}

	/* size (measured with the fit): hand to a real hand's size, gun to its real length */
	{
		/* left hand: keeps its spot on the stretched gun (moves, is not stretched) */
		double lsum = 0;
		size_t ln = 0;
		for (size_t i = 0; i < c.surfs.size(); i++)
		{
			if (!keep[i] || !partIs(c.surfs[i].part, "_L_", "HAND")) continue;
			const std::vector<float> &x = c.surfs[i].xyz;
			for (size_t v = 0; v + 2 < x.size(); v += 3) { lsum += x[v]; ln++; }
		}
		float lshift = ln ? (float)(lsum / (double)ln) * fit.k * (fit.sx - 1) : 0;
		for (size_t i = 0; i < c.surfs.size(); i++)
		{
			if (!keep[i]) continue;
			const std::string &pn = c.surfs[i].part;
			bool rhand = partIs(pn, "_R_", "HAND"), lhand = partIs(pn, "_L_", "HAND");
			std::vector<float> &x = c.surfs[i].xyz;
			for (size_t v = 0; v + 2 < x.size(); v += 3)
			{
				x[v] *= fit.k; x[v + 1] *= fit.k; x[v + 2] *= fit.k;
				if (lhand) x[v] += lshift;
				else if (!rhand) x[v] *= fit.sx;
			}
		}
	}

	/* VR rifle scope: the renderer shows the magnified view in the eyepiece. The disc sits
	   just behind the eyepiece's back face (the eye side), across its widest part. */
	{
		static void *cv_scope;
		if (!cv_scope) cv_scope = q2b_cvar("sof_vrscope", "1", 0);
		float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
		bool found = false;
		if (q2b_cvar_value(cv_scope) != 0)
			for (size_t i = 0; i < c.surfs.size(); i++)
			{
				if (!keep[i] || !partIs(c.surfs[i].part, "SCOPEEYE", 0)) continue;
				const std::vector<float> &x = c.surfs[i].xyz;
				for (size_t k = 0; k + 2 < x.size(); k += 3)
					for (int a = 0; a < 3; a++) { if (x[k + a] < lo[a]) lo[a] = x[k + a]; if (x[k + a] > hi[a]) hi[a] = x[k + a]; }
				found = true;
			}
		if (found)
		{
			c.draw.hasscope = 1;
			c.draw.scope[0] = lo[0] - 0.05f;
			c.draw.scope[1] = 0.5f * (lo[1] + hi[1]);
			c.draw.scope[2] = 0.5f * (lo[2] + hi[2]);
			float ry = 0.5f * (hi[1] - lo[1]), rz = 0.5f * (hi[2] - lo[2]);
			c.draw.scope[3] = (ry > rz ? ry : rz) * 0.95f;
		}
	}

	if (const char *dump = getenv("SOF_GUNDUMP2"))
	{
		static std::set<std::string> done;
		std::string fn = std::string(dump) + "_" + c.surfs[0].objectDir + "_" + Ghoul_PlayingSequenceName(gun) + ".obj";
		for (size_t i = strlen(dump); i < fn.size(); i++) if (fn[i] == '/') fn[i] = '_';
		if (done.insert(fn).second)
			if (FILE *df = fopen(fn.c_str(), "w"))
			{
				size_t base = 1;
				for (size_t i = 0; i < c.surfs.size(); i++)
				{
					if (!keep[i]) continue;
					const GhoulDrawSurface &s = c.surfs[i];
					fprintf(df, "o %s\nusemtl %s/%s\n", s.part.c_str(), s.objectDir.c_str(), s.skin.c_str());
					for (size_t k = 0; k + 2 < s.xyz.size(); k += 3) fprintf(df, "v %f %f %f\n", s.xyz[k], s.xyz[k + 1], s.xyz[k + 2]);
					for (size_t k = 0; k + 1 < s.st.size(); k += 2) fprintf(df, "vt %f %f\n", s.st[k], s.st[k + 1]);
					for (size_t k = 0; k + 2 < s.indices.size(); k += 3)
						fprintf(df, "f %zu/%zu %zu/%zu %zu/%zu\n", base + s.indices[k], base + s.indices[k], base + s.indices[k + 1],
						        base + s.indices[k + 1], base + s.indices[k + 2], base + s.indices[k + 2]);
					base += s.xyz.size() / 3;
				}
				fclose(df);
			}
	}

	size_t out = 0;
	for (size_t i = 0; i < c.surfs.size(); i++)
	{
		if (!keep[i]) continue;
		if (out != i) c.meshes[out] = c.meshes[i];
		out++;
	}
	c.draw.nummeshes = (int)out;
	return &c.draw;
}

extern "C" void Pmove_SetSoFHeights(int sof) __attribute__((weak));
static void registerClientHooks(void)
{
	if (Pmove_SetSoFHeights) Pmove_SetSoFHeights(1);
	if (CL_SoF_RegisterHooks) CL_SoF_RegisterHooks(hookEntityDraw, hookViewWeaponDraw);
	fxRegister();
}

/* The local client renders GHOUL entities by asking the server-side instances directly
 * (single process). Returns the number of surfaces appended. */
extern "C" void *SoF_GetEntityGhoulInst(int num)
{
	edict_t *e = edictOf(num);
	return e && e->inuse ? e->ghoulInst : 0;
}
extern "C" void *SoF_GetViewWeaponInst(int client)
{
	edict_t *e = edictOf(client);
	if (!e || !e->client) return 0;
	return ((player_state_t *)e->client)->gun;
}
extern "C" float SoF_LevelTime(void)
{
	return (float)g_frame * 0.1f;
}

/* ------------------------------------------------------------------ client effects
 * SoF's client runs effects (.eft) for svc_effect messages, temp entities and the view
 * weapon's "effect"/"sound" animation notes. Here the game and the client share one
 * process, so the messages are decoded straight from the game's writes (handleMessage)
 * and the effects run in sof/fx, which the client asks for every frame (hookFxFrame). */
extern "C" void CL_SoF_RegisterFxHook(sof_fxframe_t) __attribute__((weak));

static void cp3(const float *a, float *b);
static void angleVectors(const float *angles, float *forward, float *right, float *up);
static void normalize3(float *v);

struct MsgReader
{
	const unsigned char *d; size_t n, p; bool bad;
	MsgReader(const std::vector<unsigned char> &v) : d(v.empty() ? 0 : &v[0]), n(v.size()), p(0), bad(false) {}
	bool need(size_t k) { if (p + k > n) { bad = true; return false; } return true; }
	int byte() { if (!need(1)) return 0; return d[p++]; }
	int shrt() { short v = 0; if (need(2)) { memcpy(&v, d + p, 2); p += 2; } return v; }
	int lng() { int v = 0; if (need(4)) { memcpy(&v, d + p, 4); p += 4; } return v; }
	float flt() { float v = 0; if (need(4)) { memcpy(&v, d + p, 4); p += 4; } return v; }
	void pos(float *o) { o[0] = flt(); o[1] = flt(); o[2] = flt(); }
};

enum { SOF_SVC_TEMP_ENTITY = 1, SOF_SVC_EFFECT = 5 };
enum { EFAT_POS = 0x01, EFAT_ENT = 0x02, EFAT_BOLT = 0x04, EFAT_BOLTANDINST = 0x08, EFAT_ALTAXIS = 0x40, EFAT_HASFLAGS = 0x80 };

static bool parseEffect(MsgReader &r)
{
	int id = r.byte();
	int sf = r.byte();
	sfx::Anchor a;
	if (sf & EFAT_POS) { a.kind = sfx::ANCHOR_POS; r.pos(a.pos); }
	else if (sf & EFAT_ENT) { a.kind = sfx::ANCHOR_ENT; a.ent = r.shrt(); }
	else if (sf & EFAT_BOLT)
	{
		a.kind = sfx::ANCHOR_BOLT;
		a.ent = r.shrt();
		if (sf & EFAT_BOLTANDINST)
		{
			a.uuid = r.shrt();
			a.inst = Ghoul_FindInst((short)a.uuid);
		}
		a.bolt = r.shrt();
		a.altAxis = (sf & EFAT_ALTAXIS) != 0;
	}
	sfx::Params p;
	if (sf & EFAT_HASFLAGS)
	{
		int fl = r.byte();
		p.flags = (unsigned)fl;
		if (fl & sfx::EFF_SCALE) p.scale = (float)r.shrt() / 128.0f;
		if (fl & sfx::EFF_NUMELEMS) p.numElements = r.byte();
		if (fl & sfx::EFF_POS2) r.pos(p.pos2);
		if (fl & sfx::EFF_DIR) { r.pos(p.dir); for (int k = 0; k < 3; k++) p.dir[k] /= 2048.0f; }
		if (fl & sfx::EFF_MIN) r.pos(p.mins);
		if (fl & sfx::EFF_MAX) r.pos(p.maxs);
		if (fl & sfx::EFF_LIFETIME) p.lifetime = (float)r.shrt() / 128.0f;
		if (fl & sfx::EFF_RADIUS) p.radius = (float)r.byte();
	}
	if (r.bad) return false;
	if (id < 1 || id > (int)g_effects.size()) return true;
	if (getenv("SOF_DEBUG"))
	{
		char b[256];
		snprintf(b, sizeof(b), "[sof fx] effect %s (anchor %d ent %d bolt %d)\n", g_effects[(size_t)id - 1].c_str(), a.kind, a.ent, a.bolt);
		q2b_dprint(b);
	}
	if (a.kind == sfx::ANCHOR_BOLT && !a.inst)
	{
		edict_t *e = edictOf(a.ent);
		if (e && e->inuse && e->ghoulInst) { a.inst = e->ghoulInst; a.uuid = ((IGhoulInst *)a.inst)->MyUUID(); }
	}
	sfx::Start(g_effects[(size_t)id - 1].c_str(), a, p);
	return true;
}

/* ---- temp entities (SoF's client-side impact effects) */
/* TE_* and RI_* come from q_sh_fx.h */
enum { SOFSURF_METAL = 1, SOFSURF_BLOOD = 35, SOFSURF_NUM = 43 };

struct Impact
{
	float pos[3], normal[3];
	int surf;        /* SoF surface type (texinfo flags >> 24), -1 unknown */
	int surfFlags;
};

/* SoF's per-surface impact data: dust colour, decal sprite, impact sound and handler */
enum { IH_PUFF, IH_STONE, IH_EFFECT, IH_NONE, IH_SOUND, IH_GLASS };
struct SurfImpact { unsigned char rgba[4]; int sound; int decal; int handler; const char *effect; };
static const SurfImpact g_surfImpact[SOFSURF_NUM] =
{
	{ { 150, 150, 150, 250 }, 37, 13, IH_STONE, 0 },                          /* default */
	{ { 250, 250, 250, 250 }, 44, 42, IH_EFFECT, "environ/wallspark" },       /* metal */
	{ { 180, 140, 100, 250 }, 42, 13, IH_PUFF, 0 },                           /* sand */
	{ { 200, 200, 200, 250 }, 42, 13, IH_PUFF, 0 },
	{ { 180, 130, 70, 250 }, 42, 13, IH_PUFF, 0 },
	{ { 120, 90, 20, 250 }, 42, 13, IH_PUFF, 0 },
	{ { 160, 160, 160, 250 }, 41, 13, IH_PUFF, 0 },                           /* gravel */
	{ { 120, 90, 20, 250 }, 41, 13, IH_PUFF, 0 },
	{ { 180, 140, 30, 250 }, 41, 13, IH_PUFF, 0 },
	{ { 230, 230, 230, 150 }, 43, 13, IH_PUFF, 0 },                           /* snow */
	{ { 0, 200, 200, 250 }, 38, 13, IH_EFFECT, "environ/bulletsplash" },      /* liquids */
	{ { 0, 100, 50, 250 }, 38, 13, IH_EFFECT, "environ/splatgreen" },
	{ { 190, 100, 40, 250 }, 38, 13, IH_EFFECT, "environ/splatorange" },
	{ { 180, 140, 20, 250 }, 38, 13, IH_EFFECT, "environ/splatbrown" },
	{ { 180, 140, 60, 250 }, 39, 43, IH_STONE, 0 },                           /* wood */
	{ { 120, 90, 30, 250 }, 39, 43, IH_STONE, 0 },
	{ { 80, 80, 80, 250 }, 39, 43, IH_STONE, 0 },
	{ { 180, 180, 180, 250 }, 37, 13, IH_STONE, 0 },                          /* stone */
	{ { 90, 90, 90, 250 }, 37, 13, IH_STONE, 0 },
	{ { 180, 140, 50, 250 }, 37, 13, IH_STONE, 0 },
	{ { 130, 90, 30, 250 }, 37, 13, IH_STONE, 0 },
	{ { 240, 240, 240, 250 }, 37, 13, IH_STONE, 0 },
	{ { 80, 140, 80, 250 }, 37, 13, IH_STONE, 0 },
	{ { 120, 20, 20, 250 }, 37, 13, IH_STONE, 0 },
	{ { 10, 10, 10, 250 }, 37, 13, IH_STONE, 0 },
	{ { 20, 120, 20, 250 }, 40, 13, IH_STONE, 0 },                            /* grass */
	{ { 120, 120, 20, 250 }, 40, 13, IH_STONE, 0 },
	{ { 100, 25, 25, 250 }, 38, 13, IH_EFFECT, "environ/splatred" },
	{ { 250, 250, 250, 250 }, 44, 42, IH_EFFECT, "environ/wallspark" },       /* metal: steam, water, oil, chem, computers */
	{ { 250, 250, 250, 250 }, 44, 42, IH_EFFECT, "environ/waterspurt" },
	{ { 250, 250, 250, 250 }, 44, 42, IH_EFFECT, "environ/oilspurt" },
	{ { 250, 250, 250, 250 }, 44, 42, IH_EFFECT, "environ/chemspurt" },
	{ { 250, 250, 250, 250 }, 44, 42, IH_EFFECT, "environ/metal_computer" },
	{ { 180, 140, 50, 250 }, 37, 13, IH_PUFF, 0 },                            /* snow */
	{ { 180, 180, 180, 250 }, 37, 13, IH_PUFF, 0 },
	{ { 180, 180, 180, 250 }, 37, 13, IH_NONE, 0 },                           /* blood */
	{ { 23, 25, 25, 250 }, 38, 13, IH_EFFECT, "environ/splatblack" },
	{ { 250, 250, 250, 250 }, 44, 33, IH_GLASS, 0 },                          /* glass */
	{ { 250, 250, 250, 250 }, 44, 33, IH_EFFECT, "environ/glass_computer" },
	{ { 250, 250, 250, 250 }, 44, 33, IH_EFFECT, "environ/wallspark" },       /* soda machine */
	{ { 240, 240, 240, 250 }, 39, 43, IH_SOUND, 0 },                          /* paper wall */
	{ { 23, 25, 25, 250 }, 44, 13, IH_NONE, 0 },                              /* newspaper */
	{ { 250, 250, 250, 250 }, 44, 42, IH_EFFECT, "environ/wallspark" },
};

/* SoF's numbered client sounds: cl_fxs in the SDK's q_sh_fx.cpp (built into the adapter) */
static const char *clientSoundName(int i)
{
	static std::string names[NUM_CLSFX];
	if (i < 0 || i >= NUM_CLSFX || !cl_fxs[i].filename) return 0;
	if (names[i].empty())
	{
		names[i] = cl_fxs[i].filename;
		for (size_t k = 0; k < names[i].size(); k++) names[i][k] = (char)tolower((unsigned char)names[i][k]);
	}
	return names[i].c_str();
}
static const char *clientSound(int i)
{
	return clientSoundName(i);
}

static float frand01(void) { return (float)(rand() & 0x7fff) / 32767.0f; }
static float frandr(float a, float b) { return a + (b - a) * frand01(); }

static void readDirExp(MsgReader &r, float *d) { r.pos(d); for (int k = 0; k < 3; k++) d[k] *= 0.001f; }

/* FXMSG_WriteRelativePos on the receiving end */
static bool readImpact(MsgReader &r, Impact &im)
{
	im.surf = -1;
	im.surfFlags = 0;
	int type = r.byte();
	switch (type)
	{
		case RI_WORLD:
		{
			r.pos(im.pos);
			readDirExp(r, im.normal);
			/* the surface: trace a little into the wall */
			float a[3], b[3];
			for (int k = 0; k < 3; k++) { a[k] = im.pos[k] + im.normal[k] * 4; b[k] = im.pos[k] - im.normal[k] * 4; }
			q2b_trace_t t;
			q2b_trace(a, 0, 0, b, -1, 1 /* CONTENTS_SOLID */, &t);
			if (t.fraction < 1)
			{
				im.surf = (int)(((unsigned)t.surfflags) >> 24);
				im.surfFlags = t.surfflags;
			}
			break;
		}
		case RI_WORLD_NOSURF:
			r.pos(im.pos);
			im.normal[0] = im.normal[1] = 0; im.normal[2] = 1;
			break;
		case RI_BMODEL:
		{
			float local[3], ldir[3];
			r.pos(local);
			readDirExp(r, ldir);
			int ent = r.shrt();
			im.surf = r.byte();
			edict_t *e = edictOf(ent);
			float yaw = e ? e->s.angles[1] * (float)(M_PI / 180) : 0, c = cosf(yaw), sn = sinf(yaw);
			/* GetOffsetFromEnt in reverse */
			float x = c * local[0] + sn * local[1], y = sn * local[0] - c * local[1];
			im.pos[0] = (e ? e->s.origin[0] : 0) + x;
			im.pos[1] = (e ? e->s.origin[1] : 0) + y;
			im.pos[2] = (e ? e->s.origin[2] : 0) + local[2];
			im.normal[0] = c * ldir[0] - sn * ldir[1];
			im.normal[1] = sn * ldir[0] + c * ldir[1];
			im.normal[2] = ldir[2];
			break;
		}
		case RI_ENT:
			r.pos(im.pos);
			readDirExp(r, im.normal);
			im.surf = r.byte();
			normalize3(im.normal);
			break;
		default:
			return false;
	}
	return !r.bad;
}

/* the dust puff thrown off most surfaces */
static void impactPuff(const Impact &im, const float *dir, int count, int style)
{
	static const struct { float alongNormal, alongDir, fall; int lifeMs, grow, tex; } puffs[4] =
	{
		{ 30, 10, -150, 600, 3, 9 }, { 30, 10, -120, 600, 2, 12 }, { 20, 10, -100, 500, 2, 12 }, { 20, 10, -100, 1000, 2, 9 }
	};
	if (style < 0 || style > 3 || im.surf < 0 || im.surf >= SOFSURF_NUM) return;
	if (count > 4) count = 4;
	float sn = frandr(0.8f, 1.2f), sd = frandr(0.8f, 1.2f);
	const SurfImpact &si = g_surfImpact[im.surf];
	for (int i = 1; i <= count; i++)
	{
		sfx::Raw p;
		p.tex = sfx::SpriteName(puffs[style].tex);
		p.life = puffs[style].lifeMs * 0.001f;
		float a = i * puffs[style].alongNormal * sn, b = i * puffs[style].alongDir * sd;
		for (int k = 0; k < 3; k++) p.vel[k] = b * dir[k] + a * im.normal[k];
		p.vel[2] += 8.0f * i;
		p.acc[2] = i * puffs[style].fall;
		cp3(im.pos, p.pos);
		p.size0 = p.size1 = (float)count;
		p.grow0 = p.grow1 = (float)(puffs[style].grow * i) / p.life;
		memcpy(p.rgba, si.rgba, 4);
		p.alphaRate = -(float)si.rgba[3] / p.life;
		p.rot = frandr(0, 6.2831853f);
		sfx::SpawnRaw(p);
	}
}

/* stone / wood / grass: chips of dust kicked out of the wall, then a puff */
static void impactStone(const Impact &im, const float *dir, int size)
{
	for (int n = 0; n < 3; n++)
	{
		sfx::Raw p;
		p.tex = sfx::SpriteName(0);
		p.life = 0.6f;
		for (int k = 0; k < 3; k++)
			p.vel[k] = (frandr(0, 0.2f) + im.normal[k] - 0.1f) * (float)((rand() & 31) + 16) + (float)(rand() & 15) - 7.0f;
		cp3(im.pos, p.pos);
		p.size0 = p.size1 = (float)size * 1.5f;
		p.rgba[0] = p.rgba[1] = p.rgba[2] = 200; p.rgba[3] = 100;
		p.rot = (float)(rand() % 628) * 0.01f;
		p.grow0 = p.grow1 = (float)size * 1.6667f;
		p.alphaRate = -100 * 1.6667f;
		p.rotVel = (float)(rand() % 628) * 0.01f;
		sfx::SpawnRaw(p);
	}
	impactPuff(im, dir, (size > 2) + 1, 2);
}

/* TE_WALLDAMAGE: bullet hole, impact sound and the surface's dust / sparks / splash */
static void wallDamage(MsgReader &r, bool severe)
{
	Impact im;
	if (!readImpact(r, im)) return;
	float dir[3];
	readDirExp(r, dir);
	int size = r.byte(), markType = r.byte(), debris = r.byte();
	(void)debris;
	if (getenv("SOF_DEBUG"))
	{
		char b[200];
		snprintf(b, sizeof(b), "[sof fx] wall damage at %.0f %.0f %.0f surf %d flags %x size %d mark %d debris %d\n",
		         im.pos[0], im.pos[1], im.pos[2], im.surf, im.surfFlags, size, markType, debris);
		q2b_dprint(b);
	}
	if (r.bad || im.surf < 0 || im.surf >= SOFSURF_NUM) return;
	if (im.surfFlags & 0x84) return; /* sky / no-draw */
	const SurfImpact &si = g_surfImpact[im.surf];
	float fsize = severe ? 6.0f : (float)size;

	/* impact sound */
	if ((fsize > 1.5f || fsize == 0) && im.surf != SOFSURF_BLOOD)
	{
		float vol = (fsize * 0.0666667f * 0.4f + 0.2f) * 2;
		if (vol > 1) vol = 1;
		if (fsize == 0) vol = 0.6f;
		int snd = si.sound == 44 ? 79 + rand() % 3 : si.sound;
		if (clientSound(snd)) sfx::PlaySound(clientSound(snd), im.pos, 0, vol, 1);
	}

	/* bullet hole */
	int decal = markType == 1 ? si.decal : markType == 2 ? 34 : 0;
	if (decal && markType == 1)
	{
		if (fsize > 3) fsize = 3;
		float half = fsize * 0.6f + frandr(0, 0.1f);
		unsigned char c[4] = { si.rgba[0], si.rgba[1], si.rgba[2], (unsigned char)(200 + rand() % 41) };
		sfx::Decal(sfx::SpriteName(decal), im.pos, im.normal, half, c);
	}

	switch (si.handler)
	{
		case IH_PUFF: impactPuff(im, dir, size, 0); break;
		case IH_STONE: impactStone(im, dir, size); break;
		case IH_EFFECT: sfx::StartAt(si.effect, im.pos, im.normal); break;
		case IH_GLASS:
			impactPuff(im, dir, (size > 2) + 1, 2);
			sfx::PlaySound(clientSound(84), im.pos, 0, 0.6f, 1);
			break;
		case IH_SOUND: sfx::PlaySound(clientSound(39), im.pos, 0, 0.6f, 1); break;
		default: break;
	}
}

/* ---- entity events (footsteps, landing, bullet cracks): SoF's client plays these for
   one frame from entity_state_t event/event2/event3, which the engine then clears */
/* event numbers: entity_event_t in q_sh_fx.h */

/* per surface: first of four footstep sounds and the landing sound (cl_fxs numbers) */
static const unsigned char g_surfFoot[SOFSURF_NUM][2] =
{
	{ 30, 76 }, { 2, 73 }, { 22, 74 }, { 22, 74 }, { 22, 74 }, { 22, 74 }, { 18, 72 }, { 18, 72 }, { 18, 72 }, { 6, 75 },
	{ 10, 77 }, { 10, 77 }, { 10, 77 }, { 10, 77 }, { 26, 78 }, { 26, 78 }, { 26, 78 }, { 30, 76 }, { 30, 76 }, { 30, 76 },
	{ 30, 76 }, { 30, 76 }, { 30, 76 }, { 30, 76 }, { 30, 76 }, { 14, 71 }, { 14, 71 }, { 10, 77 }, { 2, 73 }, { 2, 73 },
	{ 2, 73 }, { 2, 73 }, { 2, 73 }, { 30, 76 }, { 30, 76 }, { 30, 76 }, { 10, 77 }, { 2, 73 }, { 2, 73 }, { 2, 73 },
	{ 26, 78 }, { 2, 73 }, { 2, 73 },
};

static int surfaceBelow(const float *from, float *hit)
{
	float a[3] = { from[0], from[1], from[2] + 10 }, b[3] = { from[0], from[1], from[2] - 64 };
	q2b_trace_t t;
	q2b_trace(a, 0, 0, b, -1, 1 | 2 /* MASK_SOLID */, &t);
	if (hit) cp3(t.endpos, hit);
	if (t.fraction >= 1 || (t.surfflags & 0x84)) return -1;
	int surf = (int)(((unsigned)t.surfflags) >> 24);
	return surf < SOFSURF_NUM ? surf : 0;
}

static void entityEvent(edict_t *e, int ev)
{
	float pos[3];
	cp3(e->s.origin, pos);
	if ((ev >= EV_FOOTSTEPLEFT && ev <= EV_FOOTSTEPRIGHTRUN) || ev == EV_FOOTSTEPMETALLEFT || ev == EV_FOOTSTEPMETALRIGHT)
	{
		bool right = ev == 4 || ev == 6 || ev == EV_FOOTSTEPMETALRIGHT;
		bool run = ev == 5 || ev == 6 || ev >= EV_FOOTSTEPMETALLEFT;
		float f[3], r[3], u[3], ang[3] = { 0, e->s.angles[1], 0 };
		angleVectors(ang, f, r, u);
		for (int k = 0; k < 3; k++) pos[k] += r[k] * (right ? 5.0f : -5.0f);
		int surf = ev >= EV_FOOTSTEPMETALLEFT ? SOFSURF_METAL : surfaceBelow(pos, pos);
		if (surf < 0) return;
		const char *snd = clientSoundName(g_surfFoot[surf][0] + (rand() & 3));
		if (snd) sfx::PlaySound(snd, pos, NUM_FOR_EDICT(e), run ? 1.0f : 0.7f, 1);
		return;
	}
	if ((ev >= EV_FALLSHORT && ev <= EV_FALLFAR) || (ev >= EV_OBJECT_COLLIDE_SHORT && ev <= EV_OBJECT_COLLIDE_FAR))
	{
		int surf = surfaceBelow(pos, pos);
		if (surf < 0) return;
		const char *snd = clientSoundName(g_surfFoot[surf][1]);
		if (snd) sfx::PlaySound(snd, pos, NUM_FOR_EDICT(e), ev == EV_FALLSHORT || ev == EV_OBJECT_COLLIDE_SHORT ? 0.8f : 1.0f, 1);
		if (ev != EV_FALLSHORT && (snd = clientSoundName(101)) != 0) sfx::PlaySound(snd, pos, NUM_FOR_EDICT(e), 1, 1);
		return;
	}
}

static void fxEntityEvents(void)
{
	if (!sge) return;
	static const char *track = getenv("SOF_TRACKENT");
	if (track && (g_frame % 10) == 0)
	{
		int n = atoi(track);
		if (n == 0 && (g_frame % 50) == 0)
			for (int i = 1; i < sge->num_edicts; i++)
			{
				edict_t *e = EDICT_NUM(i);
				if (!e->inuse || !(e->svflags & 4)) continue;
				char b[200];
				snprintf(b, sizeof(b), "[track] f%d monster %d origin %.1f %.1f %.1f yaw %.0f\n", g_frame, i,
				         e->s.origin[0], e->s.origin[1], e->s.origin[2], e->s.angles[1]);
				q2b_dprint(b);
			}
		if (n > 0 && n < sge->num_edicts)
		{
			edict_t *e = EDICT_NUM(n);
			char b[200];
			snprintf(b, sizeof(b), "[track] f%d ent %d origin %.1f %.1f %.1f inuse %d\n", g_frame, n,
			         e->s.origin[0], e->s.origin[1], e->s.origin[2], e->inuse ? 1 : 0);
			q2b_dprint(b);
		}
	}
	for (int i = 0; i < sge->num_edicts; i++)
	{
		edict_t *e = EDICT_NUM(i);
		if (!e->inuse) continue;
		int evs[3] = { e->s.event, e->s.event2, e->s.event3 };
		for (int k = 0; k < 3; k++)
			if (evs[k] & 0x7f)
			{
				if (getenv("SOF_DEBUG"))
				{
					char b[96];
					snprintf(b, sizeof(b), "[sof fx] entity %d event %d\n", i, evs[k] & 0x7f);
					q2b_dprint(b);
				}
				entityEvent(e, evs[k] & 0x7f);
			}
		e->s.event = e->s.event2 = e->s.event3 = 0;
	}
}

static bool parseTempEnt(MsgReader &r)
{
	int type = r.byte();
	if (getenv("SOF_DEBUG"))
	{
		char b[128];
		snprintf(b, sizeof(b), "[sof fx] temp entity %d (%d bytes)\n", type, (int)(r.n - r.p));
		q2b_dprint(b);
	}
	switch (type)
	{
		case TE_WALLDAMAGE: wallDamage(r, false); break;
		case TE_WALLSEVEREDAMAGE: wallDamage(r, true); break;
		default: break; /* not decoded yet */
	}
	return false; /* one temp entity per message */
}

static void handleMessage(void)
{
	MsgReader r(g_msg);
	while (r.p < r.n && !r.bad)
	{
		int cmd = r.byte();
		if (cmd == SOF_SVC_EFFECT) { if (!parseEffect(r)) break; }
		else if (cmd == SOF_SVC_TEMP_ENTITY) { if (!parseTempEnt(r)) break; }
		else break;
	}
	g_msg.clear();
}

/* ---- anchors */
static void cp3(const float *a, float *b) { b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; }
static void normalize3(float *v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
/* Quake's AngleVectors (the adapter does not link the game's q_shared) */
static void angleVectors(const float *angles, float *forward, float *right, float *up)
{
	float sr, sp, sy, cr, cp, cy, a;
	a = angles[1] * (float)(M_PI * 2 / 360); sy = sinf(a); cy = cosf(a);
	a = angles[0] * (float)(M_PI * 2 / 360); sp = sinf(a); cp = cosf(a);
	a = angles[2] * (float)(M_PI * 2 / 360); sr = sinf(a); cr = cosf(a);
	if (forward) { forward[0] = cp * cy; forward[1] = cp * sy; forward[2] = -sp; }
	if (right) { right[0] = -1 * sr * sp * cy + -1 * cr * -sy; right[1] = -1 * sr * sp * sy + -1 * cr * cy; right[2] = -1 * sr * cp; }
	if (up) { up[0] = cr * sp * cy + -sr * -sy; up[1] = cr * sp * sy + -sr * cy; up[2] = cr * cp; }
}
static int g_gunValid;
static float g_gunOrigin[3], g_gunAngles[3], g_gunScale = 1;

static bool vrScopedRifle(edict_t *ent)
{
	static void *cv_scope;
	if (!cv_scope) cv_scope = q2b_cvar("sof_vrscope", "1", 0);
	if (q2b_cvar_value(cv_scope) == 0 || !ent || !ent->client) return false;
	IGhoulInst *gun = ((player_state_t *)ent->client)->gun;
	return gun && strstr(Ghoul_ObjectDir(gun), "sniperrifle") != 0;
}

static IGhoulInst *playerGun(void)
{
	edict_t *e = edictOf(1);
	if (!e || !e->inuse || !e->client) return 0;
	return ((player_state_t *)e->client)->gun;
}

/* entity space (x forward, y left, z up) -> world, the way the renderer places GHOUL models */
static void entToWorld(const float *org, const float *ang, float scale, const float *p, bool point, float *o)
{
	vec3_t f, r, u, a;
	cp3(ang, a);
	a[2] = -a[2]; /* the renderer turns GHOUL models by the opposite roll (gl1_sof.c) */
	angleVectors(a, f, r, u);
	for (int k = 0; k < 3; k++)
		o[k] = (point ? org[k] : 0) + scale * (p[0] * f[k] - p[1] * r[k] + p[2] * u[k]);
}

struct GunFit;
static void gunFitPoint(const GunFit &g, const float *p, float *o);
static void gunFitDir(const GunFit &g, const float *p, float *o);
static bool boltFrame(IGhoulInst *inst, int bolt, const float *org, const float *ang, float scale, sfx::Frame &out,
                      const GunFit *fit = 0)
{
	if (!inst) return false;
	Matrix4 m;
	inst->GetBoltMatrix(((float)g_frame - 1.0f + g_drawLerp) * 0.1f, m, (GhoulID)bolt, IGhoulInst::MatrixType::Entity, true);
	float rows[4][3];
	for (int r = 0; r < 4; r++)
	{
		Vect3 v;
		m.GetRow(r, v);
		rows[r][0] = v.x(); rows[r][1] = v.y(); rows[r][2] = v.z();
	}
	if (fit)
	{
		float o[3];
		for (int r = 0; r < 3; r++) { gunFitDir(*fit, rows[r], o); cp3(o, rows[r]); }
		gunFitPoint(*fit, rows[3], o);
		gunFitSize(*fit, o);
		cp3(o, rows[3]);
		/* the muzzle bolts are turned for the flat screen (their effects spray across the
		   screen toward the crosshair): in the hand they go along the barrel */
		IGhoulObj *obj = inst->GetGhoulObject();
		if (obj && bolt && (GhoulID)bolt == obj->FindPart("flash"))
		{
			rows[0][0] = 1; rows[0][1] = 0; rows[0][2] = 0; /* forward: the barrel */
			rows[1][0] = 0; rows[1][1] = 0; rows[1][2] = 1; /* up */
			rows[2][0] = 1; rows[2][1] = 0; rows[2][2] = 0; /* "right", which the smoke drifts along, is the barrel too */
		}
	}
	entToWorld(org, ang, scale, rows[3], true, out.org);
	/* bolt axes: x -> forward, y -> up, z -> right (matches the muzzle and shell bolts) */
	entToWorld(org, ang, 1, rows[0], false, out.fwd);
	entToWorld(org, ang, 1, rows[1], false, out.up);
	entToWorld(org, ang, 1, rows[2], false, out.right);
	normalize3(out.fwd); normalize3(out.up); normalize3(out.right);
	return true;
}

static bool fxResolve(const sfx::Anchor &a, sfx::Frame &out)
{
	switch (a.kind)
	{
		case sfx::ANCHOR_POS:
		{
			cp3(a.pos, out.org);
			vec3_t z = { 0, 0, 0 };
			angleVectors(z, out.fwd, out.right, out.up);
			return true;
		}
		case sfx::ANCHOR_ENT:
		{
			edict_t *e = edictOf(a.ent);
			if (!e || !e->inuse) return false;
			cp3(e->s.origin, out.org);
			angleVectors(e->s.angles, out.fwd, out.right, out.up);
			return true;
		}
		case sfx::ANCHOR_BOLT:
		{
			edict_t *e = edictOf(a.ent);
			if (!e || !e->inuse) return false;
			IGhoulInst *inst = (IGhoulInst *)a.inst;
			if (inst && Ghoul_FindInst((short)a.uuid) != inst) return false;
			if (!inst) return false;
			return boltFrame(inst, a.bolt, e->s.origin, e->s.angles, 1, out);
		}
		case sfx::ANCHOR_VIEWWEAPON:
		{
			IGhoulInst *gun = playerGun();
			if (!g_gunValid || !gun || gun != (IGhoulInst *)a.inst) return false;
			out.scale = g_gunScale;
			return boltFrame(gun, a.bolt, g_gunOrigin, g_gunAngles, g_gunScale, out, g_gunFitNow.valid ? &g_gunFitNow : 0);
		}
	}
	return false;
}

static bool vrMuzzle(float *out)
{
	IGhoulInst *gun = playerGun();
	if (!g_gunValid || !gun || !g_gunFitNow.valid || !gun->GetGhoulObject()) return false;
	GhoulID flash = gun->GetGhoulObject()->FindPart("flash");
	if (!flash) return false;
	sfx::Frame fr;
	if (!boltFrame(gun, flash, g_gunOrigin, g_gunAngles, g_gunScale, fr, &g_gunFitNow)) return false;
	cp3(fr.org, out);
	return true;
}

static std::map<std::string, std::string> g_fxTexPaths;
static const char *fxTexturePath(const char *name)
{
	std::string key = name ? name : "";
	std::map<std::string, std::string>::iterator it = g_fxTexPaths.find(key);
	if (it != g_fxTexPaths.end()) return it->second.c_str();
	std::string found;
	static const char *exts[] = { ".m32", ".tga", ".pcx", 0 };
	for (int e = 0; exts[e] && found.empty(); e++)
		if (fileExists(key + exts[e])) found = key + exts[e];
	if (found.empty() && !key.empty())
	{
		char b[160];
		snprintf(b, sizeof(b), "SoF effects: missing texture %s\n", key.c_str());
		q2b_dprint(b);
		found = key + ".m32"; /* drawn as the "no texture" pattern */
	}
	return (g_fxTexPaths[key] = found).c_str();
}

static int fxLoadFile(const char *path, void **buf) { return q2b_loadfile(path, buf); }
static void fxFreeFile(void *buf) { q2b_freefile(buf); }

/* view weapon notes: SoF's client plays these (WeapSoundHelper / WeaponEffectHelper) */
static bool fxResolve(const sfx::Anchor &a, sfx::Frame &out);
static void fxNoteHook(IGhoulInst *inst, const char *token, const char *data)
{
	if (!inst || !token || inst != playerGun()) return;
	sfx::Anchor a;
	a.kind = sfx::ANCHOR_VIEWWEAPON;
	a.inst = inst;
	a.ent = 1;
	if (!strcasecmp(token, "sound"))
		sfx::NoteSound(data, a);
	else if (!strcasecmp(token, "effect") && data && *data)
	{
		/* "weapons/playermz/pistol2 flash": effect name, then the bolt it comes from */
		char name[128], bolt[64];
		name[0] = bolt[0] = 0;
		sscanf(data, "%127s %63s", name, bolt);
		if (bolt[0]) a.bolt = inst->GetGhoulObject()->FindPart(bolt);
		if (getenv("SOF_DEBUG"))
		{
			sfx::Frame fr;
			char b[300];
			bool ok = fxResolve(a, fr);
			snprintf(b, sizeof(b), "[sof fx] view weapon effect %s at bolt %s (%d): %s org %.1f %.1f %.1f fwd %.2f %.2f %.2f gun %.1f %.1f %.1f\n",
			         name, bolt, a.bolt, ok ? "ok" : "unresolved", fr.org[0], fr.org[1], fr.org[2], fr.fwd[0], fr.fwd[1], fr.fwd[2],
			         g_gunOrigin[0], g_gunOrigin[1], g_gunOrigin[2]);
			q2b_dprint(b);
		}
		sfx::Start(name, a, sfx::Params());
	}
}

static sfx::Output g_fxOut;
static std::vector<sofmesh_t> g_fxMeshes;
static std::vector<soffxlight_t> g_fxLights;
static std::vector<soffxsound_t> g_fxSounds;
static soffxframe_t g_fxFrame;

static const soffxframe_t *hookFxFrame(float time, const float *vieworg, const float *viewangles,
                                       int gunvalid, const float *gunorigin, const float *gunangles, float gunscale)
{
	g_gunValid = gunvalid;
	if (gunvalid)
	{
		cp3(gunorigin, g_gunOrigin);
		cp3(gunangles, g_gunAngles);
		g_gunScale = gunscale > 0 ? gunscale : 1;
	}
	vec3_t f, r, u, va;
	cp3(viewangles, va);
	angleVectors(va, f, r, u);
	sfx::Run(time, vieworg, r, u, g_fxOut);

	g_fxMeshes.resize(g_fxOut.batches.size());
	for (size_t i = 0; i < g_fxOut.batches.size(); i++)
	{
		const sfx::Batch &b = g_fxOut.batches[i];
		sofmesh_t &m = g_fxMeshes[i];
		memset(&m, 0, sizeof(m));
		m.numverts = (int)(b.xyz.size() / 3);
		m.xyz = b.xyz.empty() ? 0 : &b.xyz[0];
		m.st = b.st.empty() ? 0 : &b.st[0];
		m.numindices = (int)b.indices.size();
		m.indices = b.indices.empty() ? 0 : &b.indices[0];
		m.skin = b.texture.c_str();
		m.rgba[0] = m.rgba[1] = m.rgba[2] = m.rgba[3] = 1;
		m.colors = b.rgba.empty() ? 0 : &b.rgba[0];
		m.blend = b.blend == 1 ? SOFBLEND_ADD : b.blend == 2 ? SOFBLEND_SUBTRACT : SOFBLEND_ALPHA;
		m.nodepth = b.noDepth;
	}
	g_fxFrame.draw.nummeshes = (int)g_fxMeshes.size();
	g_fxFrame.draw.meshes = g_fxMeshes.empty() ? 0 : &g_fxMeshes[0];

	g_fxLights.resize(g_fxOut.lights.size());
	for (size_t i = 0; i < g_fxOut.lights.size(); i++)
	{
		cp3(g_fxOut.lights[i].org, g_fxLights[i].origin);
		g_fxLights[i].radius = g_fxOut.lights[i].radius;
		cp3(g_fxOut.lights[i].rgb, g_fxLights[i].color);
	}
	g_fxFrame.numlights = (int)g_fxLights.size();
	g_fxFrame.lights = g_fxLights.empty() ? 0 : &g_fxLights[0];

	g_fxSounds.resize(g_fxOut.sounds.size());
	for (size_t i = 0; i < g_fxOut.sounds.size(); i++)
	{
		const sfx::Sound &s = g_fxOut.sounds[i];
		soffxsound_t &o = g_fxSounds[i];
		o.name = s.name.c_str();
		cp3(s.org, o.origin);
		o.entnum = s.ent;
		o.volume = s.volume;
		o.attenuation = s.attenuation;
		o.local = s.local;
		if (getenv("SOF_SOUNDLOG"))
		{
			char b[256];
			snprintf(b, sizeof(b), "[sof fx] sound %s%s\n", s.name.c_str(), s.local ? " (view weapon)" : "");
			q2b_dprint(b);
		}
	}
	g_fxFrame.numsounds = (int)g_fxSounds.size();
	g_fxFrame.sounds = g_fxSounds.empty() ? 0 : &g_fxSounds[0];
	return &g_fxFrame;
}

static void fxRegister(void)
{
	sfx::Host h;
	h.loadFile = fxLoadFile;
	h.freeFile = fxFreeFile;
	h.resolve = fxResolve;
	h.texturePath = fxTexturePath;
	sfx::Init(h);
	Ghoul_SetNoteHook(fxNoteHook);
	if (CL_SoF_RegisterFxHook) CL_SoF_RegisterFxHook(hookFxFrame);
}

static void fxUnregister(void)
{
	if (CL_SoF_RegisterFxHook) CL_SoF_RegisterFxHook(0);
	Ghoul_SetNoteHook(0);
	sfx::Clear();
}

