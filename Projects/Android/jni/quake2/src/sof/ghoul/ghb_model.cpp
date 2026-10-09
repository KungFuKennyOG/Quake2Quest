/*
 * ghb_model.cpp - SoF GHOUL .ghb loader / decoder (clean-room, see ghb_model.h).
 */
#include "ghb_model.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

namespace ghb {

/* ------------------------------------------------------------------ matrices */

void Mat4::Identity()
{
	memset(m, 0, sizeof(m));
	m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
	flags = 1;
}

void Mat4::Mul(const Mat4 &a, const Mat4 &b)
{
	Mat4 r;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
			            a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
	r.flags = (a.flags & b.flags & 1);
	*this = r;
}

void Mat4::XFormPoint(float *d, const float *s) const
{
	float x = s[0], y = s[1], z = s[2];
	for (int j = 0; j < 3; j++)
		d[j] = x * m[0][j] + y * m[1][j] + z * m[2][j] + m[3][j];
}

void Mat4::XFormVect(float *d, const float *s) const
{
	float x = s[0], y = s[1], z = s[2];
	for (int j = 0; j < 3; j++)
		d[j] = x * m[0][j] + y * m[1][j] + z * m[2][j];
}

void Mat4::OrthoInverse(const Mat4 &a)
{
	Mat4 r;
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++)
			r.m[i][j] = a.m[j][i];
		r.m[i][3] = 0.0f;
	}
	for (int j = 0; j < 3; j++)
		r.m[3][j] = -(a.m[3][0] * r.m[0][j] + a.m[3][1] * r.m[1][j] + a.m[3][2] * r.m[2][j]);
	r.m[3][3] = 1.0f;
	r.flags = a.flags & 1;
	*this = r;
}

/* ------------------------------------------------------------------ stream */

namespace {

struct Stream
{
	const uint8_t *d;
	size_t len, p;
	bool bad;

	Stream(const uint8_t *data, size_t n, size_t pos) : d(data), len(n), p(pos), bad(false) {}

	bool need(size_t n)
	{
		if (bad || p + n > len) { bad = true; return false; }
		return true;
	}
	int32_t i32()
	{
		if (!need(4)) return 0;
		int32_t v; memcpy(&v, d + p, 4); p += 4; return v;
	}
	float f32()
	{
		if (!need(4)) return 0;
		float v; memcpy(&v, d + p, 4); p += 4; return v;
	}
	bool b()
	{
		if (!need(1)) return false;
		return d[p++] == 1;
	}
	uint8_t u8()
	{
		if (!need(1)) return 0;
		return d[p++];
	}
	void vec3(float *v) { v[0] = f32(); v[1] = f32(); v[2] = f32(); }
	void skip(size_t n) { if (need(n)) p += n; }
	std::string str()
	{
		int32_t n = i32();
		if (n < 0 || n > 100000 || !need((size_t)n)) { bad = true; return std::string(); }
		std::string s((const char *)d + p, (size_t)n);
		p += (size_t)n;
		/* strings are stored with their terminating NUL counted */
		while (!s.empty() && s[s.size() - 1] == 0) s.erase(s.size() - 1);
		return s;
	}
	/* bool constant ? one element : count + elements */
	void constOrArray(size_t elem)
	{
		if (b()) { skip(elem); return; }
		int32_t n = i32();
		if (n < 0) { bad = true; return; }
		skip((size_t)n * elem);
	}
	void bitset()
	{
		if (b()) { b(); return; }
		int32_t nbits = i32();
		if (nbits < 0) { bad = true; return; }
		skip((size_t)((nbits + 31) / 32) * 4);
	}
};

void readBounds(Stream &s, Bounds &b)
{
	b.f0 = s.b();
	b.f1 = s.b();
	s.vec3(b.mins);
	s.vec3(b.maxs);
}

void readMat(Stream &s, Mat4 &m)
{
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			m.m[i][j] = s.f32();
	m.flags = s.i32();
}

void readNode(Stream &s, Node &n)
{
	n.name = s.str();
	n.user = s.str();
	s.bitset();
	n.flags[0] = s.b(); n.flags[1] = s.b(); n.flags[2] = s.b();
	readBounds(s, n.bounds);
	n.kind = s.i32();
	n.bulkOfs = -1;
	n.hasBounds2 = false;
	memset(n.inv, 0, sizeof(n.inv));
	memset(n.scale, 0, sizeof(n.scale));
	memset(n.origin, 0, sizeof(n.origin));
	if (n.kind == 0)
	{
		int cnt = 1;
		if (!s.b())
			cnt = s.i32();
		if (cnt < 0 || cnt > 100000) { s.bad = true; return; }
		n.xforms.resize((size_t)cnt);
		for (int i = 0; i < cnt && !s.bad; i++)
			readMat(s, n.xforms[(size_t)i]);
	}
	else
	{
		s.vec3(n.inv);
		s.vec3(n.scale);
		s.vec3(n.origin);
		n.bulkOfs = s.i32();
	}
}

void readMaterial(Stream &s, Material &m)
{
	m.name = s.str();
	s.b(); s.b(); s.i32(); s.i32();
	for (int k = 0; k < 2; k++)
	{
		s.str(); s.str();
		s.b(); s.b(); s.b(); s.b();
		s.constOrArray(1);
		s.constOrArray(0x44);
		s.b();
	}
	s.constOrArray(12);
	s.constOrArray(12);
	for (int k = 0; k < 4; k++)
		s.constOrArray(4);
}

inline uint32_t rd32(const uint8_t *p)
{
	uint32_t v; memcpy(&v, p, 4); return v;
}

/* LSB-first bit reader identical to the original (reads a dword at bitpos/8, shifts by bitpos&7) */
struct Bits
{
	const uint8_t *d;
	size_t len;
	uint32_t pos;
	bool bad;
	uint32_t r(int n)
	{
		if (n <= 0) return 0;
		size_t byte = pos >> 3;
		if (byte + 4 > len) { bad = true; return 0; }
		uint32_t v = (rd32(d + byte) >> (pos & 7)) & (n >= 32 ? 0xffffffffu : ((1u << n) - 1));
		pos += (uint32_t)n;
		return v;
	}
};

inline void norm3(float *v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-10f) { v[0] /= l; v[1] /= l; v[2] /= l; }
	else v[0] = v[1] = v[2] = 0.0f;
}

inline void cross3(float *d, const float *a, const float *b)
{
	float x = a[1] * b[2] - a[2] * b[1];
	float y = a[2] * b[0] - a[0] * b[2];
	float z = a[0] * b[1] - a[1] * b[0];
	d[0] = x; d[1] = y; d[2] = z;
}

} // namespace

/* ------------------------------------------------------------------ lookup */

static int findByName(const std::vector<Node> &v, const char *name)
{
	for (size_t i = 0; i < v.size(); i++)
		if (!strcasecmp(v[i].name.c_str(), name))
			return (int)i;
	return -1;
}

int Model::FindSequence(const char *name) const
{
	for (size_t i = 0; i < sequences.size(); i++)
		if (!strcasecmp(sequences[i].name.c_str(), name))
			return (int)i;
	return -1;
}
int Model::FindPart(const char *name) const { return findByName(parts, name); }
int Model::FindBolt(const char *name) const { return findByName(bolts, name); }
int Model::FindMaterial(const char *name) const
{
	for (size_t i = 0; i < materials.size(); i++)
		if (!strcasecmp(materials[i].name.c_str(), name))
			return (int)i;
	return -1;
}

/* ------------------------------------------------------------------ load */

bool Load(const uint8_t *data, size_t len, Model &M, std::string &err)
{
	M = Model();
	if (len < 12) { err = "file too small"; return false; }
	uint32_t magic = rd32(data), a = rd32(data + 4), b = rd32(data + 8);
	if (magic != 0x198237FEu) { err = "bad container magic"; return false; }
	if ((uint64_t)a + b != len) { err = "container sizes do not match file size"; return false; }

	Stream s(data, a, 12);
	if ((uint32_t)s.i32() != 0x033FB1A3u) { err = "bad ghoul stream magic"; return false; }
	s.i32(); /* version */
	s.i32(); /* thunk flag */
	M.source = s.str();
	M.numFrames = s.i32();

	int nseq = s.i32();
	if (nseq < 0 || nseq > 10000) { err = "bad sequence count"; return false; }
	M.sequences.resize((size_t)nseq);
	for (int i = 0; i < nseq && !s.bad; i++)
	{
		Sequence &q = M.sequences[(size_t)i];
		s.vec3(q.motion);
		q.msPerFrame = s.f32();
		q.name = s.str();
		int nn = s.i32();
		if (nn < 0 || nn > 10000) { s.bad = true; break; }
		q.notes.resize((size_t)nn);
		for (int k = 0; k < nn; k++)
		{
			Note &n = q.notes[(size_t)k];
			n.timeMs = s.f32();
			n.id = s.i32();
			n.token = s.str();
			n.data = s.str();
		}
		q.firstFrame = s.i32();
		q.numFrames = s.i32();
	}

	int nmat = s.i32();
	if (nmat < 0 || nmat > 1000) { err = "bad material count"; return false; }
	M.materials.resize((size_t)nmat);
	for (int i = 0; i < nmat && !s.bad; i++)
		readMaterial(s, M.materials[(size_t)i]);

	Bounds tmpb;
	readBounds(s, tmpb);
	s.i32();
	int nb = s.i32();
	if (nb < 0 || nb > 10000) { err = "bad node count"; return false; }
	M.bolts.resize((size_t)nb);
	for (int i = 0; i < nb && !s.bad; i++)
		readNode(s, M.bolts[(size_t)i]);

	s.i32();
	int ne = s.i32();
	if (ne < 0 || ne > 10000) { err = "bad extra node count"; return false; }
	M.extra.resize((size_t)ne);
	for (int i = 0; i < ne && !s.bad; i++)
	{
		readNode(s, M.extra[(size_t)i]);
		for (int k = 0; k < 4; k++)
			s.constOrArray(12);
	}

	s.i32();
	int np = s.i32();
	if (np < 0 || np > 10000) { err = "bad part count"; return false; }
	M.parts.resize((size_t)np);
	for (int i = 0; i < np && !s.bad; i++)
	{
		Node &n = M.parts[(size_t)i];
		readNode(s, n);
		readBounds(s, n.bounds2);
		n.hasBounds2 = true;
	}
	if (s.bad) { err = "stream truncated in node lists"; return false; }

	/* geometry descriptor */
	s.i32(); s.i32(); s.i32(); /* marker -1, 1, flags */
	int c[20];
	for (int i = 0; i < 20; i++) c[i] = s.i32();
	readBounds(s, M.bounds);
	for (int i = 0; i < 4; i++) s.vec3(M.quant[i]);
	M.compressed = s.b();
	int poolBase = 0;
	if (M.compressed)
	{
		M.numAnimPos = s.i32();
		M.numAnimNor = s.i32();
		int n = s.i32();
		if (n < 0 || n > 100000) { err = "bad frame map"; return false; }
		M.frameRecord.resize((size_t)n);
		for (int i = 0; i < n; i++) M.frameRecord[(size_t)i] = s.i32();
		s.i32(); /* size of the vertex stream */
		poolBase = s.i32();
		int nr = s.i32();
		if (nr < 0 || nr > 100000) { err = "bad record count"; return false; }
		M.records.resize((size_t)nr);
		for (int i = 0; i < nr; i++)
		{
			AnimRecord &r = M.records[(size_t)i];
			r.firstFrame = s.i32();
			r.numFrames = s.i32();
			s.vec3(r.scale);
			s.vec3(r.origin);
			r.bits[0] = s.u8(); r.bits[1] = s.u8(); r.bits[2] = s.u8();
			r.ofsPos = s.i32();
			r.ofsNor = s.i32();
		}
	}
	else
	{
		M.numAnimPos = c[7];
		M.numAnimNor = c[9];
	}
	int geomSize = s.i32();
	int tailSize = s.i32();
	if (s.bad) { err = "stream truncated in geometry descriptor"; return false; }
	M.trailer.assign(data + s.p, data + a);

	M.bulk.assign(data + a, data + a + b);
	M.bulk.resize(M.bulk.size() + 8, 0);
	M.animPoolBase = poolBase;
	M.geomOfs = tailSize;
	if (tailSize < 0 || (int64_t)tailSize + geomSize > (int64_t)b) { err = "geometry block out of range"; return false; }

	const uint8_t *g = &M.bulk[(size_t)tailSize];
	const int *T = c + 11;
	for (int i = 0; i < 6; i++)
		if (T[i] < 0 || T[i] > geomSize) { err = "geometry offsets out of range"; return false; }
	if (c[0] * 12 > T[0]) { err = "material table overlaps"; return false; }
	if ((int64_t)T[0] + 12LL * c[1] > T[1] || (int64_t)T[1] + 8LL * c[2] > T[2] ||
	    (int64_t)T[2] + 12LL * c[3] > T[3] || (int64_t)T[3] + 6LL * c[4] > T[4] ||
	    (int64_t)T[4] + 2LL * c[5] > T[5] || (int64_t)T[5] + 6LL * c[6] > geomSize)
	{ err = "geometry arrays overlap"; return false; }

	M.matRange.resize((size_t)c[0] * 3);
	for (int i = 0; i < c[0] * 3; i++) M.matRange[(size_t)i] = (int)rd32(g + 4 * i);
	M.staticPos.resize((size_t)c[1] * 3);
	if (c[1]) memcpy(&M.staticPos[0], g + T[0], (size_t)c[1] * 12);
	M.uv.resize((size_t)c[2] * 2);
	if (c[2]) memcpy(&M.uv[0], g + T[1], (size_t)c[2] * 8);
	M.staticNor.resize((size_t)c[3] * 3);
	if (c[3]) memcpy(&M.staticNor[0], g + T[2], (size_t)c[3] * 12);
	M.surfaces.resize((size_t)c[4]);
	for (int i = 0; i < c[4]; i++)
	{
		uint16_t v[3];
		memcpy(v, g + T[3] + 6 * i, 6);
		M.surfaces[(size_t)i].part = v[0];
		M.surfaces[(size_t)i].cmdStart = v[1];
		M.surfaces[(size_t)i].cmdLen = v[2];
		M.surfaces[(size_t)i].material = -1;
	}
	for (int k = 0; k < c[0]; k++)
	{
		int mat = M.matRange[(size_t)k * 3], first = M.matRange[(size_t)k * 3 + 1], cnt = M.matRange[(size_t)k * 3 + 2];
		for (int i = first; i < first + cnt && i < c[4]; i++)
			if (i >= 0) M.surfaces[(size_t)i].material = mat;
	}
	M.cmds.resize((size_t)c[5]);
	if (c[5]) memcpy(&M.cmds[0], g + T[4], (size_t)c[5] * 2);
	M.corners.resize((size_t)c[6]);
	if (c[6]) memcpy(&M.corners[0], g + T[5], (size_t)c[6] * 6);

	M.rawPosOfs = M.rawNorOfs = -1;
	if (!M.compressed && M.numAnimPos > 0)
	{
		int64_t need = (int64_t)M.numAnimPos * M.numFrames * 6;
		int64_t needN = (int64_t)M.numAnimNor * M.numFrames;
		if ((int64_t)T[6] + need > geomSize || (int64_t)T[6] + need + needN > geomSize)
		{ err = "raw vertex animation out of range"; return false; }
		M.rawPosOfs = tailSize + T[6];
		M.rawNorOfs = tailSize + T[6] + (int)need;
	}

	/* validate corner and part references */
	for (size_t i = 0; i < M.corners.size(); i++)
	{
		const Corner &k = M.corners[i];
		int p = k.pos < 0 ? ~k.pos : k.pos;
		int lim = k.pos < 0 ? c[1] : M.numAnimPos;
		if (p >= lim) { err = "corner position index out of range"; return false; }
	}
	for (size_t i = 0; i < M.surfaces.size(); i++)
		if (M.surfaces[i].part >= (int)M.parts.size() ||
		    M.surfaces[i].cmdStart + M.surfaces[i].cmdLen > (int)M.cmds.size())
		{ err = "surface reference out of range"; return false; }
	for (size_t i = 0; i < M.records.size(); i++)
	{
		const AnimRecord &r = M.records[i];
		if (r.numFrames <= 0 || r.firstFrame < 0 || r.firstFrame + r.numFrames > M.numFrames ||
		    r.ofsPos < 0 || r.ofsNor < r.ofsPos || (size_t)(poolBase + r.ofsNor) > b)
		{ err = "animation record out of range"; return false; }
	}
	for (size_t i = 0; i < M.frameRecord.size(); i++)
		if (M.frameRecord[i] < 0 || M.frameRecord[i] >= (int)M.records.size())
		{ err = "frame map out of range"; return false; }
	return true;
}

/* ------------------------------------------------------------------ vertex animation */

bool DecodeRecord(const Model &m, int ri, float *outPos, float *outNor)
{
	if (ri < 0 || ri >= (int)m.records.size()) return false;
	const AnimRecord &r = m.records[(size_t)ri];
	const int G = r.numFrames, nv = m.numAnimPos;
	Bits br;
	br.d = &m.bulk[0] + m.animPoolBase + r.ofsPos;
	br.len = m.bulk.size() - (size_t)(m.animPoolBase + r.ofsPos);
	br.pos = 0;
	br.bad = false;

	if (G < 2)
	{
		for (int v = 0; v < nv; v++)
			for (int f = 0; f < G; f++)
				for (int k = 0; k < 3; k++)
					outPos[(v * G + f) * 3 + k] = (float)br.r(r.bits[k]) * r.scale[k] + r.origin[k];
	}
	else
	{
		int d8 = 1;
		for (int v = 0; v < nv; v++)
		{
			if ((1 << d8) <= v - 1) d8++;
			float *dst = outPos + (size_t)v * G * 3;
			uint32_t mode = br.r(2);
			if (mode == 0)
			{
				int q[3], wd[3];
				for (int k = 0; k < 3; k++)
				{
					q[k] = (int)br.r(r.bits[k]);
					wd[k] = (int)br.r(4);
				}
				for (int f = 0; ; )
				{
					for (int k = 0; k < 3; k++)
						dst[f * 3 + k] = (float)q[k] * r.scale[k] + r.origin[k];
					if (++f == G) break;
					for (int k = 0; k < 3; k++)
						if (wd[k])
							q[k] += (int)br.r(wd[k] + 1) - (1 << wd[k]);
				}
			}
			else
			{
				int ref = (int)br.r(d8);
				int e0 = (int)br.r(4);
				int a8[3], base[3];
				for (int k = 0; k < 3; k++)
				{
					a8[k] = (int)br.r(4);
					base[k] = 0;
					if (e0)
					{
						base[k] = (int)br.r(e0);
						if (br.r(1)) base[k] = -base[k];
					}
				}
				if (ref >= v) return false;
				const float *rp = outPos + (size_t)ref * G * 3;
				for (int f = 0; f < G; f++)
					for (int k = 0; k < 3; k++)
					{
						int e = a8[k] ? (int)br.r(a8[k]) : 0;
						dst[f * 3 + k] = (float)(base[k] + e) * r.scale[k] + rp[f * 3 + k];
					}
			}
			if (br.bad) return false;
		}
	}
	if (br.bad) return false;
	if ((int)((br.pos + 7) / 8) + 4 != r.ofsNor - r.ofsPos)
		return false; /* stream length mismatch: corrupt or unsupported data */

	if (outNor)
	{
		const uint8_t *nb = &m.bulk[0] + m.animPoolBase + r.ofsNor;
		const int nn = m.numAnimNor;
		float *o = outNor;
		if (G < 3)
		{
			for (int n = 0; n < nn; n++)
				for (int f = 0; f < G; f++, o += 3)
					memcpy(o, kDirTable[*nb++], 12);
		}
		else if (G < 5)
		{
			float step = 1.0f / (float)(G - 1);
			for (int n = 0; n < nn; n++)
			{
				const float *a = kDirTable[nb[0]], *b = kDirTable[nb[1]];
				nb += 2;
				float v[3] = { a[0], a[1], a[2] };
				for (int f = 0; f < G; f++, o += 3)
				{
					o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
					for (int k = 0; k < 3; k++) v[k] += (b[k] - a[k]) * step;
				}
			}
		}
		else
		{
			int h = G / 2;
			float s1 = 1.0f / (float)h, s2 = 1.0f / (float)((G - h) - 1);
			for (int n = 0; n < nn; n++)
			{
				const float *a = kDirTable[nb[0]], *b = kDirTable[nb[1]], *c = kDirTable[nb[2]];
				nb += 3;
				float v[3] = { a[0], a[1], a[2] };
				int f = 0;
				for (; f <= h; f++, o += 3)
				{
					o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
					for (int k = 0; k < 3; k++) v[k] += (b[k] - a[k]) * s1;
				}
				for (; f < G; f++, o += 3)
				{
					o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
					for (int k = 0; k < 3; k++) v[k] += (c[k] - b[k]) * s2;
				}
			}
		}
	}
	return true;
}

bool FrameCache::Get(const Model &m, int frame, const float **p, const float **n)
{
	if (frame < 0) frame = 0;
	if (frame >= m.numFrames) frame = m.numFrames - 1;
	const int nv = m.numAnimPos, nn = m.numAnimNor;
	outPos.resize((size_t)nv * 3);
	outNor.resize((size_t)nn * 3);

	if (m.compressed)
	{
		if (frame < 0 || frame >= (int)m.frameRecord.size()) return false;
		int ri = m.frameRecord[(size_t)frame];
		const AnimRecord &r = m.records[(size_t)ri];
		if (model != &m || rec != ri)
		{
			pos.resize((size_t)nv * r.numFrames * 3);
			nor.resize((size_t)nn * r.numFrames * 3);
			if (!DecodeRecord(m, ri, pos.empty() ? 0 : &pos[0], nor.empty() ? 0 : &nor[0]))
			{
				model = 0; rec = -1;
				return false;
			}
			model = &m;
			rec = ri;
		}
		int lf = frame - r.firstFrame, G = r.numFrames;
		for (int v = 0; v < nv; v++)
			memcpy(&outPos[(size_t)v * 3], &pos[((size_t)v * G + lf) * 3], 12);
		for (int k = 0; k < nn; k++)
			memcpy(&outNor[(size_t)k * 3], &nor[((size_t)k * G + lf) * 3], 12);
	}
	else if (m.rawPosOfs >= 0)
	{
		const int F = m.numFrames;
		const uint8_t *src = &m.bulk[(size_t)m.rawPosOfs];
		for (int v = 0; v < nv; v++)
		{
			int16_t q[3];
			memcpy(q, src + ((size_t)v * F + frame) * 6, 6);
			for (int k = 0; k < 3; k++)
				outPos[(size_t)v * 3 + k] = (float)q[k] * m.quant[1][k] + m.quant[2][k];
		}
		const uint8_t *ns = &m.bulk[(size_t)m.rawNorOfs];
		for (int k = 0; k < nn; k++)
			memcpy(&outNor[(size_t)k * 3], kDirTable[ns[(size_t)k * F + frame]], 12);
	}
	if (p) *p = outPos.empty() ? 0 : &outPos[0];
	if (n) *n = outNor.empty() ? 0 : &outNor[0];
	return true;
}

/* ------------------------------------------------------------------ node tracks */

void DecodeRotation(uint32_t dw, float rows[3][3])
{
	static const float K0 = 0.018666666f, K1 = 0.14f;
	static const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	const float *A = kDirTable[dw & 0xff];
	float P[3], V[3], Q[3], R[3], W[3];

	cross3(P, A, axes[kDirPerpAxis[dw & 0xff]]);
	V[0] = A[0] + (float)((dw >> 8) & 15) * K0 - K1;
	V[1] = A[1] + (float)((dw >> 12) & 15) * K0 - K1;
	V[2] = A[2] + (float)((dw >> 16) & 15) * K0 - K1;
	norm3(V);
	cross3(P, P, V); norm3(P);
	cross3(Q, V, P); norm3(Q);
	float th = (float)((dw >> 20) & 0x7ff) * (2.0f * 3.14159265f / 2047.0f);
	float c = cosf(th), s = sinf(th);
	for (int k = 0; k < 3; k++) R[k] = P[k] * c + Q[k] * s;
	norm3(R);
	cross3(W, V, R);
	if (dw & 0x80000000u) { W[0] = -W[0]; W[1] = -W[1]; W[2] = -W[2]; }
	norm3(W);
	memcpy(rows[0], V, 12);
	memcpy(rows[1], R, 12);
	memcpy(rows[2], W, 12);
}

static void trackFrame(const Model &m, const Node &n, int f, float rows[3][3], float pos[3])
{
	if (f < 0) f = 0;
	if (f >= n.kind) f = n.kind - 1;
	size_t ofs = (size_t)n.bulkOfs + (size_t)f * 8;
	if (n.bulkOfs < 0 || ofs + 8 > m.bulk.size())
	{
		memset(rows, 0, 36); rows[0][0] = rows[1][1] = rows[2][2] = 1;
		pos[0] = pos[1] = pos[2] = 0;
		return;
	}
	DecodeRotation(rd32(&m.bulk[ofs]), rows);
	uint32_t p = rd32(&m.bulk[ofs + 4]);
	float q[3] = { (float)(p & 0x3ff), (float)((p >> 10) & 0x3ff), (float)(p >> 20) };
	for (int k = 0; k < 3; k++) pos[k] = q[k] * n.scale[k] + n.origin[k];
}

void NodeMatrix(const Model &m, const Node &n, float frame, Mat4 &out)
{
	if (n.kind == 0)
	{
		if (n.xforms.empty()) { out.Identity(); return; }
		int f = (int)frame;
		if (f < 0) f = 0;
		if (f >= (int)n.xforms.size()) f = (int)n.xforms.size() - 1;
		out = n.xforms[(size_t)f];
		return;
	}
	int f0 = (int)floorf(frame);
	float t = frame - (float)f0;
	float r0[3][3], p0[3];
	trackFrame(m, n, f0, r0, p0);
	if (t > 0.001f && f0 + 1 < n.kind)
	{
		float r1[3][3], p1[3];
		trackFrame(m, n, f0 + 1, r1, p1);
		for (int i = 0; i < 3; i++)
		{
			for (int k = 0; k < 3; k++) r0[i][k] += (r1[i][k] - r0[i][k]) * t;
			p0[i] += (p1[i] - p0[i]) * t;
		}
		/* re-orthonormalise (Gram-Schmidt on rows 0,1; row 2 = cross) */
		norm3(r0[0]);
		float d = r0[1][0] * r0[0][0] + r0[1][1] * r0[0][1] + r0[1][2] * r0[0][2];
		for (int k = 0; k < 3; k++) r0[1][k] -= d * r0[0][k];
		norm3(r0[1]);
		float w[3];
		cross3(w, r0[0], r0[1]);
		if (w[0] * r0[2][0] + w[1] * r0[2][1] + w[2] * r0[2][2] < 0) { w[0] = -w[0]; w[1] = -w[1]; w[2] = -w[2]; }
		memcpy(r0[2], w, 12);
	}
	for (int i = 0; i < 3; i++)
	{
		for (int k = 0; k < 3; k++) out.m[i][k] = r0[i][k];
		out.m[i][3] = 0;
		out.m[3][i] = p0[i];
	}
	out.m[3][3] = 1;
	out.flags = 0;
}

/* ------------------------------------------------------------------ triangles */

void BuildTriangles(const Model &m, std::vector<int> &tris)
{
	for (size_t si = 0; si < m.surfaces.size(); si++)
	{
		const Surface &s = m.surfaces[si];
		const int16_t *l = m.cmds.empty() ? 0 : &m.cmds[(size_t)s.cmdStart];
		int i = 0, len = s.cmdLen;
		while (i < len)
		{
			int k = l[i++];
			if (k == 0) break;
			int n = k < 0 ? -k : k;
			if (i + n > len) break;
			const int16_t *v = l + i;
			i += n;
			bool ok = true;
			for (int j = 0; j < n; j++)
				if (v[j] < 0 || v[j] >= (int)m.corners.size()) ok = false;
			if (!ok) continue;
			if (k < 0)
			{
				for (int j = 1; j < n - 1; j++)
				{
					tris.push_back((int)si); tris.push_back(v[0]); tris.push_back(v[j]); tris.push_back(v[j + 1]);
				}
			}
			else
			{
				for (int j = 0; j < n - 2; j++)
				{
					tris.push_back((int)si);
					if (j & 1) { tris.push_back(v[j + 1]); tris.push_back(v[j]); }
					else { tris.push_back(v[j]); tris.push_back(v[j + 1]); }
					tris.push_back(v[j + 2]);
				}
			}
		}
	}
}

} // namespace ghb
