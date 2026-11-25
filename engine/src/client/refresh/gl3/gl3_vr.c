/*
 * SoF Quest (VR): render targets for a head mounted display.
 *
 * The client's VR layer (client/vr) draws each eye and the 2D screen into textures it gets from OpenXR. The
 * renderer keeps drawing "to the window": every bind of framebuffer 0 goes to gl3_default_fbo instead, which
 * is the framebuffer of the texture being drawn (OM_VR_Bind), and the frame is not swapped to the window.
 * The functions are looked up by name in the renderer library (client/vid/vid.c -> VR_RefLoaded).
 *
 * This file is part of the SoF Quest port; GPL v2 like the rest of the renderer.
 */

#include "header/local.h"

GLuint gl3_default_fbo = 0; /* what "framebuffer 0" means (0: the window) */
int gl3_vr_width = 0, gl3_vr_height = 0; /* the size of every VR target: the engine's video size */
qboolean gl3_vr_noswap = false;

#define VR_MAX_TARGETS 24

static struct
{
	GLuint tex, fbo, depth;
	int w, h;
} targets[VR_MAX_TARGETS];
static int numtargets;

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif

/* the engine's video size while a headset is used; 0, 0 for the window's own */
Q2_DLL_EXPORTED void
OM_VR_SetSize(int width, int height)
{
	gl3_vr_width = width;
	gl3_vr_height = height;
	gl3_vr_noswap = (width > 0);
}

/* the framebuffer that draws into a colour texture (with a depth and stencil buffer of its own) */
Q2_DLL_EXPORTED unsigned
OM_VR_Target(unsigned tex, int width, int height)
{
	int i;
	GLint oldfbo = 0;
	GLenum status;

	for (i = 0; i < numtargets; i++)
	{
		if ((targets[i].tex == tex) && (targets[i].w == width) && (targets[i].h == height))
		{
			return targets[i].fbo;
		}
	}

	if (numtargets == VR_MAX_TARGETS)
	{
		R_Printf(PRINT_ALL, "OM_VR_Target: too many targets\n");
		return 0;
	}

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldfbo);
	targets[i].tex = tex;
	targets[i].w = width;
	targets[i].h = height;
	glGenRenderbuffers(1, &targets[i].depth);
	glBindRenderbuffer(GL_RENDERBUFFER, targets[i].depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glGenFramebuffers(1, &targets[i].fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, targets[i].fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, targets[i].depth);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)oldfbo);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		R_Printf(PRINT_ALL, "OM_VR_Target: framebuffer incomplete (0x%x)\n", status);
		glDeleteFramebuffers(1, &targets[i].fbo);
		glDeleteRenderbuffers(1, &targets[i].depth);
		return 0;
	}

	numtargets++;
	return targets[i].fbo;
}

/* the following drawing goes to this framebuffer (0: back to the window); clear: 1 black and opaque, 2 clear and
   see-through (the 2D screen floats over the world) */
Q2_DLL_EXPORTED void
OM_VR_Bind(unsigned fbo, int clear)
{
	static int srgbcontrol = -1;

	GL3_DrawCurrent2Dbatch();
	GL3_SoF2D_Flush();
	gl3_default_fbo = fbo;
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	gl3state.ppFBObound = false;

	if (fbo)
	{
		/* the headset's textures are sRGB; the engine's colours already are display values: write them as they are */
		if (srgbcontrol < 0)
		{
#ifdef YQ2_GL3_GLES
			GLint n = 0, k;

			srgbcontrol = 0;
			glGetIntegerv(GL_NUM_EXTENSIONS, &n);

			for (k = 0; k < n; k++)
			{
				const char *e = (const char *)glGetStringi(GL_EXTENSIONS, k);

				if (e && !strcmp(e, "GL_EXT_sRGB_write_control"))
				{
					srgbcontrol = 1;
				}
			}
#else
			srgbcontrol = 1;
#endif
		}

		if (srgbcontrol)
		{
			glDisable(GL_FRAMEBUFFER_SRGB);
		}

		glViewport(0, 0, vid.width, vid.height);
	}

	if (clear)
	{
		glDisable(GL_SCISSOR_TEST);
		glDepthMask(GL_TRUE);
		glStencilMask(0xFF);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0, 0, 0, (clear == 2) ? 0.0f : 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	}
}

/* the depth of a finished target is not needed any more (a saving on tiled GPUs) */
Q2_DLL_EXPORTED void
OM_VR_Done(unsigned fbo)
{
#ifdef YQ2_GL3_GLES
	static const GLenum att[] = {GL_DEPTH_STENCIL_ATTACHMENT};

	GL3_DrawCurrent2Dbatch();
	GL3_SoF2D_Flush();
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, att);
#else
	(void)fbo;
	GL3_DrawCurrent2Dbatch();
	GL3_SoF2D_Flush();
#endif
}

/* desktop test (vr_sim): a texture to draw an eye into, and its pixels (RGBA, bottom row first) */
Q2_DLL_EXPORTED unsigned
OM_VR_NewTexture(int width, int height)
{
	GLuint tex = 0;
	GLint old = 0;

	glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, (GLuint)old);
	return tex;
}

Q2_DLL_EXPORTED void
OM_VR_ReadPixels(unsigned fbo, int width, int height, unsigned char *rgba)
{
	GLint oldfbo = 0;

	GL3_DrawCurrent2Dbatch();
	GL3_SoF2D_Flush();
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldfbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)oldfbo);
}

/* a new GL context: the old framebuffers are gone with it */
void
GL3_VR_ContextLost(void)
{
	numtargets = 0;
	gl3_default_fbo = 0;
}
