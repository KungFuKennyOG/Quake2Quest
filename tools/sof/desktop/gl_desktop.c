/*
 * gl_desktop.c - the few OpenGL ES 1 calls the GL1 renderer makes that a desktop
 * (compatibility profile) OpenGL context only has under their double precision names.
 * Linked into the desktop renderer that really draws (build_ref_gl.sh); the headless
 * client gives it an offscreen Mesa context (SOF_GL=1) and saves frames as pictures.
 */
typedef float GLfloat;
typedef double GLdouble;
void glOrtho(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glFrustum(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glDepthRange(GLdouble, GLdouble);

void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) { glOrtho(l, r, b, t, n, f); }
void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) { glFrustum(l, r, b, t, n, f); }
void glDepthRangef(GLfloat n, GLfloat f) { glDepthRange(n, f); }
