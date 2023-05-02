// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>

#include "atom.h"
#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "list.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "render.h"
#include "types.h"
#include "uthash_extra.h"
#include "utils.h"
#include "x.h"

// TODO(yshui) Get rid of this include
#include "opengl.h"

#include "win.h"

// TODO(yshui) Make more window states internal
struct managed_win_internal {
	struct managed_win base;
};

#define OPAQUE (0xffffffff)
static const int WIN_GET_LEADER_MAX_RECURSION = 20;

/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
static bool win_update_class(session_t *ps, struct managed_win *w);
static int win_update_role(session_t *ps, struct managed_win *w);
static void win_update_wintype(session_t *ps, struct managed_win *w);
static int win_update_name(session_t *ps, struct managed_win *w);
/**
 * Retrieve frame extents from a window.
 */
static void
win_update_frame_extents(session_t *ps, struct managed_win *w, xcb_window_t client);
/**
 * Update leader of a window.
 */
static void win_update_leader(session_t *ps, struct managed_win *w);

/// Generate a "return by value" function, from a function that returns the
/// region via a region_t pointer argument.
/// Function signature has to be (win *)
#define gen_by_val(fun)                                                                  \
	region_t fun##_by_val(const struct managed_win *w) {                             \
		region_t ret;                                                            \
		pixman_region32_init(&ret);                                              \
		fun(w, &ret);                                                            \
		return ret;                                                              \
	}

/**
 * Clear leader cache of all windows.
 */
static inline void clear_cache_win_leaders(session_t *ps) {
	win_stack_foreach_managed(w, &ps->window_stack) {
		w->cache_leader = XCB_NONE;
	}
}

static xcb_window_t win_get_leader_raw(session_t *ps, struct managed_win *w, int recursions);

/**
 * Get the leader of a window.
 *
 * This function updates w->cache_leader if necessary.
 */
static inline xcb_window_t win_get_leader(session_t *ps, struct managed_win *w) {
	return win_get_leader_raw(ps, w, 0);
}

/**
 * Whether the real content of the window is visible.
 *
 * A window is not considered "real" visible if it's fading out. Because in that case a
 * cached version of the window is displayed.
 */
static inline bool attr_pure win_is_real_visible(const struct managed_win *w) {
	return w->state != WSTATE_UNMAPPED && w->state != WSTATE_DESTROYING &&
	       w->state != WSTATE_UNMAPPING;
}

/**
 * Update focused state of a window.
 */
static void win_update_focused(session_t *ps, struct managed_win *w) {
	w->focused = win_is_focused_raw(ps, w);

	// Use wintype_focus, and treat WM windows and override-redirected
	// windows specially
	if (ps->o.wintype_option[w->window_type].focus) {
		w->focused = true;
	}
}

/**
 * Run win_on_factor_change() on all windows with the same leader window.
 *
 * @param leader leader window ID
 */
static inline void group_on_factor_change(session_t *ps, xcb_window_t leader) {
	if (!leader) {
		return;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}
		auto mw = (struct managed_win *)w;
		if (win_get_leader(ps, mw) == leader) {
			win_on_factor_change(ps, mw);
		}
	}
}

static inline const char *win_get_name_if_managed(const struct win *w) {
	if (!w->managed) {
		return "(unmanaged)";
	}
	auto mw = (struct managed_win *)w;
	return mw->name;
}

/**
 * Return whether a window group is really focused.
 *
 * @param leader leader window ID
 * @return true if the window group is focused, false otherwise
 */
static inline bool group_is_focused(session_t *ps, xcb_window_t leader) {
	if (!leader) {
		return false;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}
		auto mw = (struct managed_win *)w;
		if (win_get_leader(ps, mw) == leader && win_is_focused_raw(ps, mw)) {
			return true;
		}
	}

	return false;
}

/**
 * Get a rectangular region a window occupies.
 */
static void win_get_region_local(const struct managed_win *w, region_t *res) {
	assert(w->width >= 0 && w->height >= 0);
	pixman_region32_fini(res);
	pixman_region32_init_rect(res, 0, 0, (uint)w->width, (uint)w->height);
}

/**
 * Get a rectangular region a window occupies, excluding frame.
 */
void win_get_region_noframe_local(const struct managed_win *w, region_t *res) {
	const margin_t extents = w->frame_extents;

	int x = extents.left;
	int y = extents.top;
	int width = max2(w->width - (extents.left + extents.right), 0);
	int height = max2(w->height - (extents.top + extents.bottom), 0);

	pixman_region32_fini(res);
	if (width > 0 && height > 0) {
		pixman_region32_init_rect(res, x, y, (uint)width, (uint)height);
	} else {
		pixman_region32_init(res);
	}
}

void win_get_region_frame_local(const struct managed_win *w, region_t *res) {
	const margin_t extents = w->frame_extents;
	auto outer_width = w->width;
	auto outer_height = w->height;

	pixman_region32_fini(res);
	pixman_region32_init_rects(
	    res,
	    (rect_t[]){
	        // top
	        {.x1 = 0, .y1 = 0, .x2 = outer_width, .y2 = extents.top},
	        // bottom
	        {.x1 = 0, .y1 = outer_height - extents.bottom, .x2 = outer_width, .y2 = outer_height},
	        // left
	        {.x1 = 0, .y1 = 0, .x2 = extents.left, .y2 = outer_height},
	        // right
	        {.x1 = outer_width - extents.right, .y1 = 0, .x2 = outer_width, .y2 = outer_height},
	    },
	    4);

	// limit the frame region to inside the window
	region_t reg_win;
	pixman_region32_init_rects(&reg_win, (rect_t[]){{0, 0, outer_width, outer_height}}, 1);
	pixman_region32_intersect(res, &reg_win, res);
	pixman_region32_fini(&reg_win);
}

gen_by_val(win_get_region_frame_local);

/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, const struct managed_win *w) {
	// XXX there was a cached extents region, investigate
	//     if that's better

	// TODO(yshui) use the bounding shape when the window is shaped, otherwise the
	//             damage would be excessive
	region_t extents;
	pixman_region32_init(&extents);
	win_extents(w, &extents);
	add_damage(ps, &extents);
	pixman_region32_fini(&extents);
}

/// Release the images attached to this window
static inline void win_release_pixmap(backend_t *base, struct managed_win *w) {
	log_debug("Releasing pixmap of window %#010x (%s)", w->base.id, w->name);
	assert(w->win_image);
	if (w->win_image) {
		base->ops->release_image(base, w->win_image);
		w->win_image = NULL;
		// Bypassing win_set_flags, because `w` might have been destroyed
		w->flags |= WIN_FLAGS_PIXMAP_NONE;
	}
}

static inline bool win_bind_pixmap(struct backend_base *b, struct managed_win *w) {
	assert(!w->win_image);
	auto pixmap = x_new_id(b->c);
	auto e = xcb_request_check(
	    b->c, xcb_composite_name_window_pixmap_checked(b->c, w->base.id, pixmap));
	if (e) {
		log_error("Failed to get named pixmap for window %#010x(%s)", w->base.id,
		          w->name);
		free(e);
		return false;
	}
	log_debug("New named pixmap for %#010x (%s) : %#010x", w->base.id, w->name, pixmap);
	w->win_image =
	    b->ops->bind_pixmap(b, pixmap, x_get_visual_info(b->c, w->a.visual), true);
	if (!w->win_image) {
		log_error("Failed to bind pixmap");
		win_set_flags(w, WIN_FLAGS_IMAGE_ERROR);
		return false;
	}

	win_clear_flags(w, WIN_FLAGS_PIXMAP_NONE);
	return true;
}

void win_release_images(struct backend_base *backend, struct managed_win *w) {
	// We don't want to decide what we should do if the image we want to
	// release is stale (do we clear the stale flags or not?) But if we are
	// not releasing any images anyway, we don't care about the stale flags.

	if (!win_check_flags_all(w, WIN_FLAGS_PIXMAP_NONE)) {
		assert(!win_check_flags_all(w, WIN_FLAGS_PIXMAP_STALE));
		win_release_pixmap(backend, w);
	}
}

/// Returns true if the `prop` property is stale, as well as clears the stale
/// flag.
static bool win_fetch_and_unset_property_stale(struct managed_win *w, xcb_atom_t prop);
/// Returns true if any of the properties are stale, as well as clear all the
/// stale flags.
static void win_clear_all_properties_stale(struct managed_win *w);

/// Fetch new window properties from the X server, and run appropriate updates.
/// Might set WIN_FLAGS_FACTOR_CHANGED
static void win_update_properties(session_t *ps, struct managed_win *w) {
	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_WINDOW_TYPE)) {
		win_update_wintype(ps, w);
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_FRAME_EXTENTS)) {
		win_update_frame_extents(ps, w, w->client_win);
		add_damage_from_win(ps, w);
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_NAME) ||
	    win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_NAME)) {
		if (win_update_name(ps, w) == 1) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_CLASS)) {
		if (win_update_class(ps, w)) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_WINDOW_ROLE)) {
		if (win_update_role(ps, w) == 1) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_CLIENT_LEADER) ||
	    win_fetch_and_unset_property_stale(w, ps->atoms->aWM_TRANSIENT_FOR)) {
		win_update_leader(ps, w);
	}

	win_clear_all_properties_stale(w);
}

/// Handle non-image flags. This phase might set IMAGES_STALE flags
void win_process_update_flags(session_t *ps, struct managed_win *w) {
	// Whether the window was visible before we process the mapped flag. i.e.
	// is the window just mapped.
	bool was_visible = win_is_real_visible(w);
	log_trace("Processing flags for window %#010x (%s), was visible: %d", w->base.id,
	          w->name, was_visible);

	if (win_check_flags_all(w, WIN_FLAGS_MAPPED)) {
		map_win_start(ps, w);
		win_clear_flags(w, WIN_FLAGS_MAPPED);
	}

	if (!win_is_real_visible(w)) {
		// Flags of invisible windows are processed when they are mapped
		return;
	}

	// Check client first, because later property updates need accurate client
	// window information
	if (win_check_flags_all(w, WIN_FLAGS_CLIENT_STALE)) {
		win_recheck_client(ps, w);
		win_clear_flags(w, WIN_FLAGS_CLIENT_STALE);
	}

	bool damaged = false;
	if (win_check_flags_any(w, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE)) {
		if (was_visible) {
			// Mark the old extents of this window as damaged. The new
			// extents will be marked damaged below, after the window
			// extents are updated.
			//
			// If the window is just mapped, we don't need to mark the
			// old extent as damaged. (It's possible that the window
			// was in fading and is interrupted by being mapped. In
			// that case, the fading window will be added to damage by
			// map_win_start, so we don't need to do it here)
			add_damage_from_win(ps, w);
		}

		// Update window geometry
		w->g = w->pending_g;

		if (win_check_flags_all(w, WIN_FLAGS_SIZE_STALE)) {
			win_on_win_size_change(ps, w);
			win_update_bounding_shape(ps, w);
			damaged = true;
			win_clear_flags(w, WIN_FLAGS_SIZE_STALE);
		}

		if (win_check_flags_all(w, WIN_FLAGS_POSITION_STALE)) {
			damaged = true;
			win_clear_flags(w, WIN_FLAGS_POSITION_STALE);
		}

		win_update_monitor(ps->randr_nmonitors, ps->randr_monitor_regs, w);
	}

	if (win_check_flags_all(w, WIN_FLAGS_PROPERTY_STALE)) {
		win_update_properties(ps, w);
		win_clear_flags(w, WIN_FLAGS_PROPERTY_STALE);
	}

	// Factor change flags could be set by previous stages, so must be handled
	// last
	if (win_check_flags_all(w, WIN_FLAGS_FACTOR_CHANGED)) {
		win_on_factor_change(ps, w);
		win_clear_flags(w, WIN_FLAGS_FACTOR_CHANGED);
	}

	// Add damage, has to be done last so the window has the latest geometry
	// information.
	if (damaged) {
		add_damage_from_win(ps, w);
	}
}

void win_process_image_flags(session_t *ps, struct managed_win *w) {
	assert(!win_check_flags_all(w, WIN_FLAGS_MAPPED));

	if (w->state == WSTATE_UNMAPPED || w->state == WSTATE_DESTROYING ||
	    w->state == WSTATE_UNMAPPING) {
		// Flags of invisible windows are processed when they are mapped
		return;
	}

	// Not a loop
	while (win_check_flags_any(w, WIN_FLAGS_IMAGES_STALE) &&
	       !win_check_flags_all(w, WIN_FLAGS_IMAGE_ERROR)) {
		// Image needs to be updated, update it.
		if (!ps->backend_data) {
			// We are using legacy backend, nothing to do here.
			break;
		}

		if (win_check_flags_all(w, WIN_FLAGS_PIXMAP_STALE)) {
			// Check to make sure the window is still mapped,
			// otherwise we won't be able to rebind pixmap after
			// releasing it, yet we might still need the pixmap for
			// rendering.
			assert(w->state != WSTATE_UNMAPPING && w->state != WSTATE_DESTROYING);
			if (!win_check_flags_all(w, WIN_FLAGS_PIXMAP_NONE)) {
				// Must release images first, otherwise breaks
				// NVIDIA driver
				win_release_pixmap(ps->backend_data, w);
			}
			win_bind_pixmap(ps->backend_data, w);
		}

		// break here, loop always run only once
		break;
	}

	// Clear stale image flags
	if (win_check_flags_any(w, WIN_FLAGS_IMAGES_STALE)) {
		win_clear_flags(w, WIN_FLAGS_IMAGES_STALE);
	}
}

int win_update_name(session_t *ps, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	if (!w->client_win) {
		return 0;
	}

	if (!(wid_get_text_prop(ps, w->client_win, ps->atoms->a_NET_WM_NAME, &strlst, &nstr))) {
		log_debug("(%#010x): _NET_WM_NAME unset, falling back to "
		          "WM_NAME.",
		          w->client_win);

		if (!wid_get_text_prop(ps, w->client_win, ps->atoms->aWM_NAME, &strlst, &nstr)) {
			log_debug("Unsetting window name for %#010x", w->client_win);
			free(w->name);
			w->name = NULL;
			return -1;
		}
	}

	int ret = 0;
	if (!w->name || strcmp(w->name, strlst[0]) != 0) {
		ret = 1;
		free(w->name);
		w->name = strdup(strlst[0]);
	}

	free(strlst);

	log_debug("(%#010x): client = %#010x, name = \"%s\", "
	          "ret = %d",
	          w->base.id, w->client_win, w->name, ret);
	return ret;
}

static int win_update_role(session_t *ps, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	if (!wid_get_text_prop(ps, w->client_win, ps->atoms->aWM_WINDOW_ROLE, &strlst, &nstr)) {
		return -1;
	}

	int ret = 0;
	if (!w->role || strcmp(w->role, strlst[0]) != 0) {
		ret = 1;
		free(w->role);
		w->role = strdup(strlst[0]);
	}

	free(strlst);

	log_trace("(%#010x): client = %#010x, role = \"%s\", "
	          "ret = %d",
	          w->base.id, w->client_win, w->role, ret);
	return ret;
}

/**
 * Check if a window is bounding-shaped.
 */
static inline bool win_bounding_shaped(const session_t *ps, xcb_window_t wid) {
	if (ps->shape_exists) {
		xcb_shape_query_extents_reply_t *reply;
		Bool bounding_shaped;

		reply = xcb_shape_query_extents_reply(
		    ps->c, xcb_shape_query_extents(ps->c, wid), NULL);
		bounding_shaped = reply && reply->bounding_shaped;
		free(reply);

		return bounding_shaped;
	}

	return false;
}

static wintype_t wid_get_prop_wintype(session_t *ps, xcb_window_t wid) {
	winprop_t prop =
	    x_get_prop(ps->c, wid, ps->atoms->a_NET_WM_WINDOW_TYPE, 32L, XCB_ATOM_ATOM, 32);

	for (unsigned i = 0; i < prop.nitems; ++i) {
		for (wintype_t j = 1; j < NUM_WINTYPES; ++j) {
			if (ps->atoms_wintypes[j] == (xcb_atom_t)prop.p32[i]) {
				free_winprop(&prop);
				return j;
			}
		}
	}

	free_winprop(&prop);

	return WINTYPE_UNKNOWN;
}

// XXX should distinguish between frame has alpha and window body has alpha
bool win_has_alpha(const struct managed_win *w) {
	return w->pictfmt && w->pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->pictfmt->direct.alpha_mask;
}

bool win_client_has_alpha(const struct managed_win *w) {
	return w->client_pictfmt && w->client_pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->client_pictfmt->direct.alpha_mask;
}

winmode_t win_calc_mode(const struct managed_win *w) {
	if (win_has_alpha(w)) {
		if (w->client_win == XCB_NONE) {
			// This is a window not managed by the WM, and it has
			// alpha, so it's transparent. No need to check WM frame.
			return WMODE_TRANS;
		}
		// The WM window has alpha
		if (win_client_has_alpha(w)) {
			// The client window also has alpha, the entire window is
			// transparent
			return WMODE_TRANS;
		}
		if (win_has_frame(w)) {
			// The client window doesn't have alpha, but we have a WM
			// frame window, which has alpha.
			return WMODE_FRAME_TRANS;
		}
		// Although the WM window has alpha, the frame window has 0 size,
		// so consider the window solid
	}

	// log_trace("Window %#010x(%s) is solid", w->client_win, w->name);
	return WMODE_SOLID;
}

/**
 * Function to be called on window data changes.
 *
 * TODO(yshui) need better name
 */
void win_on_factor_change(session_t *ps, struct managed_win *w) {
	log_debug("Window %#010x (%s) factor change", w->base.id, w->name);
	// Focus needs to be updated first, as other rules might depend on the
	// focused state of the window
	win_update_focused(ps, w);

	w->mode = win_calc_mode(w);
	log_debug("Window mode changed to %d", w->mode);

	w->reg_ignore_valid = false;
}

/**
 * Update cache data in struct _win that depends on window size.
 */
void win_on_win_size_change(session_t *ps, struct managed_win *w) {
	w->width = w->g.width;
	w->height = w->g.height;

	// We don't handle property updates of non-visible windows until they are
	// mapped.
	assert(w->state != WSTATE_UNMAPPED && w->state != WSTATE_DESTROYING &&
	       w->state != WSTATE_UNMAPPING);

	win_set_flags(w, WIN_FLAGS_IMAGES_STALE);
	ps->pending_updates = true;
}

/**
 * Update window type.
 */
void win_update_wintype(session_t *ps, struct managed_win *w) {
	const wintype_t wtype_old = w->window_type;

	// Detect window type here
	w->window_type = wid_get_prop_wintype(ps, w->client_win);

	// Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
	// override-redirect windows or windows without WM_TRANSIENT_FOR as
	// _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
	if (WINTYPE_UNKNOWN == w->window_type) {
		if (w->a.override_redirect ||
		    !wid_has_prop(ps, w->client_win, ps->atoms->aWM_TRANSIENT_FOR))
			w->window_type = WINTYPE_NORMAL;
		else
			w->window_type = WINTYPE_DIALOG;
	}

	if (w->window_type != wtype_old) {
		win_on_factor_change(ps, w);
	}
}

/**
 * Mark a window as the client window of another.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 * @param client window ID of the client window
 */
void win_mark_client(session_t *ps, struct managed_win *w, xcb_window_t client) {
	w->client_win = client;

	// If the window isn't mapped yet, stop here, as the function will be
	// called in map_win()
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	auto e = xcb_request_check(
	    ps->c, xcb_change_window_attributes_checked(
	               ps->c, client, XCB_CW_EVENT_MASK,
	               (const uint32_t[]){determine_evmask(ps, client, WIN_EVMODE_CLIENT)}));
	if (e) {
		log_error("Failed to change event mask of window %#010x", client);
		free(e);
	}

	win_update_wintype(ps, w);

	// Get frame widths. The window is in damaged area already.
	win_update_frame_extents(ps, w, client);

	// Get window name and class if we are tracking them
	win_update_name(ps, w);
	win_update_class(ps, w);
	win_update_role(ps, w);

	// Update everything related to conditions
	win_on_factor_change(ps, w);

	auto r = xcb_get_window_attributes_reply(
	    ps->c, xcb_get_window_attributes(ps->c, w->client_win), &e);
	if (!r) {
		log_error("Failed to get client window attributes");
		return;
	}

	w->client_pictfmt = x_get_pictform_for_visual(ps->c, r->visual);
	free(r);
}

/**
 * Unmark current client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_unmark_client(session_t *ps, struct managed_win *w) {
	xcb_window_t client = w->client_win;
	log_debug("Detaching client window %#010x from frame %#010x (%s)", client,
	          w->base.id, w->name);

	w->client_win = XCB_NONE;

	// Recheck event mask
	xcb_change_window_attributes(
	    ps->c, client, XCB_CW_EVENT_MASK,
	    (const uint32_t[]){determine_evmask(ps, client, WIN_EVMODE_UNKNOWN)});
}

/**
 * Look for the client window of a particular window.
 */
static xcb_window_t find_client_win(session_t *ps, xcb_window_t w) {
	if (wid_has_prop(ps, w, ps->atoms->aWM_STATE)) {
		return w;
	}

	xcb_query_tree_reply_t *reply =
	    xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, w), NULL);
	if (!reply) {
		return 0;
	}

	xcb_window_t *children = xcb_query_tree_children(reply);
	int nchildren = xcb_query_tree_children_length(reply);
	int i;
	xcb_window_t ret = 0;

	for (i = 0; i < nchildren; ++i) {
		if ((ret = find_client_win(ps, children[i]))) {
			break;
		}
	}

	free(reply);

	return ret;
}

/**
 * Recheck client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_recheck_client(session_t *ps, struct managed_win *w) {
	assert(ps->server_grabbed);
	// Initialize wmwin to false
	w->wmwin = false;

	// Look for the client window

	// Always recursively look for a window with WM_STATE, as Fluxbox
	// sets override-redirect flags on all frame windows.
	xcb_window_t cw = find_client_win(ps, w->base.id);
	if (cw) {
		log_debug("(%#010x): client %#010x", w->base.id, cw);
	}
	// Set a window's client window to itself if we couldn't find a
	// client window
	if (!cw) {
		cw = w->base.id;
		w->wmwin = !w->a.override_redirect;
		log_debug("(%#010x): client self (%s)", w->base.id,
		          (w->wmwin ? "wmwin" : "override-redirected"));
	}

	// Unmark the old one
	if (w->client_win && w->client_win != cw) {
		win_unmark_client(ps, w);
	}

	// Mark the new one
	win_mark_client(ps, w, cw);
}

/**
 * Free all resources in a <code>struct _win</code>.
 */
void free_win_res(session_t *ps, struct managed_win *w) {
	// No need to call backend release_image here because
	// finish_unmap_win should've done that for us.
	// XXX unless we are called by session_destroy
	// assert(w->win_data == NULL);
	free_win_res_glx(ps, w);
	free_paint(ps, &w->paint);
	// Above should be done during unmapping
	// Except when we are called by session_destroy

	pixman_region32_fini(&w->bounding_shape);
	// BadDamage may be thrown if the window is destroyed
	set_ignore_cookie(ps, xcb_damage_destroy(ps->c, w->damage));
	rc_region_unref(&w->reg_ignore);
	free(w->name);
	free(w->class_instance);
	free(w->class_general);
	free(w->role);

	free(w->stale_props);
	w->stale_props = NULL;
	w->stale_props_capacity = 0;
}

/// Insert a new window after list_node `prev`
/// New window will be in unmapped state
static struct win *add_win(session_t *ps, xcb_window_t id, struct list_node *prev) {
	log_debug("Adding window %#010x", id);
	struct win *old_w = NULL;
	HASH_FIND_INT(ps->windows, &id, old_w);
	assert(old_w == NULL);

	auto new_w = cmalloc(struct win);
	list_insert_after(prev, &new_w->stack_neighbour);
	new_w->id = id;
	new_w->managed = false;
	new_w->is_new = true;
	new_w->destroyed = false;

	HASH_ADD_INT(ps->windows, id, new_w);
	ps->pending_updates = true;
	return new_w;
}

/// Insert a new win entry at the top of the stack
struct win *add_win_top(session_t *ps, xcb_window_t id) {
	return add_win(ps, id, &ps->window_stack);
}

/// Insert a new window above window with id `below`, if there is no window, add
/// to top New window will be in unmapped state
struct win *add_win_above(session_t *ps, xcb_window_t id, xcb_window_t below) {
	struct win *w = NULL;
	HASH_FIND_INT(ps->windows, &below, w);
	if (!w) {
		if (!list_is_empty(&ps->window_stack)) {
			// `below` window is not found even if the window stack is
			// not empty
			return NULL;
		}
		return add_win_top(ps, id);
	} else {
		// we found something from the hash table, so if the stack is
		// empty, we are in an inconsistent state.
		assert(!list_is_empty(&ps->window_stack));
		return add_win(ps, id, w->stack_neighbour.prev);
	}
}

/// Query the Xorg for information about window `win`
/// `win` pointer might become invalid after this function returns
/// Returns the pointer to the window, might be different from `w`
struct win *fill_win(session_t *ps, struct win *w) {
	static const struct managed_win win_def = {
	    // No need to initialize. (or, you can think that
	    // they are initialized right here).
	    // The following ones are updated during paint or paint preprocess
	    .to_paint = false,
	    .reg_ignore = NULL,
	    // The following ones are updated for other reasons
	    .pixmap_damaged = false,          // updated by damage events
	    .state = WSTATE_UNMAPPED,         // updated by window state changes
	    .in_openclose = true,             // set to false after first map is done,
	                                      // true here because window is just created
	    .reg_ignore_valid = false,        // set to true when damaged
	    .flags = WIN_FLAGS_IMAGES_NONE,        // updated by
	                                           // property/attributes/etc
	                                           // change
	    .stale_props = NULL,
	    .stale_props_capacity = 0,

	    // Initialized in this function
	    .a = {0},
	    .pictfmt = NULL,
	    .client_pictfmt = NULL,
	    .width = 0,
	    .height = 0,
	    .damage = XCB_NONE,

	    // Not initialized until mapped, this variables
	    // have no meaning or have no use until the window
	    // is mapped
	    .win_image = NULL,
	    .prev_trans = NULL,
	    .randr_monitor = -1,
	    .mode = WMODE_TRANS,
	    .ever_damaged = false,
	    .client_win = XCB_NONE,
	    .leader = XCB_NONE,
	    .cache_leader = XCB_NONE,
	    .window_type = WINTYPE_UNKNOWN,
	    .wmwin = false,
	    .focused = false,
	    .frame_extents = MARGIN_INIT,        // in win_mark_client
	    .bounding_shaped = false,
	    .bounding_shape = {0},
	    // following 4 are set in win_mark_client
	    .name = NULL,
	    .class_instance = NULL,
	    .class_general = NULL,
	    .role = NULL,

	    // Initialized during paint
	    .paint = PAINT_INIT,
	};

	assert(!w->destroyed);
	assert(w->is_new);

	w->is_new = false;

	// Reject overlay window and already added windows
	if (w->id == ps->overlay) {
		return w;
	}

	auto duplicated_win = find_managed_win(ps, w->id);
	if (duplicated_win) {
		log_debug("Window %#010x (recorded name: %s) added multiple "
		          "times",
		          w->id, duplicated_win->name);
		return &duplicated_win->base;
	}

	log_debug("Managing window %#010x", w->id);
	xcb_get_window_attributes_cookie_t acookie = xcb_get_window_attributes(ps->c, w->id);
	xcb_get_window_attributes_reply_t *a =
	    xcb_get_window_attributes_reply(ps->c, acookie, NULL);
	if (!a || a->map_state == XCB_MAP_STATE_UNVIEWABLE) {
		// Failed to get window attributes or geometry probably means
		// the window is gone already. Unviewable means the window is
		// already reparented elsewhere.
		// BTW, we don't care about Input Only windows, except for
		// stacking proposes, so we need to keep track of them still.
		free(a);
		return w;
	}

	if (a->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		// No need to manage this window, but we still keep it on the
		// window stack
		w->managed = false;
		free(a);
		return w;
	}

	// Allocate and initialize the new win structure
	auto new_internal = cmalloc(struct managed_win_internal);
	auto new = (struct managed_win *)new_internal;

	// Fill structure
	// We only need to initialize the part that are not initialized
	// by map_win
	*new = win_def;
	new->base = *w;
	new->base.managed = true;
	new->a = *a;
	pixman_region32_init(&new->bounding_shape);

	free(a);

	xcb_generic_error_t *e;
	auto g = xcb_get_geometry_reply(ps->c, xcb_get_geometry(ps->c, w->id), &e);
	if (!g) {
		log_error("Failed to get geometry of window %#010x", w->id);
		free(e);
		free(new);
		return w;
	}
	new->pending_g = (struct win_geometry){
	    .x = g->x,
	    .y = g->y,
	    .width = g->width,
	    .height = g->height,
	};

	free(g);

	// Create Damage for window (if not Input Only)
	new->damage = x_new_id(ps->c);
	e = xcb_request_check(
	    ps->c, xcb_damage_create_checked(ps->c, new->damage, w->id,
	                                     XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
	if (e) {
		log_error("Failed to create damage");
		free(e);
		free(new);
		return w;
	}

	// Set window event mask
	xcb_change_window_attributes(
	    ps->c, new->base.id, XCB_CW_EVENT_MASK,
	    (const uint32_t[]){determine_evmask(ps, new->base.id, WIN_EVMODE_FRAME)});

	// Get notification when the shape of a window changes
	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c, new->base.id, 1);
	}

	new->pictfmt = x_get_pictform_for_visual(ps->c, new->a.visual);
	new->client_pictfmt = NULL;

	list_replace(&w->stack_neighbour, &new->base.stack_neighbour);
	struct win *replaced = NULL;
	HASH_REPLACE_INT(ps->windows, id, &new->base, replaced);
	assert(replaced == w);
	free(w);

	// Set all the stale flags on this new window, so it's properties will get
	// updated when it's mapped
	win_set_flags(new, WIN_FLAGS_CLIENT_STALE | WIN_FLAGS_SIZE_STALE |
	                       WIN_FLAGS_POSITION_STALE | WIN_FLAGS_PROPERTY_STALE |
	                       WIN_FLAGS_FACTOR_CHANGED);
	xcb_atom_t init_stale_props[] = {
	    ps->atoms->a_NET_WM_WINDOW_TYPE,
	    ps->atoms->a_NET_FRAME_EXTENTS,
	    ps->atoms->aWM_NAME,
	    ps->atoms->a_NET_WM_NAME,
	    ps->atoms->aWM_CLASS,
	    ps->atoms->aWM_WINDOW_ROLE,
	    ps->atoms->aWM_CLIENT_LEADER,
	    ps->atoms->aWM_TRANSIENT_FOR,
	};
	win_set_properties_stale(new, init_stale_props, ARR_SIZE(init_stale_props));

	return &new->base;
}

/**
 * Set leader of a window.
 */
static inline void win_set_leader(session_t *ps, struct managed_win *w, xcb_window_t nleader) {
	// If the leader changes
	if (w->leader != nleader) {
		xcb_window_t cache_leader_old = win_get_leader(ps, w);

		w->leader = nleader;

		// Forcefully do this to deal with the case when a child window
		// gets mapped before parent, or when the window is a waypoint
		clear_cache_win_leaders(ps);

		// Update the old and new window group and active_leader if the
		// window could affect their state.
		xcb_window_t cache_leader = win_get_leader(ps, w);
		if (win_is_focused_raw(ps, w) && cache_leader_old != cache_leader) {
			ps->active_leader = cache_leader;

			group_on_factor_change(ps, cache_leader_old);
			group_on_factor_change(ps, cache_leader);
		}

		// Update everything related to conditions
		win_on_factor_change(ps, w);
	}
}

/**
 * Update leader of a window.
 */
void win_update_leader(session_t *ps, struct managed_win *w) {
	xcb_window_t leader = XCB_NONE;

	win_set_leader(ps, w, leader);

	log_trace("(%#010x): client %#010x, leader %#010x, cache %#010x", w->base.id,
	          w->client_win, w->leader, win_get_leader(ps, w));
}

/**
 * Internal function of win_get_leader().
 */
static xcb_window_t win_get_leader_raw(session_t *ps, struct managed_win *w, int recursions) {
	// Rebuild the cache if needed
	if (!w->cache_leader && (w->client_win || w->leader)) {
		// Leader defaults to client window
		if (!(w->cache_leader = w->leader))
			w->cache_leader = w->client_win;

		// If the leader of this window isn't itself, look for its
		// ancestors
		if (w->cache_leader && w->cache_leader != w->client_win) {
			auto wp = find_toplevel(ps, w->cache_leader);
			if (wp) {
				// Dead loop?
				if (recursions > WIN_GET_LEADER_MAX_RECURSION)
					return XCB_NONE;

				w->cache_leader = win_get_leader_raw(ps, wp, recursions + 1);
			}
		}
	}

	return w->cache_leader;
}

/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
bool win_update_class(session_t *ps, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	// Can't do anything if there's no client window
	if (!w->client_win)
		return false;

	// Free and reset old strings
	free(w->class_instance);
	free(w->class_general);
	w->class_instance = NULL;
	w->class_general = NULL;

	// Retrieve the property string list
	if (!wid_get_text_prop(ps, w->client_win, ps->atoms->aWM_CLASS, &strlst, &nstr)) {
		return false;
	}

	// Copy the strings if successful
	w->class_instance = strdup(strlst[0]);

	if (nstr > 1) {
		w->class_general = strdup(strlst[1]);
	}

	free(strlst);

	log_trace("(%#010x): client = %#010x, "
	          "instance = \"%s\", general = \"%s\"",
	          w->base.id, w->client_win, w->class_instance, w->class_general);

	return true;
}

/**
 * Handle window focus change.
 */
static void win_on_focus_change(session_t *ps, struct managed_win *w) {
	// Update everything related to conditions
	win_on_factor_change(ps, w);
}

/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, struct managed_win *w) {
	// Unmapped windows will have their focused state reset on map
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	if (win_is_focused_raw(ps, w)) {
		return;
	}

	auto old_active_win = ps->active_win;
	ps->active_win = w;
	assert(win_is_focused_raw(ps, w));

	if (old_active_win) {
		win_on_focus_change(ps, old_active_win);
	}
	win_on_focus_change(ps, w);
}

/**
 * Get a rectangular region a window occupies.
 */
void win_extents(const struct managed_win *w, region_t *res) {
	pixman_region32_clear(res);
	pixman_region32_union_rect(res, res, w->g.x, w->g.y, (uint)w->width, (uint)w->height);
}

gen_by_val(win_extents);

/**
 * Update the out-dated bounding shape of a window.
 *
 * Mark the window shape as updated
 */
void win_update_bounding_shape(session_t *ps, struct managed_win *w) {
	if (ps->shape_exists) {
		w->bounding_shaped = win_bounding_shaped(ps, w->base.id);
	}

	// We don't handle property updates of non-visible windows until they are
	// mapped.
	assert(w->state != WSTATE_UNMAPPED && w->state != WSTATE_DESTROYING &&
	       w->state != WSTATE_UNMAPPING);

	pixman_region32_clear(&w->bounding_shape);
	// Start with the window rectangular region
	win_get_region_local(w, &w->bounding_shape);

	// Only request for a bounding region if the window is shaped
	// (while loop is used to avoid goto, not an actual loop)
	while (w->bounding_shaped) {
		/*
		 * if window doesn't exist anymore,  this will generate an error
		 * as well as not generate a region.
		 */

		xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(
		    ps->c,
		    xcb_shape_get_rectangles(ps->c, w->base.id, XCB_SHAPE_SK_BOUNDING), NULL);

		if (!r) {
			break;
		}

		xcb_rectangle_t *xrects = xcb_shape_get_rectangles_rectangles(r);
		int nrects = xcb_shape_get_rectangles_rectangles_length(r);
		rect_t *rects = from_x_rects(nrects, xrects);
		free(r);

		region_t br;
		pixman_region32_init_rects(&br, rects, nrects);
		free(rects);

		// Intersect the bounding region we got with the window rectangle,
		// to make sure the bounding region is not bigger than the window
		// rectangle
		pixman_region32_intersect(&w->bounding_shape, &w->bounding_shape, &br);
		pixman_region32_fini(&br);
		break;
	}

	// Window shape changed, we should free old wpaint and shadow pict
	// log_trace("free out dated pict");
	win_set_flags(w, WIN_FLAGS_IMAGES_STALE);
	ps->pending_updates = true;

	free_paint(ps, &w->paint);

	win_on_factor_change(ps, w);
}

/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(session_t *ps, struct managed_win *w, xcb_window_t client) {
	winprop_t prop = x_get_prop(ps->c, client, ps->atoms->a_NET_FRAME_EXTENTS, 4L,
	                            XCB_ATOM_CARDINAL, 32);

	if (prop.nitems == 4) {
		int extents[4];
		for (int i = 0; i < 4; i++) {
			if (prop.c32[i] > (uint32_t)INT_MAX) {
				log_warn("Your window manager sets a absurd "
				         "_NET_FRAME_EXTENTS value (%u), "
				         "ignoring it.",
				         prop.c32[i]);
				memset(extents, 0, sizeof(extents));
				break;
			}
			extents[i] = (int)prop.c32[i];
		}

		const bool changed = w->frame_extents.left != extents[0] ||
		                     w->frame_extents.right != extents[1] ||
		                     w->frame_extents.top != extents[2] ||
		                     w->frame_extents.bottom != extents[3];
		w->frame_extents.left = extents[0];
		w->frame_extents.right = extents[1];
		w->frame_extents.top = extents[2];
		w->frame_extents.bottom = extents[3];

		if (changed) {
			w->reg_ignore_valid = false;
		}
	}

	log_trace("(%#010x): %d, %d, %d, %d", w->base.id, w->frame_extents.left,
	          w->frame_extents.right, w->frame_extents.top, w->frame_extents.bottom);

	free_winprop(&prop);
}

bool win_is_region_ignore_valid(session_t *ps, const struct managed_win *w) {
	win_stack_foreach_managed(i, &ps->window_stack) {
		if (i == w) {
			break;
		}
		if (!i->reg_ignore_valid) {
			return false;
		}
	}
	return true;
}

/**
 * Stop listening for events on a particular window.
 */
void win_ev_stop(session_t *ps, const struct win *w) {
	xcb_change_window_attributes(ps->c, w->id, XCB_CW_EVENT_MASK, (const uint32_t[]){0});

	if (!w->managed) {
		return;
	}

	auto mw = (struct managed_win *)w;
	if (mw->client_win) {
		xcb_change_window_attributes(ps->c, mw->client_win, XCB_CW_EVENT_MASK,
		                             (const uint32_t[]){0});
	}

	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c, w->id, 0);
	}
}

/// Finish the unmapping of a window (e.g. after fading has finished).
/// Doesn't free `w`
static void unmap_win_finish(session_t *ps, struct managed_win *w) {
	w->reg_ignore_valid = false;
	w->state = WSTATE_UNMAPPED;

	// We are in unmap_win, this window definitely was viewable
	if (ps->backend_data) {
		// Only the pixmap needs to be freed and reacquired when mapping.
		if (!win_check_flags_all(w, WIN_FLAGS_PIXMAP_NONE)) {
			win_release_pixmap(ps->backend_data, w);
		}
	} else {
		assert(!w->win_image);
	}

	free_paint(ps, &w->paint);

	// Try again at binding images when the window is mapped next time
	win_clear_flags(w, WIN_FLAGS_IMAGE_ERROR);
}

/// Finish the destruction of a window (e.g. after fading has finished).
/// Frees `w`
static void destroy_win_finish(session_t *ps, struct win *w) {
	log_trace("Trying to finish destroying (%#010x)", w->id);

	auto next_w = win_stack_find_next_managed(ps, &w->stack_neighbour);
	list_remove(&w->stack_neighbour);

	if (w->managed) {
		auto mw = (struct managed_win *)w;

		if (mw->state != WSTATE_UNMAPPED) {
			// Only UNMAPPED state has window resources freed,
			// otherwise we need to call unmap_win_finish to free
			// them.
			// XXX actually we unmap_win_finish only frees the
			//     rendering resources, we still need to call free_win_res.
			//     will fix later.
			unmap_win_finish(ps, mw);
		}

		// Invalidate reg_ignore of windows below this one
		// TODO(yshui) what if next_w is not mapped??
		/* TODO(yshui) seriously figure out how reg_ignore behaves.
		 * I think if `w` is unmapped, and destroyed after
		 * paint happened at least once, w->reg_ignore_valid would
		 * be true, and there is no need to invalid w->next->reg_ignore
		 * when w is destroyed. */
		if (next_w) {
			rc_region_unref(&next_w->reg_ignore);
			next_w->reg_ignore_valid = false;
		}

		if (mw == ps->active_win) {
			// Usually, the window cannot be the focused at
			// destruction. FocusOut should be generated before the
			// window is destroyed. We do this check just to be
			// completely sure we don't have dangling references.
			log_debug("window %#010x (%s) is destroyed while being "
			          "focused",
			          w->id, mw->name);
			ps->active_win = NULL;
		}

		free_win_res(ps, mw);

		// Drop w from all prev_trans to avoid accessing freed memory in
		// repair_win()
		// TODO(yshui) there can only be one prev_trans pointing to w
		win_stack_foreach_managed(w2, &ps->window_stack) {
			if (mw == w2->prev_trans) {
				w2->prev_trans = NULL;
			}
		}
	}

	free(w);
}

static void map_win_finish(struct managed_win *w) {
	w->in_openclose = false;
	w->state = WSTATE_MAPPED;
}

/// Move window `w` so it's before `next` in the list
static inline void restack_win(session_t *ps, struct win *w, struct list_node *next) {
	struct managed_win *mw = NULL;
	if (w->managed) {
		mw = (struct managed_win *)w;
	}

	if (mw) {
		// This invalidates all reg_ignore below the new stack position of
		// `w`
		mw->reg_ignore_valid = false;
		rc_region_unref(&mw->reg_ignore);

		// This invalidates all reg_ignore below the old stack position of
		// `w`
		auto next_w = win_stack_find_next_managed(ps, &w->stack_neighbour);
		if (next_w) {
			next_w->reg_ignore_valid = false;
			rc_region_unref(&next_w->reg_ignore);
		}
	}

	list_move_before(&w->stack_neighbour, next);

	// add damage for this window
	if (mw) {
		add_damage_from_win(ps, mw);
	}
}

/// Move window `w` so it's right above `below`
void restack_above(session_t *ps, struct win *w, xcb_window_t below) {
	xcb_window_t old_below;

	if (!list_node_is_last(&ps->window_stack, &w->stack_neighbour)) {
		old_below = list_next_entry(w, stack_neighbour)->id;
	} else {
		old_below = XCB_NONE;
	}
	log_debug("Restack %#010x (%s), old_below: %#010x, new_below: %#010x", w->id,
	          win_get_name_if_managed(w), old_below, below);

	if (old_below != below) {
		struct list_node *new_next;
		if (!below) {
			new_next = &ps->window_stack;
		} else {
			struct win *tmp_w = NULL;
			HASH_FIND_INT(ps->windows, &below, tmp_w);

			if (!tmp_w) {
				log_error("Failed to found new below window %#010x.", below);
				return;
			}

			new_next = &tmp_w->stack_neighbour;
		}
		restack_win(ps, w, new_next);
	}
}

void restack_bottom(session_t *ps, struct win *w) {
	restack_above(ps, w, 0);
}

void restack_top(session_t *ps, struct win *w) {
	log_debug("Restack %#010x (%s) to top", w->id, win_get_name_if_managed(w));
	if (&w->stack_neighbour == ps->window_stack.next) {
		// already at top
		return;
	}
	restack_win(ps, w, ps->window_stack.next);
}

/// Start destroying a window. Windows cannot always be destroyed immediately
/// because of fading and such.
///
/// @return whether the window has finished destroying and is freed
bool destroy_win_start(session_t *ps, struct win *w) {
	auto mw = (struct managed_win *)w;
	assert(w);

	log_debug("Destroying %#010x \"%s\", managed = %d", w->id,
	          (w->managed ? mw->name : NULL), w->managed);

	// Delete destroyed window from the hash table, even though the window
	// might still be rendered for a while. We need to make sure future window
	// with the same window id won't confuse us. Keep the window in the window
	// stack if it's managed and mapped, since we might still need to render
	// it (e.g. fading out). Window will be removed from the stack when it
	// finishes destroying.
	HASH_DEL(ps->windows, w);

	if (!w->managed || mw->state == WSTATE_UNMAPPED) {
		// Window is already unmapped, or is an unmanged window, just
		// destroy it
		destroy_win_finish(ps, w);
		return true;
	}

	if (w->managed) {
		// Clear IMAGES_STALE flags since the window is destroyed: Clear
		// PIXMAP_STALE as there is no pixmap available anymore, so STALE
		// doesn't make sense.
		win_clear_flags(mw, WIN_FLAGS_IMAGES_STALE);

		// If size/shape/position information is stale,
		// win_process_update_flags will update them and add the new
		// window extents to damage. Since the window has been destroyed,
		// we cannot get the complete information at this point, so we
		// just add what we currently have to the damage.
		if (win_check_flags_any(mw, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE)) {
			add_damage_from_win(ps, mw);
		}

		// Clear some flags about stale window information. Because now
		// the window is destroyed, we can't update them anyway.
		win_clear_flags(mw, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE |
		                        WIN_FLAGS_PROPERTY_STALE |
		                        WIN_FLAGS_FACTOR_CHANGED | WIN_FLAGS_CLIENT_STALE);

		// Update state flags of a managed window
		mw->state = WSTATE_DESTROYING;
		mw->a.map_state = XCB_MAP_STATE_UNMAPPED;
		mw->in_openclose = true;
	}

	// don't need win_ev_stop because the window is gone anyway

	if (!ps->redirected) {
		// Skip transition if we are not rendering
		return win_finish_transition(ps, mw);
	}

	return false;
}

void unmap_win_start(session_t *ps, struct managed_win *w) {
	assert(w);
	assert(w->base.managed);
	assert(w->a._class != XCB_WINDOW_CLASS_INPUT_ONLY);

	log_debug("Unmapping %#010x \"%s\"", w->base.id, w->name);

	if (unlikely(w->state == WSTATE_DESTROYING)) {
		log_warn("Trying to undestroy a window?");
		assert(false);
	}

	bool was_damaged = w->ever_damaged;
	w->ever_damaged = false;

	if (unlikely(w->state == WSTATE_UNMAPPING || w->state == WSTATE_UNMAPPED)) {
		if (win_check_flags_all(w, WIN_FLAGS_MAPPED)) {
			// Clear the pending map as this window is now unmapped
			win_clear_flags(w, WIN_FLAGS_MAPPED);
		} else {
			log_warn("Trying to unmapping an already unmapped window "
			         "%#010x "
			         "\"%s\"",
			         w->base.id, w->name);
			assert(false);
		}
		return;
	}

	// Note we don't update focused window here. This will either be
	// triggered by subsequence Focus{In, Out} event, or by recheck_focus

	w->a.map_state = XCB_MAP_STATE_UNMAPPED;
	w->state = WSTATE_UNMAPPING;

	if (!ps->redirected || !was_damaged) {
		// If we are not redirected, we skip fading because we aren't
		// rendering anything anyway. If the window wasn't ever damaged,
		// it shouldn't be painted either. But a fading out window is
		// always painted, so we have to skip fading here.
		CHECK(!win_finish_transition(ps, w));
	}
}

bool win_finish_transition(session_t *ps, struct managed_win *w) {
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
		return false;
	}

	switch (w->state) {
	case WSTATE_UNMAPPING: unmap_win_finish(ps, w); return false;
	case WSTATE_DESTROYING: destroy_win_finish(ps, &w->base); return true;
	case WSTATE_MAPPING: map_win_finish(w); return false;
	default: unreachable;
	}

	return false;
}

// TODO(absolutelynothelix): rename to x_update_win_(randr_?)monitor and move to
// the x.c.
void win_update_monitor(int nmons, region_t *mons, struct managed_win *mw) {
	mw->randr_monitor = -1;
	for (int i = 0; i < nmons; i++) {
		auto e = pixman_region32_extents(&mons[i]);
		if (e->x1 <= mw->g.x && e->y1 <= mw->g.y &&
		    e->x2 >= mw->g.x + mw->width && e->y2 >= mw->g.y + mw->height) {
			mw->randr_monitor = i;
			log_debug("Window %#010x (%s), %dx%d+%dx%d, is entirely on the "
			          "monitor %d (%dx%d+%dx%d)",
			          mw->base.id, mw->name, mw->g.x, mw->g.y, mw->width,
			          mw->height, i, e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1);
			return;
		}
	}
	log_debug("Window %#010x (%s), %dx%d+%dx%d, is not entirely on any monitor",
	          mw->base.id, mw->name, mw->g.x, mw->g.y, mw->width, mw->height);
}

/// Map an already registered window
void map_win_start(session_t *ps, struct managed_win *w) {
	assert(ps->server_grabbed);
	assert(w);

	// Don't care about window mapping if it's an InputOnly window
	// Also, try avoiding mapping a window twice
	if (w->a._class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		return;
	}

	log_debug("Mapping (%#010x \"%s\")", w->base.id, w->name);

	assert(w->state != WSTATE_DESTROYING);
	if (w->state != WSTATE_UNMAPPED && w->state != WSTATE_UNMAPPING) {
		log_warn("Mapping an already mapped window");
		return;
	}

	if (w->state == WSTATE_UNMAPPING) {
		CHECK(!win_finish_transition(ps, w));
		// We skipped the unmapping process, the window was rendered, now
		// it is not anymore. So we need to mark the then unmapping window
		// as damaged.
		//
		// Solves problem when, for example, a window is unmapped then
		// mapped in a different location
		add_damage_from_win(ps, w);
		assert(w);
	}

	assert(w->state == WSTATE_UNMAPPED);

	// Rant: window size could change after we queried its geometry here and
	// before we get its pixmap. Later, when we get back to the event
	// processing loop, we will get the notification about size change from
	// Xserver and try to refresh the pixmap, while the pixmap is actually
	// already up-to-date (i.e. the notification is stale). There is basically
	// no real way to prevent this, aside from grabbing the server.

	// XXX Can we assume map_state is always viewable?
	w->a.map_state = XCB_MAP_STATE_VIEWABLE;

	// Update window mode here to check for ARGB windows
	w->mode = win_calc_mode(w);

	log_debug("Window (%#010x) has type %s", w->base.id, WINTYPES[w->window_type]);

	// XXX We need to make sure that win_data is available
	// iff `state` is MAPPED
	w->state = WSTATE_MAPPING;

	// Cannot set w->ever_damaged = false here, since window mapping could be
	// delayed, so a damage event might have already arrived before this
	// function is called. But this should be unnecessary in the first place,
	// since ever_damaged is set to false in unmap_win_finish anyway.

	// Sets the WIN_FLAGS_IMAGES_STALE flag so later in the critical section
	// the window's image will be bound

	win_set_flags(w, WIN_FLAGS_PIXMAP_STALE);

	if (!ps->redirected) {
		CHECK(!win_finish_transition(ps, w));
	}
}

/**
 * Find a managed window from window id in window linked list of the session.
 */
struct win *find_win(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	struct win *w = NULL;
	HASH_FIND_INT(ps->windows, &id, w);
	assert(w == NULL || !w->destroyed);
	return w;
}

/**
 * Find a managed window from window id in window linked list of the session.
 */
struct managed_win *find_managed_win(session_t *ps, xcb_window_t id) {
	struct win *w = find_win(ps, id);
	if (!w || !w->managed) {
		return NULL;
	}

	auto mw = (struct managed_win *)w;
	assert(mw->state != WSTATE_DESTROYING);
	return mw;
}

/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param id window ID
 * @return struct win object of the found window, NULL if not found
 */
struct managed_win *find_toplevel(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}

		auto mw = (struct managed_win *)w;
		if (mw->client_win == id) {
			return mw;
		}
	}

	return NULL;
}

/**
 * Find a managed window that is, or is a parent of `wid`.
 *
 * @param ps current session
 * @param wid window ID
 * @return struct _win object of the found window, NULL if not found
 */
struct managed_win *find_managed_window_or_parent(session_t *ps, xcb_window_t wid) {
	// TODO(yshui) this should probably be an "update tree", then
	// find_toplevel. current approach is a bit more "racy", as the server
	// state might be ahead of our state
	struct win *w = NULL;

	// We traverse through its ancestors to find out the frame
	// Using find_win here because if we found a unmanaged window we know
	// about, we can stop early.
	while (wid && wid != ps->root && !(w = find_win(ps, wid))) {
		// xcb_query_tree probably fails if you run picom when X is
		// somehow initializing (like add it in .xinitrc). In this case
		// just leave it alone.
		auto reply = xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, wid), NULL);
		if (reply == NULL) {
			break;
		}

		wid = reply->parent;
		free(reply);
	}

	if (w == NULL || !w->managed) {
		return NULL;
	}

	return (struct managed_win *)w;
}

/**
 * Check if a rectangle includes the whole screen.
 */
static inline bool rect_is_fullscreen(const session_t *ps, int x, int y, int wid, int hei) {
	return (x <= 0 && y <= 0 && (x + wid) >= ps->root_width && (y + hei) >= ps->root_height);
}

/**
 * Check if a window is fulscreen using EWMH
 *
 * TODO(yshui) cache this property
 */
static inline bool
win_is_fullscreen_xcb(xcb_connection_t *c, const struct atom *a, const xcb_window_t w) {
	xcb_get_property_cookie_t prop =
	    xcb_get_property(c, 0, w, a->a_NET_WM_STATE, XCB_ATOM_ATOM, 0, 12);
	xcb_get_property_reply_t *reply = xcb_get_property_reply(c, prop, NULL);
	if (!reply) {
		return false;
	}

	if (reply->length) {
		xcb_atom_t *val = xcb_get_property_value(reply);
		for (uint32_t i = 0; i < reply->length; i++) {
			if (val[i] != a->a_NET_WM_STATE_FULLSCREEN) {
				continue;
			}
			free(reply);
			return true;
		}
	}
	free(reply);
	return false;
}

/// Set flags on a window. Some sanity checks are performed
void win_set_flags(struct managed_win *w, uint64_t flags) {
	log_debug("Set flags %" PRIu64 " to window %#010x (%s)", flags, w->base.id, w->name);
	if (unlikely(w->state == WSTATE_DESTROYING)) {
		log_error("Flags set on a destroyed window %#010x (%s)", w->base.id, w->name);
		return;
	}

	w->flags |= flags;
}

/// Clear flags on a window. Some sanity checks are performed
void win_clear_flags(struct managed_win *w, uint64_t flags) {
	log_debug("Clear flags %" PRIu64 " from window %#010x (%s)", flags, w->base.id,
	          w->name);
	if (unlikely(w->state == WSTATE_DESTROYING)) {
		log_warn("Flags cleared on a destroyed window %#010x (%s)", w->base.id,
		         w->name);
		return;
	}

	w->flags = w->flags & (~flags);
}

void win_set_properties_stale(struct managed_win *w, const xcb_atom_t *props, int nprops) {
	const auto bits_per_element = sizeof(*w->stale_props) * 8;
	size_t new_capacity = w->stale_props_capacity;

	// Calculate the new capacity of the properties array
	for (int i = 0; i < nprops; i++) {
		if (props[i] >= new_capacity * bits_per_element) {
			new_capacity = props[i] / bits_per_element + 1;
		}
	}

	// Reallocate if necessary
	if (new_capacity > w->stale_props_capacity) {
		w->stale_props =
		    realloc(w->stale_props, new_capacity * sizeof(*w->stale_props));

		// Clear the content of the newly allocated bytes
		memset(w->stale_props + w->stale_props_capacity, 0,
		       (new_capacity - w->stale_props_capacity) * sizeof(*w->stale_props));
		w->stale_props_capacity = new_capacity;
	}

	// Set the property bits
	for (int i = 0; i < nprops; i++) {
		w->stale_props[props[i] / bits_per_element] |=
		    1UL << (props[i] % bits_per_element);
	}
	win_set_flags(w, WIN_FLAGS_PROPERTY_STALE);
}

static void win_clear_all_properties_stale(struct managed_win *w) {
	memset(w->stale_props, 0, w->stale_props_capacity * sizeof(*w->stale_props));
	win_clear_flags(w, WIN_FLAGS_PROPERTY_STALE);
}

static bool win_fetch_and_unset_property_stale(struct managed_win *w, xcb_atom_t prop) {
	const auto bits_per_element = sizeof(*w->stale_props) * 8;
	if (prop >= w->stale_props_capacity * bits_per_element) {
		return false;
	}

	const auto mask = 1UL << (prop % bits_per_element);
	bool ret = w->stale_props[prop / bits_per_element] & mask;
	w->stale_props[prop / bits_per_element] &= ~mask;
	return ret;
}

bool win_check_flags_any(struct managed_win *w, uint64_t flags) {
	return (w->flags & flags) != 0;
}

bool win_check_flags_all(struct managed_win *w, uint64_t flags) {
	return (w->flags & flags) == flags;
}

/**
 * Check if a window is a fullscreen window.
 */
bool win_is_fullscreen(const session_t *ps, const struct managed_win *w) {
	return win_is_fullscreen_xcb(ps->c, ps->atoms, w->client_win);
}

/**
 * Check if a window is focused, without using any focus rules or forced focus
 * settings
 */
bool win_is_focused_raw(const session_t *ps, const struct managed_win *w) {
	return w->a.map_state == XCB_MAP_STATE_VIEWABLE && ps->active_win == w;
}

// Find the managed window immediately below `i` in the window stack
struct managed_win *
win_stack_find_next_managed(const session_t *ps, const struct list_node *i) {
	while (!list_node_is_last(&ps->window_stack, i)) {
		auto next = list_entry(i->next, struct win, stack_neighbour);
		if (next->managed) {
			return (struct managed_win *)next;
		}
		i = &next->stack_neighbour;
	}
	return NULL;
}

/// Return whether this window is mapped on the X server side
bool win_is_mapped_in_x(const struct managed_win *w) {
	return w->state == WSTATE_MAPPING || w->state == WSTATE_MAPPED ||
	       (w->flags & WIN_FLAGS_MAPPED);
}
