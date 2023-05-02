// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

/// Function pointers to init VSync modes.

#include "common.h"
#include "log.h"

#include "backend/gl/glx.h"
#include "opengl.h"

#include "vsync.h"

/**
 * Initialize OpenGL VSync.
 *
 * Stolen from:
 * http://git.tuxfamily.org/?p=ccm/cairocompmgr.git;a=commitdiff;h=efa4ceb97da501e8630ca7f12c99b1dce853c73e
 * Possible original source: http://www.inb.uni-luebeck.de/~boehme/xvideo_sync.html
 *
 * @return true for success, false otherwise
 */
// static bool vsync_opengl_init(session_t *ps) {
// 	if (!ensure_glx_context(ps))
// 		return false;

// 	return glxext.has_GLX_SGI_video_sync;
// }

// static bool vsync_opengl_oml_init(session_t *ps) {
// 	if (!ensure_glx_context(ps))
// 		return false;

// 	return glxext.has_GLX_OML_sync_control;
// }

static inline bool vsync_opengl_swc_swap_interval(session_t *ps, int interval) {
	if (glxext.has_GLX_MESA_swap_control)
		return glXSwapIntervalMESA((uint)interval) == 0;
	else if (glxext.has_GLX_SGI_swap_control)
		return glXSwapIntervalSGI(interval) == 0;
	else if (glxext.has_GLX_EXT_swap_control) {
		GLXDrawable d = glXGetCurrentDrawable();
		if (d == None) {
			// We don't have a context??
			return false;
		}
		glXSwapIntervalEXT(ps->dpy, glXGetCurrentDrawable(), interval);
		return true;
	}
	return false;
}

static bool vsync_opengl_swc_init(session_t *ps) {
	if (!bkend_use_glx(ps)) {
		log_error("OpenGL swap control requires the GLX backend.");
		return false;
	}

	if (!vsync_opengl_swc_swap_interval(ps, 1)) {
		log_error("Failed to load a swap control extension.");
		return false;
	}

	return true;
}

/**
 * Initialize current VSync method.
 */
bool vsync_init(session_t *ps) {
	if (bkend_use_glx(ps)) {
		// Mesa turns on swap control by default, undo that
		vsync_opengl_swc_swap_interval(ps, 0);

		if (!vsync_opengl_swc_init(ps)) {
			return false;
		}
		ps->vsync_wait = NULL;        // glXSwapBuffers will automatically wait
		                              // for vsync, we don't need to do anything.
		return true;
	}

	log_error("No supported vsync method found for this backend");
	return false;
}
