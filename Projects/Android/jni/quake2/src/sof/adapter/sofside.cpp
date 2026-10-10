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
static void I_AddCommandString(const char *text)
{
	if (getenv("SOF_DEBUG")) { char b[300]; snprintf(b, sizeof(b), "[sof] AddCommandString: %s\n", text); q2b_dprint(b); }
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
/* SoF's own network messages (effects, HUD, ...) are not understood by the Q2 client yet: dropped */
static void I_multicast(vec3_t, multicast_t) {}
static void I_multicastignore(vec3_t, multicast_t, int) {}
static void I_unicast(edict_t *, qboolean) {}
static void I_WriteChar(int) {}
static void I_WriteByte(int) {}
static void I_WriteShort(int) {}
static void I_WriteLong(int) {}
static void I_WriteFloat(float) {}
static void I_WriteString(char *) {}
static void I_WritePosition(vec3_t) {}
static void I_WriteDir(vec3_t) {}
static void I_WriteAngle(float) {}
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

static void *I_Sys_GetPlayerAPI(void *parmscom, void *parmscl, void *parmssv, int isClient)
{
	typedef void *(*api2_t)(void *, void *);
	if (!g_playerLib) return 0;
	api2_t f = (api2_t)dlsym(g_playerLib, isClient ? "GetPlayerClientAPI" : "GetPlayerServerAPI");
	return f ? f(parmscom, isClient ? parmscl : parmssv) : 0;
}
static void I_Sys_UnloadPlayer(int) {}
static float I_flrand(float min, float max) { return min + (max - min) * ((float)rand() / (float)RAND_MAX); }
static int I_irand(int min, int max) { if (max <= min) return min; return min + rand() % (max - min + 1); }
static qboolean I_AppendToSavegame(unsigned long, void *, int) { return true; }
static int I_ReadFromSavegame(unsigned long, void *, int, void **addressptr) { if (addressptr) *addressptr = 0; return 0; }
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
	/* single player: SoF's chat flood protection only ever fires on stray commands */
	q2b_cvar_forceset("flood_msgs", "0");
	sge->Init();
	registerClientHooks();
	return 1;
}

extern "C" void sofb_shutdown(void)
{
	if (CL_SoF_RegisterHooks) CL_SoF_RegisterHooks(0, 0);
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
	g_ghoulModelIndex = q2b_modelindex("#sofghoul");
	refreshAllCvars();
	sge->SpawnEntities((char *)mapname, (char *)entities, (char *)spawnpoint);
}

extern "C" int sofb_clientconnect(int num, char *userinfo) { refreshAllCvars(); return sge->ClientConnect(edictOf(num), userinfo); }
extern "C" void sofb_clientbegin(int num) { sge->ClientBegin(edictOf(num)); }
extern "C" void sofb_clientuserinfochanged(int num, char *userinfo) { sge->ClientUserinfoChanged(edictOf(num), userinfo, true); }
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
	/* Q2 buttons: 1 attack, 2 use, 128 any. Q2 running is already folded into the move speeds. */
	cmd.buttons = (byte)(c->buttons & (BUTTON_ATTACK | BUTTON_USE | BUTTON_ANY));
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
	if (dbg && (n++ % 100) == 0)
	{
		edict_t *e = edictOf(num);
		player_state_t *ps = e && e->client ? (player_state_t *)e->client : 0;
		char b[256];
		snprintf(b, sizeof(b), "[sofdbg] think %d: fwd %d side %d yaw %d btn %d msec %d -> pm_type %d origin %.0f %.0f %.0f\n",
		         num, cmd.forwardmove, cmd.sidemove, cmd.angles[1], cmd.buttons, cmd.msec, ps ? ps->pmove.pm_type : -1,
		         e ? e->s.origin[0] : 0, e ? e->s.origin[1] : 0, e ? e->s.origin[2] : 0);
		q2b_dprint(b);
	}
}

extern "C" void sofb_runframe(void)
{
	refreshAllCvars();
	sge->RunFrame(g_frame++);
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
	}
	c.draw.nummeshes = (int)c.meshes.size();
	c.draw.meshes = c.meshes.empty() ? 0 : &c.meshes[0];
	return &c.draw;
}

static const sofdraw_t *hookEntityDraw(int num, float lerpfrac)
{
	edict_t *e = edictOf(num);
	if (!e || !e->inuse || !e->ghoulInst) return 0;
	return buildDraw(num, (IGhoulInst *)e->ghoulInst, lerpfrac);
}
static const sofdraw_t *hookViewWeaponDraw(int client, float lerpfrac)
{
	edict_t *e = edictOf(client);
	if (!e || !e->client) return 0;
	return buildDraw(-client, ((player_state_t *)e->client)->gun, lerpfrac);
}

extern "C" void Pmove_SetSoFHeights(int sof) __attribute__((weak));
static void registerClientHooks(void)
{
	if (Pmove_SetSoFHeights) Pmove_SetSoFHeights(1);
	if (CL_SoF_RegisterHooks) CL_SoF_RegisterHooks(hookEntityDraw, hookViewWeaponDraw);
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
