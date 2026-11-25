/*
 * SoF Quest: virtual reality for the openMERC client, after Dr. Beef's Quake2Quest.
 *
 * vr_main.c  the game side: the head is the camera, the gun is in the hand, the controls
 * vr_xr.c    OpenXR (Quest, or a desktop runtime for testing)
 * vr_sim.c   a headset made of numbers, for testing on a desktop without one
 *
 * GPL v2 like the engine.
 */
#ifndef CL_VR_H
#define CL_VR_H

#include "../../common/header/common.h"

/* ---- for the engine ------------------------------------------------------------- */

void VR_Init(void);                          /* before the video starts: is there a headset? */
void VR_Shutdown(void);
qboolean VR_Enabled(void);                   /* a headset is used (the engine draws for it, not for the window) */
qboolean VR_VideoSize(int *width, int *height); /* the size the engine draws at: one eye's texture */
void VR_RefLoaded(void *reflib);             /* the renderer library was loaded */
void VR_ContextReady(void);                  /* the GL context exists: start the session on it */
void VR_ContextLost(void);

void VR_FrameStart(void);                    /* start of a client frame: wait for the headset, read head and hands */
void VR_Move(usercmd_t *cmd);                /* the head's turn and the sticks into a movement command */
void VR_AdjustView(void);                    /* cl.refdef: the head as the camera (after the engine's view) */

/* drawing a frame (cl_screen.c) */
qboolean VR_DrawBegin(void);                 /* false: the headset takes no picture now */
qboolean VR_DrawEye(int eye);                /* the following 3D view goes to this eye */
void VR_SetEyeView(void);                    /* cl.refdef for the eye being drawn */
qboolean VR_SecondEye(void);                 /* the scene was built for the first eye already */
void VR_DrawScreen(void);                    /* the following 2D drawing goes to the floating screen */
void VR_DrawEnd(qboolean world);             /* hand the frame to the headset */

/* the gun in the hand: where the view weapon is drawn (false: at the eye as on a monitor) */
/* grip, muzzle: the model's right hand and muzzle as posed now (entity space), or NULL */
qboolean VR_Weapon(int gunid, int guntype, const float *grip, const float *muzzle, float *origin, float *angles);
float VR_WeaponScale(void);                 /* the size of the view weapon's model in the hand */
qboolean VR_LeftHanded(void);

/* ---- between vr_main.c and a headset (vr_xr.c, vr_sim.c) ----------------------- */

/* a place and a direction in the room: metres, x right, y up, z back (OpenXR); q = x y z w */
typedef struct
{
	float pos[3];
	float q[4];
	qboolean valid;
} vr_pose_t;

#define VR_B_TRIGGER 0x01
#define VR_B_GRIP    0x02
#define VR_B_STICK   0x04 /* stick pressed */
#define VR_B_LOW     0x08 /* A (right), X (left) */
#define VR_B_HIGH    0x10 /* B (right), Y (left) */
#define VR_B_MENU    0x20

typedef struct
{
	vr_pose_t pose; /* pointing along -z */
	float stick[2]; /* x right, y forward */
	float trigger, grip;
	unsigned buttons;
} vr_hand_t;

typedef struct
{
	vr_pose_t head;
	vr_pose_t eye[2];
	float fov[2][4]; /* per eye: left, right, up, down (radians, left and down negative) */
	vr_hand_t hand[2]; /* 0 left, 1 right */
} vr_input_t;

typedef struct
{
	qboolean world; /* the two eye pictures are to be shown */
	vr_pose_t eye[2]; /* as they were drawn */
	float fov[4]; /* as they were drawn (both eyes) */
	qboolean screen; /* the 2D picture is to be shown */
	qboolean screen_head; /* fixed to the head (the HUD) instead of standing in the room (menus) */
	vr_pose_t screen_pose; /* its centre (in the head's space if screen_head) */
	float screen_size[2]; /* metres */
} vr_submit_t;

typedef struct
{
	const char *name;
	qboolean (*Init)(int *width, int *height); /* is a headset there; the recommended size of an eye's picture */
	qboolean (*SessionStart)(int width, int height); /* with the GL context current */
	void (*SessionEnd)(void);
	void (*Shutdown)(void);
	qboolean (*FrameBegin)(vr_input_t *in); /* waits for the headset's rhythm; false: no frame wanted */
	unsigned (*Texture)(int which); /* 0, 1 the eyes, 2 the screen: the GL texture to draw this frame's into */
	void (*Release)(int which); /* its picture is finished */
	void (*FrameEnd)(const vr_submit_t *submit);
	void (*Haptic)(int hand, float seconds, float strength);
} vr_backend_t;

extern const vr_backend_t vr_sim;
#ifdef OM_OPENXR
extern const vr_backend_t vr_openxr;
#endif

/* the renderer's side (refresh/gl3/gl3_vr.c), for vr_sim.c */
extern unsigned (*vr_ref_NewTexture)(int width, int height);
extern void (*vr_ref_ReadPixels)(unsigned fbo, int width, int height, unsigned char *rgba);
extern unsigned (*vr_ref_Target)(unsigned tex, int width, int height);

#endif
