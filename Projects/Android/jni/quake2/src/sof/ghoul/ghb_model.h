/*
 * ghb_model.h - loader/decoder for Soldier of Fortune GHOUL ".ghb" model files.
 *
 * Clean-room implementation written from observation of the file format (see
 * tools/sof/ghb (Python reference decoders) and docs/sof/ghb_format.md for
 * the format notes). Portable C++11, no platform dependencies.
 *
 * Coordinate / matrix conventions follow GHOUL: row vectors, p' = p * M,
 * rows 0..2 are the basis axes and row 3 is the translation.
 */
#ifndef SOF_GHB_MODEL_H
#define SOF_GHB_MODEL_H

#include <stdint.h>
#include <string>
#include <vector>

namespace ghb {

extern const float kDirTable[256][3];
extern const unsigned char kDirPerpAxis[256];

struct Mat4
{
	float m[4][4];
	int flags; /* 1 = identity (matches the on-disk 0x44 byte layout) */
	void Identity();
	void Mul(const Mat4 &a, const Mat4 &b); /* this = a * b (apply a, then b) */
	void XFormPoint(float *dst, const float *src) const;
	void XFormVect(float *dst, const float *src) const;
	void OrthoInverse(const Mat4 &a); /* inverse of rotation+translation matrix */
};

struct Bounds
{
	bool f0, f1;
	float mins[3], maxs[3];
};

struct Note
{
	float timeMs;  /* time from the start of the sequence */
	int   id;
	std::string token;
	std::string data;
};

struct Sequence
{
	float motion[3];
	float msPerFrame;
	std::string name;
	std::vector<Note> notes;
	int firstFrame;
	int numFrames;
};

struct Material
{
	std::string name;
	std::string texture;   /* default diffuse texture (base name, may name an .ifl list) */
	std::string specular;  /* default specular texture */
};

struct Node
{
	std::string name;
	std::string user;     /* user properties text (e.g. "NOBLOCK") */
	bool flags[3];
	Bounds bounds;
	int kind;             /* 0 = explicit matrices, else frame count of a compressed track */
	std::vector<Mat4> xforms;
	float inv[3], scale[3], origin[3];
	int bulkOfs;          /* compressed track: offset in the bulk block (8 bytes per frame) */
	bool hasBounds2;
	Bounds bounds2;       /* mesh parts only */
};

struct Surface
{
	int part;     /* index into Model::parts */
	int cmdStart; /* index into Model::cmds */
	int cmdLen;
	int material; /* derived from the material ranges */
};

struct Corner
{
	int16_t pos, nor, uv; /* negative = ~index into static arrays, else animated index */
};

struct AnimRecord
{
	int firstFrame, numFrames;
	float scale[3], origin[3];
	uint8_t bits[3];
	int ofsPos, ofsNor; /* byte offsets of the position bitstream / normal indices */
};

struct Model
{
	std::string source;
	int numFrames;
	std::vector<Sequence> sequences;
	std::vector<Material> materials;
	std::vector<Node> bolts;   /* "dummy" nodes: bolts, origins, scene root */
	std::vector<Node> extra;   /* nodes with extra channels (rare) */
	std::vector<Node> parts;   /* mesh parts */

	/* geometry */
	Bounds bounds;
	float quant[4][3];
	std::vector<int> matRange;          /* triples: material, firstSurface, numSurfaces */
	std::vector<float> staticPos;       /* 3 per vertex */
	std::vector<float> staticNor;       /* 3 per normal */
	std::vector<float> uv;              /* 2 per uv */
	std::vector<Surface> surfaces;
	std::vector<int16_t> cmds;          /* strips (>0) / fans (<0), 0 terminated */
	std::vector<Corner> corners;
	int numAnimPos;
	int numAnimNor;

	/* vertex animation */
	bool compressed;
	std::vector<int> frameRecord;       /* compressed: record index per frame */
	std::vector<AnimRecord> records;
	int animPoolBase;                   /* offset of the record pool in bulk */
	int rawPosOfs, rawNorOfs;           /* raw (uncompressed) vertex animation offsets in bulk */
	int geomOfs;                        /* offset of the geometry block in bulk */

	std::vector<uint8_t> bulk;          /* bulk block, padded with 8 zero bytes */
	std::vector<uint8_t> trailer;       /* data after the geometry descriptor (lights) */

	int FindSequence(const char *name) const;
	int FindPart(const char *name) const;
	int FindBolt(const char *name) const;
	int FindMaterial(const char *name) const;
};

/* Parse a whole .ghb file. Returns false and fills err on failure. */
bool Load(const uint8_t *data, size_t len, Model &out, std::string &err);

/* Decode one compressed record: pos receives numAnimPos*numFrames*3 floats laid out
 * [vertex][frame][xyz]; nor (optional) receives numAnimNor*numFrames*3 floats. */
bool DecodeRecord(const Model &m, int rec, float *pos, float *nor);

/* Evaluate animated vertex positions/normals for one integer frame
 * (pos: numAnimPos*3, nor: numAnimNor*3, either may be NULL). Uses a small cache. */
class FrameCache
{
public:
	FrameCache() : model(0), rec(-1) {}
	bool Get(const Model &m, int frame, const float **pos, const float **nor);
private:
	const Model *model;
	int rec;
	std::vector<float> pos, nor;
	std::vector<float> outPos, outNor;
};

/* Matrix of a node (bolt or part) at a fractional frame (linear interpolation between frames). */
void NodeMatrix(const Model &m, const Node &n, float frame, Mat4 &out);

/* Decode a rotation dword of a compressed node track into the 3x3 rotation of a node matrix (rows[row][col]). */
void DecodeRotation(uint32_t dw, float rows[3][3]);

/* Triangle list for all surfaces: appends (surface, corner0, corner1, corner2). */
void BuildTriangles(const Model &m, std::vector<int> &tris);

} // namespace ghb

#endif
