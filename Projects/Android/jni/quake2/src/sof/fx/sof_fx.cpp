/*
 * sof_fx.cpp - Soldier of Fortune client effects. See sof_fx.h.
 *
 * Effect file (.eft), version <= 3:
 *   float version; [int, float2 if version <= 3]; float numElements;
 *   per element: float type, element data, [float2 start delay if version > 2]
 *   [float2 if version > 1]
 * Element types: 0 emitter (bursts of particles), 1 sound, 2 light, 3 (unused here),
 * 4 screen effects (shake / flash / deafen / force feedback).
 */
#include "sof_fx.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <map>
#include <set>

namespace sfx {

static Host g_host;
static float g_time;      /* current client time (s) */
static float g_frameTime; /* seconds since the last Run */

/* ---------------------------------------------------------------- helpers */

static float frand(float a, float b)
{
	if (a == b) return a;
	return a + (b - a) * ((float)(rand() & 0x7fff) / 32767.0f);
}
static float frand2(const float r[2]) { return frand(r[0], r[1]); }
static void vcopy(const float *a, float *b) { b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; }
static void vma(const float *a, float s, const float *b, float *o) { o[0] = a[0] + s * b[0]; o[1] = a[1] + s * b[1]; o[2] = a[2] + s * b[2]; }
static float vlen(const float *a) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static float vnorm(float *a)
{
	float l = vlen(a);
	if (l > 0) { a[0] /= l; a[1] /= l; a[2] /= l; }
	return l;
}
static void vcross(const float *a, const float *b, float *o)
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}
static void perpendiculars(const float *n, float *r, float *u)
{
	float t[3] = { 0, 0, 1 };
	if (fabsf(n[2]) > 0.9f) { t[0] = 1; t[2] = 0; }
	vcross(n, t, r); vnorm(r);
	vcross(r, n, u); vnorm(u);
}
/* stable pointers for texture names */
static const char *intern(const std::string &s)
{
	static std::set<std::string> pool;
	return pool.insert(s).first->c_str();
}

static std::string lower(const char *s)
{
	std::string o(s ? s : "");
	for (size_t i = 0; i < o.size(); i++) o[i] = (char)tolower((unsigned char)o[i]);
	for (size_t i = 0; i < o.size(); i++) if (o[i] == '\\') o[i] = '/';
	return o;
}

/* ---------------------------------------------------------------- effect definitions */

enum { PT_SPRITE, PT_LINE, PT_ORIENTED, PT_GHOUL, PT_DEBRIS };

/* particle flags (pdef 0xe0) */
enum
{
	PF_WIDTH_IS_HEIGHT = 0x1, PF_LIT = 0x2, PF_ADDITIVE = 0x4, PF_SUBTRACTIVE = 0x8, PF_PHYSICS = 0x10,
	PF_RANDOM_ROT = 0x20, PF_FADE_TRAILS = 0x40, PF_SHRINK_TRAILS = 0x80, PF_ABS_ACCEL = 0x100,
	PF_RANDOM_ADD = 0x400, PF_CHEAP = 0x800, PF_CURVE_TO_POINT = 0x1000, PF_ONE_FRAME = 0x2000,
	PF_COLLIDE_WATER = 0x4000, PF_REMOVE_ON_COLLIDE = 0x8000, PF_NO_SCALING = 0x10000,
	PF_COLLIDE_ENTS = 0x20000, PF_DECELERATE = 0x40000, PF_NO_ZBUFFER = 0x80000
};

struct PDef
{
	float vel[3][2];    /* forward, right, up */
	float acc[3][2];
	int type;
	float size0[2], size0End[2], size1[2], size1End[2];
	int orient;         /* oriented sprite: 0 forward 1 right 2 up 3 backward; line: end point source */
	float lineLen[2];
	std::string tex;
	float life[2];
	float r[2], g[2], b[2], a[2];
	float alphaMin;     /* linear styles: ratio of the peak; rise/fall styles: fraction of 255 */
	float trailFreq;
	float rot0[2], rotVel[2];
	unsigned flags;
	float bbMin[3], bbMax[3], elasticity;
	std::string hitEffect;
	float trailLife;
	std::string trailEffect;
	float objAng[3][2], objAvel[3][2];
	int alphaStyle, colorStyle;
};

struct Element
{
	int type;
	float delay[2];     /* start delay */
	/* emitter */
	float burstDelay[2], duration[2];
	int style, count, eflags, scaleFlags;
	float regionMin[3], regionMax[3], offset[3];
	float radius, elemScale[2], timeScale[2], distPerUnit;
	PDef pd;
	/* sound */
	std::string sound;
	float volume[2], atten[2];
	/* light */
	float lradius[2], lr[2], lg[2], lb[2], llife[2];
};

struct EffectDef
{
	bool ok;
	std::vector<Element> elements;
	float duration[2];
};

struct Reader
{
	const unsigned char *d; size_t n, p; bool bad;
	Reader(const void *data, size_t len) : d((const unsigned char *)data), n(len), p(0), bad(false) {}
	float f() { float v = 0; if (p + 4 <= n) memcpy(&v, d + p, 4); else bad = true; p += 4; return v; }
	int i() { int v = 0; if (p + 4 <= n) memcpy(&v, d + p, 4); else bad = true; p += 4; return v; }
	void pair(float *o) { o[0] = f(); o[1] = f(); }
	void v3(float *o) { o[0] = f(); o[1] = f(); o[2] = f(); }
	std::string name()
	{
		char b[65];
		if (p + 64 <= n) memcpy(b, d + p, 64); else { bad = true; b[0] = 0; }
		b[64] = 0;
		p += 64;
		return lower(b);
	}
	void skip(size_t k) { p += k; }
};

static bool readPDef(Reader &r, PDef &o)
{
	float ver = r.f();
	if (ver > 7) return false;
	for (int k = 0; k < 3; k++) r.pair(o.vel[k]);
	for (int k = 0; k < 3; k++) r.pair(o.acc[k]);
	o.type = r.i();
	r.pair(o.size0); r.pair(o.size0End); r.pair(o.size1); r.pair(o.size1End);
	o.orient = r.i();
	r.pair(o.lineLen);
	if (ver < 5) r.skip(0xf4);
	o.tex = r.name();
	r.pair(o.life); r.pair(o.r); r.pair(o.g); r.pair(o.b); r.pair(o.a);
	o.alphaMin = r.f();
	o.trailFreq = r.f();
	r.pair(o.rot0); r.pair(o.rotVel);
	o.flags = (unsigned)r.i();
	if (ver >= 2) { r.v3(o.bbMin); r.v3(o.bbMax); o.elasticity = r.f(); o.hitEffect = r.name(); }
	else { for (int k = 0; k < 3; k++) o.bbMin[k] = o.bbMax[k] = 0; o.elasticity = 0.7f; }
	if (ver >= 3) { o.trailLife = r.f(); o.trailEffect = r.name(); }
	else o.trailLife = 60;
	if (ver >= 4) for (int k = 0; k < 3; k++) { r.pair(o.objAng[k]); r.pair(o.objAvel[k]); }
	else for (int k = 0; k < 3; k++) o.objAng[k][0] = o.objAng[k][1] = o.objAvel[k][0] = o.objAvel[k][1] = 0;
	if (ver < 4) o.flags &= 0x3fff;
	if (ver < 6) o.flags &= 0xfffc7fff;
	if (ver >= 7) { o.alphaStyle = r.i(); o.colorStyle = r.i(); }
	else { o.alphaStyle = 0; o.colorStyle = (o.flags & PF_RANDOM_ROT) ? 0 : 1; o.flags &= 0xfff3ffff; }
	return !r.bad;
}

static bool readElement(Reader &r, int type, Element &e)
{
	e.type = type;
	switch (type)
	{
		case 0:
		{
			float ver = r.f();
			if (ver > 3) return false;
			r.pair(e.burstDelay); r.pair(e.duration);
			if (e.duration[0] < 0.005f) e.duration[0] = 0.005f; else if (e.duration[0] > 99999) e.duration[0] = 99999;
			if (e.duration[1] < 0.005f) e.duration[1] = 0.005f; else if (e.duration[1] > 99999) e.duration[1] = 99999;
			e.style = r.i(); e.count = r.i(); e.eflags = r.i(); e.scaleFlags = r.i();
			r.v3(e.regionMin); r.v3(e.regionMax); r.v3(e.offset);
			e.radius = r.f(); e.elemScale[0] = r.f(); e.elemScale[1] = r.f(); e.timeScale[0] = r.f(); e.timeScale[1] = r.f();
			if (!readPDef(r, e.pd)) return false;
			e.distPerUnit = 0;
			if (ver >= 2) { float t[2]; r.pair(t); r.f(); }
			if (ver >= 3) { e.distPerUnit = r.f(); r.f(); }
			else if (e.eflags != 1) { for (int k = 0; k < 3; k++) e.regionMin[k] = e.regionMax[k] = 0; }
			return !r.bad;
		}
		case 1:
			e.sound = r.name(); r.pair(e.volume); r.pair(e.atten);
			return !r.bad;
		case 2:
			r.pair(e.lradius); r.pair(e.lr); r.pair(e.lg); r.pair(e.lb); r.pair(e.llife);
			return !r.bad;
		case 3:
		{
			int v = r.i();
			if (v > 1) return false;
			r.name(); r.f(); r.f(); r.f(); r.f(); r.i();
			return !r.bad;
		}
		case 4:
		{
			float t[2];
			r.pair(t); r.pair(t); r.pair(t); r.i(); r.i();
			return !r.bad;
		}
	}
	return false;
}

static std::map<std::string, EffectDef> g_defs;

static const EffectDef *findDef(const char *name)
{
	std::string key = lower(name);
	std::map<std::string, EffectDef>::iterator it = g_defs.find(key);
	if (it != g_defs.end()) return it->second.ok ? &it->second : 0;
	EffectDef &d = g_defs[key];
	d.ok = false;
	d.duration[0] = d.duration[1] = 0;
	void *buf = 0;
	int len = g_host.loadFile ? g_host.loadFile(("effects/" + key + ".eft").c_str(), &buf) : -1;
	if (len <= 0 || !buf) return 0;
	Reader r(buf, (size_t)len);
	float ver = r.f();
	bool ok = ver <= 3;
	if (ok)
	{
		r.i();
		r.pair(d.duration);
		int n = (int)r.f();
		for (int k = 0; k < n && ok; k++)
		{
			int t = (int)r.f();
			Element e;
			memset(e.delay, 0, sizeof(e.delay));
			ok = readElement(r, t, e);
			if (ver > 2) r.pair(e.delay); else e.delay[0] = e.delay[1] = 0;
			if (ok) d.elements.push_back(e);
		}
	}
	g_host.freeFile(buf);
	d.ok = ok && !r.bad;
	return d.ok ? &d : 0;
}

/* ---------------------------------------------------------------- running effects */

struct Context
{
	float scale;
	int numElements;
	unsigned flags;
	float org[3], pos2[3];
	float fwd[3], up[3], right[3];
	float mins[3], maxs[3];
	float space;      /* the anchor's space scale (Frame::scale) */
};

struct ElementRun
{
	int index;
	bool emitter, done;
	float start, next, end;
};

struct Effect
{
	const EffectDef *def;
	Anchor anchor;
	Params params;
	Context ctx;
	float startTime;
	std::vector<ElementRun> runs;
	int liveAttached;
	bool anchorGone;
};

struct Particle
{
	int type;
	float pos[3], vel[3], acc[3];
	float end[3], endVel[3];      /* lines */
	float axis[3];                /* oriented sprites */
	float size0, size1, grow0, grow1;
	float rot, rotVel;
	float rgb[3], rgbEnd[3];
	float alpha, alphaParam, alphaMin255;
	int alphaStyle, colorStyle;
	float birth, death;
	unsigned pflags;
	const char *tex;               /* interned (see intern()), so Particle stays plain data */
	int effect;                    /* attached: index into g_effects, else -1 */
	bool decelerate;
};

static std::vector<Effect *> g_effects;
static std::vector<Particle> g_particles;
static std::vector<Particle> g_decals;
static size_t g_nextDecal;
static std::vector<Sound> g_pendingSounds;
static std::vector<Light> g_lights;
struct TimedLight { Light l; float die, radius0; };
static std::vector<TimedLight> g_timedLights;

void Init(const Host &host)
{
	g_host = host;
}

void Clear()
{
	for (size_t i = 0; i < g_effects.size(); i++) delete g_effects[i];
	g_effects.clear();
	g_particles.clear();
	g_pendingSounds.clear();
	g_timedLights.clear();
	g_decals.clear();
	g_nextDecal = 0;
}

int NumParticles() { return (int)g_particles.size(); }

static void defaultAxes(Context &c)
{
	c.fwd[0] = 1; c.fwd[1] = 0; c.fwd[2] = 0;
	c.right[0] = 0; c.right[1] = -1; c.right[2] = 0;
	c.up[0] = 0; c.up[1] = 0; c.up[2] = 1;
}

static bool updateContext(Effect &e)
{
	Context &c = e.ctx;
	Frame f;
	if (!g_host.resolve || !g_host.resolve(e.anchor, f)) return false;
	vcopy(f.org, c.org);
	vcopy(f.fwd, c.fwd); vcopy(f.right, c.right); vcopy(f.up, c.up);
	c.space = f.scale > 0 ? f.scale : 1;
	if (e.params.flags & EFF_DIR)
	{
		float d[3];
		vcopy(e.params.dir, d);
		if (vnorm(d) > 0)
		{
			vcopy(d, c.fwd);
			perpendiculars(d, c.right, c.up);
		}
	}
	return true;
}

void Start(const char *name, const Anchor &a, const Params &p)
{
	const EffectDef *def = findDef(name);
	if (!def) return;
	Effect *e = new Effect;
	e->def = def;
	e->anchor = a;
	e->params = p;
	e->startTime = -1;  /* set on the first Run */
	e->liveAttached = 0;
	e->anchorGone = false;
	Context &c = e->ctx;
	c.scale = (p.flags & EFF_SCALE) ? p.scale : 1.0f;
	c.numElements = (p.flags & EFF_NUMELEMS) ? p.numElements : 0;
	c.flags = p.flags;
	vcopy(p.pos2, c.pos2);
	vcopy(p.mins, c.mins);
	vcopy(p.maxs, c.maxs);
	defaultAxes(c);
	c.space = 1;
	for (int k = 0; k < 3; k++) c.org[k] = a.pos[k];
	g_effects.push_back(e);
}

void NoteSound(const char *name, const Anchor &a)
{
	if (!name || !*name) return;
	while (*name == '/' || *name == '\\') name++;
	std::string s = lower(name);
	if (s.size() < 4 || s.compare(s.size() - 4, 4, ".wav") != 0) s += ".wav";
	Sound snd;
	snd.name = s;
	Frame f;
	if (g_host.resolve && g_host.resolve(a, f)) vcopy(f.org, snd.org);
	else vcopy(a.pos, snd.org);
	snd.ent = a.ent;
	snd.volume = 1;
	snd.attenuation = 1;
	snd.local = a.kind == ANCHOR_VIEWWEAPON;
	g_pendingSounds.push_back(snd);
}

/* SoF's particle spawn: velocity/acceleration in the effect's axes, size, colour, alpha */
static void spawnParticle(const PDef &pd, const float *pos, const Context &c, float scale, int scaleFlags,
                          bool attached, int effectIndex)
{
	if (pd.type == PT_GHOUL || pd.type == PT_DEBRIS) return; /* GHOUL debris: not yet */
	Particle p;
	memset(&p, 0, sizeof(p));
	float life = frand2(pd.life);
	if (scaleFlags & 0x20) life *= scale;
	if (life <= 0) return;

	float v[3] = { 0, 0, 0 }, ac[3] = { 0, 0, 0 };
	if (!(pd.flags & PF_CURVE_TO_POINT))
	{
		if (!attached)
		{
			float f = frand2(pd.vel[0]), r = frand2(pd.vel[1]), u = frand2(pd.vel[2]);
			for (int k = 0; k < 3; k++) v[k] = f * c.fwd[k] + r * c.right[k] + u * c.up[k];
		}
		else
		{
			v[0] = frand2(pd.vel[0]); v[1] = frand2(pd.vel[1]); v[2] = frand2(pd.vel[2]);
		}
		if (scaleFlags & 1) for (int k = 0; k < 3; k++) v[k] *= scale;
		if (!(pd.flags & PF_ABS_ACCEL))
		{
			float f = frand2(pd.acc[0]), r = frand2(pd.acc[1]), u = frand2(pd.acc[2]);
			for (int k = 0; k < 3; k++) ac[k] = f * c.fwd[k] + r * c.right[k] + u * c.up[k];
		}
		else
		{
			ac[0] = frand2(pd.acc[0]); ac[1] = frand2(pd.acc[1]); ac[2] = frand2(pd.acc[2]);
		}
		if (scaleFlags & 2) for (int k = 0; k < 3; k++) ac[k] *= scale;
	}
	else
	{
		/* curve to point: travel to pos2 over the lifetime */
		float d[3] = { c.pos2[0] - pos[0], c.pos2[1] - pos[1], c.pos2[2] - pos[2] };
		float len = vnorm(d) / life;
		float r = frand2(pd.vel[1]), u = frand2(pd.vel[2]);
		for (int k = 0; k < 3; k++)
		{
			v[k] = d[k] * len + u * c.up[k] + r * c.right[k];
			ac[k] = (u * -1.9f / life) * c.up[k] + (r * -1.9f / life) * c.right[k];
		}
	}

	/* alpha */
	float peak = frand2(pd.a);
	if (scaleFlags & 0x10) peak *= scale;
	switch (pd.alphaStyle)
	{
		case 0: /* linear fall: from the peak down to peak * min ratio */
			p.alpha = peak;
			p.alphaParam = (peak * pd.alphaMin - peak) / life;
			break;
		case 1: /* linear rise: from peak * min ratio up to the peak */
			p.alpha = peak * pd.alphaMin;
			p.alphaParam = (peak - p.alpha) / life;
			break;
		default: /* rise then fall (linear or sine) */
			p.alpha = 1;
			p.alphaParam = peak;
			break;
	}
	p.alphaStyle = pd.alphaStyle;
	p.alphaMin255 = pd.alphaMin * 255.0f;

	p.pflags = pd.flags;
	if ((pd.flags & PF_RANDOM_ADD) && frand(0, 1) < 0.5f) p.pflags |= PF_ADDITIVE;
	p.birth = g_time;
	p.death = (pd.flags & PF_ONE_FRAME) ? g_time + 0.001f : g_time + life;
	p.tex = intern(pd.tex);
	p.effect = attached ? effectIndex : -1;
	p.decelerate = (pd.flags & PF_DECELERATE) != 0;

	/* colour */
	p.colorStyle = pd.colorStyle;
	switch (pd.colorStyle)
	{
		case 0: { float t = frand(0, 1); p.rgb[0] = pd.r[0] + (pd.r[1] - pd.r[0]) * t; p.rgb[1] = pd.g[0] + (pd.g[1] - pd.g[0]) * t; p.rgb[2] = pd.b[0] + (pd.b[1] - pd.b[0]) * t; break; }
		case 1: p.rgb[0] = frand2(pd.r); p.rgb[1] = frand2(pd.g); p.rgb[2] = frand2(pd.b); break;
		default: p.rgb[0] = pd.r[0]; p.rgb[1] = pd.g[0]; p.rgb[2] = pd.b[0]; p.rgbEnd[0] = pd.r[1]; p.rgbEnd[1] = pd.g[1]; p.rgbEnd[2] = pd.b[1]; break;
	}

	p.type = pd.type;
	vcopy(pos, p.pos);
	vcopy(v, p.vel);
	vcopy(ac, p.acc);
	float s0 = frand2(pd.size0);
	if (scaleFlags & 4) s0 *= scale;
	float s1 = s0;
	if (!(pd.flags & PF_WIDTH_IS_HEIGHT)) { s1 = frand2(pd.size1); if (scaleFlags & 8) s1 *= scale; }
	float e0 = frand2(pd.size0End);
	if (scaleFlags & 4) e0 *= scale;
	float e1 = frand2(pd.size1End);
	if (scaleFlags & 8) e1 *= scale;
	if (pd.flags & PF_WIDTH_IS_HEIGHT) e1 = e0;
	/* effects on the VR view weapon live in its (scaled down) space */
	float sp = c.space;
	s0 *= sp; s1 *= sp; e0 *= sp; e1 *= sp;
	for (int k = 0; k < 3; k++) { p.vel[k] *= sp; p.acc[k] *= sp; }
	p.size0 = s0; p.size1 = s1;
	if (!(pd.flags & PF_NO_SCALING)) { p.grow0 = (e0 - s0) / life; p.grow1 = (e1 - s1) / life; }

	if (pd.type == PT_LINE)
	{
		p.size1 = s1 = s0; /* line width */
		float dir[3];
		vcopy(v, dir);
		if (vnorm(dir) < 0.1f) { vcopy(ac, dir); vnorm(dir); }
		float len = frand2(pd.lineLen) * sp;
		switch (pd.orient)
		{
			case 1: vcopy(c.pos2, p.end); break;
			case 3: vma(pos, len, dir, p.end); p.vel[0] = p.vel[1] = p.vel[2] = 0; break;
			default: vma(pos, len, dir, p.end); break;
		}
		vcopy(p.vel, p.endVel);
	}
	else
	{
		p.rot = frand2(pd.rot0);
		p.rotVel = frand2(pd.rotVel);
		if (pd.type == PT_ORIENTED)
		{
			switch (pd.orient)
			{
				case 0: vcopy(c.fwd, p.axis); break;
				case 1: vcopy(c.right, p.axis); break;
				case 2: vcopy(c.up, p.axis); break;
				default: for (int k = 0; k < 3; k++) p.axis[k] = -c.fwd[k]; break;
			}
		}
	}
	g_particles.push_back(p);
}

/* one burst of an emitter: SoF's emitter styles place each element */
static void burst(Effect &e, const Element &el, float timeFrac)
{
	const Context &c = e.ctx;
	float scale = c.scale;
	float radius = el.radius;
	if (el.eflags & 4)
	{
		scale *= timeFrac * el.timeScale[0] + (1.0f - timeFrac) * el.timeScale[1];
		radius *= scale;
	}
	int n = el.count;
	if (c.numElements) n = c.numElements;
	if (el.style == 2 && el.distPerUnit != 0)
	{
		float d[3] = { c.org[0] - c.pos2[0], c.org[1] - c.pos2[1], c.org[2] - c.pos2[2] };
		n = (int)(vlen(d) / el.distPerUnit);
	}
	if (n < 1) n = 1;
	const float *mn = (c.flags & EFF_MIN) ? c.mins : el.regionMin;
	const float *mx = (c.flags & EFF_MAX) ? c.maxs : el.regionMax;
	bool attached = (el.eflags & 1) != 0;
	float ringStart = el.style == 3 ? frand(0, 6.28f) : 0;
	int effectIndex = -1;
	for (size_t i = 0; i < g_effects.size(); i++) if (g_effects[i] == &e) { effectIndex = (int)i; break; }

	for (int i = 0; i < n; i++)
	{
		float pos[3];
		switch (el.style)
		{
			case 1: /* region */
			{
				float a = frand(mn[0], mx[0]), b = frand(mn[1], mx[1]), d = frand(mn[2], mx[2]);
				if (attached) { pos[0] = a; pos[1] = b; pos[2] = d; }
				else for (int k = 0; k < 3; k++) pos[k] = c.org[k] + a * c.fwd[k] + b * c.right[k] + d * c.up[k];
				break;
			}
			case 2: /* line from org to pos2 */
			{
				float f = 1.0f - (float)(i + 1) / (float)n;
				float a = frand(mn[0], mx[0]), b = frand(mn[1], mx[1]), d = frand(mn[2], mx[2]);
				for (int k = 0; k < 3; k++)
					pos[k] = (1 - f) * c.pos2[k] + f * c.org[k] + a * c.fwd[k] + b * c.right[k] + d * c.up[k];
				break;
			}
			case 3: /* ring */
			{
				float ang = ((float)(i + 1) / (float)n + ringStart) * 6.2831853f;
				pos[0] = cosf(ang) * radius + c.org[0];
				pos[1] = sinf(ang) * radius + c.org[1];
				pos[2] = c.org[2];
				break;
			}
			case 4: /* directed line */
			{
				float along = (float)i * el.distPerUnit;
				float a = frand(mn[0], mx[0]), b = frand(mn[1], mx[1]), d = frand(mn[2], mx[2]);
				if (attached) { pos[0] = along + a; pos[1] = b; pos[2] = d; }
				else for (int k = 0; k < 3; k++) pos[k] = c.org[k] + along * c.fwd[k] + a * c.fwd[k] + b * c.right[k] + d * c.up[k];
				break;
			}
			case 5: vcopy(c.pos2, pos); break;
			default: /* spot (floor / ceiling / surface styles fall back to the spot) */
				if (attached) pos[0] = pos[1] = pos[2] = 0;
				else vcopy(c.org, pos);
				break;
		}
		float s = scale;
		if (el.eflags & 2)
		{
			float f = 1.0f - (float)(i + 1) / (float)n;
			s = (f * el.elemScale[0] + (1.0f - f) * el.elemScale[1]) * scale;
		}
		spawnParticle(el.pd, pos, c, s, el.scaleFlags, attached, effectIndex);
		if (attached && effectIndex >= 0) e.liveAttached++;
	}
}

static void fireElement(Effect &e, const Element &el)
{
	switch (el.type)
	{
		case 1:
		{
			Sound s;
			s.name = el.sound;
			if (s.name.size() < 4 || s.name.compare(s.name.size() - 4, 4, ".wav") != 0) s.name += ".wav";
			vcopy(e.ctx.org, s.org);
			s.ent = e.anchor.kind == ANCHOR_POS ? 0 : e.anchor.ent;
			s.volume = frand2(el.volume);
			s.attenuation = frand2(el.atten);
			s.local = e.anchor.kind == ANCHOR_VIEWWEAPON;
			g_pendingSounds.push_back(s);
			break;
		}
		case 2:
		{
			TimedLight t;
			vcopy(e.ctx.org, t.l.org);
			t.radius0 = t.l.radius = frand2(el.lradius) * e.ctx.scale;
			t.l.rgb[0] = frand2(el.lr) / 255.0f;
			t.l.rgb[1] = frand2(el.lg) / 255.0f;
			t.l.rgb[2] = frand2(el.lb) / 255.0f;
			float life = frand2(el.llife);
			t.die = g_time + (life > 0 ? life : 0.1f);
			g_timedLights.push_back(t);
			break;
		}
		default:
			break; /* screen effects / type 3: not yet */
	}
}

/* returns false when the effect has finished */
static bool runEffect(Effect &e)
{
	if (e.startTime < 0)
	{
		e.startTime = g_time;
		for (size_t i = 0; i < e.def->elements.size(); i++)
		{
			const Element &el = e.def->elements[i];
			ElementRun r;
			r.index = (int)i;
			r.emitter = el.type == 0;
			r.done = false;
			r.start = g_time + frand2(el.delay);
			r.next = r.start;
			r.end = r.emitter ? r.start + frand2(el.duration) : r.start;
			e.runs.push_back(r);
		}
	}
	if (!e.anchorGone && !updateContext(e)) e.anchorGone = true;
	if (e.anchorGone) return e.liveAttached > 0;

	bool active = false;
	for (size_t i = 0; i < e.runs.size(); i++)
	{
		ElementRun &r = e.runs[i];
		if (r.done) continue;
		const Element &el = e.def->elements[(size_t)r.index];
		if (g_time < r.start) { active = true; continue; }
		if (!r.emitter)
		{
			fireElement(e, el);
			r.done = true;
			continue;
		}
		/* emitter: burst now and every "delay per burst" until the duration ends */
		int guard = 0;
		while (r.next <= g_time && r.next <= r.end && guard++ < 16)
		{
			float frac = r.end > r.start ? 1.0f - (g_time - r.start) / (r.end - r.start) : 1.0f;
			if (frac < 0) frac = 0;
			burst(e, el, frac);
			float d = frand2(el.burstDelay);
			if (d < 0.01f) { r.next = r.end + 1; break; }
			r.next += d;
		}
		if (r.next > r.end && g_time >= r.end) r.done = true;
		else active = true;
	}
	return active || e.liveAttached > 0;
}

/* ---------------------------------------------------------------- particle update */

static bool updateParticle(Particle &p, float dt)
{
	if (g_time >= p.death) return false;
	/* acceleration, then velocity (SoF: decelerating particles stop once the
	   acceleration would make them speed up again) */
	if (p.acc[0] || p.acc[1] || p.acc[2])
	{
		float before = p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2];
		for (int k = 0; k < 3; k++) p.vel[k] += dt * p.acc[k];
		if (p.type == PT_LINE) for (int k = 0; k < 3; k++) p.endVel[k] += dt * p.acc[k];
		if (p.decelerate)
		{
			float after = p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2];
			if (before < after) { p.vel[0] = p.vel[1] = p.vel[2] = 0; p.acc[0] = p.acc[1] = p.acc[2] = 0; }
		}
	}
	for (int k = 0; k < 3; k++) p.pos[k] += dt * p.vel[k];
	if (p.type == PT_LINE) for (int k = 0; k < 3; k++) p.end[k] += dt * p.endVel[k];
	p.rot += dt * p.rotVel;
	p.size0 += dt * p.grow0;
	p.size1 += dt * p.grow1;
	if (p.size0 < 0 || p.size1 < 0) return false;

	float t = (g_time - p.birth) / (p.death - p.birth) + 0.01f;
	switch (p.alphaStyle)
	{
		case 0: case 1: p.alpha += dt * p.alphaParam; break;
		case 2:
			if (t < 0.5f) p.alpha = (p.alphaParam - p.alphaMin255) * 2 * t + p.alphaMin255;
			else p.alpha = (p.alphaParam - p.alphaMin255) * 2 * (1 - t) + p.alphaMin255;
			break;
		default:
		{
			float tt = t < 0 ? 0 : (t > 1 ? 1 : t);
			p.alpha = (p.alphaParam - p.alphaMin255) * sinf(tt * 3.14159265f) + p.alphaMin255;
			break;
		}
	}
	if (p.alpha > 255) p.alpha = 255;
	if (p.alpha < 0) p.alpha = 0;
	return true;
}

static void particleColor(const Particle &p, unsigned char out[4])
{
	float rgb[3];
	vcopy(p.rgb, rgb);
	if (p.colorStyle >= 2)
	{
		float t = (g_time - p.birth) / (p.death - p.birth);
		if (t < 0) t = 0; else if (t > 1) t = 1;
		if (p.colorStyle == 3) t = sinf(t * 1.5707963f);
		for (int k = 0; k < 3; k++) rgb[k] = p.rgb[k] + (p.rgbEnd[k] - p.rgb[k]) * t;
	}
	float a = p.alpha;
	/* additive / subtractive sprites are drawn with colour * alpha (SoF's renderer) */
	if (p.pflags & (PF_ADDITIVE | PF_SUBTRACTIVE))
	{
		for (int k = 0; k < 3; k++) rgb[k] = rgb[k] * a / 255.0f;
		a = 255;
	}
	for (int k = 0; k < 3; k++) out[k] = (unsigned char)(rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : rgb[k]);
	out[3] = (unsigned char)a;
}

/* ---------------------------------------------------------------- direct particles */

static const char *g_sprites[] =
{
	"smoke2", "bldglob4", "bldglob5", "bldglob6", "blood_pool2", "footstep", "footstp2", "lit", "ring1", "dirt",
	"sparkpart", "water", "powder", "bhole2", "lightning", "sparkpartblue", "flare", "waterdrop", "dent", "rain",
	"mflash", "shock2", "shock3", "shock4", "boom3", "boom5", "scorch", "toxic_ooze", "explode1", "scorchwht",
	"snow", "bubble", "pipeleft", "bhole_glass3", "slash", "ring2", "beam", "flash1", "mflash3", "mflash4",
	"flare2", "smoke3", "bhole_mtl", "bhole_wd", "minimiflash1", "minimiflash2", "minimiflash3", "expfire1", "expfire2", "expfire3",
	"expfire4", "smkgrn", "smkgry", "smkwht", "firestreak", "whitestreak", "puddle"
};

const char *SpriteName(int index)
{
	static std::string names[sizeof(g_sprites) / sizeof(g_sprites[0])];
	if (index < 0 || index >= (int)(sizeof(g_sprites) / sizeof(g_sprites[0]))) index = 0;
	if (names[index].empty()) names[index] = std::string("textures/sprites/") + g_sprites[index];
	return names[index].c_str();
}

static Particle rawToParticle(const Raw &r)
{
	Particle p;
	memset(&p, 0, sizeof(p));
	p.type = r.oriented ? PT_ORIENTED : PT_SPRITE;
	vcopy(r.pos, p.pos); vcopy(r.vel, p.vel); vcopy(r.acc, p.acc); vcopy(r.normal, p.axis);
	p.size0 = r.size0; p.size1 = r.size1; p.grow0 = r.grow0; p.grow1 = r.grow1;
	p.rot = r.rot; p.rotVel = r.rotVel;
	p.rgb[0] = r.rgba[0]; p.rgb[1] = r.rgba[1]; p.rgb[2] = r.rgba[2];
	p.alpha = r.rgba[3];
	p.alphaParam = r.alphaRate;
	p.alphaStyle = 0;
	p.colorStyle = 1;
	p.pflags = r.blend == 1 ? PF_ADDITIVE : r.blend == 2 ? PF_SUBTRACTIVE : 0;
	p.birth = g_time;
	p.death = g_time + (r.life > 0 ? r.life : 0.001f);
	p.tex = intern(r.tex);
	p.effect = -1;
	return p;
}

void SpawnRaw(const Raw &r)
{
	if (g_particles.size() > 4096) return;
	g_particles.push_back(rawToParticle(r));
}


void Decal(const char *tex, const float *pos, const float *normal, float halfSize, const unsigned char rgba[4])
{
	Raw r;
	r.tex = tex;
	r.life = 1e9f;
	for (int k = 0; k < 3; k++) r.pos[k] = pos[k] + normal[k] * 0.3f; /* off the wall: no z-fighting */
	vcopy(normal, r.normal);
	r.oriented = true;
	r.size0 = r.size1 = halfSize;
	r.rot = frand(0, 6.2831853f);
	memcpy(r.rgba, rgba, 4);
	Particle p = rawToParticle(r);
	const size_t maxDecals = 512;
	if (g_decals.size() < maxDecals) g_decals.push_back(p);
	else { g_decals[g_nextDecal] = p; g_nextDecal = (g_nextDecal + 1) % maxDecals; }
}

void PlaySound(const char *name, const float *org, int ent, float volume, float attenuation)
{
	if (!name || !*name) return;
	Sound s;
	s.name = lower(name);
	vcopy(org, s.org);
	s.ent = ent;
	s.volume = volume;
	s.attenuation = attenuation;
	s.local = false;
	g_pendingSounds.push_back(s);
}

void StartAt(const char *name, const float *pos, const float *dir)
{
	Anchor a;
	a.kind = ANCHOR_POS;
	vcopy(pos, a.pos);
	Params p;
	if (dir) { p.flags |= EFF_DIR; vcopy(dir, p.dir); }
	Start(name, a, p);
}

/* ---------------------------------------------------------------- output */

static Batch &batchFor(Output &out, const std::string &tex, int blend, bool noDepth)
{
	for (size_t i = 0; i < out.batches.size(); i++)
		if (out.batches[i].blend == blend && out.batches[i].noDepth == noDepth && out.batches[i].texture == tex)
			return out.batches[i];
	out.batches.push_back(Batch());
	Batch &b = out.batches.back();
	b.texture = tex;
	b.blend = blend;
	b.noDepth = noDepth;
	return b;
}

static void addQuad(Batch &b, const float c[4][3], const unsigned char rgba[4])
{
	if (b.xyz.size() / 3 + 4 > 65535) return;
	unsigned short base = (unsigned short)(b.xyz.size() / 3);
	static const float st[4][2] = { { 0, 1 }, { 0, 0 }, { 1, 0 }, { 1, 1 } };
	for (int k = 0; k < 4; k++)
	{
		b.xyz.push_back(c[k][0]); b.xyz.push_back(c[k][1]); b.xyz.push_back(c[k][2]);
		b.st.push_back(st[k][0]); b.st.push_back(st[k][1]);
		for (int j = 0; j < 4; j++) b.rgba.push_back(rgba[j]);
	}
	static const unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
	for (int k = 0; k < 6; k++) b.indices.push_back((unsigned short)(base + idx[k]));
}

static void toWorld(const Particle &p, const float *local, float *w)
{
	if (p.effect < 0 || p.effect >= (int)g_effects.size()) { vcopy(local, w); return; }
	const Context &c = g_effects[(size_t)p.effect]->ctx;
	for (int k = 0; k < 3; k++) w[k] = c.org[k] + local[0] * c.fwd[k] + local[1] * c.right[k] + local[2] * c.up[k];
}

static void drawParticle(const Particle &p, const float *vright, const float *vup, const float *vieworg, Output &out)
{
	const char *texPath = g_host.texturePath ? g_host.texturePath(p.tex) : p.tex;
	if (!texPath || !*texPath) return;
	int blend = (p.pflags & PF_SUBTRACTIVE) ? 2 : (p.pflags & PF_ADDITIVE) ? 1 : 0;
	Batch &b = batchFor(out, texPath, blend, (p.pflags & PF_NO_ZBUFFER) != 0);
	unsigned char rgba[4];
	particleColor(p, rgba);
	float c[4][3];
	float pos[3];
	toWorld(p, p.pos, pos);
	if (p.type == PT_LINE)
	{
		float end[3];
		toWorld(p, p.end, end);
		float d[3] = { end[0] - pos[0], end[1] - pos[1], end[2] - pos[2] };
		float e[3] = { pos[0] - vieworg[0], pos[1] - vieworg[1], pos[2] - vieworg[2] };
		float s[3];
		vcross(e, d, s);
		if (vnorm(s) == 0) return;
		float w = p.size0;
		for (int k = 0; k < 3; k++)
		{
			c[0][k] = pos[k] - s[k] * w; c[1][k] = pos[k] + s[k] * w;
			c[2][k] = end[k] + s[k] * w; c[3][k] = end[k] - s[k] * w;
		}
		addQuad(b, c, rgba);
		return;
	}
	float R[3], U[3];
	if (p.type == PT_ORIENTED) perpendiculars(p.axis, R, U);
	else { vcopy(vright, R); vcopy(vup, U); }
	float cs = cosf(p.rot), sn = sinf(p.rot);
	float w = p.size0, h = p.size1;
	/* corners (-w,-h) (-w,h) (w,h) (w,-h), rotated */
	static const float sx[4] = { -1, -1, 1, 1 }, sy[4] = { -1, 1, 1, -1 };
	for (int i = 0; i < 4; i++)
	{
		float x = sx[i] * w, y = sy[i] * h;
		float rx = x * cs - y * sn, ry = x * sn + y * cs;
		for (int k = 0; k < 3; k++) c[i][k] = pos[k] + rx * R[k] + ry * U[k];
	}
	addQuad(b, c, rgba);
}

void Run(float time, const float vieworg[3], const float vright[3], const float vup[3], Output &out)
{
	out.batches.clear();
	out.lights.clear();
	out.sounds.clear();
	g_frameTime = g_time > 0 ? time - g_time : 0;
	if (g_frameTime < 0 || g_frameTime > 0.5f) g_frameTime = 0;
	g_time = time;

	/* effects (new ones start here) */
	for (size_t i = 0; i < g_effects.size(); i++) g_effects[i]->liveAttached = 0;
	for (size_t i = 0; i < g_particles.size(); i++)
		if (g_particles[i].effect >= 0 && g_particles[i].effect < (int)g_effects.size())
			g_effects[(size_t)g_particles[i].effect]->liveAttached++;
	std::vector<bool> keep(g_effects.size(), true);
	for (size_t i = 0; i < g_effects.size(); i++) keep[i] = runEffect(*g_effects[i]);

	/* particles */
	size_t w = 0;
	for (size_t i = 0; i < g_particles.size(); i++)
	{
		Particle &p = g_particles[i];
		if (p.effect >= 0 && (p.effect >= (int)g_effects.size() || g_effects[(size_t)p.effect]->anchorGone)) continue;
		if (!updateParticle(p, g_frameTime)) continue;
		if (w != i) g_particles[w] = p;
		w++;
	}
	g_particles.resize(w);

	/* drop finished effects, remapping attached particles */
	std::vector<int> remap(g_effects.size(), -1);
	size_t ew = 0;
	for (size_t i = 0; i < g_effects.size(); i++)
	{
		if (keep[i]) { remap[i] = (int)ew; g_effects[ew++] = g_effects[i]; }
		else delete g_effects[i];
	}
	g_effects.resize(ew);
	w = 0;
	for (size_t i = 0; i < g_particles.size(); i++)
	{
		Particle &p = g_particles[i];
		if (p.effect >= 0)
		{
			p.effect = p.effect < (int)remap.size() ? remap[(size_t)p.effect] : -1;
			if (p.effect < 0) continue;
		}
		if (w != i) g_particles[w] = p;
		w++;
	}
	g_particles.resize(w);

	for (size_t i = 0; i < g_decals.size(); i++) drawParticle(g_decals[i], vright, vup, vieworg, out);
	for (size_t i = 0; i < g_particles.size(); i++) drawParticle(g_particles[i], vright, vup, vieworg, out);

	/* lights fade out over their lifetime */
	size_t lw = 0;
	for (size_t i = 0; i < g_timedLights.size(); i++)
	{
		TimedLight &t = g_timedLights[i];
		if (g_time >= t.die) continue;
		out.lights.push_back(t.l);
		g_timedLights[lw++] = t;
	}
	g_timedLights.resize(lw);

	out.sounds.swap(g_pendingSounds);
	g_pendingSounds.clear();
}

} // namespace sfx
