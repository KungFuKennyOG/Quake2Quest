// Runtime test: drive the clean-room GHOUL runtime like the game does.
#include "ighoul.h"
#include "../../../Projects/Android/jni/quake2/src/sof/ghoul/ghoul_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <strings.h>
#include <string>
#include <vector>
static std::vector<std::string> roots;
static int LoadFile(const char *p, void **buf)
{
	for (size_t r = 0; r < roots.size(); r++) {
	std::string path = roots[r] + "/" + p;
	// case-insensitive lookup of the last component
	std::string dir = path.substr(0, path.find_last_of('/')), base = path.substr(path.find_last_of('/') + 1);
	DIR *d = opendir(dir.c_str()); if (!d) continue;
	struct dirent *e; std::string real;
	while ((e = readdir(d))) if (!strcasecmp(e->d_name, base.c_str())) real = dir + "/" + e->d_name;
	closedir(d);
	if (real.empty()) continue;
	FILE *f = fopen(real.c_str(), "rb"); if (!f) return -1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	*buf = malloc((size_t)n); if (fread(*buf, 1, (size_t)n, f) != (size_t)n) n = -1; fclose(f); return (int)n;
	}
	return -1;
}
static void FreeFile(void *b) { free(b); }
static void ListFiles(const char *dir, const char *ext, std::vector<std::string> &out)
{
	for (size_t r = 0; r < roots.size(); r++) {
	DIR *d = opendir((roots[r] + "/" + dir).c_str()); if (!d) continue;
	struct dirent *e;
	while ((e = readdir(d))) { size_t l = strlen(e->d_name), el = strlen(ext); if (l > el && !strcasecmp(e->d_name + l - el, ext)) out.push_back(e->d_name); }
	closedir(d); }
}
static void Printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
struct CB : IGhoulCallBack { const char *name; int n; CB(const char *s) : name(s), n(0) {} bool Execute(IGhoulInst *, void *, float now, const void *) { n++; printf("  callback %s at %.2f\n", name, now); return true; } };
int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) roots.push_back(argv[i]);
	GhoulEngineImports imp = { LoadFile, FreeFile, ListFiles, Printf };
	Ghoul_Init(imp);
	Ghoul_SetLevelName("tut1");
	IGhoul *g = GetGhoul(false, false);
	IGhoulObj *o = g->NewObj();
	int nreg = GSQ_RegisterSequences((char *)"enemy/meso", (char *)"irqsoldier1", o);
	printf("gsq registered %d sequences\n", nreg);
	GhoulID st = o->FindSequence("ghoul/enemy/meso/std_istand_n_a_n.ghl");
	GhoulID tk = o->FindSequence("ghoul/enemy/meso/STD_ETALK_N_A_N.ghl");
	o->RegisterEverything(true);
	printf("seq ids %d %d, parts %d, mats %d, tokens %d\n", st, tk, o->NumParts(), o->NumMaterials(), o->NumNoteTokens());
	IGhoulInst *in = o->NewInst();
	CB eos("EOS"), bos("BOS");
	in->AddNoteCallBack(&eos, o->FindNoteToken("EOS"));
	in->AddNoteCallBack(&bos, o->FindNoteToken("BOS"));
	in->Play(st, 0.0f, 0.0f, true, IGhoulInst::Loop, false, false);
	float spf; float len = in->GetSequenceLength(st, IGhoulInst::Loop, &spf);
	printf("istand length %.2fs spf %.3f\n", len, spf);
	for (float t = 0; t < 4.0f; t += 0.1f) in->ServerUpdate(t);
	printf("EOS fired %d times, BOS %d\n", eos.n, bos.n);
	Matrix4 m;
	in->GetBoltMatrix(1.0f, m, o->FindPart("wbolt_hand_r"), IGhoulInst::Local, true);
	printf("hand_r pos %.2f %.2f %.2f rows %.2f %.2f %.2f / %.2f %.2f %.2f\n", m[3][0], m[3][1], m[3][2], m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2]);
	HitRecord hits[8];
	int n = in->RayTrace(1.0f, Vect3(100, 0, 10), Vect3(-1, 0, 0), hits, 8);
	char pn[128];
	for (int i = 0; i < n; i++) { o->GetPartName(hits[i].Mesh, pn); printf("hit %d dist %.2f part %s at %.2f %.2f %.2f\n", i, hits[i].Distance, pn, hits[i].ContactPoint[0], hits[i].ContactPoint[1], hits[i].ContactPoint[2]); }
	GSQ_TurnOffParts((char *)"enemy/meso", (char *)"irqsoldier1_poff", o, in);
	int off = 0; for (int i = 1; i <= o->NumParts(); i++) if (!in->GetPartOnOff((GhoulID)i)) off++;
	printf("parts off after poff: %d, skins registered %d\n", off, o->NumSkins());
	for (int i = 1; i <= o->NumSkins() && i < 6; i++) { char sn[128]; o->GetSkinName(i, sn); printf("  skin %d %s mat %d\n", i, sn, o->GetSkinMaterial(i)); }
	std::vector<GhoulDrawSurface> ds;
	Ghoul_BuildDrawList(in, 1.0f, ds);
	size_t tris = 0; for (size_t i = 0; i < ds.size(); i++) tris += ds[i].indices.size() / 3;
	printf("draw surfaces %zu tris %zu\n", ds.size(), tris);
	FILE *f = fopen("rt_frame.obj", "w"); int base = 1;
	for (size_t i = 0; i < ds.size(); i++)
	{
		for (size_t v = 0; v < ds[i].xyz.size() / 3; v++) fprintf(f, "v %f %f %f\n", ds[i].xyz[v * 3], ds[i].xyz[v * 3 + 1], ds[i].xyz[v * 3 + 2]);
		for (size_t t = 0; t < ds[i].indices.size(); t += 3) fprintf(f, "f %d %d %d\n", base + ds[i].indices[t], base + ds[i].indices[t + 1], base + ds[i].indices[t + 2]);
		base += (int)ds[i].xyz.size() / 3;
	}
	fclose(f);
	// a bolted static object (a hat) on the head bolt
	IGhoulObj *ho = g->NewObj();
	ho->RegisterSequence("ghoul/enemy/bolt/acc_hat_beret.ghl");
	ho->RegisterEverything(true);
	IGhoulInst *hat = ho->NewInst();
	char bn[64]; for (int i = 1; i <= ho->NumParts(); i++) { ho->GetPartName(i, bn); printf("hat part %d %s type %d\n", i, bn, ho->GetPartType(i)); }
	in->Bolt(o->FindPart("abolt_head_t"), hat, ho->FindPart("to_abolt_head_t") ? ho->FindPart("to_abolt_head_t") : 0);
	ds.clear(); Ghoul_BuildDrawList(in, 1.0f, ds);
	printf("with hat: surfaces %zu\n", ds.size());
	in->Destroy();
	Ghoul_Shutdown();
	return 0;
}
