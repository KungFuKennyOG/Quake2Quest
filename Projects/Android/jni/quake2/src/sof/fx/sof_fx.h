/*
 * sof_fx.h - Soldier of Fortune client effects (.eft effect files): particles, sprites,
 * lines, sounds and lights, run on the client side and handed to the renderer as meshes.
 *
 * The .eft layout and the behaviour (emitter bursts, particle motion, size, colour and
 * alpha over time) follow SoF's FX_System. Portable C++11; the engine is reached through
 * the callbacks in FxHost.
 */
#ifndef SOF_FX_H
#define SOF_FX_H

#include <vector>
#include <string>

namespace sfx {

/* where an effect sits; resolved again every frame so attached effects follow their owner */
enum AnchorKind { ANCHOR_POS, ANCHOR_ENT, ANCHOR_BOLT, ANCHOR_VIEWWEAPON };

struct Anchor
{
	int kind;
	int ent;          /* entity number (ENT, BOLT) */
	int bolt;         /* GHOUL part/bolt id (BOLT, VIEWWEAPON) */
	void *inst;       /* GHOUL instance (BOLT with instance, VIEWWEAPON) */
	int uuid;         /* the instance's UUID, to notice when it is gone */
	bool altAxis;
	float pos[3];     /* POS */
	Anchor() : kind(ANCHOR_POS), ent(0), bolt(0), inst(0), uuid(0), altAxis(false) { pos[0] = pos[1] = pos[2] = 0; }
};

/* optional data sent with an effect (CFXSender flags) */
struct Params
{
	unsigned flags;   /* EFF_* bits below */
	float scale;
	int numElements;
	float pos2[3], dir[3], mins[3], maxs[3];
	float lifetime, radius;
	Params() : flags(0), scale(1), numElements(0), lifetime(0), radius(0)
	{
		for (int i = 0; i < 3; i++) pos2[i] = dir[i] = mins[i] = maxs[i] = 0;
	}
};
enum { EFF_SCALE = 1, EFF_NUMELEMS = 2, EFF_POS2 = 4, EFF_DIR = 8, EFF_MIN = 0x10, EFF_MAX = 0x20, EFF_LIFETIME = 0x40, EFF_RADIUS = 0x80 };

/* frame orientation of an anchor: origin and forward/right/up axes */
struct Frame
{
	float org[3], fwd[3], right[3], up[3];
	float scale;      /* size of the anchor's space (the scaled VR view weapon); 1 = world */
	Frame() : scale(1) {}
};

struct Host
{
	/* load a file from the game file system; returns length or -1, *buf freed with freeFile */
	int (*loadFile)(const char *path, void **buf);
	void (*freeFile)(void *buf);
	/* resolve an anchor to a frame; false = owner gone (the effect ends) */
	bool (*resolve)(const Anchor &a, Frame &out);
	/* "textures/sprites/flash1" -> a path the renderer can load ("...flash1.m32"); "" if none */
	const char *(*texturePath)(const char *name);
};

/* output of one frame */
struct Quad
{
	float xyz[4][3];
	float st[4][2];
	unsigned char rgba[4];
};
struct Batch
{
	std::string texture;
	int blend;        /* 0 alpha blend, 1 additive, 2 subtractive */
	bool noDepth;
	std::vector<float> xyz, st;
	std::vector<unsigned char> rgba;
	std::vector<unsigned short> indices;
};
struct Light { float org[3]; float radius; float rgb[3]; };
struct Sound { std::string name; float org[3]; int ent; float volume, attenuation; bool local; };

struct Output
{
	std::vector<Batch> batches;
	std::vector<Light> lights;
	std::vector<Sound> sounds;
};

void Init(const Host &host);
void Clear();                         /* level change: drop every effect and particle */
/* start an effect by name ("weapons/playermz/pistol2"); runs from the next Frame() */
void Start(const char *name, const Anchor &a, const Params &p);
/* play a sound the way a GHOUL "sound" note does ("/weapons/dpistol/spin") */
void NoteSound(const char *name, const Anchor &a);
/* advance to time (seconds) and build the output; view axes are for camera-facing sprites */
void Run(float time, const float vieworg[3], const float vright[3], const float vup[3], Output &out);

int NumParticles();

} // namespace sfx

#endif
