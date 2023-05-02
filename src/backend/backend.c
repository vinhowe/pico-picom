// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "win.h"
#include "x.h"

extern struct backend_operations xrender_ops, dummy_ops;
extern struct backend_operations glx_ops;

struct backend_operations *backend_list[NUM_BKEND] = {
    [BKEND_DUMMY] = &dummy_ops,
    [BKEND_GLX] = &glx_ops,
};

/**
 * @param all_damage if true ignore damage and repaint the whole screen
 */
region_t get_damage(session_t *ps, bool all_damage) {
	region_t region;
	auto buffer_age_fn = ps->backend_data->ops->buffer_age;
	int buffer_age = buffer_age_fn ? buffer_age_fn(ps->backend_data) : -1;

	if (all_damage) {
		buffer_age = -1;
	}

	pixman_region32_init(&region);
	if (buffer_age == -1 || buffer_age > ps->ndamage) {
		pixman_region32_copy(&region, &ps->screen_reg);
	} else {
		for (int i = 0; i < buffer_age; i++) {
			auto curr = ((ps->damage - ps->damage_ring) + i) % ps->ndamage;
			log_trace("damage index: %d, damage ring offset: %td", i, curr);
			dump_region(&ps->damage_ring[curr]);
			pixman_region32_union(&region, &region, &ps->damage_ring[curr]);
		}
		pixman_region32_intersect(&region, &region, &ps->screen_reg);
	}
	return region;
}

void handle_device_reset(session_t *ps) {
	log_error("Device reset detected");
	// Wait for reset to complete
	// Although ideally the backend should return DEVICE_STATUS_NORMAL after a reset
	// is completed, it's not always possible.
	//
	// According to ARB_robustness (emphasis mine):
	//
	//     "If a reset status other than NO_ERROR is returned and subsequent
	//     calls return NO_ERROR, the context reset was encountered and
	//     completed. If a reset status is repeatedly returned, the context **may**
	//     be in the process of resetting."
	//
	//  Which means it may also not be in the process of resetting. For example on
	//  AMDGPU devices, Mesa OpenGL always return CONTEXT_RESET after a reset has
	//  started, completed or not.
	//
	//  So here we blindly wait 5 seconds and hope ourselves best of the luck.
	sleep(5);

	// Reset picom
	log_info("Resetting picom after device reset");
	ev_break(ps->loop, EVBREAK_ALL);
}

/// paint all windows
void paint_all_new(session_t *ps, struct managed_win *t, bool ignore_damage) {
	if (ps->backend_data->ops->device_status &&
	    ps->backend_data->ops->device_status(ps->backend_data) != DEVICE_STATUS_NORMAL) {
		return handle_device_reset(ps);
	}
	if (ps->o.xrender_sync_fence) {
		if (ps->xsync_exists && !x_fence_sync(ps->c, ps->sync_fence)) {
			log_error("x_fence_sync failed, xrender-sync-fence will be "
			          "disabled from now on.");
			xcb_sync_destroy_fence(ps->c, ps->sync_fence);
			ps->sync_fence = XCB_NONE;
			ps->o.xrender_sync_fence = false;
			ps->xsync_exists = false;
		}
	}
	// All painting will be limited to the damage, if _some_ of
	// the paints bleed out of the damage region, it will destroy
	// part of the image we want to reuse
	region_t reg_damage;
	if (!ignore_damage) {
		reg_damage = get_damage(ps, !ps->o.use_damage);
	} else {
		pixman_region32_init(&reg_damage);
		pixman_region32_copy(&reg_damage, &ps->screen_reg);
	}

	if (!pixman_region32_not_empty(&reg_damage)) {
		pixman_region32_fini(&reg_damage);
		return;
	}

	// <damage-note>
	// If use_damage is enabled, we MUST make sure only the damaged regions of the
	// screen are ever touched by the compositor. The reason is that at the beginning
	// of each render, we clear the damaged regions with the wallpaper, and nothing
	// else. If later during the render we changed anything outside the damaged
	// region, that won't be cleared by the next render, and will thus accumulate.
	// (e.g. if shadow is drawn outside the damaged region, it will become thicker and
	// thicker over time.)

	/// The adjusted damaged regions
	region_t reg_paint;
	pixman_region32_init(&reg_paint);
	pixman_region32_copy(&reg_paint, &reg_damage);

	// A hint to backend, the region that will be visible on screen
	// backend can optimize based on this info
	region_t reg_visible;
	pixman_region32_init(&reg_visible);
	pixman_region32_copy(&reg_visible, &ps->screen_reg);

	if (ps->root_image) {
		ps->backend_data->ops->compose(ps->backend_data, ps->root_image,
		                               (coord_t){0}, &reg_paint, &reg_visible);
	} else {
		ps->backend_data->ops->fill(ps->backend_data, (struct color){0, 0, 0, 1},
		                            &reg_paint);
	}

	// Windows are sorted from bottom to top
	// Each window has a reg_ignore, which is the region obscured by all the windows
	// on top of that window. This is used to reduce the number of pixels painted.
	//
	// Whether this is beneficial is to be determined XXX
	for (auto w = t; w; w = w->prev_trans) {
		pixman_region32_subtract(&reg_visible, &ps->screen_reg, w->reg_ignore);
		assert(!(w->flags & WIN_FLAGS_IMAGE_ERROR));
		assert(!(w->flags & WIN_FLAGS_PIXMAP_STALE));
		assert(!(w->flags & WIN_FLAGS_PIXMAP_NONE));

		// The bounding shape of the window, in global/target coordinates
		// reminder: bounding shape contains the WM frame
		auto reg_bound = win_get_bounding_shape_global_by_val(w);

		// The clip region for the current window, in global/target coordinates
		// reg_paint_in_bound \in reg_paint
		region_t reg_paint_in_bound;
		pixman_region32_init(&reg_paint_in_bound);
		pixman_region32_intersect(&reg_paint_in_bound, &reg_bound, &reg_paint);

		/* TODO(yshui) since the backend might change the content of the window
		 * (e.g. with shaders), we should consult the backend whether the window
		 * is transparent or not. */
		coord_t window_coord = {.x = w->g.x, .y = w->g.y};

		ps->backend_data->ops->compose(ps->backend_data, w->win_image, window_coord,
		                               &reg_paint_in_bound, &reg_visible);

		pixman_region32_fini(&reg_bound);
		pixman_region32_fini(&reg_paint_in_bound);
	}
	pixman_region32_fini(&reg_paint);

	// Move the head of the damage ring
	ps->damage = ps->damage - 1;
	if (ps->damage < ps->damage_ring) {
		ps->damage = ps->damage_ring + ps->ndamage - 1;
	}
	pixman_region32_clear(ps->damage);

	if (ps->backend_data->ops->present) {
		// Present the rendered scene
		// Vsync is done here
		ps->backend_data->ops->present(ps->backend_data, &reg_damage);
	}

	pixman_region32_fini(&reg_damage);
}

// vim: set noet sw=8 ts=8 :
