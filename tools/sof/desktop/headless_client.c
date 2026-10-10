/*
 * headless_client.c - lets the full engine (client + server) run on a desktop without
 * video, sound, input or VR, for testing. The "renderer" records the refdef of each frame
 * and, when SOF_DUMP is set, writes the frame's GHOUL meshes (already placed in the world
 * like the GL renderer would) plus the camera to an OBJ file.
 */
#include "client/header/client.h"
#include "client/sound/header/local.h"
#include "../../../Projects/Android/jni/quake2/src/sof/sof_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ---- video ----
 * Without SOF_REF this is a null renderer. With SOF_REF=<path to a desktop build of
 * the GL1 renderer linked against gl_stub.c> the real renderer runs on every frame
 * (nothing is drawn; run it under AddressSanitizer to catch memory errors). */
#include <dlfcn.h>
viddef_t viddef = { 1280, 720 };
cvar_t *vid_renderer, *vid_gamma, *vid_fullscreen;
refexport_t re;
static int real_ref;

static qboolean H_GetModeInfo(int *w, int *h, int mode) { (void)mode; *w = viddef.width; *h = viddef.height; return true; }
static void H_MenuInit(void) {}
static void H_WriteScreenshot(int w, int h, int c, const void *d) { (void)w; (void)h; (void)c; (void)d; }
static qboolean H_InitGraphics(int fs, int *w, int *h) { (void)fs; *w = viddef.width; *h = viddef.height; return true; }
static qboolean H_GetDesktopMode(int *w, int *h) { *w = viddef.width; *h = viddef.height; return true; }

void VID_Init(void)
{
	const char *lib = getenv("SOF_REF");
	vid_renderer = Cvar_Get("vid_renderer", "null", 0);
	vid_gamma = Cvar_Get("vid_gamma", "1", 0);
	vid_fullscreen = Cvar_Get("vid_fullscreen", "0", 0);
	if (lib)
	{
		void *h = dlopen(lib, RTLD_NOW);
		GetRefAPI_t get = h ? (GetRefAPI_t)dlsym(h, "GetRefAPI") : NULL;
		refimport_t ri;
		if (!get) Com_Error(ERR_FATAL, "SOF_REF: %s", dlerror());
		memset(&ri, 0, sizeof(ri));
		ri.Cmd_AddCommand = Cmd_AddCommand; ri.Cmd_Argc = Cmd_Argc; ri.Cmd_Argv = Cmd_Argv;
		ri.Cmd_ExecuteText = Cbuf_ExecuteText; ri.Cmd_RemoveCommand = Cmd_RemoveCommand;
		ri.Com_VPrintf = Com_VPrintf; ri.Cvar_Get = Cvar_Get; ri.Cvar_Set = Cvar_Set; ri.Cvar_SetValue = Cvar_SetValue;
		ri.FS_FreeFile = FS_FreeFile; ri.FS_Gamedir = FS_Gamedir; ri.FS_LoadFile = FS_LoadFile;
		ri.GLimp_InitGraphics = H_InitGraphics; ri.GLimp_GetDesktopMode = H_GetDesktopMode;
		ri.Sys_Error = Com_Error; ri.Vid_GetModeInfo = H_GetModeInfo; ri.Vid_MenuInit = H_MenuInit;
		ri.Vid_WriteScreenshot = H_WriteScreenshot;
		re = get(ri);
		if (!re.Init(1)) Com_Error(ERR_FATAL, "SOF_REF: renderer Init failed");
		real_ref = 1;
		Com_Printf("[render] real renderer %s loaded\n", lib);
	}
}
void VID_Shutdown(void) { if (real_ref) re.Shutdown(); }
void VID_CheckChanges(void) {}
int GLimp_GetRefreshRate(void) { return 60; }
qboolean R_IsVSyncActive(void) { return false; }

static int dummy_model;
#define FWD(call) do { if (real_ref) { call; } } while (0)
void R_BeginRegistration(char *map) { FWD(re.BeginRegistration(map)); }
struct model_s *R_RegisterModel(char *name) { if (real_ref) return re.RegisterModel(name); return (struct model_s *)&dummy_model; }
struct image_s *R_RegisterSkin(char *name) { if (real_ref) return re.RegisterSkin(name); return (struct image_s *)&dummy_model; }
void R_SetSky(char *name, float rotate, vec3_t axis) { Com_Printf("[render] R_SetSky '%s' rotate %g\n", name, rotate); FWD(re.SetSky(name, rotate, axis)); }
void R_EndRegistration(void) { FWD(re.EndRegistration()); }
struct image_s *Draw_FindPic(char *name) { if (real_ref) return re.DrawFindPic(name); return (struct image_s *)&dummy_model; }
void Draw_GetPicSize(int *w, int *h, char *name) { if (real_ref) { re.DrawGetPicSize(w, h, name); return; } *w = 32; *h = 32; }
void Draw_StretchPic(int x, int y, int w, int h, char *name) { FWD(re.DrawStretchPic(x, y, w, h, name)); }
void Draw_PicScaled(int x, int y, char *pic, float factor) { FWD(re.DrawPicScaled(x, y, pic, factor)); }
void Draw_CharScaled(int x, int y, int num, float scale) { FWD(re.DrawCharScaled(x, y, num, scale)); }
void Draw_TileClear(int x, int y, int w, int h, char *name) { FWD(re.DrawTileClear(x, y, w, h, name)); }
void Draw_Fill(int x, int y, int w, int h, int c) { FWD(re.DrawFill(x, y, w, h, c)); }
void Draw_FadeScreen(void) { FWD(re.DrawFadeScreen()); }
void Draw_StretchRaw(int x, int y, int w, int h, int cols, int rows, byte *data) { FWD(re.DrawStretchRaw(x, y, w, h, cols, rows, data)); }
void R_SetPalette(const unsigned char *palette) { FWD(re.SetPalette(palette)); }
void R_BeginFrame(float camera_separation) { FWD(re.BeginFrame(camera_separation)); }
void R_EndFrame(void) { FWD(re.EndFrame()); }

/* entity transform used by the GL renderer for GHOUL meshes (alias-model convention) */
static void placePoint(const entity_t *e, const float *p, float *out)
{
	vec3_t f, r, u;
	vec3_t a;
	VectorCopy(e->angles, a);
	AngleVectors(a, f, r, u);
	/* model x = forward, y = left, z = up */
	out[0] = e->origin[0] + p[0] * f[0] - p[1] * r[0] + p[2] * u[0];
	out[1] = e->origin[1] + p[0] * f[1] - p[1] * r[1] + p[2] * u[1];
	out[2] = e->origin[2] + p[0] * f[2] - p[1] * r[2] + p[2] * u[2];
}

static int frames_rendered;
void R_RenderFrame(refdef_t *fd)
{
	const char *dump = getenv("SOF_DUMP");
	int dumpframe = getenv("SOF_DUMP_FRAME") ? atoi(getenv("SOF_DUMP_FRAME")) : 100;
	int i, m, v, ghouls = 0, tris = 0;

	frames_rendered++;
	if (real_ref)
	{
		/* SOF_TEST_DLIGHT: add a big light just in front of the view every frame so
		   the renderer's dynamic lightmap path runs (muzzle flashes do this in game) */
		static dlight_t extra[MAX_DLIGHTS];
		if (getenv("SOF_TEST_DLIGHT") && fd->num_dlights < MAX_DLIGHTS)
		{
			vec3_t f, r, u;
			memcpy(extra, fd->dlights, sizeof(dlight_t) * fd->num_dlights);
			AngleVectors(fd->viewangles, f, r, u);
			VectorMA(fd->vieworg, 64, f, extra[fd->num_dlights].origin);
			extra[fd->num_dlights].intensity = 1000;
			VectorSet(extra[fd->num_dlights].color, 1, 0.8f, 0.6f);
			fd->dlights = extra;
			fd->num_dlights++;
		}
		re.RenderFrame(fd);
	}
	for (i = 0; i < fd->num_entities; i++)
		if (fd->entities[i].sofdraw)
		{
			ghouls++;
			for (m = 0; m < fd->entities[i].sofdraw->nummeshes; m++) tris += fd->entities[i].sofdraw->meshes[m].numindices / 3;
		}
	if (frames_rendered % 50 == 0)
		Com_Printf("[render] frame %d: view %.0f %.0f %.0f ang %.0f %.0f, %d entities, %d ghoul (%d tris)\n", frames_rendered,
		           fd->vieworg[0], fd->vieworg[1], fd->vieworg[2], fd->viewangles[0], fd->viewangles[1], fd->num_entities, ghouls, tris);

	if (dump && frames_rendered == dumpframe)
	{
		FILE *f = fopen(dump, "w");
		int base = 1;
		if (!f) return;
		fprintf(f, "# view %f %f %f angles %f %f %f\n", fd->vieworg[0], fd->vieworg[1], fd->vieworg[2],
		        fd->viewangles[0], fd->viewangles[1], fd->viewangles[2]);
		for (i = 0; i < fd->num_entities; i++)
		{
			const entity_t *e = &fd->entities[i];
			if (!e->sofdraw) continue;
			fprintf(f, "o ent%d\n", i);
			for (m = 0; m < e->sofdraw->nummeshes; m++)
			{
				const sofmesh_t *ms = &e->sofdraw->meshes[m];
				fprintf(f, "# skin %s\n", ms->skin);
				for (v = 0; v < ms->numverts; v++)
				{
					float w[3];
					placePoint(e, ms->xyz + v * 3, w);
					fprintf(f, "v %f %f %f\n", w[0], w[1], w[2]);
				}
				for (v = 0; v + 2 < ms->numindices; v += 3)
					fprintf(f, "f %d %d %d\n", base + ms->indices[v], base + ms->indices[v + 1], base + ms->indices[v + 2]);
				base += ms->numverts;
			}
		}
		fclose(f);
		Com_Printf("[render] dumped frame %d to %s\n", frames_rendered, dump);
	}
}

/* ---- sound ---- */
sndstarted_t sound_started = SS_NOT;
qboolean snd_is_underwater;
void S_Init(void) {}
void S_Shutdown(void) {}
void S_BeginRegistration(void) {}
struct sfx_s *S_RegisterSound(char *sample) { (void)sample; return NULL; }
void S_EndRegistration(void) {}
void S_StartSound(vec3_t origin, int entnum, int entchannel, struct sfx_s *sfx, float fvol, float attenuation, float timeofs)
{ (void)origin; (void)entnum; (void)entchannel; (void)sfx; (void)fvol; (void)attenuation; (void)timeofs; }
void S_StartLocalSound(char *s) { (void)s; }
void S_RawSamples(int samples, int rate, int width, int channels, byte *data, float volume)
{ (void)samples; (void)rate; (void)width; (void)channels; (void)data; (void)volume; }
void S_StopAllSounds(void) {}
void S_Update(vec3_t origin, vec3_t forward, vec3_t right, vec3_t up) { (void)origin; (void)forward; (void)right; (void)up; }
void OGG_Init(void) {}
void OGG_Shutdown(void) {}
void OGG_PlayTrack(int track) { (void)track; }
void OGG_Stop(void) {}
void OGG_InitTrackList(void) {}
void OGG_SaveState(void) {}
void OGG_RecoverState(void) {}

/* ---- input / VR ---- */
int sys_frame_time;
int Sys_Milliseconds(void);
void IN_Update(void) { sys_frame_time = Sys_Milliseconds(); }
void IN_Shutdown(void) {}
void In_FlushQueue(void) {}
vec3_t weaponangles, weaponoffset, hmdPosition;
vec2_t polarCursor;
int segment;
qboolean isItems, draw_item_wheel, joy_altselector_pressed;
_Bool player_moving;
cvar_t *vr_worldscale, *vr_comfort_mask, *vr_walkdirection, *vr_lasersight, *vr_control_scheme, *vr_use_wheels,
       *vr_smoothturn, *vr_jump_sound, *vr_height_adjust, *vr_weaponscale;
void VR_Init(void)
{
	vr_worldscale = Cvar_Get("vr_worldscale", "26.2467", 0);
	vr_comfort_mask = Cvar_Get("vr_comfort_mask", "0", 0);
	vr_walkdirection = Cvar_Get("vr_walkdirection", "0", 0);
	vr_lasersight = Cvar_Get("vr_lasersight", "0", 0);
	vr_control_scheme = Cvar_Get("vr_control_scheme", "0", 0);
	vr_use_wheels = Cvar_Get("vr_use_wheels", "0", 0);
	vr_smoothturn = Cvar_Get("vr_smoothturn", "0", 0);
	vr_jump_sound = Cvar_Get("vr_jump_sound", "0", 0);
	vr_height_adjust = Cvar_Get("vr_height_adjust", "0", 0);
	vr_weaponscale = Cvar_Get("vr_weaponscale", "1", 0);
}
/* the test drives the player with these cvars */
void VR_GetMove(float *forward, float *side, float *up, float *yaw, float *pitch, float *roll)
{
	*forward = Cvar_VariableValue("test_forward");
	*side = 0; *up = 0;
	*yaw = Cvar_VariableValue("test_yaw");
	*pitch = 0; *roll = 0;
}
void Android_Vibrate(float duration, int channel, float intensity) { (void)duration; (void)channel; (void)intensity; }
void getVROrigins(vec3_t o, vec3_t a, vec3_t h) { VectorClear(o); VectorClear(a); VectorClear(h); }
float getFOV(void) { return 90.0f; }
void SinCos(float radians, float *sine, float *cosine) { *sine = sinf(radians); *cosine = cosf(radians); }
