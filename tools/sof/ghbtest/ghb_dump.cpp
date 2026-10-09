// ghb_dump: load .ghb files with the engine-side decoder and print checks / values.
#include "../../../Projects/Android/jni/quake2/src/sof/ghoul/ghb_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
using namespace ghb;
static bool readFile(const char *p, std::vector<uint8_t> &d)
{
	FILE *f = fopen(p, "rb"); if (!f) return false;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	d.resize((size_t)n); bool ok = fread(d.data(), 1, (size_t)n, f) == (size_t)n; fclose(f); return ok;
}
int main(int argc, char **argv)
{
	int okc = 0, fail = 0;
	for (int i = 1; i < argc; i++)
	{
		std::vector<uint8_t> d; Model m; std::string err;
		if (!readFile(argv[i], d)) { printf("%s READFAIL\n", argv[i]); fail++; continue; }
		if (!Load(d.data(), d.size(), m, err)) { printf("%s LOADFAIL %s\n", argv[i], err.c_str()); fail++; continue; }
		int badrec = 0;
		for (size_t r = 0; r < m.records.size(); r++)
		{
			std::vector<float> p((size_t)m.numAnimPos * m.records[r].numFrames * 3), n((size_t)m.numAnimNor * m.records[r].numFrames * 3);
			if (!DecodeRecord(m, (int)r, p.data(), n.empty() ? 0 : n.data())) badrec++;
		}
		FrameCache fc; const float *P = 0, *N = 0; double sum = 0;
		if (m.numAnimPos && fc.Get(m, 0, &P, &N))
			for (int v = 0; v < m.numAnimPos * 3; v++) sum += P[v];
		std::vector<int> tris; BuildTriangles(m, tris);
		printf("%s OK frames %d seq %zu mats %zu bolts %zu parts %zu surf %zu tris %zu animpos %d animnor %d %s recs %zu badrec %d f0sum %.4f",
			argv[i], m.numFrames, m.sequences.size(), m.materials.size(), m.bolts.size(), m.parts.size(), m.surfaces.size(), tris.size() / 4,
			m.numAnimPos, m.numAnimNor, m.compressed ? "comp" : (m.rawPosOfs >= 0 ? "raw" : "static"), m.records.size(), badrec, sum);
		for (size_t b = 0; b < m.bolts.size(); b++)
			if (m.bolts[b].kind)
			{
				Mat4 M; NodeMatrix(m, m.bolts[b], 0, M);
				printf(" bolt0 %s %.4f %.4f %.4f | %.4f %.4f %.4f", m.bolts[b].name.c_str(), M.m[3][0], M.m[3][1], M.m[3][2], M.m[0][0], M.m[0][1], M.m[0][2]);
				break;
			}
		printf("\n");
		if (badrec) fail++; else okc++;
	}
	fprintf(stderr, "ok %d fail %d\n", okc, fail);
	return fail ? 1 : 0;
}
