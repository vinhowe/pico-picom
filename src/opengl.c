// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include "backend/gl/gl_common.h"
#include "backend/gl/glx.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "uthash_extra.h"
#include "utils.h"
#include "win.h"

#include "opengl.h"

#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif

static inline XVisualInfo *get_visualinfo_from_visual(session_t *ps, xcb_visualid_t visual) {
	XVisualInfo vreq = {.visualid = visual};
	int nitems = 0;

	return XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
}

/**
 * Initialize OpenGL.
 */
bool glx_init(session_t *ps, bool need_render) {
	bool success = false;
	XVisualInfo *pvis = NULL;

	// Check for GLX extension
	if (!ps->glx_exists) {
		log_error("No GLX extension.");
		goto glx_init_end;
	}

	// Get XVisualInfo
	pvis = get_visualinfo_from_visual(ps, ps->vis);
	if (!pvis) {
		log_error("Failed to acquire XVisualInfo for current visual.");
		goto glx_init_end;
	}

	// Ensure the visual is double-buffered
	if (need_render) {
		int value = 0;
		if (Success != glXGetConfig(ps->dpy, pvis, GLX_USE_GL, &value) || !value) {
			log_error("Root visual is not a GL visual.");
			goto glx_init_end;
		}

		if (Success != glXGetConfig(ps->dpy, pvis, GLX_DOUBLEBUFFER, &value) || !value) {
			log_error("Root visual is not a double buffered GL visual.");
			goto glx_init_end;
		}
	}

	// Ensure GLX_EXT_texture_from_pixmap exists
	if (need_render && !glxext.has_GLX_EXT_texture_from_pixmap)
		goto glx_init_end;

	// Initialize GLX data structure
	if (!ps->psglx) {
		static const glx_session_t CGLX_SESSION_DEF = CGLX_SESSION_INIT;
		ps->psglx = cmalloc(glx_session_t);
		memcpy(ps->psglx, &CGLX_SESSION_DEF, sizeof(glx_session_t));
	}

	glx_session_t *psglx = ps->psglx;

	if (!psglx->context) {
		// Get GLX context
		psglx->context = glXCreateContext(ps->dpy, pvis, None, GL_TRUE);

		if (!psglx->context) {
			log_error("Failed to get GLX context.");
			goto glx_init_end;
		}

		// Attach GLX context
		if (!glXMakeCurrent(ps->dpy, get_tgt_window(ps), psglx->context)) {
			log_error("Failed to attach GLX context.");
			goto glx_init_end;
		}
	}

	// Ensure we have a stencil buffer. X Fixes does not guarantee rectangles
	// in regions don't overlap, so we must use stencil buffer to make sure
	// we don't paint a region for more than one time, I think?
	if (need_render && !ps->o.glx_no_stencil) {
		GLint val = 0;
		glGetIntegerv(GL_STENCIL_BITS, &val);
		if (!val) {
			log_error("Target window doesn't have stencil buffer.");
			goto glx_init_end;
		}
	}

	// Check GL_ARB_texture_non_power_of_two, requires a GLX context and
	// must precede FBConfig fetching
	if (need_render)
		psglx->has_texture_non_power_of_two =
		    gl_has_extension("GL_ARB_texture_non_power_of_two");

	// Render preparations
	if (need_render) {
		glx_on_root_change(ps);

		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glDisable(GL_BLEND);

		if (!ps->o.glx_no_stencil) {
			// Initialize stencil buffer
			glClear(GL_STENCIL_BUFFER_BIT);
			glDisable(GL_STENCIL_TEST);
			glStencilMask(0x1);
			glStencilFunc(GL_EQUAL, 0x1, 0x1);
		}

		// Clear screen
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		// glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		// glXSwapBuffers(ps->dpy, get_tgt_window(ps));
	}

	success = true;

glx_init_end:
	XFree(pvis);

	if (!success)
		glx_destroy(ps);

	return success;
}

static void glx_free_prog_main(glx_prog_main_t *pprogram) {
	if (!pprogram)
		return;
	if (pprogram->prog) {
		glDeleteProgram(pprogram->prog);
		pprogram->prog = 0;
	}
	pprogram->unifm_tex = -1;
}

/**
 * Destroy GLX related resources.
 */
void glx_destroy(session_t *ps) {
	if (!ps->psglx)
		return;

	// Free all GLX resources of windows
	win_stack_foreach_managed(w, &ps->window_stack) {
		free_win_res_glx(ps, w);
	}

	glx_free_prog_main(&ps->glx_prog_win);

	gl_check_err();

	// Destroy GLX context
	if (ps->psglx->context) {
		glXMakeCurrent(ps->dpy, None, NULL);
		glXDestroyContext(ps->dpy, ps->psglx->context);
		ps->psglx->context = NULL;
	}

	free(ps->psglx);
	ps->psglx = NULL;
	ps->argb_fbconfig = NULL;
}

/**
 * Callback to run on root window size change.
 */
void glx_on_root_change(session_t *ps) {
	glViewport(0, 0, ps->root_width, ps->root_height);

	// Initialize matrix, copied from dcompmgr
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, ps->root_width, 0, ps->root_height, -1000.0, 1000.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/**
 * Bind a X pixmap to an OpenGL texture.
 */
bool glx_bind_pixmap(session_t *ps, glx_texture_t **pptex, xcb_pixmap_t pixmap, int width,
                     int height, bool repeat, const struct glx_fbconfig_info *fbcfg) {
	if (ps->o.backend != BKEND_GLX)
		return true;

	if (!pixmap) {
		log_error("Binding to an empty pixmap %#010x. This can't work.", pixmap);
		return false;
	}

	assert(fbcfg);
	glx_texture_t *ptex = *pptex;
	bool need_release = true;

	// Release pixmap if parameters are inconsistent
	if (ptex && ptex->texture && ptex->pixmap != pixmap) {
		glx_release_pixmap(ps, ptex);
	}

	// Allocate structure
	if (!ptex) {
		static const glx_texture_t GLX_TEX_DEF = {
		    .texture = 0,
		    .glpixmap = 0,
		    .pixmap = 0,
		    .target = 0,
		    .width = 0,
		    .height = 0,
		    .y_inverted = false,
		};

		ptex = cmalloc(glx_texture_t);
		memcpy(ptex, &GLX_TEX_DEF, sizeof(glx_texture_t));
		*pptex = ptex;
	}

	// Create GLX pixmap
	int depth = 0;
	if (!ptex->glpixmap) {
		need_release = false;

		// Retrieve pixmap parameters, if they aren't provided
		if (!width || !height) {
			auto r = xcb_get_geometry_reply(
			    ps->c, xcb_get_geometry(ps->c, pixmap), NULL);
			if (!r) {
				log_error("Failed to query info of pixmap %#010x.", pixmap);
				return false;
			}
			if (r->depth > OPENGL_MAX_DEPTH) {
				log_error("Requested depth %d higher than %d.", depth,
				          OPENGL_MAX_DEPTH);
				return false;
			}
			depth = r->depth;
			width = r->width;
			height = r->height;
			free(r);
		}

		// Determine texture target, copied from compiz
		// The assumption we made here is the target never changes based on any
		// pixmap-specific parameters, and this may change in the future
		GLenum tex_tgt = 0;
		if (GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts &&
		    ps->psglx->has_texture_non_power_of_two)
			tex_tgt = GLX_TEXTURE_2D_EXT;
		else if (GLX_TEXTURE_RECTANGLE_BIT_EXT & fbcfg->texture_tgts)
			tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
		else if (!(GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts))
			tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
		else
			tex_tgt = GLX_TEXTURE_2D_EXT;

		log_debug("depth %d, tgt %#x, rgba %d", depth, tex_tgt,
		          (GLX_TEXTURE_FORMAT_RGBA_EXT == fbcfg->texture_fmt));

		GLint attrs[] = {
		    GLX_TEXTURE_FORMAT_EXT,
		    fbcfg->texture_fmt,
		    GLX_TEXTURE_TARGET_EXT,
		    (GLint)tex_tgt,
		    0,
		};

		ptex->glpixmap = glXCreatePixmap(ps->dpy, fbcfg->cfg, pixmap, attrs);
		ptex->pixmap = pixmap;
		ptex->target =
		    (GLX_TEXTURE_2D_EXT == tex_tgt ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE);
		ptex->width = width;
		ptex->height = height;
		ptex->y_inverted = fbcfg->y_inverted;
	}
	if (!ptex->glpixmap) {
		log_error("Failed to allocate GLX pixmap.");
		return false;
	}

	glEnable(ptex->target);

	// Create texture
	if (!ptex->texture) {
		need_release = false;

		GLuint texture = 0;
		glGenTextures(1, &texture);
		glBindTexture(ptex->target, texture);

		glTexParameteri(ptex->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(ptex->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (repeat) {
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_REPEAT);
		} else {
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}

		glBindTexture(ptex->target, 0);

		ptex->texture = texture;
	}
	if (!ptex->texture) {
		log_error("Failed to allocate texture.");
		return false;
	}

	glBindTexture(ptex->target, ptex->texture);

	// The specification requires rebinding whenever the content changes...
	// We can't follow this, too slow.
	if (need_release)
		glXReleaseTexImageEXT(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);

	glXBindTexImageEXT(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT, NULL);

	// Cleanup
	glBindTexture(ptex->target, 0);
	glDisable(ptex->target);

	gl_check_err();

	return true;
}

/**
 * @brief Release binding of a texture.
 */
void glx_release_pixmap(session_t *ps, glx_texture_t *ptex) {
	// Release binding
	if (ptex->glpixmap && ptex->texture) {
		glBindTexture(ptex->target, ptex->texture);
		glXReleaseTexImageEXT(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);
		glBindTexture(ptex->target, 0);
	}

	// Free GLX Pixmap
	if (ptex->glpixmap) {
		glXDestroyPixmap(ps->dpy, ptex->glpixmap);
		ptex->glpixmap = 0;
	}

	gl_check_err();
}

/**
 * Set clipping region on the target window.
 */
void glx_set_clip(session_t *ps, const region_t *reg) {
	// Quit if we aren't using stencils
	if (ps->o.glx_no_stencil)
		return;

	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);

	if (!reg)
		return;

	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);

	if (nrects == 1) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(rects[0].x1, ps->root_height - rects[0].y2,
		          rects[0].x2 - rects[0].x1, rects[0].y2 - rects[0].y1);
	}

	gl_check_err();
}

#define P_PAINTREG_START(var)                                                            \
	region_t reg_new;                                                                \
	int nrects;                                                                      \
	const rect_t *rects;                                                             \
	assert(width >= 0 && height >= 0);                                               \
	pixman_region32_init_rect(&reg_new, dx, dy, (uint)width, (uint)height);          \
	pixman_region32_intersect(&reg_new, &reg_new, (region_t *)reg_tgt);              \
	rects = pixman_region32_rectangles(&reg_new, &nrects);                           \
	glBegin(GL_QUADS);                                                               \
                                                                                         \
	for (int ri = 0; ri < nrects; ++ri) {                                            \
		rect_t var = rects[ri];

#define P_PAINTREG_END()                                                                 \
	}                                                                                \
	glEnd();                                                                         \
                                                                                         \
	pixman_region32_fini(&reg_new);

/**
 * @brief Render a region with texture data.
 */
bool glx_render(session_t *ps, const glx_texture_t *ptex, int x, int y, int dx, int dy,
                int width, int height, int z, bool argb, const region_t *reg_tgt,
                const glx_prog_main_t *pprogram) {
	if (!ptex || !ptex->texture) {
		log_error("Missing texture.");
		return false;
	}

	const bool has_prog = pprogram && pprogram->prog;
	bool dual_texture = false;

	// It's required by legacy versions of OpenGL to enable texture target
	// before specifying environment. Thanks to madsy for telling me.
	glEnable(ptex->target);

	// Enable blending if needed
	if (argb) {

		glEnable(GL_BLEND);

		// Needed for handling opacity of ARGB texture
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

		// This is all weird, but X Render is using premultiplied ARGB format, and
		// we need to use those things to correct it. Thanks to derhass for help.
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glColor4d(1.0, 1.0, 1.0, 1.0);
	}

	if (!has_prog) {
		// The default, fixed-function path
	} else {
		// Programmable path
		assert(pprogram->prog);
		glUseProgram(pprogram->prog);
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (pprogram->unifm_tex >= 0)
			glUniform1i(pprogram->unifm_tex, 0);
		if (pprogram->unifm_time >= 0)
			glUniform1f(pprogram->unifm_time, (float)ts.tv_sec * 1000.0f +
			                                      (float)ts.tv_nsec / 1.0e6f);
	}

	// Bind texture
	glBindTexture(ptex->target, ptex->texture);
	if (dual_texture) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(ptex->target, ptex->texture);
		glActiveTexture(GL_TEXTURE0);
	}

	// Painting
	{
		P_PAINTREG_START(crect) {
			// texture-local coordinates
			auto rx = (GLfloat)(crect.x1 - dx + x);
			auto ry = (GLfloat)(crect.y1 - dy + y);
			auto rxe = rx + (GLfloat)(crect.x2 - crect.x1);
			auto rye = ry + (GLfloat)(crect.y2 - crect.y1);
			// Rectangle textures have [0-w] [0-h] while 2D texture has [0-1]
			// [0-1] Thanks to amonakov for pointing out!
			if (GL_TEXTURE_2D == ptex->target) {
				rx = rx / (GLfloat)ptex->width;
				ry = ry / (GLfloat)ptex->height;
				rxe = rxe / (GLfloat)ptex->width;
				rye = rye / (GLfloat)ptex->height;
			}

			// coordinates for the texture in the target
			GLint rdx = crect.x1;
			GLint rdy = ps->root_height - crect.y1;
			GLint rdxe = rdx + (crect.x2 - crect.x1);
			GLint rdye = rdy - (crect.y2 - crect.y1);

			// Invert Y if needed, this may not work as expected, though. I
			// don't have such a FBConfig to test with.
			if (!ptex->y_inverted) {
				ry = 1.0f - ry;
				rye = 1.0f - rye;
			}

			// log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d", ri, rx,
			// ry, rxe, rye,
			//          rdx, rdy, rdxe, rdye);

#define P_TEXCOORD(cx, cy)                                                               \
	{                                                                                \
		if (dual_texture) {                                                      \
			glMultiTexCoord2f(GL_TEXTURE0, cx, cy);                          \
			glMultiTexCoord2f(GL_TEXTURE1, cx, cy);                          \
		} else                                                                   \
			glTexCoord2f(cx, cy);                                            \
	}
			P_TEXCOORD(rx, ry);
			glVertex3i(rdx, rdy, z);

			P_TEXCOORD(rxe, ry);
			glVertex3i(rdxe, rdy, z);

			P_TEXCOORD(rxe, rye);
			glVertex3i(rdxe, rdye, z);

			P_TEXCOORD(rx, rye);
			glVertex3i(rdx, rdye, z);
		}
		P_PAINTREG_END();
	}

	// Cleanup
	glBindTexture(ptex->target, 0);
	glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glDisable(GL_BLEND);
	glDisable(GL_COLOR_LOGIC_OP);
	glDisable(ptex->target);

	if (dual_texture) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(ptex->target, 0);
		glDisable(ptex->target);
		glActiveTexture(GL_TEXTURE0);
	}

	if (has_prog)
		glUseProgram(0);

	gl_check_err();

	return true;
}
