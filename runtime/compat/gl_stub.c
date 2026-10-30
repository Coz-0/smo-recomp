/* The OpenGL 1.1 functions of compat/GL/gl.h on macOS: nothing. The OpenGL renderer (nvn_gl.c) cannot run on macOS and is
 * never started there (the renderer is Vulkan); these only let its file, which also holds the shared front end, link.
 * glGetError answers GL_NO_ERROR, glGetString NULL. */
#ifndef _WIN32
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "GL/gl.h"
void glAlphaFunc(GLenum func, GLclampf ref) { }
void glBindTexture(GLenum target, GLuint texture) { }
void glClear(GLbitfield mask) { }
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { }
void glClearDepth(GLclampd depth) { }
void glClearStencil(GLint s) { }
void glCullFace(GLenum mode) { }
void glDeleteTextures(GLsizei n, const GLuint *textures) { }
void glDepthFunc(GLenum func) { }
void glDepthMask(GLboolean flag) { }
void glDisable(GLenum cap) { }
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { }
void glEnable(GLenum cap) { }
void glFinish(void) { }
void glFlush(void) { }
void glFrontFace(GLenum mode) { }
void glGenTextures(GLsizei n, GLuint *textures) { }
void glGetBooleanv(GLenum pname, GLboolean *params) { }
GLenum glGetError(void) { return 0; }
void glGetFloatv(GLenum pname, GLfloat *params) { }
void glGetIntegerv(GLenum pname, GLint *params) { }
const GLubyte * glGetString(GLenum name) { return 0; }
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) { }
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) { }
GLboolean glIsEnabled(GLenum cap) { return 0; }
void glLineWidth(GLfloat width) { }
void glLogicOp(GLenum opcode) { }
void glPixelStorei(GLenum pname, GLint param) { }
void glPointSize(GLfloat size) { }
void glPolygonMode(GLenum face, GLenum mode) { }
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels) { }
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { }
void glStencilFunc(GLenum func, GLint ref, GLuint mask) { }
void glTexParameteri(GLenum target, GLenum pname, GLint param) { }
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { }
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels) { }
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) { }
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { }
void *wglGetProcAddress(const char *name) { (void)name; return 0; }
#endif
