/*
 * ghoul_gsq.cpp - .gsq text lists used by the SoF game module (clean-room).
 *
 * A .gsq file lists one name per line: sequence names (registered as
 * ghoul/<dir>/<name>.ghl) or part names (for *_poff files, parts to switch off).
 * "#name" includes another list, looked up in the same directory and then in ghoul/comskin.
 */
#include "ighoul.h"
#include "ghoul_engine.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern GhoulEngineImports *Ghoul_Imports(void);

static bool loadText(const std::string &path, std::string &out)
{
	GhoulEngineImports *imp = Ghoul_Imports();
	void *buf = 0;
	int len = imp && imp->LoadFile ? imp->LoadFile(path.c_str(), &buf) : -1;
	if (len < 0 || !buf) return false;
	out.assign((const char *)buf, (size_t)len);
	imp->FreeFile(buf);
	return true;
}

static void lines(const std::string &t, std::vector<std::string> &out)
{
	size_t p = 0;
	while (p < t.size())
	{
		size_t e = t.find_first_of("\r\n", p);
		if (e == std::string::npos) e = t.size();
		std::string l = t.substr(p, e - p);
		p = e + 1;
		size_t c = l.find("//");
		if (c != std::string::npos) l.erase(c);
		while (!l.empty() && isspace((unsigned char)l[l.size() - 1])) l.erase(l.size() - 1);
		while (!l.empty() && isspace((unsigned char)l[0])) l.erase(0, 1);
		if (!l.empty()) out.push_back(l);
	}
}

/* expand a list with its includes; depth-limited, each include read once */
static void expand(const std::string &dir, const std::string &file, std::vector<std::string> &out,
                   std::vector<std::string> &seen, int depth)
{
	if (depth > 8) return;
	std::string key = dir + "|" + file;
	for (size_t i = 0; i < seen.size(); i++) if (seen[i] == key) return;
	seen.push_back(key);
	std::string t;
	if (!loadText("ghoul/" + dir + "/" + file + ".gsq", t) && !loadText("ghoul/comskin/" + file + ".gsq", t))
		return;
	std::vector<std::string> ls;
	lines(t, ls);
	for (size_t i = 0; i < ls.size(); i++)
	{
		if (ls[i][0] == '#') expand(dir, ls[i].substr(1), out, seen, depth + 1);
		else out.push_back(ls[i]);
	}
}

int GSQ_FindFile(char *gsqdir, char *gsqfile, void **buffer)
{
	std::string t;
	if (!loadText(std::string("ghoul/") + gsqdir + "/" + gsqfile + ".gsq", t) &&
	    !loadText(std::string("ghoul/comskin/") + gsqfile + ".gsq", t))
	{
		*buffer = 0;
		return -1;
	}
	char *b = (char *)malloc(t.size() + 1);
	memcpy(b, t.data(), t.size());
	b[t.size()] = 0;
	*buffer = b;
	return (int)t.size();
}

bool GSQ_ReadEntry(int &filesize, char **tdata, char *seqname)
{
	seqname[0] = 0;
	while (filesize > 0)
	{
		char *p = *tdata;
		int n = 0;
		while (n < filesize && p[n] != '\n' && p[n] != '\r') n++;
		int len = n;
		while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
		int st = 0;
		while (st < len && isspace((unsigned char)p[st])) st++;
		int skip = n;
		while (skip < filesize && (p[skip] == '\n' || p[skip] == '\r')) skip++;
		*tdata += skip;
		filesize -= skip;
		if (len - st > 0)
		{
			int l = len - st < 99 ? len - st : 99;
			memcpy(seqname, p + st, (size_t)l);
			seqname[l] = 0;
			return true;
		}
	}
	return false;
}

int GSQ_RegisterSequences(char *gsqdir, char *subclass, IGhoulObj *object)
{
	if (!object || !gsqdir || !subclass) return 0;
	std::vector<std::string> names, seen;
	expand(gsqdir, subclass, names, seen, 0);
	/* simple objects have no list: the subclass name is the sequence itself */
	if (seen.size() <= 1 && names.empty())
		names.push_back(subclass);
	int n = 0;
	for (size_t i = 0; i < names.size(); i++)
	{
		std::string path = std::string("ghoul/") + gsqdir + "/" + names[i] + ".ghl";
		if (object->RegisterSequence(path.c_str())) n++;
	}
	return n;
}

void GSQ_Precache(char *dirname, char *gsq_file, IGhoulObj *object)
{
	GSQ_RegisterSequences(dirname, gsq_file, object);
}

void GSQ_TurnOffParts(char *dirname, char *poff_file, IGhoulObj *obj, IGhoulInst *inst)
{
	if (!inst || !obj) return;
	std::vector<std::string> names, seen;
	expand(dirname, poff_file, names, seen, 0);
	for (size_t i = 0; i < names.size(); i++)
	{
		GhoulID p = obj->FindPart(names[i].c_str());
		if (p) inst->SetPartOnOff(p, false);
	}
}
