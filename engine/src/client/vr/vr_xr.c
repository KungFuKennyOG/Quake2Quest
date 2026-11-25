/*
 * SoF Quest: the headset through OpenXR (Quest: the Khronos loader for Android, the GL context SDL made).
 *
 * Three swap chains: one per eye (a projection layer) and one for the 2D picture (a quad layer: standing in the
 * room for menus, fixed to the head for the HUD). The renderer draws into their textures (gl3_vr.c).
 *
 * GPL v2 like the engine.
 */

#ifdef OM_OPENXR

#include "../header/client.h"
#include "vr.h"

#include <jni.h>
#include <EGL/egl.h>
#include <SDL3/SDL.h>
#include <android/log.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define XLOG(...) __android_log_print(ANDROID_LOG_INFO, "SoFQuestVR", __VA_ARGS__)
#define X_SRGB8_ALPHA8 0x8C43
#define X_RGBA8 0x8058
#define MAX_IMAGES 8

typedef struct
{
	XrSwapchain handle;
	XrSwapchainImageOpenGLESKHR images[MAX_IMAGES];
	uint32_t count, index;
	qboolean acquired, used;
} xchain_t;

static struct
{
	JavaVM *vm;
	jobject activity;
	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSpace stage, view;
	qboolean stage_is_local;
	xchain_t chain[3];
	int width, height;
	qboolean running, focused, began, should_render, was_running;
	XrFrameState frame;
	XrView views[2];
	XrActionSet set;
	XrAction a_pose, a_trigger, a_grip, a_stick, a_stickclick, a_low, a_high, a_menu, a_haptic;
	XrPath hand_path[2];
	XrSpace hand_space[2];
	float floor; /* added to heights when only the LOCAL space (origin at the head) is there */
} xr;

static qboolean
XrOk(XrResult r, const char *what)
{
	if (XR_FAILED(r))
	{
		char msg[XR_MAX_RESULT_STRING_SIZE] = "?";

		if (xr.instance)
		{
			xrResultToString(xr.instance, r, msg);
		}

		XLOG("%s failed: %s (%d)", what, msg, (int)r);
		Com_Printf("VR: %s failed: %s (%d)\n", what, msg, (int)r);
		return false;
	}

	return true;
}

static XrPath
Path(const char *s)
{
	XrPath p = XR_NULL_PATH;

	xrStringToPath(xr.instance, s, &p);
	return p;
}

static qboolean
HasExt(const XrExtensionProperties *p, uint32_t n, const char *name)
{
	uint32_t i;

	for (i = 0; i < n; i++)
	{
		if (!strcmp(p[i].extensionName, name))
		{
			return true;
		}
	}

	return false;
}

static qboolean
XR_Init(int *width, int *height)
{
	JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
	jobject act = (jobject)SDL_GetAndroidActivity();
	PFN_xrInitializeLoaderKHR initloader = NULL;
	XrExtensionProperties *props = NULL;
	uint32_t nprops = 0, i, nviews = 0;
	const char *exts[4];
	uint32_t next = 0;
	XrInstanceCreateInfoAndroidKHR ainfo = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
	XrViewConfigurationView vc[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};

	if (!env || !act)
	{
		return false;
	}

	(*env)->GetJavaVM(env, &xr.vm);
	xr.activity = (*env)->NewGlobalRef(env, act);
	(*env)->DeleteLocalRef(env, act);

	/* the game goes on while the app is paused (the headset is off the head): the session's events must be
	   answered; vr_main.c stops the game's time instead */
	SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");

	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initloader);

	if (initloader)
	{
		XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};

		li.applicationVM = xr.vm;
		li.applicationContext = xr.activity;
		initloader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
	}

	xrEnumerateInstanceExtensionProperties(NULL, 0, &nprops, NULL);

	if (nprops)
	{
		props = calloc(nprops, sizeof(*props));

		for (i = 0; i < nprops; i++)
		{
			props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
		}

		if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, nprops, &nprops, props)))
		{
			nprops = 0;
		}
	}

	exts[next++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	exts[next++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;

	if (HasExt(props, nprops, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
	{
		exts[next++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
	}

	free(props);

	ainfo.applicationVM = xr.vm;
	ainfo.applicationActivity = xr.activity;
	ci.next = &ainfo;
	Q_strlcpy(ci.applicationInfo.applicationName, "SoF Quest", sizeof(ci.applicationInfo.applicationName));
	ci.applicationInfo.applicationVersion = 1;
	Q_strlcpy(ci.applicationInfo.engineName, "openMERC", sizeof(ci.applicationInfo.engineName));
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ci.enabledExtensionCount = next;
	ci.enabledExtensionNames = exts;

	if (!XrOk(xrCreateInstance(&ci, &xr.instance), "xrCreateInstance"))
	{
		xr.instance = XR_NULL_HANDLE;
		return false;
	}

	if (XR_SUCCEEDED(xrGetInstanceProperties(xr.instance, &ip)))
	{
		XLOG("runtime: %s", ip.runtimeName);
		Com_Printf("VR: OpenXR runtime %s\n", ip.runtimeName);
	}

	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	if (!XrOk(xrGetSystem(xr.instance, &si, &xr.system), "xrGetSystem"))
	{
		return false;
	}

	{
		PFN_xrGetOpenGLESGraphicsRequirementsKHR req = NULL;

		xrGetInstanceProcAddr(xr.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req);

		if (req)
		{
			XrGraphicsRequirementsOpenGLESKHR r = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};

			req(xr.instance, xr.system, &r);
		}
	}

	if (!XrOk(xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
			&nviews, vc), "xrEnumerateViewConfigurationViews") || (nviews != 2))
	{
		return false;
	}

	*width = (int)vc[0].recommendedImageRectWidth;
	*height = (int)vc[0].recommendedImageRectHeight;
	XLOG("eye %dx%d", *width, *height);
	return true;
}

static qboolean
MakeChain(xchain_t *c, int width, int height, int64_t format)
{
	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	uint32_t i;

	ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	if (!XrOk(xrCreateSwapchain(xr.session, &ci, &c->handle), "xrCreateSwapchain"))
	{
		return false;
	}

	xrEnumerateSwapchainImages(c->handle, 0, &c->count, NULL);

	if (c->count > MAX_IMAGES)
	{
		c->count = MAX_IMAGES;
	}

	for (i = 0; i < c->count; i++)
	{
		c->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
		c->images[i].next = NULL;
	}

	return XrOk(xrEnumerateSwapchainImages(c->handle, c->count, &c->count,
			(XrSwapchainImageBaseHeader *)c->images), "xrEnumerateSwapchainImages");
}

static void
MakeAction(XrActionType type, const char *name, XrAction *a)
{
	XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};

	ci.actionType = type;
	Q_strlcpy(ci.actionName, name, sizeof(ci.actionName));
	Q_strlcpy(ci.localizedActionName, name, sizeof(ci.localizedActionName));
	ci.countSubactionPaths = 2;
	ci.subactionPaths = xr.hand_path;
	XrOk(xrCreateAction(xr.set, &ci, a), name);
}

static void
MakeActions(void)
{
	XrActionSetCreateInfo sci = {XR_TYPE_ACTION_SET_CREATE_INFO};
	XrActionSuggestedBinding b[32];
	XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	uint32_t n = 0;
	int h;

#define BIND(a, p) b[n].action = (a); b[n].binding = Path(p); n++

	xr.hand_path[0] = Path("/user/hand/left");
	xr.hand_path[1] = Path("/user/hand/right");
	Q_strlcpy(sci.actionSetName, "gameplay", sizeof(sci.actionSetName));
	Q_strlcpy(sci.localizedActionSetName, "Gameplay", sizeof(sci.localizedActionSetName));

	if (!XrOk(xrCreateActionSet(xr.instance, &sci, &xr.set), "xrCreateActionSet"))
	{
		return;
	}

	MakeAction(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", &xr.a_pose);
	MakeAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", &xr.a_trigger);
	MakeAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", &xr.a_grip);
	MakeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", &xr.a_stick);
	MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", &xr.a_stickclick);
	MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "low_button", &xr.a_low);
	MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "high_button", &xr.a_high);
	MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", &xr.a_menu);
	MakeAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", &xr.a_haptic);

	BIND(xr.a_pose, "/user/hand/left/input/aim/pose");
	BIND(xr.a_pose, "/user/hand/right/input/aim/pose");
	BIND(xr.a_trigger, "/user/hand/left/input/trigger/value");
	BIND(xr.a_trigger, "/user/hand/right/input/trigger/value");
	BIND(xr.a_grip, "/user/hand/left/input/squeeze/value");
	BIND(xr.a_grip, "/user/hand/right/input/squeeze/value");
	BIND(xr.a_stick, "/user/hand/left/input/thumbstick");
	BIND(xr.a_stick, "/user/hand/right/input/thumbstick");
	BIND(xr.a_stickclick, "/user/hand/left/input/thumbstick/click");
	BIND(xr.a_stickclick, "/user/hand/right/input/thumbstick/click");
	BIND(xr.a_low, "/user/hand/left/input/x/click");
	BIND(xr.a_low, "/user/hand/right/input/a/click");
	BIND(xr.a_high, "/user/hand/left/input/y/click");
	BIND(xr.a_high, "/user/hand/right/input/b/click");
	BIND(xr.a_menu, "/user/hand/left/input/menu/click");
	BIND(xr.a_haptic, "/user/hand/left/output/haptic");
	BIND(xr.a_haptic, "/user/hand/right/output/haptic");
#undef BIND

	sb.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
	sb.countSuggestedBindings = n;
	sb.suggestedBindings = b;
	XrOk(xrSuggestInteractionProfileBindings(xr.instance, &sb), "xrSuggestInteractionProfileBindings");

	for (h = 0; h < 2; h++)
	{
		XrActionSpaceCreateInfo as = {XR_TYPE_ACTION_SPACE_CREATE_INFO};

		as.action = xr.a_pose;
		as.subactionPath = xr.hand_path[h];
		as.poseInActionSpace.orientation.w = 1;
		XrOk(xrCreateActionSpace(xr.session, &as, &xr.hand_space[h]), "xrCreateActionSpace");
	}

	at.countActionSets = 1;
	at.actionSets = &xr.set;
	XrOk(xrAttachSessionActionSets(xr.session, &at), "xrAttachSessionActionSets");
}

static void XR_SessionEnd(void);

static qboolean
XR_SessionStart(int width, int height)
{
	XrGraphicsBindingOpenGLESAndroidKHR gb = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	EGLint cfgid = 0, ncfg = 0, attr[3];
	int64_t formats[64], format = 0;
	uint32_t nformats = 0, i;

	gb.display = eglGetCurrentDisplay();
	gb.context = eglGetCurrentContext();

	if ((gb.display == EGL_NO_DISPLAY) || (gb.context == EGL_NO_CONTEXT))
	{
		Com_Printf("VR: no EGL context\n");
		return false;
	}

	eglQueryContext(gb.display, gb.context, EGL_CONFIG_ID, &cfgid);
	attr[0] = EGL_CONFIG_ID;
	attr[1] = cfgid;
	attr[2] = EGL_NONE;
	eglChooseConfig(gb.display, attr, &gb.config, 1, &ncfg);

	sci.next = &gb;
	sci.systemId = xr.system;

	if (!XrOk(xrCreateSession(xr.instance, &sci, &xr.session), "xrCreateSession"))
	{
		xr.session = XR_NULL_HANDLE;
		return false;
	}

	{
		PFN_xrRequestDisplayRefreshRateFB rate = NULL;
		float hz = Cvar_Get("vr_refresh", "72", CVAR_ARCHIVE)->value;

		xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&rate);

		if (rate && (hz >= 60))
		{
			XLOG("refresh %g Hz: %d", hz, (int)rate(xr.session, hz));
		}
	}

	rs.poseInReferenceSpace.orientation.w = 1;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	xr.stage_is_local = false;

	if (XR_FAILED(xrCreateReferenceSpace(xr.session, &rs, &xr.stage)))
	{
		rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		xr.stage_is_local = true;

		if (!XrOk(xrCreateReferenceSpace(xr.session, &rs, &xr.stage), "xrCreateReferenceSpace"))
		{
			XR_SessionEnd();
			return false;
		}
	}

	xr.floor = xr.stage_is_local ? 1.57f : 0.0f;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XrOk(xrCreateReferenceSpace(xr.session, &rs, &xr.view), "xrCreateReferenceSpace(view)");

	xrEnumerateSwapchainFormats(xr.session, 64, &nformats, formats);

	for (i = 0; i < nformats; i++)
	{
		if (formats[i] == X_SRGB8_ALPHA8)
		{
			format = X_SRGB8_ALPHA8;
			break;
		}

		if ((formats[i] == X_RGBA8) && !format)
		{
			format = X_RGBA8;
		}
	}

	if (!format && nformats)
	{
		format = formats[0];
	}

	XLOG("swap chain format 0x%x, %dx%d", (int)format, width, height);
	xr.width = width;
	xr.height = height;

	for (i = 0; i < 3; i++)
	{
		if (!MakeChain(&xr.chain[i], width, height, format))
		{
			XR_SessionEnd();
			return false;
		}
	}

	MakeActions();
	xr.views[0].type = xr.views[1].type = XR_TYPE_VIEW;
	return true;
}

static void
XR_SessionEnd(void)
{
	int i;

	for (i = 0; i < 2; i++)
	{
		if (xr.hand_space[i])
		{
			xrDestroySpace(xr.hand_space[i]);
			xr.hand_space[i] = XR_NULL_HANDLE;
		}
	}

	if (xr.set)
	{
		xrDestroyActionSet(xr.set);
		xr.set = XR_NULL_HANDLE;
	}

	for (i = 0; i < 3; i++)
	{
		if (xr.chain[i].handle)
		{
			xrDestroySwapchain(xr.chain[i].handle);
		}

		memset(&xr.chain[i], 0, sizeof(xr.chain[i]));
	}

	if (xr.view)
	{
		xrDestroySpace(xr.view);
		xr.view = XR_NULL_HANDLE;
	}

	if (xr.stage)
	{
		xrDestroySpace(xr.stage);
		xr.stage = XR_NULL_HANDLE;
	}

	if (xr.session)
	{
		xrDestroySession(xr.session);
		xr.session = XR_NULL_HANDLE;
	}

	xr.running = xr.began = false;
}

static void
XR_Shutdown(void)
{
	XR_SessionEnd();

	if (xr.instance)
	{
		xrDestroyInstance(xr.instance);
		xr.instance = XR_NULL_HANDLE;
	}
}

static void
Events(void)
{
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};

	while (xrPollEvent(xr.instance, &ev) == XR_SUCCESS)
	{
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;

			XLOG("session state %d", (int)sc->state);

			switch (sc->state)
			{
				case XR_SESSION_STATE_READY:
				{
					XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};

					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					xr.running = XrOk(xrBeginSession(xr.session, &bi), "xrBeginSession");
					break;
				}
				case XR_SESSION_STATE_STOPPING:
					xrEndSession(xr.session);
					xr.running = false;
					break;
				case XR_SESSION_STATE_FOCUSED:
					xr.focused = true;
					break;
				case XR_SESSION_STATE_VISIBLE:
				case XR_SESSION_STATE_SYNCHRONIZED:
					xr.focused = false;
					break;
				case XR_SESSION_STATE_EXITING:
				case XR_SESSION_STATE_LOSS_PENDING:
					xr.running = false;
					Cbuf_AddText("quit\n");
					break;
				default:
					break;
			}
		}

		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		ev.next = NULL;
	}
}

static void
CopyPose(const XrPosef *p, vr_pose_t *o)
{
	o->pos[0] = p->position.x;
	o->pos[1] = p->position.y + xr.floor;
	o->pos[2] = p->position.z;
	o->q[0] = p->orientation.x;
	o->q[1] = p->orientation.y;
	o->q[2] = p->orientation.z;
	o->q[3] = p->orientation.w;
	o->valid = true;
}

static qboolean
GetBool(XrAction a, int hand)
{
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};

	gi.action = a;
	gi.subactionPath = xr.hand_path[hand];
	return XR_SUCCEEDED(xrGetActionStateBoolean(xr.session, &gi, &s)) && s.isActive && s.currentState;
}

static float
GetFloat(XrAction a, int hand)
{
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};

	gi.action = a;
	gi.subactionPath = xr.hand_path[hand];
	return (XR_SUCCEEDED(xrGetActionStateFloat(xr.session, &gi, &s)) && s.isActive) ? s.currentState : 0.0f;
}

static qboolean
XR_FrameBegin(vr_input_t *in)
{
	XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
	XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
	XrViewState vs = {XR_TYPE_VIEW_STATE};
	XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
	uint32_t nv = 0;
	int e, h;

	memset(in, 0, sizeof(*in));
	Events();

	if (xr.running != xr.was_running)
	{
		/* the headset was taken off or put on: the single player game waits */
		Cvar_Set("paused", xr.running ? "0" : "1");
		xr.was_running = xr.running;
	}

	if (!xr.session || !xr.running)
	{
		usleep(50000);
		return false;
	}

	xr.frame.type = XR_TYPE_FRAME_STATE;
	xr.frame.next = NULL;

	if (XR_FAILED(xrWaitFrame(xr.session, &wi, &xr.frame)) || XR_FAILED(xrBeginFrame(xr.session, &bi)))
	{
		usleep(10000);
		return false;
	}

	xr.began = true;
	xr.should_render = xr.frame.shouldRender != XR_FALSE;

	if (!xr.should_render)
	{
		return false;
	}

	li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	li.displayTime = xr.frame.predictedDisplayTime;
	li.space = xr.stage;

	if (XR_FAILED(xrLocateViews(xr.session, &li, &vs, 2, &nv, xr.views)) || (nv != 2) ||
		!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
	{
		return false;
	}

	for (e = 0; e < 2; e++)
	{
		CopyPose(&xr.views[e].pose, &in->eye[e]);
		in->fov[e][0] = xr.views[e].fov.angleLeft;
		in->fov[e][1] = xr.views[e].fov.angleRight;
		in->fov[e][2] = xr.views[e].fov.angleUp;
		in->fov[e][3] = xr.views[e].fov.angleDown;
	}

	if (XR_SUCCEEDED(xrLocateSpace(xr.view, xr.stage, xr.frame.predictedDisplayTime, &loc)) &&
		(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		CopyPose(&loc.pose, &in->head);
	}
	else
	{
		in->head = in->eye[0];

		for (e = 0; e < 3; e++)
		{
			in->head.pos[e] = (in->eye[0].pos[e] + in->eye[1].pos[e]) * 0.5f;
		}
	}

	if (xr.set)
	{
		XrActiveActionSet as = {xr.set, XR_NULL_PATH};
		XrActionsSyncInfo sy = {XR_TYPE_ACTIONS_SYNC_INFO};

		sy.countActiveActionSets = 1;
		sy.activeActionSets = &as;
		xrSyncActions(xr.session, &sy);

		for (h = 0; h < 2; h++)
		{
			vr_hand_t *hd = &in->hand[h];
			XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
			XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
			XrSpaceLocation hl = {XR_TYPE_SPACE_LOCATION};

			if (xr.hand_space[h] &&
				XR_SUCCEEDED(xrLocateSpace(xr.hand_space[h], xr.stage, xr.frame.predictedDisplayTime, &hl)) &&
				(hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
				(hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
			{
				CopyPose(&hl.pose, &hd->pose);
			}

			gi.action = xr.a_stick;
			gi.subactionPath = xr.hand_path[h];

			if (XR_SUCCEEDED(xrGetActionStateVector2f(xr.session, &gi, &st)) && st.isActive)
			{
				hd->stick[0] = st.currentState.x;
				hd->stick[1] = st.currentState.y;
			}

			hd->trigger = GetFloat(xr.a_trigger, h);
			hd->grip = GetFloat(xr.a_grip, h);
			hd->buttons = ((hd->trigger > 0.5f) ? VR_B_TRIGGER : 0) | ((hd->grip > 0.6f) ? VR_B_GRIP : 0) |
				(GetBool(xr.a_stickclick, h) ? VR_B_STICK : 0) | (GetBool(xr.a_low, h) ? VR_B_LOW : 0) |
				(GetBool(xr.a_high, h) ? VR_B_HIGH : 0) | (GetBool(xr.a_menu, h) ? VR_B_MENU : 0);
		}
	}

	return true;
}

static unsigned
XR_Texture(int which)
{
	xchain_t *c = &xr.chain[which];
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};

	if (!xr.began || !c->handle)
	{
		return 0;
	}

	if (c->acquired)
	{
		return c->images[c->index].image;
	}

	if (XR_FAILED(xrAcquireSwapchainImage(c->handle, &ai, &c->index)))
	{
		return 0;
	}

	wi.timeout = XR_INFINITE_DURATION;

	if (XR_FAILED(xrWaitSwapchainImage(c->handle, &wi)))
	{
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

		xrReleaseSwapchainImage(c->handle, &ri);
		return 0;
	}

	c->acquired = true;
	return c->images[c->index].image;
}

static void
XR_Release(int which)
{
	xchain_t *c = &xr.chain[which];

	if (c->acquired)
	{
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

		xrReleaseSwapchainImage(c->handle, &ri);
		c->acquired = false;
		c->used = true; /* a picture is in the chain: it can be shown */
	}
}

static void
ToXrPose(const vr_pose_t *p, XrPosef *o, qboolean room)
{
	o->position.x = p->pos[0];
	o->position.y = p->pos[1] - (room ? xr.floor : 0.0f);
	o->position.z = p->pos[2];
	o->orientation.x = p->q[0];
	o->orientation.y = p->q[1];
	o->orientation.z = p->q[2];
	o->orientation.w = p->q[3];
}

static void
XR_FrameEnd(const vr_submit_t *sub)
{
	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerProjectionView pv[2];
	XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	const XrCompositionLayerBaseHeader *layers[2];
	uint32_t n = 0;
	int e;

	if (!xr.began)
	{
		return;
	}

	for (e = 0; e < 3; e++)
	{
		XR_Release(e);
	}

	if (xr.should_render && sub->world && xr.chain[0].used && xr.chain[1].used)
	{
		memset(pv, 0, sizeof(pv));

		for (e = 0; e < 2; e++)
		{
			pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			ToXrPose(&sub->eye[e], &pv[e].pose, true);
			pv[e].fov.angleLeft = sub->fov[0];
			pv[e].fov.angleRight = sub->fov[1];
			pv[e].fov.angleUp = sub->fov[2];
			pv[e].fov.angleDown = sub->fov[3];
			pv[e].subImage.swapchain = xr.chain[e].handle;
			pv[e].subImage.imageRect.extent.width = xr.width;
			pv[e].subImage.imageRect.extent.height = xr.height;
		}

		proj.space = xr.stage;
		proj.viewCount = 2;
		proj.views = pv;
		layers[n++] = (const XrCompositionLayerBaseHeader *)&proj;
	}

	if (xr.should_render && sub->screen && xr.chain[2].used)
	{
		quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.space = sub->screen_head ? xr.view : xr.stage;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = xr.chain[2].handle;
		quad.subImage.imageRect.extent.width = xr.width;
		quad.subImage.imageRect.extent.height = xr.height;
		ToXrPose(&sub->screen_pose, &quad.pose, !sub->screen_head);
		quad.size.width = sub->screen_size[0];
		quad.size.height = sub->screen_size[1];
		layers[n++] = (const XrCompositionLayerBaseHeader *)&quad;
	}

	ei.displayTime = xr.frame.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = n;
	ei.layers = n ? layers : NULL;
	xrEndFrame(xr.session, &ei);
	xr.began = false;
}

static void
XR_Haptic(int hand, float seconds, float strength)
{
	XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
	XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};

	if (!xr.session || !xr.running || !xr.a_haptic || (hand < 0) || (hand > 1))
	{
		return;
	}

	hi.action = xr.a_haptic;
	hi.subactionPath = xr.hand_path[hand];

	if (strength <= 0)
	{
		xrStopHapticFeedback(xr.session, &hi);
		return;
	}

	v.amplitude = strength > 1 ? 1 : strength;
	v.duration = (XrDuration)(seconds * 1e9f);
	v.frequency = XR_FREQUENCY_UNSPECIFIED;
	xrApplyHapticFeedback(xr.session, &hi, (const XrHapticBaseHeader *)&v);
}

const vr_backend_t vr_openxr = {
	"OpenXR", XR_Init, XR_SessionStart, XR_SessionEnd, XR_Shutdown, XR_FrameBegin, XR_Texture, XR_Release,
	XR_FrameEnd, XR_Haptic
};

#endif /* OM_OPENXR */
