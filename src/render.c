// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_renderutil.h>

#include "common.h"
#include "options.h"

#include "backend/gl/glx.h"
#include "opengl.h"

#ifndef GLX_BACK_BUFFER_AGE_EXT
#define GLX_BACK_BUFFER_AGE_EXT 0x20F4
#endif

#include "compiler.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "vsync.h"
#include "win.h"
#include "x.h"

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "render.h"

/**
 * Bind texture in paint_t if we are using GLX backend.
 */
static inline bool paint_bind_tex(session_t *ps, paint_t *ppaint, int wid, int hei,
                                  bool repeat, int depth, xcb_visualid_t visual, bool force) {
	// XXX This is a mess. But this will go away after the backend refactor.
	if (!ppaint->pixmap)
		return false;

	struct glx_fbconfig_info *fbcfg;
	if (!visual) {
		assert(depth == 32);
		if (!ps->argb_fbconfig) {
			ps->argb_fbconfig =
			    glx_find_fbconfig(ps->dpy, ps->scr,
			                      (struct xvisual_info){.red_size = 8,
			                                            .green_size = 8,
			                                            .blue_size = 8,
			                                            .alpha_size = 8,
			                                            .visual_depth = 32});
		}
		if (!ps->argb_fbconfig) {
			log_error("Failed to find appropriate FBConfig for 32 bit depth");
			return false;
		}
		fbcfg = ps->argb_fbconfig;
	} else {
		auto m = x_get_visual_info(ps->c, visual);
		if (m.visual_depth < 0) {
			return false;
		}

		if (depth && depth != m.visual_depth) {
			log_error("Mismatching visual depth: %d != %d", depth, m.visual_depth);
			return false;
		}

		if (!ppaint->fbcfg) {
			ppaint->fbcfg = glx_find_fbconfig(ps->dpy, ps->scr, m);
		}
		if (!ppaint->fbcfg) {
			log_error("Failed to find appropriate FBConfig for X pixmap");
			return false;
		}
		fbcfg = ppaint->fbcfg;
	}

	if (force || !glx_tex_binded(ppaint->ptex, ppaint->pixmap))
		return glx_bind_pixmap(ps, &ppaint->ptex, ppaint->pixmap, wid, hei,
		                       repeat, fbcfg);
	return true;
}

static int get_buffer_age(session_t *ps) {
	if (bkend_use_glx(ps)) {
		if (!glxext.has_GLX_EXT_buffer_age && ps->o.use_damage) {
			log_warn("GLX_EXT_buffer_age not supported by your driver,"
			         "`use-damage` has to be disabled");
			ps->o.use_damage = false;
		}
		if (ps->o.use_damage) {
			unsigned int val;
			glXQueryDrawable(ps->dpy, get_tgt_window(ps),
			                 GLX_BACK_BUFFER_AGE_EXT, &val);
			return (int)val ?: -1;
		}
		return -1;
	}
	return ps->o.use_damage ? 1 : -1;
}

/**
 * Reset filter on a <code>Picture</code>.
 */
static inline void xrfilter_reset(session_t *ps, xcb_render_picture_t p) {
#define FILTER "Nearest"
	xcb_render_set_picture_filter(ps->c, p, strlen(FILTER), FILTER, 0, NULL);
#undef FILTER
}

/// Set the input/output clip region of the target buffer (not the actual target!)
static inline void attr_nonnull(1, 2) set_tgt_clip(session_t *ps, region_t *reg) {
	switch (ps->o.backend) {
	case BKEND_GLX: glx_set_clip(ps, reg); break;
	default: assert(false);
	}
}

/**
 * Destroy a <code>Picture</code>.
 */
void free_picture(xcb_connection_t *c, xcb_render_picture_t *p) {
	if (*p) {
		xcb_render_free_picture(c, *p);
		*p = XCB_NONE;
	}
}

/**
 * Free paint_t.
 */
void free_paint(session_t *ps, paint_t *ppaint) {
	free_paint_glx(ps, ppaint);
	free_picture(ps->c, &ppaint->pict);
	if (ppaint->pixmap)
		xcb_free_pixmap(ps->c, ppaint->pixmap);
	ppaint->pixmap = XCB_NONE;
}

void render(session_t *ps, int x, int y, int dx, int dy, int wid, int hei, bool argb,
            glx_texture_t *ptex, const region_t *reg_paint, const glx_prog_main_t *pprogram) {
	switch (ps->o.backend) {
	case BKEND_GLX:
		glx_render(ps, ptex, x, y, dx, dy, wid, hei, ps->psglx->z, argb,
		           reg_paint, pprogram);
		ps->psglx->z += 1;
		break;
	default: assert(0);
	}
}

static inline void paint_region(session_t *ps, const struct managed_win *w, int x, int y,
                                int wid, int hei, const region_t *reg_paint) {
	const int dx = (w ? w->g.x : 0) + x;
	const int dy = (w ? w->g.y : 0) + y;
	const bool argb = (w && win_has_alpha(w));

	render(ps, x, y, dx, dy, wid, hei, argb,
	       (w ? w->paint.ptex : ps->root_tile_paint.ptex), reg_paint,
	       w ? &ps->glx_prog_win : NULL);
}

/**
 * Check whether a paint_t contains enough data.
 */
static inline bool paint_isvalid(session_t *ps, const paint_t *ppaint) {
	// Don't check for presence of Pixmap here, because older X Composite doesn't
	// provide it
	if (!ppaint)
		return false;

	if (BKEND_GLX == ps->o.backend && !glx_tex_binded(ppaint->ptex, XCB_NONE))
		return false;

	return true;
}

/**
 * Paint a window itself.
 */
void paint_one(session_t *ps, struct managed_win *w, const region_t *reg_paint) {
	// Fetch Pixmap
	if (!w->paint.pixmap) {
		w->paint.pixmap = x_new_id(ps->c);
		set_ignore_cookie(ps, xcb_composite_name_window_pixmap(ps->c, w->base.id,
		                                                       w->paint.pixmap));
	}

	xcb_drawable_t draw = w->paint.pixmap;
	if (!draw) {
		log_error("Failed to get pixmap from window %#010x (%s), window won't be "
		          "visible",
		          w->base.id, w->name);
		return;
	}

	// GLX: Build texture
	// Let glx_bind_pixmap() determine pixmap size, because if the user
	// is resizing windows, the width and height we get may not be up-to-date,
	// causing the jittering issue M4he reported in #7.
	if (!paint_bind_tex(ps, &w->paint, 0, 0, false, 0, w->a.visual,
	                    (!ps->o.glx_no_rebind_pixmap && w->pixmap_damaged))) {
		log_error("Failed to bind texture for window %#010x.", w->base.id);
	}
	w->pixmap_damaged = false;

	if (!paint_isvalid(ps, &w->paint)) {
		log_error("Window %#010x is missing painting data.", w->base.id);
		return;
	}

	const uint16_t wid = to_u16_checked(w->width);
	const uint16_t hei = to_u16_checked(w->height);

	xcb_render_picture_t pict = w->paint.pict;

	paint_region(ps, w, 0, 0, wid, hei, reg_paint);

	if (pict != w->paint.pict)
		free_picture(ps->c, &pict);
}

extern const char *background_props_str[];

static bool get_root_tile(session_t *ps) {
	/*
	if (ps->o.paint_on_overlay) {
	  return ps->root_picture;
	} */

	assert(!ps->root_tile_paint.pixmap);
	ps->root_tile_fill = false;

	bool fill = false;
	xcb_pixmap_t pixmap = x_get_root_back_pixmap(ps->c, ps->root, ps->atoms);

	// Make sure the pixmap we got is valid
	if (pixmap && !x_validate_pixmap(ps->c, pixmap))
		pixmap = XCB_NONE;

	// Create a pixmap if there isn't any
	if (!pixmap) {
		pixmap = x_create_pixmap(ps->c, (uint8_t)ps->depth, ps->root, 1, 1);
		if (pixmap == XCB_NONE) {
			log_error("Failed to create pixmaps for root tile.");
			return false;
		}
		fill = true;
	}

	// Create Picture
	xcb_render_create_picture_value_list_t pa = {
	    .repeat = true,
	};
	ps->root_tile_paint.pict = x_create_picture_with_visual_and_pixmap(
	    ps->c, ps->vis, pixmap, XCB_RENDER_CP_REPEAT, &pa);

	// Fill pixmap if needed
	if (fill) {
		xcb_render_color_t col;
		xcb_rectangle_t rect;

		col.red = col.green = col.blue = 0x8080;
		col.alpha = 0xffff;

		rect.x = rect.y = 0;
		rect.width = rect.height = 1;

		xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_SRC,
		                           ps->root_tile_paint.pict, col, 1, &rect);
	}

	ps->root_tile_fill = fill;
	ps->root_tile_paint.pixmap = pixmap;
	if (BKEND_GLX == ps->o.backend)
		return paint_bind_tex(ps, &ps->root_tile_paint, 0, 0, true, 0, ps->vis, false);

	return true;
}

/**
 * Paint root window content.
 */
static void paint_root(session_t *ps, const region_t *reg_paint) {
	// If there is no root tile pixmap, try getting one.
	// If that fails, give up.
	if (!ps->root_tile_paint.pixmap && !get_root_tile(ps)) {
		return;
	}

	paint_region(ps, NULL, 0, 0, ps->root_width, ps->root_height, reg_paint);
}

/// paint all windows
/// region = ??
/// region_real = the damage region
void paint_all(session_t *ps, struct managed_win *t, bool ignore_damage) {
	if (ps->o.xrender_sync_fence || (ps->drivers & DRIVER_NVIDIA)) {
		if (ps->xsync_exists && !x_fence_sync(ps->c, ps->sync_fence)) {
			log_error("x_fence_sync failed, xrender-sync-fence will be "
			          "disabled from now on.");
			xcb_sync_destroy_fence(ps->c, ps->sync_fence);
			ps->sync_fence = XCB_NONE;
			ps->o.xrender_sync_fence = false;
			ps->xsync_exists = false;
		}
	}

	region_t region;
	pixman_region32_init(&region);
	int buffer_age = get_buffer_age(ps);
	if (buffer_age == -1 || buffer_age > ps->ndamage || ignore_damage) {
		pixman_region32_copy(&region, &ps->screen_reg);
	} else {
		for (int i = 0; i < get_buffer_age(ps); i++) {
			auto curr = ((ps->damage - ps->damage_ring) + i) % ps->ndamage;
			pixman_region32_union(&region, &region, &ps->damage_ring[curr]);
		}
	}

	if (!pixman_region32_not_empty(&region)) {
		return;
	}

	// Remove the damaged area out of screen
	pixman_region32_intersect(&region, &region, &ps->screen_reg);

	if (!paint_isvalid(ps, &ps->tgt_buffer)) {
		if (!ps->tgt_buffer.pixmap) {
			free_paint(ps, &ps->tgt_buffer);
			ps->tgt_buffer.pixmap =
			    x_create_pixmap(ps->c, (uint8_t)ps->depth, ps->root,
			                    ps->root_width, ps->root_height);
			if (ps->tgt_buffer.pixmap == XCB_NONE) {
				log_fatal("Failed to allocate a screen-sized pixmap for"
				          "painting");
				exit(1);
			}
		}

		if (BKEND_GLX != ps->o.backend)
			ps->tgt_buffer.pict = x_create_picture_with_visual_and_pixmap(
			    ps->c, ps->vis, ps->tgt_buffer.pixmap, 0, 0);
	}

	if (bkend_use_glx(ps)) {
		ps->psglx->z = 0.0;
	}

	region_t reg_tmp, *reg_paint;
	pixman_region32_init(&reg_tmp);
	if (t) {
		// Calculate the region upon which the root window is to be
		// painted based on the ignore region of the lowest window, if
		// available
		pixman_region32_subtract(&reg_tmp, &region, t->reg_ignore);
		reg_paint = &reg_tmp;
	} else {
		reg_paint = &region;
	}

	set_tgt_clip(ps, reg_paint);
	paint_root(ps, reg_paint);

	// Windows are sorted from bottom to top
	// Each window has a reg_ignore, which is the region obscured by all the
	// windows on top of that window. This is used to reduce the number of
	// pixels painted.
	//
	// Whether this is beneficial is to be determined XXX
	for (auto w = t; w; w = w->prev_trans) {
		region_t bshape = win_get_bounding_shape_global_by_val(w);

		// Calculate the paint region based on the reg_ignore of the current
		// window and its bounding region.
		// Remember, reg_ignore is the union of all windows above the current
		// window.
		pixman_region32_subtract(&reg_tmp, &region, w->reg_ignore);
		pixman_region32_intersect(&reg_tmp, &reg_tmp, &bshape);
		pixman_region32_fini(&bshape);

		if (pixman_region32_not_empty(&reg_tmp)) {
			set_tgt_clip(ps, &reg_tmp);

			// Painting the window
			paint_one(ps, w, &reg_tmp);
		}
	}

	// Free up all temporary regions
	pixman_region32_fini(&reg_tmp);

	// Move the head of the damage ring
	ps->damage = ps->damage - 1;
	if (ps->damage < ps->damage_ring) {
		ps->damage = ps->damage_ring + ps->ndamage - 1;
	}
	pixman_region32_clear(ps->damage);

	// Do this as early as possible
	set_tgt_clip(ps, &ps->screen_reg);

	// Make sure all previous requests are processed to achieve best
	// effect
	x_sync(ps->c);
	if (glx_has_context(ps)) {
		glFlush();
		glXWaitX();
	}

	if (ps->vsync_wait) {
		ps->vsync_wait(ps);
	}

	switch (ps->o.backend) {
	case BKEND_GLX: glXSwapBuffers(ps->dpy, get_tgt_window(ps)); break;
	default: assert(0);
	}

	x_sync(ps->c);

	if (glx_has_context(ps)) {
		glFlush();
		glXWaitX();
	}

	// Free the paint region
	pixman_region32_fini(&region);
}

bool init_render(session_t *ps) {
	if (ps->o.backend == BKEND_DUMMY) {
		return false;
	}

	// Initialize OpenGL as early as possible
	glxext_init(ps->dpy, ps->scr);
	if (bkend_use_glx(ps)) {
		if (!glx_init(ps, true))
			return false;
	}

	// Initialize VSync
	if (!vsync_init(ps)) {
		return false;
	}

	return true;
}

/**
 * Free root tile related things.
 */
void free_root_tile(session_t *ps) {
	free_picture(ps->c, &ps->root_tile_paint.pict);
	free_texture(ps, &ps->root_tile_paint.ptex);
	if (ps->root_tile_fill) {
		xcb_free_pixmap(ps->c, ps->root_tile_paint.pixmap);
		ps->root_tile_paint.pixmap = XCB_NONE;
	}
	ps->root_tile_paint.pixmap = XCB_NONE;
	ps->root_tile_fill = false;
}

// vim: set ts=8 sw=8 noet :
