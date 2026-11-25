/*
 * SoF Quest: virtual reality, the game side (after Dr. Beef's Quake2Quest).
 *
 * - The head is the camera: its turn and tilt are the view angles, its height above the floor is the eye height,
 *   its steps in the room move the player.
 * - Each eye gets its own picture (the scene is built once and drawn twice), the 2D screen (menus, console, HUD)
 *   a third one that floats in front of the player.
 * - The gun is in the hand: the view weapon is drawn at the weapon hand's controller and shots leave along it
 *   (the game reads the cvar vr_aim, see quest/sdk).
 * - The Touch controllers are the controls (VR_Controls).
 *
 * Spaces: the room (OpenXR: metres, x right, y up, z back), "local" (game axes at the room's turn: x forward = the
 * room's -z, y left, z up, game units) and the world (local turned by the angle between the game's and the
 * room's yaw, vr.yawoffset; snap turns change it).
 *
 * GPL v2 like the engine.
 */

#include "../header/client.h"
#include "../header/sofclient.h"
#include "vr.h"

#ifndef DEG2RAD
#define DEG2RAD(a) ((a) * (float)(M_PI / 180.0))
#endif
#ifndef RAD2DEG
#define RAD2DEG(a) ((a) * (float)(180.0 / M_PI))
#endif

void M_MouseAbs(int x, int y);
void CL_ClearState(void);

#define EYE_HEIGHT_M 1.57f /* the head height of a standing player that is the game's eye height (Quake2Quest) */

static cvar_t *vr_sim_cv, *vr_enable, *vr_worldscale, *vr_height_adjust, *vr_snapturn_angle, *vr_turn_deadzone,
	*vr_walkdirection, *vr_weapon_pitchadjust, *vr_control_scheme, *vr_supersampling, *vr_screen_depth,
	*vr_screen_size, *vr_hud_depth, *vr_hud_size, *vr_positional, *vr_weapon_offset, *vr_aim, *vr_gunhand,
	*vr_debug, *vr_weapon_stabilised, *vr_weaponscale, *vr_weapon_turn;

static struct
{
	const vr_backend_t *be;
	qboolean enabled; /* a headset was found */
	qboolean session; /* its session runs on the GL context */
	int width, height;

	/* the renderer */
	void (*ref_SetSize)(int, int);
	void (*ref_Bind)(unsigned, int);
	void (*ref_Done)(unsigned);

	/* this frame */
	qboolean frame; /* FrameBegin was called and a picture is wanted */
	qboolean began; /* FrameBegin was called (FrameEnd is owed) */
	vr_input_t in, old;
	qboolean have_old;
	float head_angles[3]; /* pitch yaw roll at the room's turn */
	float head_local[3]; /* the head in local axes, metres */
	float last_local[3];
	qboolean have_last;
	float move_local[2]; /* the head's steps since the last command, metres (local x, y) */

	/* turning */
	float snap; /* the sum of the stick turns */
	float last_yaw; /* what was added to cl.viewangles so far */
	qboolean have_yaw;
	float yawoffset; /* world yaw - room yaw */

	/* drawing */
	int eye; /* -1 none, 0, 1 */
	qboolean built; /* the scene of this frame was built (first eye) */
	unsigned fbo[3];
	qboolean drawn[3];
	float base_org[3]; /* the engine's eye of this frame */
	float centre[3]; /* the head in the world */
	float fovx, fovy; /* degrees, the same for both eyes */
	float tanx, tany;
	qboolean remote; /* a camera of the game shows the scene (cutscene) */
	float remote_yaw0; /* the room yaw when it began */
	qboolean was_remote;

	/* the floating screen */
	qboolean screen_mode, was_screen_mode; /* menus and the like: a screen standing in the room */
	vr_pose_t screen_pose;
	float screen_w, screen_h;

	/* the gun hand in the world, for drawing and for the shots */
	qboolean weapon_valid;
	float weapon_hand[3], weapon_org[3], weapon_angles[3];
	float muzzle[3]; /* from the hand to the muzzle: forward, left, up */
	float muzzle_world[3];

	/* controls */
	unsigned held; /* which of our +commands are down */
	qboolean snapped;
	qboolean switched;
} vr;

unsigned (*vr_ref_NewTexture)(int width, int height);
void (*vr_ref_ReadPixels)(unsigned fbo, int width, int height, unsigned char *rgba);
unsigned (*vr_ref_Target)(unsigned tex, int width, int height);

/* ---- small mathematics ---------------------------------------------------------- */

static void
QuatRotate(const float *q, const float *v, float *out)
{
	float u[3] = {q[0], q[1], q[2]}, uv[3], uuv[3];

	CrossProduct(u, v, uv);
	CrossProduct(u, uv, uuv);
	out[0] = v[0] + (uv[0] * q[3] + uuv[0]) * 2.0f;
	out[1] = v[1] + (uv[1] * q[3] + uuv[1]) * 2.0f;
	out[2] = v[2] + (uv[2] * q[3] + uuv[2]) * 2.0f;
}

/* a room vector in local axes (still metres) */
static void
RoomToLocal(const float *r, float *l)
{
	float x = -r[2], y = -r[0], z = r[1];

	l[0] = x;
	l[1] = y;
	l[2] = z;
}

/* the game's pitch, yaw and roll of a pose (pointing along -z), tilted about its own right axis first */
static void
PoseAngles(const float *q, float tilt, float *angles)
{
	static const float f0[3] = {0, 0, -1}, r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0};
	float qa[4], t[3], f[3], r[3], u[3];

	if (tilt != 0.0f)
	{
		float h = DEG2RAD(tilt) * 0.5f, px = sinf(h), pw = cosf(h);

		qa[0] = q[3] * px + q[0] * pw;
		qa[1] = q[1] * pw + q[2] * px;
		qa[2] = q[2] * pw - q[1] * px;
		qa[3] = q[3] * pw - q[0] * px;
	}
	else
	{
		memcpy(qa, q, sizeof(qa));
	}

	QuatRotate(qa, f0, t);
	RoomToLocal(t, f);
	QuatRotate(qa, r0, t);
	RoomToLocal(t, r);
	QuatRotate(qa, u0, t);
	RoomToLocal(t, u);
	angles[PITCH] = RAD2DEG(-asinf(Q_clamp(f[2], -1.0f, 1.0f)));
	angles[YAW] = RAD2DEG(atan2f(f[1], f[0]));
	angles[ROLL] = RAD2DEG(atan2f(-r[2], u[2]));
}

static float
AngleNorm(float a)
{
	while (a > 180.0f)
	{
		a -= 360.0f;
	}

	while (a < -180.0f)
	{
		a += 360.0f;
	}

	return a;
}

/* a local vector (metres) in the world (game units) */
static void
LocalToWorld(const float *l, float *w)
{
	float a = DEG2RAD(vr.yawoffset), c = cosf(a), s = sinf(a), ws = vr_worldscale->value;

	w[0] = (l[0] * c - l[1] * s) * ws;
	w[1] = (l[0] * s + l[1] * c) * ws;
	w[2] = l[2] * ws;
}

/* a point of the room in the world: relative to the head, which is at vr.centre */
static void
RoomPointToWorld(const float *p, float *w)
{
	float d[3], l[3];

	VectorSubtract(p, vr.in.head.pos, d);
	RoomToLocal(d, l);
	LocalToWorld(l, w);
	VectorAdd(w, vr.centre, w);
}

/* ---- start and end ---------------------------------------------------------------- */

qboolean
VR_Enabled(void)
{
	return vr.enabled;
}

qboolean
VR_VideoSize(int *width, int *height)
{
	if (!vr.enabled)
	{
		return false;
	}

	*width = vr.width;
	*height = vr.height;
	return true;
}

void
VR_Init(void)
{
	int w = 0, h = 0;

	vr_enable = Cvar_Get("vr_enable", "1", CVAR_ARCHIVE);
	vr_sim_cv = Cvar_Get("vr_sim", "0", 0);
	vr_worldscale = Cvar_Get("vr_worldscale", "36", CVAR_ARCHIVE); /* game units in a metre */
	vr_height_adjust = Cvar_Get("vr_height_adjust", "0", CVAR_ARCHIVE);
	vr_snapturn_angle = Cvar_Get("vr_snapturn_angle", "45", CVAR_ARCHIVE); /* 10 and less: smooth turning */
	vr_turn_deadzone = Cvar_Get("vr_turn_deadzone", "0.2", CVAR_ARCHIVE);
	vr_walkdirection = Cvar_Get("vr_walkdirection", "1", CVAR_ARCHIVE); /* 1 where the head looks, 0 the off hand */
	vr_weapon_pitchadjust = Cvar_Get("vr_weapon_pitchadjust", "-20", CVAR_ARCHIVE);
	vr_control_scheme = Cvar_Get("vr_control_scheme", "0", CVAR_ARCHIVE); /* 0 right handed, 10 left handed */
	vr_supersampling = Cvar_Get("vr_supersampling", "1.0", CVAR_ARCHIVE);
	vr_screen_depth = Cvar_Get("vr_screen_depth", "2.8", CVAR_ARCHIVE);
	vr_screen_size = Cvar_Get("vr_screen_size", "3.0", CVAR_ARCHIVE);
	vr_hud_depth = Cvar_Get("vr_hud_depth", "2.0", CVAR_ARCHIVE);
	vr_hud_size = Cvar_Get("vr_hud_size", "2.2", CVAR_ARCHIVE);
	vr_positional = Cvar_Get("vr_positional", "1", CVAR_ARCHIVE); /* steps in the room move the player */
	/* a correction of where the hand holds the view weapon (forward, left, up; game units) and the model's size */
	vr_weapon_offset = Cvar_Get("vr_weapon_offset", "0 0 0", CVAR_ARCHIVE);
	vr_weaponscale = Cvar_Get("vr_weaponscale", "0.22", CVAR_ARCHIVE);
	vr_weapon_turn = Cvar_Get("vr_weapon_turn", "", 0);
	vr_gunhand = Cvar_Get("vr_gunhand", "1", CVAR_ARCHIVE); /* 0: the view weapon stays at the eye */
	vr_weapon_stabilised = Cvar_Get("vr_weapon_stabilised", "0", 0);
	vr_aim = Cvar_Get("vr_aim", "0", 0); /* "1 x y z pitch yaw roll": where shots leave (read by the game) */
	vr_debug = Cvar_Get("vr_debug", "0", 0);
	vr.eye = -1;

	if (!vr_enable->value)
	{
		return;
	}

	if (vr_sim_cv->value)
	{
		vr.be = &vr_sim;
	}

#ifdef OM_OPENXR
	if (!vr.be)
	{
		vr.be = &vr_openxr;
	}
#endif

	if (!vr.be)
	{
		return;
	}

	if (!vr.be->Init(&w, &h) || (w < 16) || (h < 16))
	{
		Com_Printf("VR: no headset (%s)\n", vr.be->name);
		vr.be = NULL;
		return;
	}

	{
		float ss = Q_clamp(vr_supersampling->value, 0.5f, 2.0f);

		vr.width = ((int)(w * ss) + 3) & ~3;
		vr.height = ((int)(h * ss) + 3) & ~3;
	}

	vr.enabled = true;
	Cvar_Set("r_vsync", "0"); /* the headset sets the rhythm */
	Cvar_Set("in_touch", "0"); /* no touch screen buttons in the HUD */
	Com_Printf("VR: %s, %dx%d for an eye (recommended %dx%d)\n", vr.be->name, vr.width, vr.height, w, h);
}

void
VR_RefLoaded(void *reflib)
{
	vr.ref_SetSize = (void (*)(int, int))Sys_GetProcAddress(reflib, "OM_VR_SetSize");
	vr.ref_Bind = (void (*)(unsigned, int))Sys_GetProcAddress(reflib, "OM_VR_Bind");
	vr.ref_Done = (void (*)(unsigned))Sys_GetProcAddress(reflib, "OM_VR_Done");
	vr_ref_Target = (unsigned (*)(unsigned, int, int))Sys_GetProcAddress(reflib, "OM_VR_Target");
	vr_ref_NewTexture = (unsigned (*)(int, int))Sys_GetProcAddress(reflib, "OM_VR_NewTexture");
	vr_ref_ReadPixels = (void (*)(unsigned, int, int, unsigned char *))Sys_GetProcAddress(reflib, "OM_VR_ReadPixels");

	if (vr.enabled && (!vr.ref_SetSize || !vr.ref_Bind || !vr_ref_Target))
	{
		Com_Printf("VR: this renderer cannot draw for a headset\n");
		vr.enabled = false;
	}

	if (vr.ref_SetSize)
	{
		vr.ref_SetSize(vr.enabled ? vr.width : 0, vr.enabled ? vr.height : 0);
	}
}

void
VR_ContextReady(void)
{
	if (!vr.enabled || vr.session)
	{
		return;
	}

	if (!vr.be->SessionStart(vr.width, vr.height))
	{
		Com_Printf("VR: the session did not start\n");
		return;
	}

	vr.session = true;
}

void
VR_ContextLost(void)
{
	if (vr.session)
	{
		vr.be->SessionEnd();
		vr.session = false;
		vr.frame = vr.began = false;
	}
}

void
VR_Shutdown(void)
{
	VR_ContextLost();

	if (vr.be)
	{
		vr.be->Shutdown();
	}

	vr.enabled = false;
	vr.be = NULL;
}

/* ---- the controls ----------------------------------------------------------------- */

enum
{
	H_ATTACK = 1, H_ALTATTACK = 2, H_CROUCH = 4, H_JUMP = 8, H_RUN = 16, H_USE = 32
};

static void
Hold(unsigned bit, qboolean down, const char *command)
{
	if (down == ((vr.held & bit) != 0))
	{
		return;
	}

	vr.held ^= bit;
	Cbuf_AddText(va("%c%s\n", down ? '+' : '-', command));
}

static qboolean
Pressed(const vr_hand_t *now, const vr_hand_t *before, unsigned button)
{
	return (now->buttons & button) && !(before->buttons & button);
}

static void
KeyChange(const vr_hand_t *now, const vr_hand_t *before, unsigned button, int key)
{
	if ((now->buttons ^ before->buttons) & button)
	{
		Key_Event(key, (now->buttons & button) != 0, true);
	}
}

static void
StickKey(float now, float before, float at, int key)
{
	qboolean a = (at > 0) ? (now > at) : (now < at), b = (at > 0) ? (before > at) : (before < at);

	if (a != b)
	{
		Key_Event(key, a, true);
	}
}

/* the controller as a pointer on the floating screen: where its ray meets it, in screen pixels */
static qboolean
ScreenPointer(const vr_pose_t *hand, int *x, int *y)
{
	static const float f0[3] = {0, 0, -1}, n0[3] = {0, 0, 1}, r0[3] = {1, 0, 0};
	float d[3], n[3], r[3], rel[3], hit[3], t, dn, u, v;

	if (!hand->valid || !vr.screen_mode)
	{
		return false;
	}

	QuatRotate(hand->q, f0, d);
	QuatRotate(vr.screen_pose.q, n0, n);
	QuatRotate(vr.screen_pose.q, r0, r);
	dn = DotProduct(d, n);

	if (dn > -0.01f)
	{
		return false;
	}

	VectorSubtract(vr.screen_pose.pos, hand->pos, rel);
	t = DotProduct(rel, n) / dn;
	VectorMA(hand->pos, t, d, hit);
	VectorSubtract(hit, vr.screen_pose.pos, rel);
	u = DotProduct(rel, r) / vr.screen_w + 0.5f;
	v = 0.5f - rel[1] / vr.screen_h;

	if ((u < 0) || (u > 1) || (v < 0) || (v > 1))
	{
		return false;
	}

	*x = (int)(u * viddef.width);
	*y = (int)(v * viddef.height);
	return true;
}

static float
StickCurve(float v)
{
	float a = fabsf(v), dead = 0.1f;

	if (a <= dead)
	{
		return 0;
	}

	a = powf((a - dead) / (1.0f - dead), 2.2f);
	return (v < 0) ? -a : a;
}

static float stick_forward, stick_side;

/* Dr. Beef's layout with Soldier of Fortune's commands:
   weapon hand  trigger fire, grip alternate fire, stick left / right turn, up / down previous / next weapon,
                stick press use the item, lower button crouch, upper button jump
   off hand     stick walk, trigger run, grip next item (near the weapon hand: steady the gun with two hands),
                stick press use (doors, switches), lower button objectives, upper button reload, menu button menu
   in menus     point at the screen and pull a trigger; sticks move, lower button accepts, upper button goes back */
static void
VR_Controls(void)
{
	qboolean lefty = (vr_control_scheme->value >= 10);
	const vr_hand_t *gun = &vr.in.hand[lefty ? 0 : 1], *off = &vr.in.hand[lefty ? 1 : 0];
	const vr_hand_t *ogun = &vr.old.hand[lefty ? 0 : 1], *ooff = &vr.old.hand[lefty ? 1 : 0];
	qboolean game = (cls.key_dest == key_game) && (cls.state == ca_active) && !(cl.cinematictime > 0);

	stick_forward = stick_side = 0;

	if (!vr.have_old)
	{
		return;
	}

	/* the menu button is Escape everywhere */
	KeyChange(off, ooff, VR_B_MENU, K_ESCAPE);
	KeyChange(gun, ogun, VR_B_MENU, K_ESCAPE);

	if (!game)
	{
		int x, y;

		Hold(H_ATTACK, false, "attack");
		Hold(H_ALTATTACK, false, "altattack");
		Hold(H_CROUCH, false, "movedown");
		Hold(H_JUMP, false, "moveup");
		Hold(H_RUN, false, "speed");
		Hold(H_USE, false, "use");

		if (cls.key_dest == key_menu)
		{
			if (ScreenPointer(&gun->pose, &x, &y) || ScreenPointer(&off->pose, &x, &y))
			{
				M_MouseAbs(x, y);
			}

			KeyChange(gun, ogun, VR_B_TRIGGER, K_MOUSE1);
			KeyChange(off, ooff, VR_B_TRIGGER, K_MOUSE1);
		}
		else
		{
			/* a cutscene, a film, the console: a trigger is "go on" */
			KeyChange(gun, ogun, VR_B_TRIGGER, K_ENTER);
			KeyChange(off, ooff, VR_B_TRIGGER, K_ENTER);
		}

		KeyChange(gun, ogun, VR_B_LOW, K_ENTER);
		KeyChange(off, ooff, VR_B_LOW, K_ENTER);
		KeyChange(gun, ogun, VR_B_HIGH, K_ESCAPE);
		KeyChange(off, ooff, VR_B_HIGH, K_ESCAPE);
		StickKey(gun->stick[1], ogun->stick[1], 0.7f, K_UPARROW);
		StickKey(gun->stick[1], ogun->stick[1], -0.7f, K_DOWNARROW);
		StickKey(gun->stick[0], ogun->stick[0], 0.7f, K_RIGHTARROW);
		StickKey(gun->stick[0], ogun->stick[0], -0.7f, K_LEFTARROW);
		StickKey(off->stick[1], ooff->stick[1], 0.7f, K_UPARROW);
		StickKey(off->stick[1], ooff->stick[1], -0.7f, K_DOWNARROW);
		StickKey(off->stick[0], ooff->stick[0], 0.7f, K_RIGHTARROW);
		StickKey(off->stick[0], ooff->stick[0], -0.7f, K_LEFTARROW);
		return;
	}

	/* the weapon hand */
	Hold(H_ATTACK, (gun->buttons & VR_B_TRIGGER) != 0, "attack");
	Hold(H_ALTATTACK, (gun->buttons & VR_B_GRIP) != 0, "altattack");
	Hold(H_CROUCH, (gun->buttons & VR_B_LOW) != 0, "movedown");
	Hold(H_JUMP, (gun->buttons & VR_B_HIGH) != 0, "moveup");

	if (Pressed(gun, ogun, VR_B_STICK))
	{
		Cbuf_AddText("itemuse\n");
	}

	if ((fabsf(gun->stick[0]) < 0.3f) && (fabsf(gun->stick[1]) > 0.8f))
	{
		if (!vr.switched)
		{
			vr.switched = true;
			Cbuf_AddText((gun->stick[1] > 0) ? "weapprev\n" : "weapnext\n");
		}
	}
	else if (fabsf(gun->stick[1]) < 0.5f)
	{
		vr.switched = false;
	}

	/* turning: in steps, or smoothly */
	if (vr_snapturn_angle->value > 10.0f)
	{
		if (fabsf(gun->stick[0]) > 0.6f)
		{
			if (!vr.snapped)
			{
				vr.snapped = true;
				vr.snap = AngleNorm(vr.snap - ((gun->stick[0] > 0) ? 1 : -1) * vr_snapturn_angle->value);
			}
		}
		else if (fabsf(gun->stick[0]) < 0.4f)
		{
			vr.snapped = false;
		}
	}
	else if (fabsf(gun->stick[0]) > vr_turn_deadzone->value)
	{
		float rate = (vr_snapturn_angle->value < 1.0f) ? 1.0f : vr_snapturn_angle->value;

		vr.snap = AngleNorm(vr.snap - gun->stick[0] * rate * 30.0f * cls.rframetime);
	}

	/* the off hand */
	Hold(H_RUN, (off->buttons & VR_B_TRIGGER) != 0, "speed");
	Hold(H_USE, (off->buttons & VR_B_STICK) != 0, "use");

	if (Pressed(off, ooff, VR_B_HIGH))
	{
		Cbuf_AddText("reload\n");
	}

	if ((off->buttons ^ ooff->buttons) & VR_B_LOW)
	{
		Key_Event(K_TAB, (off->buttons & VR_B_LOW) != 0, true); /* the objectives while held */
	}

	if ((off->buttons ^ ooff->buttons) & VR_B_GRIP)
	{
		/* a grip close to the weapon hand steadies the gun (it then points from the weapon hand to the off hand) */
		float d[3];

		VectorSubtract(off->pose.pos, gun->pose.pos, d);

		if (!(off->buttons & VR_B_GRIP))
		{
			Cvar_SetValue("vr_weapon_stabilised", 0);
		}
		else if (off->pose.valid && gun->pose.valid && (VectorLength(d) < 0.5f))
		{
			Cvar_SetValue("vr_weapon_stabilised", 1);
		}
		else
		{
			Cbuf_AddText("itemnext\n");
		}
	}

	/* walking */
	{
		float x = StickCurve(off->stick[0]), y = StickCurve(off->stick[1]);

		if (!vr_walkdirection->value && off->pose.valid)
		{
			float a[3], turn, c, s;

			PoseAngles(off->pose.q, 0, a);
			turn = DEG2RAD(a[YAW] - vr.head_angles[YAW]);
			c = cosf(turn);
			s = sinf(turn);
			stick_forward = y * c + x * s;
			stick_side = x * c - y * s;
		}
		else
		{
			stick_forward = y;
			stick_side = x;
		}
	}
}

/* ---- a frame ---------------------------------------------------------------------- */

void
VR_FrameStart(void)
{
	float l[3];

	if (!vr.session)
	{
		return;
	}

	if (!vr.began)
	{
		if (vr.have_old)
		{
			vr.old = vr.in;
		}

		vr.frame = vr.be->FrameBegin(&vr.in);
		vr.began = true;
		vr.built = false;
		memset(vr.drawn, 0, sizeof(vr.drawn));

		if (!vr.in.head.valid)
		{
			/* the headset is not tracking (not worn): keep the last pose */
			vr.in.head = vr.old.head;
			vr.in.eye[0] = vr.old.eye[0];
			vr.in.eye[1] = vr.old.eye[1];

			if (!vr.in.head.valid)
			{
				vr.in.head.q[3] = vr.in.eye[0].q[3] = vr.in.eye[1].q[3] = 1;
				vr.in.head.pos[1] = vr.in.eye[0].pos[1] = vr.in.eye[1].pos[1] = EYE_HEIGHT_M;
			}
		}
	}

	PoseAngles(vr.in.head.q, 0, vr.head_angles);
	RoomToLocal(vr.in.head.pos, l);
	VectorCopy(l, vr.head_local);

	if (vr.have_last)
	{
		float dx = l[0] - vr.last_local[0], dy = l[1] - vr.last_local[1];

		/* a jump of the tracking (recentring, the headset put on) is not a step */
		if ((dx * dx + dy * dy) < 0.25f * 0.25f)
		{
			vr.move_local[0] += dx;
			vr.move_local[1] += dy;
		}
	}

	VectorCopy(l, vr.last_local);
	vr.have_last = true;

	/* a fresh view of everything: the same field of view for both eyes, wide enough for either */
	{
		float tx = 0, ty = 0;
		int e, k;

		for (e = 0; e < 2; e++)
		{
			for (k = 0; k < 4; k++)
			{
				float t = fabsf(tanf(vr.in.fov[e][k]));

				if (k < 2)
				{
					tx = Q_max(tx, t);
				}
				else
				{
					ty = Q_max(ty, t);
				}
			}
		}

		if ((tx < 0.1f) || (ty < 0.1f))
		{
			tx = ty = 1.0f;
		}

		vr.tanx = tx;
		vr.tany = ty;
		vr.fovx = RAD2DEG(2.0f * atanf(tx));
		vr.fovy = RAD2DEG(2.0f * atanf(ty));
	}

	VR_Controls();
	vr.have_old = true;
}

/* the movement command: the head turns the player, the stick and the steps in the room move him */
void
VR_Move(usercmd_t *cmd)
{
	float yaw, f, s;

	if (!vr.session)
	{
		return;
	}

	/* the head's yaw and the stick's turns are added to the view yaw as changes: the game and the engine keep
	   their own say in it (spawn points, teleporters, cutscenes) */
	yaw = vr.head_angles[YAW] + vr.snap;

	if (vr.have_yaw)
	{
		cl.viewangles[YAW] = AngleNorm(cl.viewangles[YAW] + AngleNorm(yaw - vr.last_yaw));
	}

	vr.last_yaw = yaw;
	vr.have_yaw = true;
	cl.viewangles[PITCH] = vr.head_angles[PITCH];

	f = stick_forward * cl_forwardspeed->value;
	s = stick_side * cl_sidespeed->value;

	if (vr_positional->value && (cls.nframetime > 0.0001f))
	{
		/* the steps since the last command as a speed: local axes are the room's, the view looks along the head */
		float a = DEG2RAD(vr.head_angles[YAW]), c = cosf(a), sn = sinf(a), k = vr_worldscale->value / cls.nframetime;
		float fwd = (vr.move_local[0] * c + vr.move_local[1] * sn) * k;
		float right = (vr.move_local[0] * sn - vr.move_local[1] * c) * k;

		f += Q_clamp(fwd, -300.0f, 300.0f);
		s += Q_clamp(right, -300.0f, 300.0f);
	}

	vr.move_local[0] = vr.move_local[1] = 0;
	cmd->forwardmove = (short)Q_clamp(cmd->forwardmove + f, -400.0f, 400.0f);
	cmd->sidemove = (short)Q_clamp(cmd->sidemove + s, -400.0f, 400.0f);
}

/* ---- the view ----------------------------------------------------------------------- */

/* cl.refdef of the head (eye -1) or of one eye, from the engine's view of this frame */
static void
ApplyView(int eye)
{
	float gameyaw, ws = vr_worldscale->value;

	if (vr.remote)
	{
		/* a camera of the game: it gives the place and the direction, the head looks around from there */
		VectorCopy(vr.base_org, vr.centre);
	}
	else
	{
		/* the eye height is the head's real height above the floor */
		VectorCopy(vr.base_org, vr.centre);
		vr.centre[2] += (vr.in.head.pos[1] + vr_height_adjust->value - EYE_HEIGHT_M) * ws;
	}

	/* the game's yaw without the head's share (VR_Move adds the head and the stick turns to cl.viewangles), with
	   the stick turns of this moment */
	gameyaw = cl.viewangles[YAW] - (vr.have_yaw ? vr.last_yaw : 0) + vr.snap + vr.head_angles[YAW] +
		SHORT2ANGLE(cl.frame.playerstate.pmove.delta_angles[YAW]);

	if (vr.remote)
	{
		gameyaw = cl.refdef.viewangles[YAW];

		if (!vr.was_remote)
		{
			vr.remote_yaw0 = vr.head_angles[YAW];
		}

		vr.yawoffset = AngleNorm(gameyaw - vr.remote_yaw0);
	}
	else
	{
		vr.yawoffset = AngleNorm(gameyaw - vr.head_angles[YAW]);
	}

	cl.refdef.viewangles[PITCH] = vr.head_angles[PITCH];
	cl.refdef.viewangles[YAW] = AngleNorm(vr.head_angles[YAW] + vr.yawoffset);
	cl.refdef.viewangles[ROLL] = vr.head_angles[ROLL];
	VectorCopy(vr.centre, cl.refdef.vieworg);

	if (eye >= 0)
	{
		float w[3], d[3], l[3];

		VectorSubtract(vr.in.eye[eye].pos, vr.in.head.pos, d);
		RoomToLocal(d, l);
		LocalToWorld(l, w);
		VectorAdd(cl.refdef.vieworg, w, cl.refdef.vieworg);
	}

	cl.refdef.fov_x = vr.fovx;
	cl.refdef.fov_y = vr.fovy;
	AngleVectors(cl.refdef.viewangles, cl.v_forward, cl.v_right, cl.v_up);
}

static void
UpdateWeapon(void)
{
	qboolean lefty = (vr_control_scheme->value >= 10);
	const vr_hand_t *gun = &vr.in.hand[lefty ? 0 : 1], *off = &vr.in.hand[lefty ? 1 : 0];
	float hand[3];

	vr.weapon_valid = false;

	if (!gun->pose.valid || vr.remote || !vr_gunhand->value)
	{
		Cvar_Set("vr_aim", "0");
		return;
	}

	PoseAngles(gun->pose.q, vr_weapon_pitchadjust->value, vr.weapon_angles);
	vr.weapon_angles[YAW] = AngleNorm(vr.weapon_angles[YAW] + vr.yawoffset);
	RoomPointToWorld(gun->pose.pos, hand);

	if (vr_weapon_stabilised->value && off->pose.valid)
	{
		/* two hands: the gun points from the weapon hand to the off hand */
		float o[3], d[3], len;

		RoomPointToWorld(off->pose.pos, o);
		VectorSubtract(o, hand, d);
		len = VectorNormalize(d);

		if (len > 1.0f)
		{
			vr.weapon_angles[PITCH] = RAD2DEG(-asinf(Q_clamp(d[2], -1.0f, 1.0f)));
			vr.weapon_angles[YAW] = RAD2DEG(atan2f(d[1], d[0]));
		}
	}

	VectorCopy(hand, vr.weapon_hand);
	vr.weapon_valid = true;

	/* shots leave the muzzle along the barrel (the muzzle of the model as drawn last frame; else the hand) */
	{
		float f[3], r[3], u[3], m[3];

		AngleVectors(vr.weapon_angles, f, r, u);
		VectorMA(hand, vr.muzzle[0], f, m);
		VectorMA(m, -vr.muzzle[1], r, m);
		VectorMA(m, vr.muzzle[2], u, m);
		VectorCopy(m, vr.muzzle_world);
		Cvar_Set("vr_aim", va("1 %.2f %.2f %.2f %.3f %.3f %.3f %.2f %.2f %.2f", m[0], m[1], m[2],
			vr.weapon_angles[PITCH], vr.weapon_angles[YAW], vr.weapon_angles[ROLL], hand[0], hand[1], hand[2]));
	}
}

/* after the engine made its view of this frame (CL_AddEntities, before the entities are added) */
void
VR_AdjustView(void)
{
	const sofps_t *x = CL_SoF_CurrentPS();

	if (!vr.session)
	{
		return;
	}

	VectorCopy(cl.refdef.vieworg, vr.base_org);
	vr.was_remote = vr.remote;
	vr.remote = (x->remote_type != 0) || CL_CinematicFreezeActive();
	ApplyView(-1);
	UpdateWeapon();
	vr.built = true;

	if (vr_debug->value >= 1)
	{
		/* marks at the hands, to place things by */
		int h;

		for (h = 0; h < 2; h++)
		{
			if (vr.in.hand[h].pose.valid)
			{
				vec3_t w;

				RoomPointToWorld(vr.in.hand[h].pose.pos, w);
				V_AddParticle(w, h ? 0xf2 : 0xd0, 1.0f);
			}
		}
	}

	if (vr_debug->value >= 2)
	{
		Com_Printf("VR view %.1f %.1f %.1f angles %.1f %.1f %.1f yawoffset %.1f\n", cl.refdef.vieworg[0],
			cl.refdef.vieworg[1], cl.refdef.vieworg[2], cl.refdef.viewangles[0], cl.refdef.viewangles[1],
			cl.refdef.viewangles[2], vr.yawoffset);
		Com_Printf("VR gun %d (last frame) at %.1f %.1f %.1f angles %.1f %.1f %.1f head %.1f %.1f %.1f\n", vr.weapon_valid,
			vr.weapon_org[0], vr.weapon_org[1], vr.weapon_org[2], vr.weapon_angles[0], vr.weapon_angles[1],
			vr.weapon_angles[2], vr.head_angles[0], vr.head_angles[1], vr.head_angles[2]);
	}
}

float
VR_WeaponScale(void)
{
	return (vr.session && vr.weapon_valid) ? Q_clamp(vr_weaponscale->value, 0.05f, 2.0f) : 1.0f;
}

qboolean
VR_LeftHanded(void)
{
	return vr_control_scheme && (vr_control_scheme->value >= 10);
}

/* How each weapon's view model has its barrel turned in its own space, degrees: yaw to the left, pitch up.
   Measured from pictures of the models (quest/tools/gunturn.py); index: the weapon type of the player state. */
static const struct
{
	qboolean known;
	float yaw, pitch;
} weapon_turns[16] = {
	{false, 0, 0}
};

/* The view weapon in the hand. The model is made to be seen from the eye of a monitor player: far away, several
   times life size (VR_WeaponScale), and turned inward. It is placed so that its own right hand (grip) is at the
   controller and its barrel points where the controller points: the muzzle straight ahead of the hand. The inward
   turn is measured when the model holds still (a reload swings the gun about in the hand, as it should). */
qboolean
VR_Weapon(int gunid, int guntype, const float *grip, const float *muzzle, float *origin, float *angles)
{
	static int id, still;
	static float lastd[3], turn;
	static qboolean have_turn;
	float g[3] = {0, 0, 0}, adj[3] = {0, 0, 0}, f[3], r[3], u[3], l[3], c, sn, pitchturn;
	int k;

	if (!vr.session || !vr.weapon_valid)
	{
		return false;
	}

	if (grip)
	{
		VectorCopy(grip, g);
	}
	else
	{
		/* a model without the hand: about where the hand of a pistol is */
		float sc = VR_WeaponScale();

		VectorSet(g, 34 * sc, -20 * sc, -26 * sc);
	}

	if (gunid != id)
	{
		id = gunid;
		still = 0;
		have_turn = false;
	}

	if (grip && muzzle)
	{
		float d[3];

		VectorSubtract(muzzle, grip, d);

		if ((fabsf(d[0] - lastd[0]) + fabsf(d[1] - lastd[1]) + fabsf(d[2] - lastd[2])) < 0.01f)
		{
			still++;
		}
		else
		{
			still = 0;
		}

		VectorCopy(d, lastd);

		if ((d[0] > 0.5f) && (!have_turn || (still == 8)))
		{
			turn = atan2f(d[1], d[0]);
			have_turn = true;
		}

		/* the muzzle from the hand, with the barrel straight: forward, left (none), up */
		vr.muzzle[0] = sqrtf(d[0] * d[0] + d[1] * d[1]);
		vr.muzzle[1] = 0;
		vr.muzzle[2] = d[2];
	}
	else
	{
		VectorSet(vr.muzzle, 3, 0, 3);

		if (!have_turn)
		{
			turn = 0;
		}
	}

	/* how the model's barrel is turned in its own space: measured for the game's weapons (WeaponTurns), else the
	   guess above; vr_weapon_turn "yaw pitch" overrides */
	{
		float ty = RAD2DEG(turn) * 0.5f, tp = 0;

		if ((guntype > 0) && (guntype < (int)(sizeof(weapon_turns) / sizeof(weapon_turns[0]))) &&
			weapon_turns[guntype].known)
		{
			ty = weapon_turns[guntype].yaw;
			tp = weapon_turns[guntype].pitch;
		}

		if (vr_weapon_turn->string[0])
		{
			sscanf(vr_weapon_turn->string, "%f %f", &ty, &tp);
		}

		turn = DEG2RAD(ty);
		pitchturn = DEG2RAD(tp);
	}

	/* the model's axes: the controller's with the barrel's turn taken out - first the pitch (the barrel up by
	   pitchturn in the model: the model's forward is the barrel's turned down), then the yaw to the left */
	AngleVectors(vr.weapon_angles, f, r, u);
	VectorScale(r, -1, l);
	c = cosf(pitchturn);
	sn = sinf(pitchturn);

	for (k = 0; k < 3; k++)
	{
		float fk = f[k] * c - u[k] * sn, uk = f[k] * sn + u[k] * c;

		f[k] = fk;
		u[k] = uk;
	}

	c = cosf(turn);
	sn = sinf(turn);

	for (k = 0; k < 3; k++)
	{
		float fk = f[k] * c - l[k] * sn, lk = f[k] * sn + l[k] * c;

		f[k] = fk;
		l[k] = lk;
	}

	/* whatever the player likes better (forward, left, up) */
	sscanf(vr_weapon_offset->string, "%f %f %f", &adj[0], &adj[1], &adj[2]);
	VectorAdd(g, adj, g);
	VectorMA(vr.weapon_hand, -g[0], f, vr.weapon_org);
	VectorMA(vr.weapon_org, -g[1], l, vr.weapon_org);
	VectorMA(vr.weapon_org, -g[2], u, vr.weapon_org);
	VectorCopy(vr.weapon_org, origin);
	angles[PITCH] = RAD2DEG(-asinf(Q_clamp(f[2], -1.0f, 1.0f)));
	angles[YAW] = RAD2DEG(atan2f(f[1], f[0]));
	angles[ROLL] = RAD2DEG(atan2f(l[2], u[2])); /* right.z = -left.z */
	return true;
}

/* ---- drawing ----------------------------------------------------------------------- */

qboolean
VR_DrawBegin(void)
{
	if (!vr.session)
	{
		return false;
	}

	VR_FrameStart(); /* a frame drawn outside the client's own (loading): begin it here */

	if (!vr.frame)
	{
		/* nothing to draw (the headset is off the head): the frame still ends, and time must pass */
		vr_submit_t none;

		memset(&none, 0, sizeof(none));
		vr.be->FrameEnd(&none);
		vr.began = false;
		return false;
	}

	return true;
}

static qboolean
BindTarget(int which, int clear)
{
	unsigned tex = vr.be->Texture(which);

	if (!tex)
	{
		return false;
	}

	vr.fbo[which] = vr_ref_Target(tex, vr.width, vr.height);

	if (!vr.fbo[which])
	{
		vr.be->Release(which);
		return false;
	}

	vr.ref_Bind(vr.fbo[which], clear);
	vr.drawn[which] = true;
	return true;
}

static void
FinishTarget(int which)
{
	if (vr.drawn[which] && vr.fbo[which])
	{
		if (vr.ref_Done)
		{
			vr.ref_Done(vr.fbo[which]);
		}

		vr.be->Release(which);
	}
}

qboolean
VR_DrawEye(int eye)
{
	if (vr.eye >= 0)
	{
		FinishTarget(vr.eye);
	}

	vr.eye = eye;

	if (!BindTarget(eye, 1))
	{
		vr.eye = -1;
		return false;
	}

	return true;
}

qboolean
VR_SecondEye(void)
{
	return vr.session && (vr.eye == 1) && vr.built;
}

void
VR_SetEyeView(void)
{
	if (vr.session && (vr.eye >= 0))
	{
		ApplyView(vr.eye);
	}
}

void
VR_DrawScreen(void)
{
	if (vr.eye >= 0)
	{
		FinishTarget(vr.eye);
		vr.eye = -1;
	}

	BindTarget(2, 2);
}

void
VR_DrawEnd(qboolean world)
{
	vr_submit_t sub;
	int e;

	FinishTarget(2);
	vr.ref_Bind(0, 0);
	memset(&sub, 0, sizeof(sub));
	sub.world = world && vr.drawn[0] && vr.drawn[1];

	for (e = 0; e < 2; e++)
	{
		sub.eye[e] = vr.in.eye[e];
	}

	sub.fov[0] = -atanf(vr.tanx);
	sub.fov[1] = atanf(vr.tanx);
	sub.fov[2] = atanf(vr.tany);
	sub.fov[3] = -atanf(vr.tany);

	/* menus, the console, films, loading: a screen that stands in the room where the player looked when it came up;
	   in the game the same picture is the HUD and goes with the head */
	vr.was_screen_mode = vr.screen_mode;
	vr.screen_mode = (cls.state != ca_active) || (cls.key_dest != key_game) || (cl.cinematictime > 0) ||
		cls.disable_screen || !world;

	if (vr.screen_mode)
	{
		vr.screen_w = vr_screen_size->value;
		vr.screen_h = vr.screen_w * (float)vr.height / (float)vr.width;

		if (!vr.was_screen_mode)
		{
			float yaw = DEG2RAD(vr.head_angles[YAW]), d = vr_screen_depth->value;

			/* local yaw is about the room's y axis; forward is (-sin, 0, -cos) */
			vr.screen_pose.q[0] = 0;
			vr.screen_pose.q[1] = sinf(yaw * 0.5f);
			vr.screen_pose.q[2] = 0;
			vr.screen_pose.q[3] = cosf(yaw * 0.5f);
			vr.screen_pose.pos[0] = vr.in.head.pos[0] - sinf(yaw) * d;
			vr.screen_pose.pos[1] = vr.in.head.pos[1];
			vr.screen_pose.pos[2] = vr.in.head.pos[2] - cosf(yaw) * d;
			vr.screen_pose.valid = true;
		}

		sub.screen_pose = vr.screen_pose;
		sub.screen_size[0] = vr.screen_w;
		sub.screen_size[1] = vr.screen_h;
	}
	else
	{
		sub.screen_head = true;
		sub.screen_pose.q[3] = 1;
		sub.screen_pose.pos[2] = -vr_hud_depth->value;
		sub.screen_size[0] = vr_hud_size->value;
		sub.screen_size[1] = vr_hud_size->value * (float)vr.height / (float)vr.width;
	}

	sub.screen = vr.drawn[2];
	vr.be->FrameEnd(&sub);
	vr.began = vr.frame = false;
	vr.eye = -1;
}
