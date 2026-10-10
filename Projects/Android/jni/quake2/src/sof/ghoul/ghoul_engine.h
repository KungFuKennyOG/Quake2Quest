/*
 * ghoul_engine.h - engine-facing API of the clean-room GHOUL runtime.
 *
 * The runtime implements the IGhoul / IGhoulObj / IGhoulInst interfaces that the
 * Soldier of Fortune game module expects (declared in the SoF SDK header ighoul.h,
 * which is NOT part of this repository: point the build at your SDK copy).
 */
#ifndef SOF_GHOUL_ENGINE_H
#define SOF_GHOUL_ENGINE_H

#include <string>
#include <vector>

class IGhoul;
class IGhoulInst;

struct GhoulEngineImports
{
	/* load a whole file through the engine filesystem (pak aware); returns length or -1 */
	int  (*LoadFile)(const char *path, void **buffer);
	void (*FreeFile)(void *buffer);
	/* list files in a directory (e.g. "ghoul/enemy/meso") with an extension (e.g. ".ghb"),
	 * returning bare file names */
	void (*ListFiles)(const char *dir, const char *ext, std::vector<std::string> &out);
	void (*Printf)(const char *fmt, ...);
};

void    Ghoul_Init(const GhoulEngineImports &imp);
void    Ghoul_Shutdown(void);
IGhoul *Ghoul_Get(bool client, bool menu);

/* Name of the current map (e.g. "tsr1"); used to pick the per-level model packs (MESO_xxx.ghb). */
void    Ghoul_SetLevelName(const char *mapname);

/* ---- rendering support ---------------------------------------------------- */

struct GhoulDrawSurface
{
	std::vector<float> xyz;    /* 3 per vertex, in the instance's entity space (row-vector convention) */
	std::vector<float> normal; /* 3 per vertex */
	std::vector<float> st;     /* 2 per vertex */
	std::vector<unsigned short> indices;
	std::string skin;          /* skin/texture name (without extension), may be empty */
	std::string objectDir;     /* e.g. "enemy/meso" (where the skin textures live) */
	std::string part;          /* the model part the surface belongs to ("_R_HIGH_RES_HAND") */
	float tint[4];
};

/* Evaluate an instance (and everything bolted to it) at time t and append its visible
 * surfaces, transformed by the instance's own XForm and its bolt chain. */
void Ghoul_BuildDrawList(IGhoulInst *inst, float time, std::vector<GhoulDrawSurface> &out);

/* Name of the sequence an instance is playing ("idle_a"), or "" */
const char *Ghoul_PlayingSequenceName(IGhoulInst *inst);
/* The object's directory ("weapon/inview/sniperrifle"), or "" */
const char *Ghoul_ObjectDir(IGhoulInst *inst);
/* Hold an instance at the first frame of one of its sequences (at time 0) for measuring,
   then put it back exactly as it was. Not nestable. */
bool Ghoul_SetPose(IGhoulInst *inst, const char *seqName);
void Ghoul_RestorePose(IGhoulInst *inst);
/* Names of the object's sequences, in registration order */
void Ghoul_SequenceNames(IGhoulInst *inst, std::vector<std::string> &out);

/* ---- .gsq sequence lists (game_import_t entries) ------------------------- */
class IGhoulObj;
int  GSQ_FindFile(char *gsqdir, char *gsqfile, void **buffer);
bool GSQ_ReadEntry(int &filesize, char **tdata, char *seqname);
void GSQ_Precache(char *dirname, char *gsq_file, IGhoulObj *object);
int  GSQ_RegisterSequences(char *gsqdir, char *subclass, IGhoulObj *object);
void GSQ_TurnOffParts(char *dirname, char *poff_file, IGhoulObj *obj, IGhoulInst *inst);

/* Look up an instance by its UUID (local game: client and server share instances). */
IGhoulInst *Ghoul_FindInst(short uuid);

/* Called for every animation note an instance passes (token, e.g. "effect", and the note's
 * text); the adapter uses it to run the client-side parts of SoF (view weapon sounds and
 * muzzle effects) that the game leaves to SoF's own client. */
typedef void (*GhoulNoteHook)(IGhoulInst *inst, const char *token, const char *data);
void Ghoul_SetNoteHook(GhoulNoteHook hook);

#endif
