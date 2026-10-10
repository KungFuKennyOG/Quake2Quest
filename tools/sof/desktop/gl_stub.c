/*
 * gl_stub.c - a fake OpenGL ES 1 for running the real GL1 renderer on a desktop
 * without a GPU, under AddressSanitizer. Nothing is drawn, but every texture
 * upload and draw call reads all the memory it is handed, so the renderer's
 * buffer overruns show up as ASan reports instead of crashes on the headset.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLclampf;

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_ALPHA 0x1906
#define GL_LUMINANCE 0x1909
#define GL_BGRA 0x80E1
#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

volatile unsigned gl_stub_sink;

static void touch(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned s = 0;
	size_t i;
	if (!p) return;
	for (i = 0; i < n; i++) s += b[i];
	gl_stub_sink += s;
}

static int components(GLenum format)
{
	switch (format)
	{
		case GL_RGBA: case GL_BGRA: return 4;
		case GL_RGB: return 3;
		case GL_ALPHA: case GL_LUMINANCE: return 1;
		default: return 4;
	}
}

static int unpack_align = 4;

const GLubyte *glGetString(GLenum name)
{
	switch (name)
	{
		case GL_VENDOR: return (const GLubyte *)"gl_stub";
		case GL_RENDERER: return (const GLubyte *)"gl_stub (no GPU)";
		case GL_VERSION: return (const GLubyte *)"2.0 gl_stub";
		case GL_EXTENSIONS: return (const GLubyte *)"GL_ARB_texture_non_power_of_two";
	}
	return (const GLubyte *)"";
}
/* matrix stacks and viewport, so code that reads them back (projection of points to
   the screen) can be tested; column-major like OpenGL */
#include <math.h>
static GLfloat mstack[2][32][16];
static int mdepth[2], mmode;
static GLint viewport[4] = { 0, 0, 1280, 720 };
static GLfloat *cur(void) { return mstack[mmode][mdepth[mmode]]; }
static void mident(GLfloat *m) { int i; for (i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
static void mmul(const GLfloat *b)
{
	GLfloat *a = cur(), r[16];
	int i, j, k;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
		{
			r[j * 4 + i] = 0;
			for (k = 0; k < 4; k++) r[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
		}
	memcpy(a, r, sizeof(r));
}
void glMatrixMode(GLenum m) { mmode = (m == 0x1701 /* GL_PROJECTION */) ? 1 : 0; }
void glLoadIdentity(void) { mident(cur()); }
void glLoadMatrixf(const GLfloat *m) { touch(m, 16 * sizeof(GLfloat)); memcpy(cur(), m, 16 * sizeof(GLfloat)); }
void glMultMatrixf(const GLfloat *m) { touch(m, 16 * sizeof(GLfloat)); mmul(m); }
void glPushMatrix(void) { if (mdepth[mmode] < 31) { memcpy(mstack[mmode][mdepth[mmode] + 1], cur(), 16 * sizeof(GLfloat)); mdepth[mmode]++; } }
void glPopMatrix(void) { if (mdepth[mmode] > 0) mdepth[mmode]--; }
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { GLfloat m[16]; mident(m); m[12] = x; m[13] = y; m[14] = z; mmul(m); }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { GLfloat m[16]; mident(m); m[0] = x; m[5] = y; m[10] = z; mmul(m); }
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
	GLfloat m[16], l = sqrtf(x * x + y * y + z * z), c, s;
	if (l <= 0) return;
	x /= l; y /= l; z /= l;
	a *= 3.14159265f / 180.0f; c = cosf(a); s = sinf(a);
	mident(m);
	m[0] = x * x * (1 - c) + c;     m[4] = x * y * (1 - c) - z * s; m[8] = x * z * (1 - c) + y * s;
	m[1] = y * x * (1 - c) + z * s; m[5] = y * y * (1 - c) + c;     m[9] = y * z * (1 - c) - x * s;
	m[2] = x * z * (1 - c) - y * s; m[6] = y * z * (1 - c) + x * s; m[10] = z * z * (1 - c) + c;
	mmul(m);
}
void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
	GLfloat m[16];
	memset(m, 0, sizeof(m));
	m[0] = 2 * n / (r - l); m[5] = 2 * n / (t - b);
	m[8] = (r + l) / (r - l); m[9] = (t + b) / (t - b); m[10] = -(f + n) / (f - n); m[11] = -1;
	m[14] = -2 * f * n / (f - n);
	mmul(m);
}
void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
	GLfloat m[16];
	mident(m);
	m[0] = 2 / (r - l); m[5] = 2 / (t - b); m[10] = -2 / (f - n);
	m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b); m[14] = -(f + n) / (f - n);
	mmul(m);
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { viewport[0] = x; viewport[1] = y; viewport[2] = w; viewport[3] = h; }
void glGetFloatv(GLenum p, GLfloat *v)
{
	if (!v) return;
	if (p == 0x0BA6 /* GL_MODELVIEW_MATRIX */) memcpy(v, mstack[0][mdepth[0]], 16 * sizeof(GLfloat));
	else if (p == 0x0BA7 /* GL_PROJECTION_MATRIX */) memcpy(v, mstack[1][mdepth[1]], 16 * sizeof(GLfloat));
	else { int i; for (i = 0; i < 16; i++) v[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
}
void glGetIntegerv(GLenum p, GLint *v)
{
	if (!v) return;
	if (p == 0x0BA2 /* GL_VIEWPORT */) memcpy(v, viewport, sizeof(viewport));
	else v[0] = 4096;
}

void glPixelStorei(GLenum p, GLint v) { if (p == 0x0CF5 /* GL_UNPACK_ALIGNMENT */) unpack_align = v; }

static void touch_pixels(GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data)
{
	size_t bpp = (size_t)components(format) * (type == GL_UNSIGNED_BYTE ? 1 : 4);
	size_t row = (size_t)w * bpp;
	if (unpack_align > 1) row = (row + unpack_align - 1) / unpack_align * unpack_align;
	if (w > 0 && h > 0) touch(data, row * (h - 1) + (size_t)w * bpp);
}
void glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d)
{ (void)t; (void)l; (void)ifmt; (void)b; touch_pixels(w, h, f, ty, d); }
void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *d)
{ (void)t; (void)l; (void)x; (void)y; touch_pixels(w, h, f, ty, d); }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, void *d)
{ (void)x; (void)y; if (d && w > 0 && h > 0) memset(d, 0, (size_t)w * h * components(f) * (ty == GL_UNSIGNED_BYTE ? 1 : 4)); }

/* client arrays */
typedef struct { int on, size; GLenum type; GLsizei stride; const void *ptr; } arr_t;
static arr_t va, ca, ta;
static arr_t *arr_for(GLenum a) { return a == GL_VERTEX_ARRAY ? &va : a == GL_COLOR_ARRAY ? &ca : a == GL_TEXTURE_COORD_ARRAY ? &ta : NULL; }
void glEnableClientState(GLenum a) { arr_t *p = arr_for(a); if (p) p->on = 1; }
void glDisableClientState(GLenum a) { arr_t *p = arr_for(a); if (p) p->on = 0; }
static void setp(arr_t *a, GLint s, GLenum t, GLsizei st, const void *p) { a->size = s; a->type = t; a->stride = st; a->ptr = p; }
void glVertexPointer(GLint s, GLenum t, GLsizei st, const void *p) { setp(&va, s, t, st, p); }
void glColorPointer(GLint s, GLenum t, GLsizei st, const void *p) { setp(&ca, s, t, st, p); }
void glTexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p) { setp(&ta, s, t, st, p); }
static void touch_vertex(const arr_t *a, int i)
{
	size_t el = a->type == GL_UNSIGNED_BYTE ? 1 : 4;
	size_t stride = a->stride ? (size_t)a->stride : el * a->size;
	if (a->on && a->ptr) touch((const char *)a->ptr + stride * i, el * a->size);
}
static void touch_index(int i) { touch_vertex(&va, i); touch_vertex(&ca, i); touch_vertex(&ta, i); }
void glDrawArrays(GLenum m, GLint first, GLsizei count) { int i; (void)m; for (i = first; i < first + count; i++) touch_index(i); }
void glDrawElements(GLenum m, GLsizei count, GLenum type, const void *idx)
{
	int i; (void)m;
	for (i = 0; i < count; i++)
	{
		int v = type == GL_UNSIGNED_SHORT ? ((const unsigned short *)idx)[i] :
		        type == GL_UNSIGNED_INT ? (int)((const unsigned int *)idx)[i] : ((const unsigned char *)idx)[i];
		touch_index(v);
	}
}

/* state calls: nothing to check */
#define NOP(name, args) void name args {}
NOP(glAlphaFunc, (GLenum a, GLfloat b))
NOP(glBindTexture, (GLenum a, GLuint b))
NOP(glBlendFunc, (GLenum a, GLenum b))
NOP(glClear, (GLbitfield a))
NOP(glClearColor, (GLclampf a, GLclampf b, GLclampf c, GLclampf d))
NOP(glClearStencil, (GLint a))
NOP(glColor4f, (GLfloat a, GLfloat b, GLfloat c, GLfloat d))
NOP(glColorMask, (GLboolean a, GLboolean b, GLboolean c, GLboolean d))
NOP(glCullFace, (GLenum a))
void glDeleteTextures(GLsizei n, const GLuint *t) { touch(t, sizeof(GLuint) * (n > 0 ? n : 0)); }
NOP(glDepthFunc, (GLenum a))
NOP(glDepthMask, (GLboolean a))
NOP(glDepthRangef, (GLfloat a, GLfloat b))
NOP(glDisable, (GLenum a))
NOP(glEnable, (GLenum a))
NOP(glFinish, (void))
NOP(glHint, (GLenum a, GLenum b))
NOP(glPointSize, (GLfloat a))
NOP(glPolygonOffset, (GLfloat a, GLfloat b))
NOP(glScissor, (GLint a, GLint b, GLsizei c, GLsizei d))
NOP(glShadeModel, (GLenum a))
NOP(glStencilFunc, (GLenum a, GLint b, GLuint c))
NOP(glStencilOp, (GLenum a, GLenum b, GLenum c))
NOP(glTexEnvf, (GLenum a, GLenum b, GLfloat c))
NOP(glTexEnvi, (GLenum a, GLenum b, GLint c))
NOP(glTexParameteri, (GLenum a, GLenum b, GLint c))
