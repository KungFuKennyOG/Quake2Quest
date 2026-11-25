/*
 * SoF Quest: a headset made of numbers, to test the VR drawing and controls on a desktop (vr_sim 1).
 *
 *   vr_sim_head    "x y z pitch yaw roll"  the head in the room (metres; degrees: pitch up, yaw left, roll right)
 *   vr_sim_right   the same for the right controller, vr_sim_left for the left ("" = not tracked)
 *   vr_sim_buttons "<right> <left>"         bit masks of VR_B_*
 *   vr_sim_sticks  "rx ry lx ly"
 *   vr_sim_shot    <prefix>                 writes <prefix>_left.ppm, _right.ppm and _screen.ppm of the next frame
 *
 * GPL v2 like the engine.
 */

#include "../header/client.h"
#include "vr.h"

#ifndef DEG2RAD
#define DEG2RAD(a) ((a) * (float)(M_PI / 180.0))
#endif
#ifndef RAD2DEG
#define RAD2DEG(a) ((a) * (float)(180.0 / M_PI))
#endif

static cvar_t *sim_head, *sim_right, *sim_left, *sim_buttons, *sim_sticks, *sim_shot, *sim_size, *sim_ipd, *sim_fov;
static unsigned sim_tex[3];
static int sim_w, sim_h;
static vr_submit_t sim_last;

static void
QuatMul(const float *a, const float *b, float *o)
{
	float r[4];

	r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(o, r, sizeof(r));
}

static qboolean
ParsePose(const char *s, vr_pose_t *p)
{
	float v[6] = {0, 0, 0, 0, 0, 0}, qy[4], qx[4], qz[4], h;

	memset(p, 0, sizeof(*p));
	p->q[3] = 1;

	if (sscanf(s, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 3)
	{
		return false;
	}

	VectorCopy(v, p->pos);
	h = DEG2RAD(v[4]) * 0.5f;
	qy[0] = 0; qy[1] = sinf(h); qy[2] = 0; qy[3] = cosf(h);
	h = DEG2RAD(v[3]) * 0.5f;
	qx[0] = sinf(h); qx[1] = 0; qx[2] = 0; qx[3] = cosf(h);
	h = DEG2RAD(-v[5]) * 0.5f;
	qz[0] = 0; qz[1] = 0; qz[2] = sinf(h); qz[3] = cosf(h);
	QuatMul(qy, qx, p->q);
	QuatMul(p->q, qz, p->q);
	p->valid = true;
	return true;
}

static qboolean
Sim_Init(int *width, int *height)
{
	sim_head = Cvar_Get("vr_sim_head", "0 1.57 0 0 0 0", 0);
	sim_right = Cvar_Get("vr_sim_right", "0.2 1.25 -0.35 0 0 0", 0);
	sim_left = Cvar_Get("vr_sim_left", "-0.2 1.25 -0.3 0 0 0", 0);
	sim_buttons = Cvar_Get("vr_sim_buttons", "0 0", 0);
	sim_sticks = Cvar_Get("vr_sim_sticks", "0 0 0 0", 0);
	sim_shot = Cvar_Get("vr_sim_shot", "", 0);
	sim_size = Cvar_Get("vr_sim_size", "1024", 0);
	sim_ipd = Cvar_Get("vr_sim_ipd", "0.064", 0);
	sim_fov = Cvar_Get("vr_sim_fov", "50", 0); /* half angle, degrees */
	*width = *height = (int)sim_size->value;
	return true;
}

static qboolean
Sim_SessionStart(int width, int height)
{
	int i;

	if (!vr_ref_NewTexture)
	{
		return false;
	}

	sim_w = width;
	sim_h = height;

	for (i = 0; i < 3; i++)
	{
		sim_tex[i] = vr_ref_NewTexture(width, height);
	}

	return true;
}

static void
Sim_SessionEnd(void)
{
}

static void
Sim_Shutdown(void)
{
}

static qboolean
Sim_FrameBegin(vr_input_t *in)
{
	static const float right[3] = {1, 0, 0};
	unsigned br = 0, bl = 0;
	float st[4] = {0, 0, 0, 0}, half = DEG2RAD(sim_fov->value);
	int e;

	memset(in, 0, sizeof(*in));
	ParsePose(sim_head->string, &in->head);
	ParsePose(sim_right->string, &in->hand[1].pose);
	ParsePose(sim_left->string, &in->hand[0].pose);
	sscanf(sim_buttons->string, "%u %u", &br, &bl);
	sscanf(sim_sticks->string, "%f %f %f %f", &st[0], &st[1], &st[2], &st[3]);
	in->hand[1].buttons = br;
	in->hand[0].buttons = bl;
	in->hand[1].stick[0] = st[0];
	in->hand[1].stick[1] = st[1];
	in->hand[0].stick[0] = st[2];
	in->hand[0].stick[1] = st[3];

	for (e = 0; e < 2; e++)
	{
		float u[3] = {in->head.q[0], in->head.q[1], in->head.q[2]}, uv[3], uuv[3], r[3];
		int k;

		CrossProduct(u, right, uv);
		CrossProduct(u, uv, uuv);

		for (k = 0; k < 3; k++)
		{
			r[k] = right[k] + (uv[k] * in->head.q[3] + uuv[k]) * 2.0f;
		}

		in->eye[e] = in->head;
		VectorMA(in->head.pos, (e ? 0.5f : -0.5f) * sim_ipd->value, r, in->eye[e].pos);
		/* like a Quest: more to the outside than to the nose */
		in->fov[e][0] = e ? -half * 0.8f : -half;
		in->fov[e][1] = e ? half : half * 0.8f;
		in->fov[e][2] = half * 0.9f;
		in->fov[e][3] = -half;
	}

	return true;
}

static unsigned
Sim_Texture(int which)
{
	return sim_tex[which];
}

static void
Sim_Release(int which)
{
	(void)which;
}

static void
WritePPM(const char *name, unsigned tex)
{
	unsigned char *px = malloc((size_t)sim_w * sim_h * 4);
	FILE *f;
	int x, y;

	if (!px)
	{
		return;
	}

	vr_ref_ReadPixels(vr_ref_Target(tex, sim_w, sim_h), sim_w, sim_h, px);
	f = fopen(name, "wb");

	if (f)
	{
		fprintf(f, "P6\n%d %d\n255\n", sim_w, sim_h);

		for (y = sim_h - 1; y >= 0; y--)
		{
			for (x = 0; x < sim_w; x++)
			{
				/* the screen picture over a checker board, to see what is see-through */
				unsigned char *p = px + ((size_t)y * sim_w + x) * 4;
				fwrite(p, 1, 3, f);
			}
		}

		fclose(f);
		Com_Printf("vr_sim: wrote %s\n", name);
	}

	free(px);
}

static void
Sim_FrameEnd(const vr_submit_t *submit)
{
	sim_last = *submit;

	if (sim_shot->string[0] && vr_ref_ReadPixels)
	{
		char name[MAX_OSPATH];

		Com_Printf("vr_sim: world %d screen %d (%s, %.2f x %.2f m) fov %.1f %.1f %.1f %.1f\n", submit->world,
			submit->screen, submit->screen_head ? "head" : "room", submit->screen_size[0], submit->screen_size[1],
			RAD2DEG(submit->fov[0]), RAD2DEG(submit->fov[1]), RAD2DEG(submit->fov[2]), RAD2DEG(submit->fov[3]));
		snprintf(name, sizeof(name), "%s_left.ppm", sim_shot->string);
		WritePPM(name, sim_tex[0]);
		snprintf(name, sizeof(name), "%s_right.ppm", sim_shot->string);
		WritePPM(name, sim_tex[1]);
		snprintf(name, sizeof(name), "%s_screen.ppm", sim_shot->string);
		WritePPM(name, sim_tex[2]);
		Cvar_Set("vr_sim_shot", "");
	}
}

static void
Sim_Haptic(int hand, float seconds, float strength)
{
	(void)hand;
	(void)seconds;
	(void)strength;
}

const vr_backend_t vr_sim = {
	"simulated headset", Sim_Init, Sim_SessionStart, Sim_SessionEnd, Sim_Shutdown, Sim_FrameBegin, Sim_Texture,
	Sim_Release, Sim_FrameEnd, Sim_Haptic
};
