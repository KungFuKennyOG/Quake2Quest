/*
 * ghoul_runtime.cpp - clean-room implementation of the GHOUL runtime interfaces used by the
 * Soldier of Fortune game module (IGhoul, IGhoulObj, IGhoulInst from the SoF SDK's ighoul.h).
 *
 * Written from the interface declarations and from how the SDK game code uses them; the
 * behaviour of the original library is approximated where it is not observable from there.
 *
 * Model data comes from ghb_model.{h,cpp}.
 */
#include "ighoul.h"
#include "ghoul_engine.h"
#include "ghb_model.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strings.h>
#include <algorithm>
#include <map>
#include <ctype.h>
#include <set>

namespace {

GhoulEngineImports g_imp;
GhoulNoteHook g_noteHook;
bool g_inited = false;
std::string g_level;

void dprintf(const char *fmt, ...)
{
	if (!g_imp.Printf) return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_imp.Printf("%s", buf);
}

std::string lower(const std::string &s)
{
	std::string r(s);
	for (size_t i = 0; i < r.size(); i++)
		if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
		else if (r[i] == '\\') r[i] = '/';
	return r;
}

/* "ghoul/enemy/meso/std_istand_n_a_n.ghl" -> dir "ghoul/enemy/meso", base "std_istand_n_a_n" */
void splitPath(const char *path, std::string &dir, std::string &base)
{
	std::string p = lower(path);
	size_t sl = p.find_last_of('/');
	dir = sl == std::string::npos ? std::string() : p.substr(0, sl);
	base = sl == std::string::npos ? p : p.substr(sl + 1);
	size_t dot = base.find_last_of('.');
	if (dot != std::string::npos) base = base.substr(0, dot);
}

/* Per-level model packs are named <class>_<tag>.ghb (MESO_TUT1, MESO_CAS2, MESO_PLAYER...).
 * Score how well a pack tag fits the current map name. */
int levelScore(const std::string &tag)
{
	if (g_level.empty()) return tag == "player" ? 1 : 0;
	std::string lv = g_level;
	size_t sl = lv.find_last_of('/');
	if (sl != std::string::npos) lv = lv.substr(sl + 1);
	if (g_level.compare(0, 3, "dm/") == 0) return tag == "player" ? 3 : 0;
	static const char *alias[][2] = { { "ger", "cas" }, { "jpn", "tok" }, { "trn", "train" }, { 0, 0 } };
	for (int i = 0; alias[i][0]; i++)
		if (lv.compare(0, strlen(alias[i][0]), alias[i][0]) == 0)
		{
			std::string rest = lv.substr(strlen(alias[i][0]));
			lv = std::string(alias[i][1]) + (std::string(alias[i][1]) == "train" ? std::string() : rest);
			break;
		}
	if (tag == lv) return 3;
	/* arm1 -> arm, nyc1 -> nyc, tsr1 -> tsr2 style near matches */
	std::string a = lv, b = tag;
	while (!a.empty() && (isdigit((unsigned char)a[a.size() - 1]) || (a.size() > 3 && isalpha((unsigned char)a[a.size() - 1]) && isdigit((unsigned char)a[a.size() - 2])))) a.erase(a.size() - 1);
	while (!b.empty() && (isdigit((unsigned char)b[b.size() - 1]) || (b.size() > 3 && isalpha((unsigned char)b[b.size() - 1]) && isdigit((unsigned char)b[b.size() - 2])))) b.erase(b.size() - 1);
	if (!a.empty() && a == b) return 2;
	if (tag.find("menu") != std::string::npos) return -2;
	return 0;
}

/* ---------------------------------------------------------------- file cache */

struct FileEntry
{
	ghb::Model model;
	bool ok;
	std::vector<std::string> seqNames; /* lower case */
};

std::map<std::string, FileEntry *> g_files;        /* key: lower-case path */
std::map<std::string, std::vector<std::string> > g_dirIndex; /* dir -> .ghb paths */

FileEntry *loadFile(const std::string &path)
{
	std::map<std::string, FileEntry *>::iterator it = g_files.find(path);
	if (it != g_files.end()) return it->second;
	FileEntry *e = new FileEntry;
	e->ok = false;
	void *buf = 0;
	int len = g_imp.LoadFile ? g_imp.LoadFile(path.c_str(), &buf) : -1;
	if (len > 0 && buf)
	{
		std::string err;
		e->ok = ghb::Load((const uint8_t *)buf, (size_t)len, e->model, err);
		if (!e->ok) dprintf("GHOUL: %s: %s\n", path.c_str(), err.c_str());
		g_imp.FreeFile(buf);
	}
	if (e->ok)
		for (size_t i = 0; i < e->model.sequences.size(); i++)
			e->seqNames.push_back(lower(e->model.sequences[i].name));
	g_files[path] = e;
	return e;
}

const std::vector<std::string> &dirPacks(const std::string &dir)
{
	std::map<std::string, std::vector<std::string> >::iterator it = g_dirIndex.find(dir);
	if (it != g_dirIndex.end()) return it->second;
	std::vector<std::string> names, paths;
	if (g_imp.ListFiles) g_imp.ListFiles(dir.c_str(), ".ghb", names);
	for (size_t i = 0; i < names.size(); i++)
		paths.push_back(dir + "/" + lower(names[i]));
	std::sort(paths.begin(), paths.end());
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	return g_dirIndex[dir] = paths;
}

int seqIndexIn(const FileEntry *e, const std::string &base)
{
	for (size_t i = 0; i < e->seqNames.size(); i++)
		if (e->seqNames[i] == base) return (int)i;
	return -1;
}

/* ---------------------------------------------------------------- matrices */

void toM4(Matrix4 &d, const ghb::Mat4 &s)
{
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			d.SetElem(i, j, s.m[i][j]);
	d.CalcFlags();
}

void fromM4(ghb::Mat4 &d, const Matrix4 &s)
{
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			d.m[i][j] = s.Elem(i, j);
	d.flags = 0;
}

class GObj;
class GInst;
class GGhoul;

std::vector<GInst *> g_instList; /* for UUID lookup */
short g_nextUUID = 1;

struct SeqRef
{
	std::string dir, base; /* registered name */
	FileEntry *file;
	int index;             /* sequence index in file */
};

struct SkinRef
{
	GhoulID mat;
	std::string name;
	TexChannel channel;
};

/* ---------------------------------------------------------------- object */

class GObj : public IGhoulObj
{
public:
	GGhoul *owner;
	GhoulUUID uuid;
	std::vector<SeqRef> seqs;          /* GhoulID = index + 1 */
	std::vector<std::string> partNames; /* lower case; GhoulID = index + 1 */
	std::vector<bool> partIsMesh;
	std::vector<std::string> tokens;    /* lower case; GhoulID = index + 1 */
	std::vector<std::string> materials; /* lower case; GhoulID = index + 1 */
	std::vector<SkinRef> skins;         /* GhoulID = index + 1 */
	bool resolved;
	std::string objectDir;              /* e.g. "enemy/meso" */

	GObj(GGhoul *g) : owner(g), resolved(false)
	{
		uuid = g_nextUUID++;
		tokens.push_back("bos");
		tokens.push_back("eos");
	}

	static GhoulID findIn(const std::vector<std::string> &v, const char *name)
	{
		std::string n = lower(name ? name : "");
		for (size_t i = 0; i < v.size(); i++)
			if (v[i] == n) return (GhoulID)(i + 1);
		return 0;
	}
	static GhoulID addTo(std::vector<std::string> &v, const char *name)
	{
		GhoulID id = findIn(v, name);
		if (id) return id;
		v.push_back(lower(name ? name : ""));
		return (GhoulID)v.size();
	}

	/* Bind every registered sequence to a file. Packs (several sequences per file) are chosen
	 * per directory: the pack that contains most of the requested sequences wins, preferring
	 * one whose name contains the current level name. */
	void resolve()
	{
		if (resolved) return;
		resolved = true;
		std::map<std::string, std::vector<size_t> > byDir;
		for (size_t i = 0; i < seqs.size(); i++)
		{
			SeqRef &s = seqs[i];
			if (s.file) continue;
			FileEntry *e = loadFile(s.dir + "/" + s.base + ".ghb");
			if (e->ok)
			{
				s.file = e;
				s.index = seqIndexIn(e, s.base);
				if (s.index < 0) s.index = 0;
				continue;
			}
			byDir[s.dir].push_back(i);
		}
		for (std::map<std::string, std::vector<size_t> >::iterator it = byDir.begin(); it != byDir.end(); ++it)
		{
			const std::vector<std::string> &packs = dirPacks(it->first);
			FileEntry *best = 0;
			int bestScore = -1;
			for (size_t p = 0; p < packs.size(); p++)
			{
				FileEntry *e = loadFile(packs[p]);
				if (!e->ok) continue;
				int score = 0;
				for (size_t k = 0; k < it->second.size(); k++)
					if (seqIndexIn(e, seqs[it->second[k]].base) >= 0) score++;
				if (!score) continue;
				std::string fname = packs[p].substr(packs[p].find_last_of('/') + 1);
				fname = fname.substr(0, fname.find_last_of('.'));
				size_t us = fname.find('_');
				std::string tag = us == std::string::npos ? fname : fname.substr(us + 1);
				/* the pack made for this level wins whenever it has any of the sequences;
				 * otherwise the pack with the most sequences */
				score += levelScore(tag) * 100000;
				if (score > bestScore) { bestScore = score; best = e; }
			}
			int missing = 0;
			for (size_t k = 0; k < it->second.size(); k++)
			{
				SeqRef &s = seqs[it->second[k]];
				if (best && (s.index = seqIndexIn(best, s.base)) >= 0)
					s.file = best;
				else
				{
					s.file = 0;
					s.index = -1;
					if (missing++ < 2) dprintf("GHOUL: missing sequence %s/%s\n", s.dir.c_str(), s.base.c_str());
				}
			}
			if (missing)
				dprintf("GHOUL: %s: %d of %d sequences not in the level pack%s%s\n", it->first.c_str(), missing,
				        (int)it->second.size(), best ? " " : "", best ? best->model.source.c_str() : "");
		}
		/* register names from the bound files */
		std::set<FileEntry *> done;
		for (size_t i = 0; i < seqs.size(); i++)
		{
			FileEntry *e = seqs[i].file;
			if (!e || done.count(e)) continue;
			done.insert(e);
			const ghb::Model &m = e->model;
			for (size_t b = 0; b < m.bolts.size(); b++)
				if (!findIn(partNames, m.bolts[b].name.c_str()))
				{ partNames.push_back(lower(m.bolts[b].name)); partIsMesh.push_back(false); }
			for (size_t p = 0; p < m.parts.size(); p++)
				if (!findIn(partNames, m.parts[p].name.c_str()))
				{ partNames.push_back(lower(m.parts[p].name)); partIsMesh.push_back(true); }
			for (size_t k = 0; k < m.materials.size(); k++)
				addTo(materials, m.materials[k].name.c_str());
			for (size_t q = 0; q < m.sequences.size(); q++)
				for (size_t n = 0; n < m.sequences[q].notes.size(); n++)
					addTo(tokens, m.sequences[q].notes[n].token.c_str());
		}
		if (objectDir.empty() && !seqs.empty())
		{
			objectDir = seqs[0].dir;
			if (objectDir.compare(0, 6, "ghoul/") == 0) objectDir = objectDir.substr(6);
		}
	}

	const SeqRef *seq(GhoulID id)
	{
		resolve();
		if (!id || id > seqs.size()) return 0;
		const SeqRef *s = &seqs[id - 1];
		return s->file ? s : 0;
	}

	/* IGhoulObj */
	GhoulID RegisterSequence(const char *Filename, bool)
	{
		std::string dir, base;
		splitPath(Filename, dir, base);
		static const char *trace = getenv("GHOUL_TRACE");
		if (trace && strstr(Filename, trace))
			dprintf("GHOUL trace: RegisterSequence(%s) from %p\n", Filename, __builtin_return_address(0));
		for (size_t i = 0; i < seqs.size(); i++)
			if (seqs[i].dir == dir && seqs[i].base == base)
				return (GhoulID)(i + 1);
		SeqRef s;
		s.dir = dir; s.base = base; s.file = 0; s.index = -1;
		seqs.push_back(s);
		resolved = false;
		return (GhoulID)seqs.size();
	}
	void RegisterEverything(bool Skins)
	{
		resolve();
		if (!Skins) return;
		/* all skins listed in <material>.ifl (object directory, then ghoul/comskin) */
		for (size_t mi = 0; mi < materials.size(); mi++)
		{
			std::vector<std::string> names;
			readIfl("ghoul/" + objectDir + "/" + materials[mi] + ".ifl", names);
			readIfl("ghoul/comskin/" + materials[mi] + ".ifl", names);
			for (size_t k = 0; k < names.size(); k++)
				RegisterSkin((GhoulID)(mi + 1), names[k].c_str(), Diffuse);
		}
	}
	static void readIfl(const std::string &path, std::vector<std::string> &out)
	{
		void *buf = 0;
		int len = g_imp.LoadFile ? g_imp.LoadFile(path.c_str(), &buf) : -1;
		if (len <= 0 || !buf) return;
		std::string t((const char *)buf, (size_t)len);
		g_imp.FreeFile(buf);
		size_t p = 0;
		while (p < t.size())
		{
			size_t e = t.find_first_of("\r\n", p);
			if (e == std::string::npos) e = t.size();
			std::string line = t.substr(p, e - p);
			p = e + 1;
			while (!line.empty() && isspace((unsigned char)line[line.size() - 1])) line.erase(line.size() - 1);
			while (!line.empty() && isspace((unsigned char)line[0])) line.erase(0, 1);
			if (line.empty()) continue;
			size_t dot = line.find_last_of('.');
			if (dot != std::string::npos) line = line.substr(0, dot);
			out.push_back(lower(line));
		}
	}
	void PreCache(bool) { resolve(); }
	GhoulID FindSequence(const char *Filename)
	{
		std::string dir, base;
		splitPath(Filename, dir, base);
		for (size_t i = 0; i < seqs.size(); i++)
			if (seqs[i].base == base && (dir.empty() || seqs[i].dir == dir))
				return seq((GhoulID)(i + 1)) ? (GhoulID)(i + 1) : 0;
		return 0;
	}
	GhoulID NumSequences() const { return (GhoulID)seqs.size(); }
	void GetSequenceName(GhoulID who, char *dest) const
	{
		if (!who || who > seqs.size()) { dest[0] = 0; return; }
		snprintf(dest, 256, "%s/%s.ghl", seqs[who - 1].dir.c_str(), seqs[who - 1].base.c_str());
	}
	GhoulID RegisterPart(const char *PartName) { resolve(); return FindPart(PartName); }
	GhoulID FindPart(const char *PartName) { resolve(); return findIn(partNames, PartName); }
	GhoulID NumParts() const { return (GhoulID)partNames.size(); }
	PartType GetPartType(GhoulID who) const
	{
		if (!who || who > partNames.size()) return ptDummy;
		return partIsMesh[who - 1] ? ptMesh : ptDummy;
	}
	void GetPartName(GhoulID who, char *dest) const
	{
		if (!who || who > partNames.size()) { dest[0] = 0; return; }
		strcpy(dest, partNames[who - 1].c_str());
	}
	GhoulID RegisterNoteToken(const char *Token) { resolve(); return addTo(tokens, Token); }
	GhoulID FindNoteToken(const char *Token) { resolve(); return findIn(tokens, Token); }
	GhoulID NumNoteTokens() const { return (GhoulID)tokens.size(); }
	void FireAllNoteCallBacks(IGhoulCallBack *, GhoulID) {}
	GhoulID RegisterMaterial(const char *Mat) { resolve(); return addTo(materials, Mat); }
	GhoulID FindMaterial(const char *Mat) { resolve(); return findIn(materials, Mat); }
	GhoulID NumMaterials() const { return (GhoulID)materials.size(); }
	void GetMaterialName(GhoulID who, char *dest) const
	{
		if (!who || who > materials.size()) { dest[0] = 0; return; }
		strcpy(dest, materials[who - 1].c_str());
	}
	GhoulID RegisterSkin(GhoulID Mat, const char *Skin, TexChannel Channel)
	{
		GhoulID id = FindSkin(Mat, Skin, Channel);
		if (id) return id;
		if (!Mat || Mat > materials.size() || !Skin) return 0;
		SkinRef s;
		s.mat = Mat; s.name = lower(Skin); s.channel = Channel;
		skins.push_back(s);
		return (GhoulID)skins.size();
	}
	GhoulID FindSkin(GhoulID Mat, const char *Skin, TexChannel Channel)
	{
		if (!Skin) return 0;
		std::string n = lower(Skin);
		for (size_t i = 0; i < skins.size(); i++)
			if (skins[i].mat == Mat && skins[i].name == n && skins[i].channel == Channel)
				return (GhoulID)(i + 1);
		return 0;
	}
	GhoulID NumSkins() const { return (GhoulID)skins.size(); }
	void GetSkinName(GhoulID who, char *dest) const
	{
		if (!who || who > skins.size()) { dest[0] = 0; return; }
		strcpy(dest, skins[who - 1].name.c_str());
	}
	GhoulID GetSkinMaterial(GhoulID who) const
	{
		if (!who || who > skins.size()) return 0;
		return skins[who - 1].mat;
	}
	IGhoulInst *NewInst();
	GhoulUUID MyUUID() const { return uuid; }
	IGhoul *GetMyGhoul() const;
	void Destroy();
};

/* ---------------------------------------------------------------- instance */

struct NoteCB { IGhoulCallBack *cb; GhoulID token; };
struct MatCB { IGhoulCallBack *cb; GhoulID part; IGhoulInst::MatrixType kind; };
struct Child { GInst *inst; GhoulID bolt; };

class GInst : public IGhoulInst
{
public:
	GObj *obj;
	GhoulUUID uuid;
	Matrix4 xform;
	bool on;
	GhoulSpeed speed;
	std::vector<bool> partOff;
	std::map<std::pair<int, int>, GhoulID> overrides; /* (mat, part) -> skin */
	float tint[4];
	/* playback */
	GhoulID seqId;
	float startTime, pauseTime, holdPos;
	EndCondition ec;
	bool paused, reverse;
	float lastUpdate;
	bool callbacksOn, matCallbacksOn;
	void *user;
	std::vector<NoteCB> notes;
	std::vector<MatCB> matcbs;
	/* bolting */
	GInst *parent;
	GhoulID myBolt;      /* bolt on this instance attached to the parent */
	GhoulID parentBolt;  /* bolt on the parent */
	std::vector<Child> children;

	GInst(GObj *o) : obj(o), on(true), speed(gsOne), seqId(0), startTime(0), pauseTime(0), holdPos(0),
		ec(Loop), paused(false), reverse(false), lastUpdate(-1), callbacksOn(true), matCallbacksOn(true),
		user(0), parent(0), myBolt(0), parentBolt(0)
	{
		uuid = g_nextUUID++;
		xform.Identity();
		tint[0] = tint[1] = tint[2] = tint[3] = 1.0f;
		g_instList.push_back(this);
	}
	~GInst()
	{
		g_instList.erase(std::remove(g_instList.begin(), g_instList.end(), this), g_instList.end());
	}

	float speedFactor() const { return (float)(4 + (int)speed) / 8.0f; }

	/* Current sequence, file and fractional frame (in file frame numbers) at time t. */
	bool frameAt(float t, const SeqRef **sr, float *fileFrame, float *seqFrame = 0, bool *ended = 0)
	{
		const SeqRef *s = obj->seq(seqId);
		if (!s)
		{
			/* no sequence playing: show the first sequence's first frame */
			for (GhoulID i = 1; i <= obj->seqs.size() && !s; i++) s = obj->seq(i);
			if (!s) return false;
			*sr = s;
			*fileFrame = (float)s->file->model.sequences[(size_t)s->index].firstFrame;
			if (seqFrame) *seqFrame = 0;
			if (ended) *ended = false;
			return true;
		}
		const ghb::Sequence &q = s->file->model.sequences[(size_t)s->index];
		float spf = (q.msPerFrame > 0 ? q.msPerFrame : 100.0f) / 1000.0f;
		int n = q.numFrames > 0 ? q.numFrames : 1;
		float tt = paused ? pauseTime : t;
		float f;
		bool end = false;
		if (ec == HoldFrame)
			f = holdPos / spf;
		else
			f = (tt - startTime) * speedFactor() / spf;
		if (f < 0) f = 0;
		float last = (float)(n - 1);
		switch (ec)
		{
		case Loop:
			if (n > 1) f = fmodf(f, (float)n); else f = 0;
			if (f > last) f = last; /* last->first blend is not interpolated */
			break;
		case BackAndForth:
			if (n > 1)
			{
				float per = 2.0f * last;
				f = fmodf(f, per);
				if (f > last) f = per - f;
			}
			else f = 0;
			break;
		default:
			if (f >= last) { f = last; end = true; }
			break;
		}
		if (reverse) f = last - f;
		*sr = s;
		*fileFrame = (float)q.firstFrame + f;
		if (seqFrame) *seqFrame = f;
		if (ended) *ended = end;
		return true;
	}

	/* Local (model space) matrix of a part/bolt at time t. */
	bool partMatrix(float t, GhoulID part, ghb::Mat4 &out)
	{
		out.Identity();
		if (!part) return true;
		const SeqRef *s;
		float ff;
		if (!frameAt(t, &s, &ff)) return false;
		return partMatrixAt(s, ff, part, out);
	}
	/* the same at a given file frame of a sequence */
	bool partMatrixAt(const SeqRef *s, float ff, GhoulID part, ghb::Mat4 &out)
	{
		out.Identity();
		if (!part) return true;
		const ghb::Model &m = s->file->model;
		if (part > obj->partNames.size()) return false;
		const char *name = obj->partNames[part - 1].c_str();
		int b = m.FindBolt(name);
		if (b >= 0) { ghb::NodeMatrix(m, m.bolts[(size_t)b], ff, out); return true; }
		int p = m.FindPart(name);
		if (p >= 0) { ghb::NodeMatrix(m, m.parts[(size_t)p], ff, out); return true; }
		return false;
	}

	/* Transform from this instance's model space to the root instance's model space
	 * (before the root's own XForm) - includes our XForm. */
	void toRoot(float t, ghb::Mat4 &out)
	{
		ghb::Mat4 x;
		fromM4(x, xform);
		if (!parent) { out = x; return; }
		/* child model space -> (inverse of our bolt) -> parent bolt space -> parent model space */
		ghb::Mat4 mine, inv, pb, chain, tmp;
		partMatrix(t, myBolt, mine);
		inv.OrthoInverse(mine);
		parent->partMatrix(t, parentBolt, pb);
		ghb::Mat4 ptoRoot;
		parent->toRoot(t, ptoRoot);
		/* our XForm applies in bolt space, after moving onto our own bolt: the game scales
		   bolt-ons (hats, glasses on the bigger "meso" bodies) and expects them to grow
		   around the attachment point rather than around their model origin */
		tmp.Mul(inv, x);
		chain.Mul(tmp, pb);
		out.Mul(chain, ptoRoot);
	}

	/* Jacobian matrices: how a part moves per second at time t. GHOUL evaluates the part
	 * at the current point of the sequence and 0.001 of the sequence later (earlier at the
	 * very end), takes the change B * inverse(A) (in the part's own frame), puts it in
	 * entity space with the instance XForm and scales it by 1 / (0.001 * sequence length
	 * in seconds). The AI walks monsters with the translation row of this (root motion)
	 * and turns them with its first row. */
	bool jacobian(float t, ghb::Mat4 &out, GhoulID part, bool entity, bool ToRoot)
	{
		out.Identity();
		const SeqRef *s = on ? obj->seq(seqId) : 0;
		if (!s) return false;
		const ghb::Sequence &q = s->file->model.sequences[(size_t)s->index];
		int n = q.numFrames > 0 ? q.numFrames : 1;
		float spf = (q.msPerFrame > 0 ? q.msPerFrame : 100.0f) / 1000.0f / speedFactor();
		float tt = paused ? pauseTime : t;
		bool loop = ec == Loop, rev = reverse;
		float dur, f;
		if (ec == HoldFrame)
		{
			dur = spf * (float)(n > 1 ? n - 1 : 1);
			f = n > 1 ? holdPos / (q.msPerFrame > 0 ? q.msPerFrame / 1000.0f : 0.1f) / (float)(n - 1) : 0;
		}
		else if (loop)
		{
			dur = spf * (float)n;
			float x = (tt - startTime) / dur;
			f = x - floorf(x);
		}
		else if (ec == BackAndForth)
		{
			dur = 2.0f * spf * (float)(n > 1 ? n - 1 : 1);
			float x = (tt - startTime) / dur;
			x -= floorf(x);
			if (x >= 0.5f) { rev = !rev; f = 1.0f - (1.0f - x) * 2.0f; }
			else f = x + x;
		}
		else
		{
			dur = spf * (float)(n > 1 ? n - 1 : 1);
			f = (tt - startTime) / dur;
		}
		if (rev) f = 1.0f - f;
		if (f > 1) f = 1;
		if (f < 0) f = 0;
		if (dur <= 0) return false;

		const float eps = 0.001f;
		bool back = false;
		if (loop)
		{
			float lastF = (float)(n - 1) / (float)n - eps;
			if (lastF < f) { back = true; f = lastF; }
		}
		else if (1.0f < f + eps)
			back = true;
		/* sequence fraction -> file frame (loops run over n frames, the rest over n - 1) */
		float span = loop ? (float)n : (float)(n > 1 ? n - 1 : 0);
		float fa = (float)q.firstFrame + f * span;
		float fb = (float)q.firstFrame + (back ? f - eps : f + eps) * span;
		ghb::Mat4 a, b, ai, bi, d;
		if (!partMatrixAt(s, fa, part, a) || !partMatrixAt(s, fb, part, b)) return false;
		if (back) { bi.OrthoInverse(b); d.Mul(a, bi); }
		else { ai.OrthoInverse(a); d.Mul(b, ai); }
		ghb::Mat4 r = d;
		if (entity)
		{
			ghb::Mat4 x;
			if (ToRoot) toRoot(t, x);
			else fromM4(x, xform);
			r.Mul(d, x);
		}
		float sc = 1.0f / (dur * eps);
		for (int i = 0; i < 4; i++)
			for (int k = 0; k < 3; k++) r.m[i][k] *= sc;
		r.flags = 0; /* not an identity any more */
		out = r;
		return true;
	}

	void computeMatrix(float t, ghb::Mat4 &out, GhoulID part, MatrixType kind, bool ToRoot)
	{
		int jk = (int)kind;
		if (jk == JacobianLocal || jk == JacobianEntity || jk == JacobianLocalInv || jk == JacobianEntityInv)
		{
			/* the game only asks for JacobianEntity; the inverse kinds are not used */
			jacobian(t, out, part, jk == JacobianEntity || jk == JacobianEntityInv, ToRoot);
			return;
		}
		ghb::Mat4 local;
		partMatrix(t, part, local);
		int k = (int)kind;
		bool entity = (k == JacobianEntity || k == JacobianEntityInv || k == Entity || k == EntityInv);
		bool inv = (k == JacobianLocalInv || k == JacobianEntityInv || k == LocalInv || k == EntityInv);
		ghb::Mat4 r = local;
		if (entity)
		{
			ghb::Mat4 x;
			if (ToRoot) toRoot(t, x);
			else fromM4(x, xform);
			r.Mul(local, x);
		}
		else if (ToRoot && parent)
		{
			ghb::Mat4 x, xi, xr;
			toRoot(t, xr);
			fromM4(x, xform);
			xi.OrthoInverse(x);
			ghb::Mat4 tmp;
			tmp.Mul(xi, xr); /* parent chain without our own XForm */
			r.Mul(local, tmp);
		}
		if (inv) { ghb::Mat4 i; i.OrthoInverse(r); r = i; }
		out = r;
	}

	/* ---- IGhoulInst ---- */
	void SetXForm(const float *m) { xform.SetFromMem(m); }
	void GetXForm(float *m) { xform.GetFromMem(m); }
	void SetXForm(const Matrix4 &m) { xform = m; }
	void GetXForm(Matrix4 &m) { m = xform; }
	void SetOnOff(bool OnOff, float) { on = OnOff; }
	bool GetOnOff() { return on; }
	void SetSpeed(GhoulSpeed s) { speed = s; }
	GhoulSpeed GetSpeed() { return speed; }
	void SetPartOnOff(GhoulID Part, bool OnOff)
	{
		if (!Part) return;
		if (partOff.size() < Part) partOff.resize(Part, false);
		partOff[Part - 1] = !OnOff;
	}
	bool GetPartOnOff(GhoulID Part)
	{
		if (!Part || Part > partOff.size()) return true;
		return !partOff[Part - 1];
	}
	void SetAllPartsOnOff(bool OnOff)
	{
		partOff.assign(obj->partNames.size(), !OnOff);
	}
	void SetFrameOverride(GhoulID Mat, GhoulID Skin, GhoulID Part) { overrides[std::make_pair((int)Mat, (int)Part)] = Skin; }
	GhoulID GetFrameOverride(GhoulID Mat, GhoulID Part, TexChannel)
	{
		std::map<std::pair<int, int>, GhoulID>::iterator it = overrides.find(std::make_pair((int)Mat, (int)Part));
		return it == overrides.end() ? 0 : it->second;
	}
	void ClearFrameOverride(GhoulID Mat, GhoulID Part, TexChannel) { overrides.erase(std::make_pair((int)Mat, (int)Part)); }
	void SetTint(float r, float g, float b, float a) { tint[0] = r; tint[1] = g; tint[2] = b; tint[3] = a; }
	void GetTint(float *r, float *g, float *b, float *a) { *r = tint[0]; *g = tint[1]; *b = tint[2]; *a = tint[3]; }
	void SetTintOnAll(float r, float g, float b, float a)
	{
		SetTint(r, g, b, a);
		for (size_t i = 0; i < children.size(); i++) children[i].inst->SetTintOnAll(r, g, b, a);
	}

	void Play(GhoulID Seq, float Now, float PlayPos, bool Restart, EndCondition e, bool MatchCurrentPos, bool reverseAnim)
	{
		if (!Restart && Seq == seqId && !paused) { ec = e; return; }
		float cur = 0;
		if (MatchCurrentPos && seqId)
		{
			const SeqRef *s; float ff, sf;
			if (frameAt(Now, &s, &ff, &sf))
			{
				const ghb::Sequence &q = s->file->model.sequences[(size_t)s->index];
				cur = sf * (q.msPerFrame > 0 ? q.msPerFrame : 100.0f) / 1000.0f;
			}
		}
		static const char *notelog = getenv("SOF_GHOUL_NOTELOG");
		if (notelog)
			dprintf("[ghoul play] %s t=%.2f seq %s -> %s\n", obj->objectDir.c_str(), Now,
			        seqId && seqId <= obj->seqs.size() ? obj->seqs[seqId - 1].base.c_str() : "-",
			        Seq && Seq <= obj->seqs.size() ? obj->seqs[Seq - 1].base.c_str() : "-");
		seqId = Seq;
		ec = e;
		reverse = reverseAnim;
		paused = false;
		holdPos = PlayPos;
		float pos = MatchCurrentPos ? cur : PlayPos;
		startTime = Now - pos / speedFactor();
		lastUpdate = Now - 0.0001f; /* so that a BOS note fires on the next update */
		fireToken(obj->FindNoteToken("bos"), Now);
	}
	GhoulID GetPlayingSequence() { return seqId; }
	void Pause(float Now) { if (!paused) { paused = true; pauseTime = Now; } }
	void Resume(float Now) { if (paused) { paused = false; startTime += Now - pauseTime; } }
	void SetUserData(void *u) { user = u; }
	void *GetUserData() { return user; }
	void AddNoteCallBack(IGhoulCallBack *c, GhoulID Token)
	{
		NoteCB n; n.cb = c; n.token = Token;
		notes.push_back(n);
	}
	void RemoveNoteCallBack(IGhoulCallBack *c, GhoulID Token)
	{
		for (size_t i = 0; i < notes.size(); )
			if (notes[i].cb == c && (!Token || notes[i].token == Token)) notes.erase(notes.begin() + (long)i);
			else i++;
	}
	void AddMatrixCallBack(IGhoulCallBack *c, GhoulID Part, MatrixType kind)
	{
		MatCB m; m.cb = c; m.part = Part; m.kind = kind;
		matcbs.push_back(m);
	}
	void RemoveMatrixCallBack(IGhoulCallBack *c, GhoulID Part, MatrixType kind)
	{
		for (size_t i = 0; i < matcbs.size(); )
			if (matcbs[i].cb == c && matcbs[i].part == Part && matcbs[i].kind == kind) matcbs.erase(matcbs.begin() + (long)i);
			else i++;
	}
	float GetSequenceLength(GhoulID Seq, EndCondition, float *SecsPerFrame)
	{
		const SeqRef *s = obj->seq(Seq);
		if (!s) { if (SecsPerFrame) *SecsPerFrame = 0.1f; return 0; }
		const ghb::Sequence &q = s->file->model.sequences[(size_t)s->index];
		float spf = (q.msPerFrame > 0 ? q.msPerFrame : 100.0f) / 1000.0f / speedFactor();
		if (SecsPerFrame) *SecsPerFrame = spf;
		return spf * (float)(q.numFrames > 1 ? q.numFrames - 1 : 1);
	}

	void Bolt(GhoulID B, IGhoulInst *Bolted, GhoulID BoltedBolt)
	{
		GInst *c = static_cast<GInst *>(Bolted);
		if (!c || c == this) return;
		if (c->parent) c->parent->UnBolt(c);
		c->parent = this;
		c->parentBolt = B;
		c->myBolt = BoltedBolt;
		Child ch; ch.inst = c; ch.bolt = B;
		children.push_back(ch);
	}
	void SetMyBolt(GhoulID B) { myBolt = B; }
	GhoulID GetMyBolt() const { return myBolt; }
	void UnBolt(IGhoulInst *Bolted)
	{
		for (size_t i = 0; i < children.size(); i++)
			if (children[i].inst == Bolted)
			{
				children[i].inst->parent = 0;
				children.erase(children.begin() + (long)i);
				return;
			}
	}
	int GetNumChildren() const { return (int)children.size(); }
	IGhoulInst *GetChild(int c, GhoulID &bolt) const
	{
		if (c < 0 || c >= (int)children.size()) { bolt = 0; return 0; }
		bolt = children[(size_t)c].bolt;
		return children[(size_t)c].inst;
	}
	IGhoulObj *GetGhoulObject() const { return obj; }
	IGhoulInst *GetParent() const { return parent; }
	IGhoulInst *Clone(bool CloneChildren) const
	{
		GInst *n = new GInst(obj);
		GhoulUUID keep = n->uuid;
		n->xform = xform; n->on = on; n->speed = speed; n->partOff = partOff; n->overrides = overrides;
		memcpy(n->tint, tint, sizeof(tint));
		n->seqId = seqId; n->startTime = startTime; n->pauseTime = pauseTime; n->holdPos = holdPos;
		n->ec = ec; n->paused = paused; n->reverse = reverse; n->lastUpdate = lastUpdate;
		n->user = user; n->myBolt = myBolt;
		n->uuid = keep;
		if (CloneChildren)
			for (size_t i = 0; i < children.size(); i++)
			{
				GInst *cc = static_cast<GInst *>(children[i].inst->Clone(true));
				n->Bolt(children[i].bolt, cc, children[i].inst->myBolt);
			}
		return n;
	}

	/* Save state: a flat record of the playback state (callbacks by index). */
	int SaveState(unsigned char *buffer, int size, IGhoulCallBack **CallBacks, int NumCallBacks, const char *) const
	{
		std::vector<unsigned char> d;
		putv(d, (int)seqId); putv(d, startTime); putv(d, pauseTime); putv(d, holdPos); putv(d, (int)ec);
		putv(d, (int)paused); putv(d, (int)reverse); putv(d, (int)on); putv(d, (int)speed);
		putv(d, tint[0]); putv(d, tint[1]); putv(d, tint[2]); putv(d, tint[3]);
		float m[16]; xform.GetFromMem(m);
		for (int i = 0; i < 16; i++) putv(d, m[i]);
		putv(d, (int)partOff.size());
		for (size_t i = 0; i < partOff.size(); i++) putv(d, (int)partOff[i]);
		putv(d, (int)overrides.size());
		for (std::map<std::pair<int, int>, GhoulID>::const_iterator it = overrides.begin(); it != overrides.end(); ++it)
		{ putv(d, it->first.first); putv(d, it->first.second); putv(d, (int)it->second); }
		putv(d, (int)notes.size());
		for (size_t i = 0; i < notes.size(); i++) { putv(d, cbIndex(notes[i].cb, CallBacks, NumCallBacks)); putv(d, (int)notes[i].token); }
		putv(d, (int)matcbs.size());
		for (size_t i = 0; i < matcbs.size(); i++)
		{ putv(d, cbIndex(matcbs[i].cb, CallBacks, NumCallBacks)); putv(d, (int)matcbs[i].part); putv(d, (int)matcbs[i].kind); }
		if ((int)d.size() > size) return 0;
		memcpy(buffer, &d[0], d.size());
		return (int)d.size();
	}
	GhoulUUID RestoreState(unsigned char *buffer, int size, IGhoulCallBack **CallBacks, int NumCallBacks, const char *)
	{
		const unsigned char *p = buffer, *e = buffer + size;
		int iv; float fv;
		getv(p, e, iv); seqId = (GhoulID)iv; getv(p, e, startTime); getv(p, e, pauseTime); getv(p, e, holdPos);
		getv(p, e, iv); ec = (EndCondition)iv; getv(p, e, iv); paused = iv != 0; getv(p, e, iv); reverse = iv != 0;
		getv(p, e, iv); on = iv != 0; getv(p, e, iv); speed = (GhoulSpeed)iv;
		for (int i = 0; i < 4; i++) getv(p, e, tint[i]);
		float m[16];
		for (int i = 0; i < 16; i++) getv(p, e, m[i]);
		xform.SetFromMem(m);
		int n = 0;
		getv(p, e, n); partOff.assign((size_t)(n > 0 && n < 65536 ? n : 0), false);
		for (size_t i = 0; i < partOff.size(); i++) { getv(p, e, iv); partOff[i] = iv != 0; }
		getv(p, e, n); overrides.clear();
		for (int i = 0; i < n && i < 65536; i++) { int a, b, c; getv(p, e, a); getv(p, e, b); getv(p, e, c); overrides[std::make_pair(a, b)] = (GhoulID)c; }
		getv(p, e, n); notes.clear();
		for (int i = 0; i < n && i < 65536; i++)
		{
			int ci, tk; getv(p, e, ci); getv(p, e, tk);
			if (ci >= 0 && ci < NumCallBacks && CallBacks[ci]) { NoteCB nc; nc.cb = CallBacks[ci]; nc.token = (GhoulID)tk; notes.push_back(nc); }
		}
		getv(p, e, n); matcbs.clear();
		for (int i = 0; i < n && i < 65536; i++)
		{
			int ci, pt, kd; getv(p, e, ci); getv(p, e, pt); getv(p, e, kd);
			if (ci >= 0 && ci < NumCallBacks && CallBacks[ci]) { MatCB mc; mc.cb = CallBacks[ci]; mc.part = (GhoulID)pt; mc.kind = (MatrixType)kd; matcbs.push_back(mc); }
		}
		(void)fv;
		lastUpdate = -1;
		return uuid;
	}
	static int cbIndex(IGhoulCallBack *c, IGhoulCallBack **l, int n)
	{
		for (int i = 0; i < n; i++) if (l[i] == c) return i;
		return -1;
	}
	template <class T> static void putv(std::vector<unsigned char> &d, T v)
	{
		const unsigned char *b = (const unsigned char *)&v;
		d.insert(d.end(), b, b + sizeof(T));
	}
	template <class T> static void getv(const unsigned char *&p, const unsigned char *e, T &v)
	{
		if (p + sizeof(T) > e) { memset(&v, 0, sizeof(T)); return; }
		memcpy(&v, p, sizeof(T)); p += sizeof(T);
	}

	GhoulUUID MyUUID() const { return uuid; }
	GhoulUUID MyObjUUID() const { return obj->uuid; }
	void Render(RenderInfo &) {}
	void PreCache(bool, bool, bool) { obj->resolve(); }

	/* triangles of the current frame in this instance's entity space (XForm/bolt chain applied) */
	void worldTris(float t, std::vector<float> &v, std::vector<int> &partOfTri)
	{
		const SeqRef *s; float ff;
		if (!frameAt(t, &s, &ff)) return;
		const ghb::Model &m = s->file->model;
		const float *ap = 0, *an = 0;
		static ghb::FrameCache cache; /* shared; decoding is deterministic */
		if (m.numAnimPos) cache.Get(m, (int)(ff + 0.5f), &ap, &an);
		ghb::Mat4 x;
		toRoot(t, x);
		std::vector<int> tris;
		ghb::BuildTriangles(m, tris);
		for (size_t i = 0; i < tris.size(); i += 4)
		{
			const ghb::Surface &sf = m.surfaces[(size_t)tris[i]];
			GhoulID pid = obj->FindPart(m.parts[(size_t)sf.part].name.c_str());
			if (!GetPartOnOff(pid)) continue;
			for (int k = 1; k <= 3; k++)
			{
				const ghb::Corner &c = m.corners[(size_t)tris[i + (size_t)k]];
				const float *p = c.pos < 0 ? &m.staticPos[(size_t)(~c.pos) * 3] : (ap ? ap + (size_t)c.pos * 3 : 0);
				float w[3] = { 0, 0, 0 };
				if (p) x.XFormPoint(w, p);
				v.push_back(w[0]); v.push_back(w[1]); v.push_back(w[2]);
			}
			partOfTri.push_back(pid);
		}
	}

	int RayTrace(float Time, const Vect3 &start, const Vect3 &dir, HitRecord *Hits, int MaxHits)
	{
		if (!on || MaxHits <= 0) return 0;
		std::vector<float> v;
		std::vector<int> parts;
		worldTris(Time, v, parts);
		int n = 0;
		Vect3 d = dir;
		for (size_t t = 0; t < parts.size(); t++)
		{
			const float *a = &v[t * 9], *b = a + 3, *c = a + 6;
			float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			float pv[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
			float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
			if (fabsf(det) < 1e-8f) continue;
			float inv = 1.0f / det;
			float tv[3] = { start[0] - a[0], start[1] - a[1], start[2] - a[2] };
			float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
			if (u < 0 || u > 1) continue;
			float qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
			float w = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
			if (w < 0 || u + w > 1) continue;
			float dist = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
			if (dist < 0) continue;
			HitRecord h;
			h.Inst = this;
			h.Mesh = (GhoulID)parts[t];
			h.Distance = dist;
			Vect3 nrm(e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]);
			nrm.ZeroNorm();
			if (nrm[0] * d[0] + nrm[1] * d[1] + nrm[2] * d[2] > 0) nrm *= -1.0f;
			h.Normal = nrm;
			h.ContactPoint = Vect3(start[0] + d[0] * dist, start[1] + d[1] * dist, start[2] + d[2] * dist);
			/* keep the hits sorted by distance */
			int pos = n < MaxHits ? n : MaxHits - 1;
			if (n >= MaxHits && dist >= Hits[MaxHits - 1].Distance) continue;
			while (pos > 0 && Hits[pos - 1].Distance > dist) { Hits[pos] = Hits[pos - 1]; pos--; }
			Hits[pos] = h;
			if (n < MaxHits) n++;
		}
		for (size_t i = 0; i < children.size() && n < MaxHits; i++)
			n += children[i].inst->RayTrace(Time, start, dir, Hits + n, MaxHits - n);
		return n;
	}
	int BoxTrace(float Time, const Matrix4 &, const Vect3 &start, const Vect3 &end, const Vect3 &, const Vect3 &, HitRecord *Hits, int MaxHits)
	{
		Vect3 d(end[0] - start[0], end[1] - start[1], end[2] - start[2]);
		float len = d.Len();
		if (len < 1e-6f) return 0;
		d /= len;
		int n = RayTrace(Time, start, d, Hits, MaxHits), k = 0;
		for (int i = 0; i < n; i++)
			if (Hits[i].Distance <= len) Hits[k++] = Hits[i];
		return k;
	}

	void fireToken(GhoulID token, float now, const char *data = 0)
	{
		static const char *notelog = getenv("SOF_GHOUL_NOTELOG");
		if (notelog && token)
			dprintf("[ghoul note] %s t=%.2f token %s callbacks %d%s\n", obj->objectDir.c_str(), now,
			        token <= obj->tokens.size() ? obj->tokens[token - 1].c_str() : "?", (int)notes.size(), callbacksOn ? "" : " (off)");
		if (g_noteHook && token && token <= obj->tokens.size()) g_noteHook(this, obj->tokens[token - 1].c_str(), data);
		if (!callbacksOn || !token) return;
		std::vector<NoteCB> copy(notes);
		for (size_t i = 0; i < copy.size(); i++)
			if (copy[i].token == token || copy[i].token == 0)
				copy[i].cb->Execute(this, user, now, data); /* the note's text, e.g. a sound name */
	}

	void ServerUpdate(float Time)
	{
		if (lastUpdate < 0) lastUpdate = Time;
		float t0 = lastUpdate, t1 = Time;
		lastUpdate = Time;
		const SeqRef *s;
		float f0, f1, sf0, sf1;
		bool e0, e1;
		if (callbacksOn && seqId && !paused && t1 > t0 && frameAt(t0, &s, &f0, &sf0, &e0) && frameAt(t1, &s, &f1, &sf1, &e1))
		{
			const ghb::Sequence &q = s->file->model.sequences[(size_t)s->index];
			float spf = (q.msPerFrame > 0 ? q.msPerFrame : 100.0f) / 1000.0f;
			float raw0 = (t0 - startTime) * speedFactor() / spf, raw1 = (t1 - startTime) * speedFactor() / spf;
			int n = q.numFrames > 0 ? q.numFrames : 1;
			float len = (float)(n > 1 ? n - 1 : 1);
			/* notes stored in the file */
			for (size_t i = 0; i < q.notes.size(); i++)
			{
				float nf = q.notes[i].timeMs / (q.msPerFrame > 0 ? q.msPerFrame : 100.0f);
				bool hit = false;
				if (ec == Loop && n > 1)
				{
					float period = (float)n;
					float k0 = floorf((raw0 - nf) / period), k1 = floorf((raw1 - nf) / period);
					hit = k1 > k0 && raw1 >= nf;
				}
				else
					hit = raw0 < nf && raw1 >= nf;
				if (hit) fireToken(obj->FindNoteToken(q.notes[i].token.c_str()), Time, q.notes[i].data.c_str());
			}
			/* synthesised end-of-sequence note */
			bool eos = false;
			if (ec == Loop && n > 1) eos = floorf(raw1 / (float)n) > floorf(raw0 / (float)n);
			else if (ec == Hold || ec == BackAndForth) eos = raw0 < len && raw1 >= len;
			if (eos && !hasExplicitEOS(q)) fireToken(obj->FindNoteToken("eos"), Time);
		}
		if (matCallbacksOn)
		{
			std::vector<MatCB> copy(matcbs);
			for (size_t i = 0; i < copy.size(); i++)
			{
				Matrix4 m;
				GetBoltMatrix(Time, m, copy[i].part, copy[i].kind, true);
				copy[i].cb->Execute(this, user, Time, &m);
			}
		}
		for (size_t i = 0; i < children.size(); i++)
			children[i].inst->ServerUpdate(Time);
	}
	static bool hasExplicitEOS(const ghb::Sequence &q)
	{
		for (size_t i = 0; i < q.notes.size(); i++)
			if (!strcasecmp(q.notes[i].token.c_str(), "eos")) return true;
		return false;
	}
	void TurnCallBacksOff() { callbacksOn = false; }
	void TurnMatrixCallBacksOff() { matCallbacksOn = false; }
	void TurnCallBacksOn(float Now) { callbacksOn = true; lastUpdate = Now; }
	void TurnMatrixCallBacksOn(float) { matCallbacksOn = true; }
	void GetBoltMatrix(float Time, Matrix4 &m, GhoulID Part, MatrixType kind, bool ToRoot)
	{
		ghb::Mat4 r;
		computeMatrix(Time, r, Part, kind, ToRoot);
		toM4(m, r);
	}
	void GetBoundBox(float Time, const Matrix4 &ToWorld, Vect3 &mins, Vect3 &maxs, GhoulID Part, bool Accurate)
	{
		const SeqRef *s; float ff;
		mins = Vect3(0.0f); maxs = Vect3(0.0f);
		if (!frameAt(Time, &s, &ff)) return;
		const ghb::Model &m = s->file->model;
		const ghb::Bounds *b = &m.bounds;
		if (Part && Part <= obj->partNames.size())
		{
			int p = m.FindPart(obj->partNames[Part - 1].c_str());
			if (p >= 0 && m.parts[(size_t)p].hasBounds2) b = &m.parts[(size_t)p].bounds2;
		}
		(void)Accurate;
		ghb::Mat4 x, w, tw;
		toRoot(Time, x);
		fromM4(tw, ToWorld);
		w.Mul(x, tw);
		bool first = true;
		for (int i = 0; i < 8; i++)
		{
			float c[3] = { (i & 1) ? b->maxs[0] : b->mins[0], (i & 2) ? b->maxs[1] : b->mins[1], (i & 4) ? b->maxs[2] : b->mins[2] };
			float o[3];
			w.XFormPoint(o, c);
			for (int k = 0; k < 3; k++)
			{
				if (first || o[k] < mins[k]) mins[k] = o[k];
				if (first || o[k] > maxs[k]) maxs[k] = o[k];
			}
			first = false;
		}
	}
	int QueueForClient(float) { return 0; }
	void Destroy()
	{
		while (!children.empty())
		{
			GInst *c = children.back().inst;
			children.pop_back();
			c->parent = 0;
			c->Destroy();
		}
		if (parent) parent->UnBolt(this);
		delete this;
	}
	GhoulID GetStateSequence() { return seqId; }

	/* ---- drawing ---- */
	/* Vertex positions/normals at a fractional frame: blends the two neighbouring
	   frames of the sequence so animation is smooth at any render rate. */
	static void blendedFrame(ghb::FrameCache &cache, const ghb::Model &m, const ghb::Sequence &q, float ff,
	                         const float **ap, const float **an)
	{
		static std::vector<float> bp, bn;
		int f0 = (int)floorf(ff);
		float fr = ff - (float)f0;
		int last = q.firstFrame + (q.numFrames > 0 ? q.numFrames : 1) - 1;
		const float *p0 = 0, *n0 = 0, *p1 = 0, *n1 = 0;
		*ap = *an = 0;
		if (!cache.Get(m, f0, &p0, &n0)) return;
		if (fr < 0.01f || f0 + 1 > last || !p0)
		{
			*ap = p0; *an = n0;
			return;
		}
		bp.assign(p0, p0 + (size_t)m.numAnimPos * 3);
		if (n0) bn.assign(n0, n0 + (size_t)m.numAnimNor * 3); else bn.clear();
		if (!cache.Get(m, f0 + 1, &p1, &n1) || !p1)
		{
			*ap = &bp[0]; *an = bn.empty() ? 0 : &bn[0];
			return;
		}
		for (size_t i = 0; i < bp.size(); i++) bp[i] += (p1[i] - bp[i]) * fr;
		if (n1 && !bn.empty())
			for (size_t i = 0; i < bn.size(); i++) bn[i] += (n1[i] - bn[i]) * fr;
		*ap = &bp[0];
		*an = bn.empty() ? 0 : &bn[0];
	}

	void draw(float t, const ghb::Mat4 *parentChain, std::vector<GhoulDrawSurface> &out)
	{
		if (!on) return;
		(void)parentChain;
		const SeqRef *s; float ff;
		if (frameAt(t, &s, &ff))
		{
			const ghb::Model &m = s->file->model;
			const float *ap = 0, *an = 0;
			static ghb::FrameCache cache;
			if (m.numAnimPos) blendedFrame(cache, m, m.sequences[(size_t)s->index], ff, &ap, &an);
			ghb::Mat4 x;
			toRoot(t, x);
			std::vector<int> tris;
			ghb::BuildTriangles(m, tris);
			std::map<int, size_t> surfOut; /* model surface -> output index */
			for (size_t i = 0; i < tris.size(); i += 4)
			{
				int si = tris[i];
				const ghb::Surface &sf = m.surfaces[(size_t)si];
				GhoulID pid = obj->FindPart(m.parts[(size_t)sf.part].name.c_str());
				if (!GetPartOnOff(pid)) continue;
				std::map<int, size_t>::iterator it = surfOut.find(si);
				if (it == surfOut.end())
				{
					GhoulDrawSurface d;
					d.objectDir = obj->objectDir;
					d.part = m.parts[(size_t)sf.part].name;
					memcpy(d.tint, tint, sizeof(tint));
					GhoulID matId = sf.material >= 0 && sf.material < (int)m.materials.size()
						? obj->FindMaterial(m.materials[(size_t)sf.material].name.c_str()) : 0;
					GhoulID skin = GetFrameOverride(matId, pid, Diffuse);
					if (!skin) skin = GetFrameOverride(matId, 0, Diffuse);
					if (!skin)
						for (size_t k = 0; k < obj->skins.size(); k++)
							if (obj->skins[k].mat == matId && obj->skins[k].channel == Diffuse) { skin = (GhoulID)(k + 1); break; }
					if (skin) d.skin = obj->skins[skin - 1].name;
					else if (sf.material >= 0 && sf.material < (int)m.materials.size())
						d.skin = lower(m.materials[(size_t)sf.material].texture); /* the model's default texture */
					static const char *drawlog = getenv("SOF_GHOUL_DRAWLOG");
					if (drawlog)
					{
						static std::set<std::string> seen;
						std::string key = obj->objectDir + " | part " + m.parts[(size_t)sf.part].name + " | mat " +
							(sf.material >= 0 && sf.material < (int)m.materials.size() ? m.materials[(size_t)sf.material].name : std::string("?")) +
							" | skin " + d.skin;
						if (seen.insert(key).second) dprintf("[ghoul draw] %s\n", key.c_str());
					}
					out.push_back(d);
					it = surfOut.insert(std::make_pair(si, out.size() - 1)).first;
				}
				GhoulDrawSurface &d = out[it->second];
				for (int k = 1; k <= 3; k++)
				{
					const ghb::Corner &c = m.corners[(size_t)tris[i + (size_t)k]];
					const float *p = c.pos < 0 ? &m.staticPos[(size_t)(~c.pos) * 3] : (ap ? ap + (size_t)c.pos * 3 : 0);
					const float *nn = c.nor < 0 ? (m.staticNor.empty() ? 0 : &m.staticNor[(size_t)(~c.nor) * 3]) : (an ? an + (size_t)c.nor * 3 : 0);
					int ui = c.uv < 0 ? ~c.uv : c.uv;
					float w[3] = { 0, 0, 0 }, wn[3] = { 0, 0, 1 };
					if (p) x.XFormPoint(w, p);
					if (nn) x.XFormVect(wn, nn);
					d.xyz.push_back(w[0]); d.xyz.push_back(w[1]); d.xyz.push_back(w[2]);
					d.normal.push_back(wn[0]); d.normal.push_back(wn[1]); d.normal.push_back(wn[2]);
					if (ui >= 0 && (size_t)ui * 2 + 1 < m.uv.size()) { d.st.push_back(m.uv[(size_t)ui * 2]); d.st.push_back(m.uv[(size_t)ui * 2 + 1]); }
					else { d.st.push_back(0); d.st.push_back(0); }
					d.indices.push_back((unsigned short)(d.xyz.size() / 3 - 1));
				}
			}
		}
		for (size_t i = 0; i < children.size(); i++)
			children[i].inst->draw(t, 0, out);
	}
};

/* ---------------------------------------------------------------- ghoul */

class GGhoul : public IGhoul
{
public:
	std::vector<GObj *> objs;
	void (*mapper)(char *, const char *);

	GGhoul() : mapper(0) {}

	IGhoulObj *NewObj() { GObj *o = new GObj(this); objs.push_back(o); return o; }
	void GlPrep(void *) {}
	void PreCache(bool) {}
	void GlUnprep() {}
	void BeginServerFrame() {}
	void EndServerFrame() {}
	void DestroyAllObjects()
	{
		std::vector<GObj *> copy(objs);
		for (size_t i = 0; i < copy.size(); i++) copy[i]->Destroy();
	}
	void FlushUnusedFiles() {}
	void Destroy() { DestroyAllObjects(); }
	IGhoulInst *FindClientInst(GhoulUUID key) { return Ghoul_FindInst(key); }
	IGhoulObj *FindClientObj(GhoulUUID key)
	{
		for (size_t i = 0; i < objs.size(); i++) if (objs[i]->uuid == key) return objs[i];
		return 0;
	}
	void SetFilenameMapper(void (*Map)(char *dest, const char *src)) { mapper = Map; }
	void AddClient(int) {}
	void RemoveClient(int) {}
	void FlushClients() {}
	void RemoveAllClients() {}
	bool PackReliable(int, int, OutPacket &) { return false; }
	bool NeedReliable(int) { return false; }
	bool Pack(int, int, OutPacket &, float) { return false; }
	void AckPack(int, int) {}
	void ReliableHitWire(int, int) {}
	void GhoulClearClientQueue(int, int) {}
	void AddFileForDownLoad(const char *) {}
	void Precache() {}
	void UnPackReliable(int, InPacket &, bool) {}
	void UnPack(int, InPacket &, float) {}
	int GetSavedReliableSize() const { return 0; }
	const unsigned char *GetSavedReliable() { return 0; }
	int GetNumExportLights() { return 0; }
	void GetExportLight(int, QuakeLight &) {}
	void BeginRenderFrame() {}
	void AddImportLight(const QuakeLight &) {}
	void EndRenderFrame() {}
	void FlushMeshCache() {}
	void AddMapLight(const QuakeLight &) {}
	void FinishMapLights() {}
	void ClearMapLights() {}
	void SetGammaTable(unsigned char *) {}
	void SetMip(int) {}
	void SetPolyOffset(float, float) {}
	int GetNumTexturesBound() { return 0; }
	int GetNumTexturesBoundBytes() { return 0; }
	int GetNumMeshesRendered() { return 0; }
	int GetNumTrianglesRendered() { return 0; }
	int GetNumTrianglesRenderedSpec() { return 0; }
	int GetNumVertsRendered() { return 0; }
	int GetNumCornersRendered() { return 0; }
	int GetNumLightRaysRendered() { return 0; }
	void ClearStats() {}
};

GGhoul *g_server = 0, *g_client = 0, *g_menu = 0;

IGhoulInst *GObj::NewInst()
{
	resolve();
	return new GInst(this);
}
IGhoul *GObj::GetMyGhoul() const { return owner; }
void GObj::Destroy()
{
	/* destroy the instances of this object */
	std::vector<GInst *> copy(g_instList);
	for (size_t i = 0; i < copy.size(); i++)
		if (copy[i]->obj == this && std::find(g_instList.begin(), g_instList.end(), copy[i]) != g_instList.end())
		{
			if (copy[i]->parent) copy[i]->parent->UnBolt(copy[i]);
			copy[i]->Destroy();
		}
	owner->objs.erase(std::remove(owner->objs.begin(), owner->objs.end(), this), owner->objs.end());
	delete this;
}

} // namespace

/* ---------------------------------------------------------------- engine API */

GhoulEngineImports *Ghoul_Imports(void)
{
	return &g_imp;
}

void Ghoul_Init(const GhoulEngineImports &imp)
{
	g_imp = imp;
	g_inited = true;
}

void Ghoul_Shutdown(void)
{
	if (g_server) { g_server->Destroy(); delete g_server; g_server = 0; }
	if (g_client) { g_client->Destroy(); delete g_client; g_client = 0; }
	if (g_menu) { g_menu->Destroy(); delete g_menu; g_menu = 0; }
	for (std::map<std::string, FileEntry *>::iterator it = g_files.begin(); it != g_files.end(); ++it) delete it->second;
	g_files.clear();
	g_dirIndex.clear();
}

IGhoul *Ghoul_Get(bool client, bool menu)
{
	GGhoul *&g = menu ? g_menu : (client ? g_client : g_server);
	if (!g) g = new GGhoul();
	return g;
}

void Ghoul_SetLevelName(const char *mapname)
{
	g_level = lower(mapname ? mapname : "");
}

IGhoulInst *Ghoul_FindInst(short uuid)
{
	for (size_t i = 0; i < g_instList.size(); i++)
		if (g_instList[i]->uuid == uuid) return g_instList[i];
	return 0;
}

void Ghoul_BuildDrawList(IGhoulInst *inst, float time, std::vector<GhoulDrawSurface> &out)
{
	if (!inst) return;
	static_cast<GInst *>(inst)->draw(time, 0, out);
}

const char *Ghoul_PlayingSequenceName(IGhoulInst *inst)
{
	if (!inst) return "";
	GInst *g = static_cast<GInst *>(inst);
	if (!g->seqId || g->seqId > g->obj->seqs.size()) return "";
	return g->obj->seqs[g->seqId - 1].base.c_str();
}

struct SavedPose { GhoulID seqId; float startTime, pauseTime, holdPos; int ec; bool paused, reverse; };
static std::map<IGhoulInst *, SavedPose> g_savedPoses;

bool Ghoul_SetPose(IGhoulInst *inst, const char *seqName)
{
	if (!inst) return false;
	GInst *g = static_cast<GInst *>(inst);
	GhoulID id = g->obj->FindSequence(seqName);
	if (!id) return false;
	SavedPose s;
	s.seqId = g->seqId; s.startTime = g->startTime; s.pauseTime = g->pauseTime; s.holdPos = g->holdPos;
	s.ec = (int)g->ec; s.paused = g->paused; s.reverse = g->reverse;
	g_savedPoses[inst] = s;
	g->seqId = id; g->startTime = 0; g->pauseTime = 0; g->holdPos = 0;
	g->ec = IGhoulInst::Hold; g->paused = true; g->reverse = false;
	return true;
}

void Ghoul_RestorePose(IGhoulInst *inst)
{
	std::map<IGhoulInst *, SavedPose>::iterator it = g_savedPoses.find(inst);
	if (it == g_savedPoses.end()) return;
	GInst *g = static_cast<GInst *>(inst);
	const SavedPose &s = it->second;
	g->seqId = s.seqId; g->startTime = s.startTime; g->pauseTime = s.pauseTime; g->holdPos = s.holdPos;
	g->ec = (IGhoulInst::EndCondition)s.ec; g->paused = s.paused; g->reverse = s.reverse;
	g_savedPoses.erase(it);
}

void Ghoul_SequenceNames(IGhoulInst *inst, std::vector<std::string> &out)
{
	out.clear();
	if (!inst) return;
	GInst *g = static_cast<GInst *>(inst);
	for (size_t i = 0; i < g->obj->seqs.size(); i++) out.push_back(g->obj->seqs[i].base);
}

const char *Ghoul_ObjectDir(IGhoulInst *inst)
{
	return inst ? static_cast<GInst *>(inst)->obj->objectDir.c_str() : "";
}

IGhoul *GetGhoul(bool Client, bool Menu)
{
	return Ghoul_Get(Client, Menu);
}

void Ghoul_SetNoteHook(GhoulNoteHook hook) { g_noteHook = hook; }
