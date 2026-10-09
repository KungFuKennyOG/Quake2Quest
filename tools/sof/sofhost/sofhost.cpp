/*
 * sofhost.cpp - headless test host for the ported SoF game module.
 *
 * Loads gamex64.so, gives it a complete game_import_t (collision from Yamagi's
 * collision.c through cm_shim, GHOUL from the clean-room runtime), loads a map,
 * spawns its entities and runs server frames. No client, no networking, no rendering.
 *
 * This is a development harness: the same import functions are later moved into the
 * engine's server. Built against the SoF SDK headers (not part of this repository).
 *
 * usage: sofhost <gamex64.so> <player.so> <map> <frames> <datadir> [datadir...]
 */
#include "q_shared.h"
#include "../qcommon/mathlib.h"
#include "../qcommon/pmove.h"
#include "../qcommon/configstring.h"
#include "game.h"
#include "../ghoul/ighoul.h"
#include "ghoul_engine.h"
#include "cm_shim.h"
#include "ghb_model.h"

#include <dlfcn.h>
#include <dirent.h>
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

/* ------------------------------------------------------------------ filesystem */

static std::vector<std::string> g_roots;
static std::map<std::string, std::string> g_fileIndex; /* lower-case relative path -> real path */

static void indexDir(const std::string &root, const std::string &rel)
{
	DIR *d = opendir((root + "/" + rel).c_str());
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)))
	{
		if (e->d_name[0] == '.') continue;
		std::string r = rel.empty() ? e->d_name : rel + "/" + e->d_name;
		std::string full = root + "/" + r;
		if (e->d_type == DT_DIR) { indexDir(root, r); continue; }
		std::string key(r);
		for (size_t i = 0; i < key.size(); i++) key[i] = (char)tolower((unsigned char)key[i]);
		if (!g_fileIndex.count(key)) g_fileIndex[key] = full;
	}
	closedir(d);
}

static std::string normPath(const char *p)
{
	std::string k(p ? p : "");
	for (size_t i = 0; i < k.size(); i++) { k[i] = (char)tolower((unsigned char)k[i]); if (k[i] == '\\') k[i] = '/'; }
	while (k.find("//") != std::string::npos) k.erase(k.find("//"), 1);
	if (!k.empty() && k[0] == '/') k.erase(0, 1);
	return k;
}

static int FS_Load(const char *name, void **buf)
{
	std::map<std::string, std::string>::iterator it = g_fileIndex.find(normPath(name));
	if (it == g_fileIndex.end()) { if (buf) *buf = 0; return -1; }
	FILE *f = fopen(it->second.c_str(), "rb");
	if (!f) { if (buf) *buf = 0; return -1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (!buf) { fclose(f); return (int)n; }
	char *b = (char *)malloc((size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
	b[n] = 0;
	fclose(f);
	*buf = b;
	return (int)n;
}
static void FS_Free(void *b) { free(b); }

static void FS_List(const char *dir, const char *ext, std::vector<std::string> &out)
{
	std::string d = normPath(dir) + "/";
	std::string e = normPath(ext);
	for (std::map<std::string, std::string>::iterator it = g_fileIndex.begin(); it != g_fileIndex.end(); ++it)
	{
		const std::string &k = it->first;
		if (k.compare(0, d.size(), d) != 0) continue;
		std::string rest = k.substr(d.size());
		if (rest.find('/') != std::string::npos) continue;
		if (rest.size() > e.size() && rest.compare(rest.size() - e.size(), e.size(), e) == 0) out.push_back(rest);
	}
}

/* ------------------------------------------------------------------ console / cvars / commands */

static int g_dev = 0;
static void hostPrintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static std::map<std::string, cvar_t *> g_cvars;
static cvar_t *Cvar_Find(const char *n)
{
	std::map<std::string, cvar_t *>::iterator it = g_cvars.find(normPath(n));
	return it == g_cvars.end() ? 0 : it->second;
}
static void cvarSetString(cvar_t *c, const char *v)
{
	free(c->string);
	c->string = strdup(v ? v : "");
	c->value = (float)atof(c->string);
	c->modified = true;
}
static cvar_t *I_cvar(const char *name, const char *value, int flags, cvarcommand_t command)
{
	cvar_t *c = Cvar_Find(name);
	if (c) { c->flags |= flags; return c; }
	c = (cvar_t *)calloc(1, sizeof(cvar_t));
	c->name = strdup(name);
	c->string = strdup(value ? value : "");
	c->value = (float)atof(c->string);
	c->flags = flags;
	c->command = command;
	g_cvars[normPath(name)] = c;
	return c;
}
static cvar_t *I_cvar_set(const char *name, const char *value)
{
	cvar_t *c = Cvar_Find(name);
	if (!c) return I_cvar(name, value, 0, 0);
	cvarSetString(c, value);
	return c;
}
static void I_cvar_setvalue(const char *name, float v)
{
	char b[64];
	if (v == (int)v) snprintf(b, sizeof(b), "%d", (int)v); else snprintf(b, sizeof(b), "%f", v);
	I_cvar_set(name, b);
}
static cvar_t *I_cvar_forceset(const char *name, const char *value) { return I_cvar_set(name, value); }
static char *I_cvar_info(int flag)
{
	static std::string s;
	s.clear();
	for (std::map<std::string, cvar_t *>::iterator it = g_cvars.begin(); it != g_cvars.end(); ++it)
		if (it->second->flags & flag) s += std::string("\\") + it->second->name + "\\" + it->second->string;
	return (char *)s.c_str();
}
static float I_cvar_variablevalue(const char *name) { cvar_t *c = Cvar_Find(name); return c ? c->value : 0.0f; }

static std::vector<std::string> g_argv;
static std::string g_args;
static void setCmd(const char *line)
{
	g_argv.clear();
	g_args.clear();
	const char *p = line;
	while (*p)
	{
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		if (g_argv.size() == 1) g_args = p;
		std::string t;
		if (*p == '"') { p++; while (*p && *p != '"') t += *p++; if (*p) p++; }
		else while (*p && *p != ' ' && *p != '\t') t += *p++;
		g_argv.push_back(t);
	}
}
static int I_argc(void) { return (int)g_argv.size(); }
static char *I_argv(int n) { static char empty[1] = { 0 }; return n < (int)g_argv.size() ? (char *)g_argv[(size_t)n].c_str() : empty; }
static char *I_args(void) { return (char *)g_args.c_str(); }

/* ------------------------------------------------------------------ printing */

static void I_bprintf(int, char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
static void I_dprintf(char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
static void I_cprintf(edict_t *, int, char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
static void I_clprintf(edict_t *, edict_t *, int, char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
static void I_welcomeprint(edict_t *) {}
static void I_centerprintf(edict_t *, char *fmt, ...) { va_list a; va_start(a, fmt); printf("[center] "); vprintf(fmt, a); va_end(a); printf("\n"); }
static void I_cinprintf(edict_t *, int, int, int, char *text) { printf("[cin] %s\n", text); }
static void I_bcaption(int, unsigned short id) { if (g_dev) printf("[caption %u]\n", id); }
static void I_captionprintf(edict_t *, unsigned short id) { if (g_dev) printf("[caption %u]\n", id); }
static void I_Con_ClearNotify(void) {}
static void I_error(char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	printf("GAME ERROR: ");
	vprintf(fmt, a);
	va_end(a);
	printf("\n");
	exit(3);
}
static void I_Sys_ConsoleOutput(char *s) { fputs(s, stdout); }

/* ------------------------------------------------------------------ configstrings / indexes */

static std::vector<std::string> g_cs(MAX_CONFIGSTRINGS);
static void I_configstring(int num, char *s)
{
	if (num < 0 || num >= MAX_CONFIGSTRINGS) { printf("configstring: bad index %d\n", num); return; }
	g_cs[(size_t)num] = s ? s : "";
}
static int findIndex(const char *name, int start, int max)
{
	if (!name || !name[0]) return 0;
	for (int i = 1; i < max; i++)
	{
		if (g_cs[(size_t)(start + i)].empty()) { g_cs[(size_t)(start + i)] = name; return i; }
		if (!strcasecmp(g_cs[(size_t)(start + i)].c_str(), name)) return i;
	}
	printf("index overflow for %s\n", name);
	return 0;
}
static int I_modelindex(const char *n) { return findIndex(n, CS_MODELS, MAX_MODELS); }
static int I_soundindex(const char *n) { return findIndex(n, CS_SOUNDS, MAX_SOUNDS); }
static int I_effectindex(const char *n) { return findIndex(n, CS_EFFECTS, MAX_EFPACKS); }
static int I_imageindex(const char *n) { return findIndex(n, CS_IMAGES, MAX_IMAGES); }
static void I_unload_sound(const char *) {}
static qboolean I_FilterPacket(char *) { return false; }
static void I_CreateGhoulConfigStrings(void) {}

/* ------------------------------------------------------------------ memory */

struct TagBlock { int tag; size_t size; };
static std::set<TagBlock *> g_blocks;
static void *I_TagMalloc(int size, int tag)
{
	TagBlock *b = (TagBlock *)calloc(1, sizeof(TagBlock) + (size_t)size + 16);
	b->tag = tag;
	b->size = (size_t)size;
	g_blocks.insert(b);
	return (char *)b + sizeof(TagBlock);
}
static void I_TagFree(void *p)
{
	if (!p) return;
	TagBlock *b = (TagBlock *)((char *)p - sizeof(TagBlock));
	if (g_blocks.erase(b)) free(b);
}
static void I_FreeTags(int tag)
{
	std::vector<TagBlock *> v(g_blocks.begin(), g_blocks.end());
	for (size_t i = 0; i < v.size(); i++)
		if (v[i]->tag == tag) { g_blocks.erase(v[i]); free(v[i]); }
}

/* ------------------------------------------------------------------ world: edicts, linking */

static game_export_t *ge;
static int g_numInline;
static std::vector<int> g_modelHeadnode; /* by modelindex, -1 if not inline */

#define EDICT_NUM(n) ((edict_t *)((char *)ge->edicts + ge->edict_size * (n)))
#define NUM_FOR_EDICT(e) ((int)(((char *)(e) - (char *)ge->edicts) / ge->edict_size))

static int modelHeadnode(int modelindex, float *mins = 0, float *maxs = 0)
{
	if (modelindex <= 0 || modelindex >= MAX_MODELS) return -1;
	const std::string &n = g_cs[(size_t)(CS_MODELS + modelindex)];
	if (n.empty() || n[0] != '*') return -1;
	float mn[3], mx[3], org[3];
	int h = cms_inline(n.c_str(), mn, mx, org);
	if (mins) { mins[0] = mn[0]; mins[1] = mn[1]; mins[2] = mn[2]; }
	if (maxs) { maxs[0] = mx[0]; maxs[1] = mx[1]; maxs[2] = mx[2]; }
	return h;
}

static void I_unlinkentity(edict_t *ent)
{
	ent->area.prev = ent->area.next = 0;
}

static void I_linkentity(edict_t *ent)
{
	if (ent->area.prev) I_unlinkentity(ent);
	if (ent == EDICT_NUM(0)) return; /* world is never linked */
	if (!ent->inuse) return;

	VectorSubtract(ent->maxs, ent->mins, ent->size);

	/* encode the size into the entity_state for client prediction */
	if (ent->solid == SOLID_BBOX && !(ent->svflags & SVF_DEADMONSTER))
	{
		int i = (int)(ent->maxs[0] / 8), j = (int)(-ent->mins[2] / 8), k = (int)((ent->maxs[2] + 32) / 8);
		if (i < 1) i = 1; if (i > 31) i = 31;
		if (j < 1) j = 1; if (j > 31) j = 31;
		if (k < 1) k = 1; if (k > 63) k = 63;
		ent->s.solid = (k << 10) | (j << 5) | i;
	}
	else if (ent->solid == SOLID_BSP)
		ent->s.solid = 31;
	else
		ent->s.solid = 0;

	if (ent->solid == SOLID_BSP && (ent->s.angles[0] || ent->s.angles[1] || ent->s.angles[2]))
	{
		float max = 0;
		for (int i = 0; i < 3; i++)
		{
			float v = fabsf(ent->mins[i]); if (v > max) max = v;
			v = fabsf(ent->maxs[i]); if (v > max) max = v;
		}
		for (int i = 0; i < 3; i++) { ent->absmin[i] = ent->s.origin[i] - max; ent->absmax[i] = ent->s.origin[i] + max; }
	}
	else
	{
		VectorAdd(ent->s.origin, ent->mins, ent->absmin);
		VectorAdd(ent->s.origin, ent->maxs, ent->absmax);
	}
	for (int i = 0; i < 3; i++) { ent->absmin[i] -= 1; ent->absmax[i] += 1; }

	/* clusters / areas */
	ent->num_clusters = 0;
	ent->areanum = ent->areanum2 = 0;
	int leafs[128], clusters[128], topnode;
	int n = cms_boxleafnums(ent->absmin, ent->absmax, leafs, 128, &topnode);
	for (int i = 0; i < n; i++)
	{
		clusters[i] = cms_leafcluster(leafs[i]);
		int area = cms_leafarea(leafs[i]);
		if (area)
		{
			if (ent->areanum && ent->areanum != area)
			{
				if (ent->areanum2 && ent->areanum2 != area && g_dev) printf("object touching 3 areas\n");
				ent->areanum2 = area;
			}
			else ent->areanum = area;
		}
	}
	if (n >= 128)
	{
		ent->num_clusters = -1;
		ent->headnode = topnode;
	}
	else
	{
		for (int i = 0; i < n; i++)
		{
			if (clusters[i] == -1) continue;
			int j;
			for (j = 0; j < i; j++) if (clusters[j] == clusters[i]) break;
			if (j == i)
			{
				if (ent->num_clusters == MAX_ENT_CLUSTERS) { ent->num_clusters = -1; ent->headnode = topnode; break; }
				ent->clusternums[ent->num_clusters++] = (short)clusters[i];
			}
		}
	}

	ent->linkcount++;
	if (ent->solid == SOLID_NOT) return;
	ent->area.prev = ent->area.next = &ent->area; /* mark linked; lookups scan all edicts */
}

static int I_BoxEdicts(vec3_t mins, vec3_t maxs, edict_t **list, int maxcount, int areatype)
{
	int n = 0;
	for (int i = 1; i < ge->num_edicts && n < maxcount; i++)
	{
		edict_t *e = EDICT_NUM(i);
		if (!e->inuse || !e->area.prev) continue;
		if (areatype == AREA_SOLID ? (e->solid == SOLID_TRIGGER || e->solid == SOLID_NOT) : e->solid != SOLID_TRIGGER) continue;
		if (e->absmin[0] > maxs[0] || e->absmin[1] > maxs[1] || e->absmin[2] > maxs[2] ||
		    e->absmax[0] < mins[0] || e->absmax[1] < mins[1] || e->absmax[2] < mins[2]) continue;
		list[n++] = e;
	}
	return n;
}

/* surfaces: SoF game code reads trace.surface->flags (and ->textureinfo->name) */
struct HostTexInfo { char name[64]; int surfaceType; };
static std::map<const void *, mtexinfo_t *> g_surfs;
static mtexinfo_t *surfFor(const cms_trace_t &t)
{
	static mtexinfo_t none;
	static HostTexInfo noneInfo;
	if (!t.surface)
	{
		none.textureinfo = (struct ctextureinfo_s *)&noneInfo;
		return &none;
	}
	std::map<const void *, mtexinfo_t *>::iterator it = g_surfs.find(t.surface);
	if (it != g_surfs.end()) return it->second;
	mtexinfo_t *m = (mtexinfo_t *)calloc(1, sizeof(mtexinfo_t));
	HostTexInfo *ti = (HostTexInfo *)calloc(1, sizeof(HostTexInfo));
	strncpy(ti->name, t.surfname, sizeof(ti->name) - 1);
	ti->surfaceType = (int)((unsigned)t.surfflags >> 24);
	m->flags = t.surfflags;
	m->value = t.surfvalue;
	m->textureinfo = m->origtextureinfo = (struct ctextureinfo_s *)ti;
	g_surfs[t.surface] = m;
	return m;
}

static void toTrace(const cms_trace_t &c, trace_t *t, edict_t *ent)
{
	memset(t, 0, sizeof(*t));
	t->allsolid = c.allsolid != 0;
	t->startsolid = c.startsolid != 0;
	t->fraction = c.fraction;
	for (int k = 0; k < 3; k++) t->endpos[k] = c.endpos[k];
	for (int k = 0; k < 3; k++) t->plane.normal[k] = c.normal[k];
	t->plane.dist = c.dist;
	t->plane.type = (byte)c.planetype;
	t->plane.signbits = (byte)c.signbits;
	t->surface = surfFor(c);
	t->contents = c.contents;
	t->ent = ent;
	t->leaf_num = cms_pointleafnum(c.endpos);
}

static int entHeadnode(edict_t *e)
{
	if (e->solid == SOLID_BSP)
	{
		int h = modelHeadnode(e->s.modelindex);
		if (h >= 0) return h;
	}
	return cms_headnodeforbox(e->mins, e->maxs);
}

static std::set<edict_t *> g_ignore; /* used by polyTrace */

static void doTrace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *passent, int contentmask, trace_t *tr)
{
	static vec3_t zero = { 0, 0, 0 };
	if (!mins) mins = zero;
	if (!maxs) maxs = zero;
	cms_trace_t w;
	cms_boxtrace(start, end, mins, maxs, 0, contentmask, &w);
	toTrace(w, tr, EDICT_NUM(0));
	if (tr->fraction == 0) return;

	/* move bounds */
	vec3_t bmin, bmax;
	for (int i = 0; i < 3; i++)
	{
		if (end[i] > start[i]) { bmin[i] = start[i] + mins[i] - 1; bmax[i] = end[i] + maxs[i] + 1; }
		else { bmin[i] = end[i] + mins[i] - 1; bmax[i] = start[i] + maxs[i] + 1; }
	}
	edict_t *list[MAX_EDICTS];
	int n = I_BoxEdicts(bmin, bmax, list, MAX_EDICTS, AREA_SOLID);
	for (int i = 0; i < n; i++)
	{
		edict_t *t = list[i];
		if (t->solid == SOLID_NOT || t == passent || g_ignore.count(t)) continue;
		if (passent)
		{
			if (t->owner == passent) continue;
			if (passent->owner == t) continue;
		}
		if (!(contentmask & CONTENTS_DEADMONSTER) && (t->svflags & SVF_DEADMONSTER)) continue;
		int head = entHeadnode(t);
		cms_trace_t c;
		if (t->solid == SOLID_BSP)
			cms_transformedtrace(start, end, mins, maxs, head, contentmask, t->s.origin, t->s.angles, &c);
		else
		{
			static vec3_t za = { 0, 0, 0 };
			cms_transformedtrace(start, end, mins, maxs, head, contentmask, t->s.origin, za, &c);
		}
		if (c.allsolid || c.startsolid || c.fraction < tr->fraction)
		{
			bool startsolid = tr->startsolid;
			trace_t r;
			toTrace(c, &r, t);
			if (tr->startsolid) { r.startsolid = true; }
			*tr = r;
			if (startsolid) tr->startsolid = true;
		}
		else if (c.startsolid)
			tr->startsolid = true;
	}
}

static qboolean I_trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *passent, int contentmask, trace_t *tr)
{
	doTrace(start, mins, maxs, end, passent, contentmask, tr);
	return tr->fraction < 1.0f;
}

static qboolean I_polyTrace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *passent, int contentmask, trace_t *tr,
                            int (*PolyHitFunc)(trace_t *tr, vec3_t start, vec3_t end, int clipMask))
{
	g_ignore.clear();
	for (int iter = 0; iter < 16; iter++)
	{
		doTrace(start, mins, maxs, end, passent, contentmask, tr);
		if (tr->fraction >= 1.0f || !tr->ent || !PolyHitFunc) break;
		if (PolyHitFunc(tr, start, end, contentmask)) break;
		if (tr->ent == EDICT_NUM(0)) { tr->fraction = 1.0f; VectorCopy(end, tr->endpos); tr->ent = 0; break; }
		g_ignore.insert(tr->ent); /* missed the model inside its box: trace on through it */
	}
	g_ignore.clear();
	return tr->fraction < 1.0f;
}

static int I_pointcontents(vec3_t p)
{
	int c = cms_pointcontents(p, 0);
	edict_t *list[MAX_EDICTS];
	int n = I_BoxEdicts(p, p, list, MAX_EDICTS, AREA_SOLID);
	for (int i = 0; i < n; i++)
	{
		edict_t *t = list[i];
		int head = entHeadnode(t);
		static vec3_t za = { 0, 0, 0 };
		c |= cms_transformedpointcontents(p, head, t->s.origin, t->solid == SOLID_BSP ? t->s.angles : za);
	}
	return c;
}

static float I_RegionDistance(vec3_t) { return 0; }

static int clusterOf(vec3_t p) { return cms_leafcluster(cms_pointleafnum(p)); }
static qboolean I_inPVS(vec3_t p1, vec3_t p2)
{
	int l1 = cms_pointleafnum(p1), l2 = cms_pointleafnum(p2);
	int c1 = cms_leafcluster(l1), c2 = cms_leafcluster(l2);
	if (c1 < 0 || c2 < 0) return false;
	const unsigned char *m = cms_clusterpvs(c1);
	if (!(m[c2 >> 3] & (1 << (c2 & 7)))) return false;
	return cms_areasconnected(cms_leafarea(l1), cms_leafarea(l2)) != 0;
}
static qboolean I_inPHS(vec3_t p1, vec3_t p2)
{
	int l1 = cms_pointleafnum(p1), l2 = cms_pointleafnum(p2);
	int c1 = cms_leafcluster(l1), c2 = cms_leafcluster(l2);
	if (c1 < 0 || c2 < 0) return false;
	const unsigned char *m = cms_clusterphs(c1);
	if (!(m[c2 >> 3] & (1 << (c2 & 7)))) return false;
	return cms_areasconnected(cms_leafarea(l1), cms_leafarea(l2)) != 0;
}
static void I_SetAreaPortalState(int portal, qboolean open) { cms_setportal(portal, open ? 1 : 0); }
static qboolean I_AreasConnected(int a, int b) { return cms_areasconnected(a, b) != 0; }

static void I_setmodel(edict_t *ent, char *name)
{
	if (!name) { I_error((char *)"setmodel: NULL"); return; }
	ent->s.modelindex = I_modelindex(name);
	if (name[0] == '*')
	{
		float mn[3], mx[3];
		modelHeadnode(ent->s.modelindex, mn, mx);
		VectorCopy(mn, ent->mins);
		VectorCopy(mx, ent->maxs);
		I_linkentity(ent);
	}
}
static void I_setrendermodel(edict_t *ent, char *name)
{
	if (!name) return;
	ent->s.renderindex = I_modelindex(name);
}

/* ------------------------------------------------------------------ sound / network output (discarded) */

static int g_sounds, g_multicasts;
static void I_sound(edict_t *, int, int, float, float, float, int) { g_sounds++; }
static void I_positioned_sound(vec3_t, edict_t *, int, int, float, float, float, int) { g_sounds++; }
static void I_DebugGraph(float, int) {}
static bool I_DamageTexture(struct mtexinfo_s *, int) { return false; }
static int I_SurfaceTypeList(byte *mat_list, int max_size) { if (mat_list && max_size > 0) memset(mat_list, 0, (size_t)max_size); return 0; }
static void I_Update(float, bool) {}
static void I_multicast(vec3_t, multicast_t) { g_multicasts++; }
static void I_multicastignore(vec3_t, multicast_t, int) { g_multicasts++; }
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

/* ------------------------------------------------------------------ string packages (.sp) */

static std::map<unsigned short, std::string> g_strings;
static void I_SP_Print(edict_t *, unsigned short ID, ...)
{
	std::map<unsigned short, std::string>::iterator it = g_strings.find(ID);
	if (it == g_strings.end()) { printf("[sp %04x]\n", ID); return; }
	char buf[2048];
	va_list ap;
	va_start(ap, ID);
	vsnprintf(buf, sizeof(buf), it->second.c_str(), ap);
	va_end(ap);
	printf("[sp] %s\n", buf);
}
static void I_SP_Print_Obit(edict_t *, unsigned short ID, ...) { (void)ID; }
static int I_SP_SPrint(char *buffer, int size, unsigned short ID, ...)
{
	std::map<unsigned short, std::string>::iterator it = g_strings.find(ID);
	if (it == g_strings.end()) { if (size > 0) buffer[0] = 0; return 0; }
	va_list ap;
	va_start(ap, ID);
	vsnprintf(buffer, (size_t)size, it->second.c_str(), ap);
	va_end(ap);
	return (int)strlen(buffer);
}

/* strip/<package>.sp: "ID n" then "INDEX i { REFERENCE x  TEXT_ENGLISH "..." }"; string id = (n << 8) | i */
static std::set<std::string> g_packages;
static void I_SP_Register(const char *pkg)
{
	std::string key = normPath(pkg);
	if (g_packages.count(key)) return;
	g_packages.insert(key);
	char *buf = 0;
	if (FS_Load((std::string("strip/") + pkg + ".sp").c_str(), (void **)&buf) < 0) { printf("SP_Register: no strip/%s.sp\n", pkg); return; }
	int id = 0, index = -1, count = 0;
	const char *p = buf;
	while (*p)
	{
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
		if (!*p) break;
		const char *ls = p;
		while (*p && *p != '\n') p++;
		std::string line(ls, (size_t)(p - ls));
		while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) line.erase(line.size() - 1);
		if (!line.compare(0, 3, "ID ")) id = atoi(line.c_str() + 3);
		else if (!line.compare(0, 6, "INDEX ")) index = atoi(line.c_str() + 6);
		else if (!line.compare(0, 13, "TEXT_ENGLISH ") && index >= 0)
		{
			size_t q1 = line.find('"'), q2 = line.find_last_of('"');
			std::string t = q1 != std::string::npos && q2 > q1 ? line.substr(q1 + 1, q2 - q1 - 1) : std::string();
			std::string o;
			for (size_t i = 0; i < t.size(); i++)
			{
				if (t[i] == '\\' && i + 1 < t.size()) { char c = t[++i]; o += c == 'n' ? '\n' : (c == 't' ? '\t' : c); }
				else o += t[i];
			}
			g_strings[(unsigned short)((id << 8) | index)] = o;
			count++;
		}
	}
	FS_Free(buf);
	if (g_dev) printf("SP_Register %s: %d strings (package id %d)\n", pkg, count, id);
}
static const char *I_SP_GetStringText(unsigned short ID)
{
	std::map<unsigned short, std::string>::iterator it = g_strings.find(ID);
	return it == g_strings.end() ? "" : it->second.c_str();
}

/* ------------------------------------------------------------------ player module / model info */

class HostModelInfo : public IPlayerModelInfoC
{
};
static IPlayerModelInfoC *I_NewPlayerModelInfo(char *) { return new HostModelInfo; }

static void *g_playerLib;
static void *I_Sys_GetPlayerAPI(void *parmscom, void *parmscl, void *parmssv, int isClient)
{
	typedef void *(*api2_t)(void *, void *);
	if (!g_playerLib) return 0;
	if (isClient)
	{
		api2_t f = (api2_t)dlsym(g_playerLib, "GetPlayerClientAPI");
		return f ? f(parmscom, parmscl) : 0;
	}
	api2_t f = (api2_t)dlsym(g_playerLib, "GetPlayerServerAPI");
	return f ? f(parmscom, parmssv) : 0;
}
static void I_Sys_UnloadPlayer(int) {}

static float I_flrand(float min, float max) { return min + (max - min) * ((float)rand() / (float)RAND_MAX); }
static int I_irand(int min, int max) { if (max <= min) return min; return min + rand() % (max - min + 1); }

static void I_Pmove(pmove_t *pm)
{
	/* no clients in the headless host yet */
	pm->numtouch = 0;
	pm->groundentity = 0;
}

static qboolean I_AppendToSavegame(unsigned long, void *, int) { return true; }
static int I_ReadFromSavegame(unsigned long, void *, int, void **addressptr) { if (addressptr) *addressptr = 0; return 0; }

static int I_FS_LoadFile(char *name, void **buf, bool) { return FS_Load(name, buf); }
static void I_FS_FreeFile(void *buf) { FS_Free(buf); }
static char *I_FS_Userdir(void) { static char d[] = "./sofhost_user"; return d; }
static void I_FS_CreatePath(char *) {}
static int I_FS_FileExists(char *path) { return FS_Load(path, 0) >= 0; }
static std::vector<std::string> g_cmdQueue;
static void I_AddCommandString(const char *text) { g_cmdQueue.push_back(text); }

static void *I_GetGhoul(void) { return Ghoul_Get(false, false); }
static int g_isClientVal = 0;
static int *g_isClientPtr = &g_isClientVal;

/* ------------------------------------------------------------------ main */

static game_import_t gi;

static void buildImports(void)
{
	memset(&gi, 0, sizeof(gi));
	gi.modelindex = I_modelindex;
	gi.soundindex = I_soundindex;
	gi.effectindex = I_effectindex;
	gi.imageindex = I_imageindex;
	gi.unload_sound = I_unload_sound;
	gi.FilterPacket = I_FilterPacket;
	gi.CreateGhoulConfigStrings = I_CreateGhoulConfigStrings;
	gi.setmodel = I_setmodel;
	gi.setrendermodel = I_setrendermodel;
	gi.argc = I_argc;
	gi.argv = I_argv;
	gi.args = I_args;
	gi.bprintf = I_bprintf;
	gi.dprintf = I_dprintf;
	gi.cprintf = I_cprintf;
	gi.clprintf = I_clprintf;
	gi.welcomeprint = I_welcomeprint;
	gi.centerprintf = I_centerprintf;
	gi.cinprintf = I_cinprintf;
	gi.bcaption = I_bcaption;
	gi.captionprintf = I_captionprintf;
	gi.Con_ClearNotify = I_Con_ClearNotify;
	gi.sound = I_sound;
	gi.positioned_sound = I_positioned_sound;
	gi.DebugGraph = I_DebugGraph;
	gi.DamageTexture = I_DamageTexture;
	gi.SurfaceTypeList = I_SurfaceTypeList;
	gi.Update = I_Update;
	gi.multicast = I_multicast;
	gi.multicastignore = I_multicastignore;
	gi.unicast = I_unicast;
	gi.WriteChar = I_WriteChar;
	gi.WriteByte = I_WriteByte;
	gi.WriteShort = I_WriteShort;
	gi.WriteLong = I_WriteLong;
	gi.WriteFloat = I_WriteFloat;
	gi.WriteString = I_WriteString;
	gi.WritePosition = I_WritePosition;
	gi.WriteDir = I_WriteDir;
	gi.WriteAngle = I_WriteAngle;
	gi.WriteByteSizebuf = I_WriteByteSizebuf;
	gi.WriteShortSizebuf = I_WriteShortSizebuf;
	gi.WriteLongSizebuf = I_WriteLongSizebuf;
	gi.ReliableWriteByteToClient = I_ReliableWriteByteToClient;
	gi.ReliableWriteDataToClient = I_ReliableWriteDataToClient;
	gi.GetNearestByteNormal = I_GetNearestByteNormal;
	gi.sendPlayernameColors = I_sendPlayernameColors;
	gi.SP_Register = I_SP_Register;
	gi.SP_Print = I_SP_Print;
	gi.SP_Print_Obit = I_SP_Print_Obit;
	gi.SP_SPrint = I_SP_SPrint;
	gi.SP_GetStringText = I_SP_GetStringText;
	gi.trace = I_trace;
	gi.polyTrace = I_polyTrace;
	gi.pointcontents = I_pointcontents;
	gi.RegionDistance = I_RegionDistance;
	gi.inPVS = I_inPVS;
	gi.inPHS = I_inPHS;
	gi.SetAreaPortalState = I_SetAreaPortalState;
	gi.AreasConnected = I_AreasConnected;
	gi.GetGhoul = I_GetGhoul;
	gi.NewPlayerModelInfo = I_NewPlayerModelInfo;
	gi.FindGSQFile = GSQ_FindFile;
	gi.ReadGsqEntry = GSQ_ReadEntry;
	gi.PrecacheGSQFile = GSQ_Precache;
	gi.RegisterGSQSequences = GSQ_RegisterSequences;
	gi.TurnOffPartsFromGSQFile = GSQ_TurnOffParts;
	gi.isClient = &g_isClientPtr;
	gi.configstring = I_configstring;
	gi.SZ_Init = I_SZ_Init;
	gi.SZ_Clear = I_SZ_Clear;
	gi.SZ_Write = SZ_Write;
	gi.error = I_error;
	gi.Sys_ConsoleOutput = I_Sys_ConsoleOutput;
	gi.Sys_GetPlayerAPI = I_Sys_GetPlayerAPI;
	gi.Sys_UnloadPlayer = I_Sys_UnloadPlayer;
	gi.flrand = I_flrand;
	gi.irand = I_irand;
	gi.linkentity = I_linkentity;
	gi.unlinkentity = I_unlinkentity;
	gi.BoxEdicts = I_BoxEdicts;
	gi.Pmove = I_Pmove;
	gi.TagMalloc = I_TagMalloc;
	gi.TagFree = I_TagFree;
	gi.FreeTags = I_FreeTags;
	gi.AppendToSavegame = I_AppendToSavegame;
	gi.ReadFromSavegame = I_ReadFromSavegame;
	gi.cvar = I_cvar;
	gi.cvar_set = I_cvar_set;
	gi.cvar_setvalue = I_cvar_setvalue;
	gi.cvar_forceset = I_cvar_forceset;
	gi.cvar_info = I_cvar_info;
	gi.cvar_variablevalue = I_cvar_variablevalue;
	gi.FS_LoadFile = I_FS_LoadFile;
	gi.FS_FreeFile = I_FS_FreeFile;
	gi.FS_Userdir = I_FS_Userdir;
	gi.FS_CreatePath = I_FS_CreatePath;
	gi.FS_FileExists = I_FS_FileExists;
	gi.AddCommandString = I_AddCommandString;
}

static void ghoulPrintf(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vprintf(fmt, a);
	va_end(a);
}

int main(int argc, char **argv)
{
	if (argc < 6)
	{
		printf("usage: sofhost <gamex64.so> <player.so> <map> <frames> <datadir> [datadir...]\n");
		return 1;
	}
	setvbuf(stdout, 0, _IONBF, 0);
	for (int i = 5; i < argc; i++) { g_roots.push_back(argv[i]); indexDir(argv[i], ""); }
	printf("indexed %zu files\n", g_fileIndex.size());
	g_dev = getenv("SOFHOST_DEV") ? 1 : 0;

	GhoulEngineImports gimp = { FS_Load, FS_Free, FS_List, ghoulPrintf };
	Ghoul_Init(gimp);
	cms_setfs(FS_Load, FS_Free);

	/* cvars the engine normally creates */
	I_cvar("maxclients", "1", CVAR_SERVERINFO | CVAR_LATCH, 0);
	I_cvar("deathmatch", "0", CVAR_LATCH, 0);
	I_cvar("coop", "0", CVAR_LATCH, 0);
	I_cvar("skill", "1", CVAR_LATCH, 0);
	I_cvar("dedicated", "0", 0, 0);
	I_cvar("developer", g_dev ? "1" : "0", 0, 0);
	I_cvar("ghl_specular", "0", 0, 0);
	I_cvar("ghl_light_method", "0", 0, 0);
	I_cvar("game", "", 0, 0);
	I_cvar("basegame", "base", 0, 0);

	g_playerLib = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
	if (!g_playerLib) { printf("dlopen player: %s\n", dlerror()); return 1; }
	void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) { printf("dlopen game: %s\n", dlerror()); return 1; }
	typedef game_export_t *(*getapi_t)(game_import_t *);
	getapi_t getapi = (getapi_t)dlsym(lib, "GetGameAPI");
	if (!getapi) { printf("no GetGameAPI\n"); return 1; }

	buildImports();
	ge = getapi(&gi);
	printf("game api version %d, edict_size %d\n", ge->apiversion, ge->edict_size);
	ge->Init();
	printf("Init done: max_edicts %d edict_size %d\n", ge->max_edicts, ge->edict_size);

	const char *map = argv[3];
	Ghoul_SetLevelName(map);
	unsigned checksum = 0;
	std::string mappath = std::string("maps/") + map + ".bsp";
	if (!cms_load(mappath.c_str(), &checksum)) { printf("cannot load %s\n", mappath.c_str()); return 1; }
	printf("map %s loaded, %d inline models, %d clusters\n", map, cms_numinline(), cms_numclusters());

	for (size_t i = 0; i < g_cs.size(); i++) g_cs[i].clear();
	g_cs[CS_NAME] = map;
	char buf[32];
	snprintf(buf, sizeof(buf), "%u", checksum);
	g_cs[CS_MAPCHECKSUM] = buf;
	/* inline models get the first model indexes, like the Q2 server does */
	for (int i = 1; i < cms_numinline(); i++)
	{
		snprintf(buf, sizeof(buf), "*%d", i);
		g_cs[(size_t)(CS_MODELS + 1 + i)] = buf;
	}
	g_cs[CS_MODELS + 1] = mappath;

	std::string ents = cms_entities();
	ge->SpawnEntities((char *)map, (char *)ents.c_str(), (char *)"");
	int inuse = 0;
	std::map<std::string, int> classes;
	for (int i = 0; i < ge->num_edicts; i++) if (EDICT_NUM(i)->inuse) inuse++;
	printf("SpawnEntities done: num_edicts %d, in use %d\n", ge->num_edicts, inuse);

	int frames = atoi(argv[4]);
	for (int f = 0; f < frames; f++)
	{
		ge->RunFrame(f);
		for (size_t c = 0; c < g_cmdQueue.size(); c++) if (g_dev) printf("cmd: %s\n", g_cmdQueue[c].c_str());
		g_cmdQueue.clear();
	}
	inuse = 0;
	for (int i = 0; i < ge->num_edicts; i++) if (EDICT_NUM(i)->inuse) inuse++;
	printf("ran %d frames: num_edicts %d in use %d, sounds %d, multicasts %d\n", frames, ge->num_edicts, inuse, g_sounds, g_multicasts);
	int models = 0, sounds = 0, effects = 0;
	for (int i = 1; i < MAX_MODELS; i++) if (!g_cs[(size_t)(CS_MODELS + i)].empty()) models++;
	for (int i = 1; i < MAX_SOUNDS; i++) if (!g_cs[(size_t)(CS_SOUNDS + i)].empty()) sounds++;
	for (int i = 1; i < MAX_EFPACKS; i++) if (!g_cs[(size_t)(CS_EFFECTS + i)].empty()) effects++;
	printf("configstrings: %d models, %d sounds, %d effects\n", models, sounds, effects);
	ge->Shutdown();
	printf("shutdown ok\n");
	return 0;
}
