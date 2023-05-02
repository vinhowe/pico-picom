// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
#pragma once
#include <stdbool.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include <backend/backend.h>

#include "uthash_extra.h"

// FIXME shouldn't need this
#include <GL/gl.h>

#include "compiler.h"
#include "list.h"
#include "region.h"
#include "render.h"
#include "types.h"
#include "utils.h"
#include "win_defs.h"
#include "x.h"

struct backend_base;
typedef struct session session_t;
typedef struct _glx_texture glx_texture_t;

#define win_stack_foreach_managed(w, win_stack)                                          \
	list_foreach(struct managed_win, w, win_stack, base.stack_neighbour) if (w->base.managed)

#define win_stack_foreach_managed_safe(w, win_stack)                                     \
	list_foreach_safe(struct managed_win, w, win_stack,                              \
	                  base.stack_neighbour) if (w->base.managed)

/// An entry in the window stack. May or may not correspond to a window we know about.
struct window_stack_entry {
	struct list_node stack_neighbour;
	/// The actual window correspond to this stack entry. NULL if we didn't know about
	/// this window (e.g. an InputOnly window, or we haven't handled the window
	/// creation yet)
	struct win *win;
	/// The window id. Might not be unique in the stack, because there might be
	/// destroyed window still fading out in the stack.
	xcb_window_t id;
};

/**
 * About coordinate systems
 *
 * In general, X is the horizontal axis, Y is the vertical axis.
 * X goes from left to right, Y goes downwards.
 *
 * Global: the origin is the top left corner of the Xorg screen.
 * Local: the origin is the top left corner of the window.
 */

/// Structure representing a top-level managed window.
typedef struct win win;
struct win {
	UT_hash_handle hh;
	struct list_node stack_neighbour;
	/// ID of the top-level frame window.
	xcb_window_t id;
	/// Whether the window is destroyed from Xorg's perspective
	bool destroyed : 1;
	/// True if we just received CreateNotify, and haven't queried X for any info
	/// about the window
	bool is_new : 1;
	/// True if this window is managed, i.e. this struct is actually a `managed_win`.
	/// Always false if `is_new` is true.
	bool managed : 1;
};

struct win_geometry {
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
};

struct managed_win {
	struct win base;
	/// backend data attached to this window. Only available when
	/// `state` is not UNMAPPED
	void *win_image;
	/// Pointer to the next higher window to paint.
	struct managed_win *prev_trans;
	/// Number of windows above this window
	int stacking_rank;
	// TODO(yshui) rethink reg_ignore

	// Core members
	/// The "mapped state" of this window, doesn't necessary
	/// match X mapped state, because of fading.
	winstate_t state;
	/// Window attributes.
	xcb_get_window_attributes_reply_t a;
	/// The geometry of the window body.
	struct win_geometry g;
	/// Updated geometry received in events
	struct win_geometry pending_g;
	/// X RandR monitor this window is on.
	int randr_monitor;
	/// Window visual pict format
	const xcb_render_pictforminfo_t *pictfmt;
	/// Client window visual pict format
	const xcb_render_pictforminfo_t *client_pictfmt;
	/// Window painting mode.
	winmode_t mode;
	/// Whether the window has been damaged at least once.
	bool ever_damaged;
	/// Whether the window was damaged after last paint.
	bool pixmap_damaged;
	/// Damage of the window.
	xcb_damage_damage_t damage;
	/// Paint info of the window.
	paint_t paint;
	/// bitmap for properties which needs to be updated
	uint64_t *stale_props;
	/// number of uint64_ts that has been allocated for stale_props
	size_t stale_props_capacity;

	/// Bounding shape of the window. In local coordinates.
	/// See above about coordinate systems.
	region_t bounding_shape;
	/// Window flags. Definitions above.
	uint64_t flags;
	/// The region of screen that will be obscured when windows above is painted,
	/// in global coordinates.
	/// We use this to reduce the pixels that needed to be paint when painting
	/// this window and anything underneath. Depends on window geometry,
	/// window mapped/unmapped state, window mode of the windows above.
	/// DOES NOT INCLUDE the body of THIS WINDOW.
	/// NULL means reg_ignore has not been calculated for this window.
	rc_region_t *reg_ignore;
	/// Whether the reg_ignore of all windows beneath this window are valid
	bool reg_ignore_valid;
	/// Cached width/height of the window.
	int width, height;
	/// Whether the window is bounding-shaped.
	bool bounding_shaped;
	/// Whether this window is to be painted.
	bool to_paint;
	/// Whether this window is in open/close state.
	bool in_openclose;

	// Client window related members
	/// ID of the top-level client window of the window.
	xcb_window_t client_win;
	/// Type of the window.
	wintype_t window_type;
	/// Whether it looks like a WM window. We consider a window WM window if
	/// it does not have a decedent with WM_STATE and it is not override-
	/// redirected itself.
	bool wmwin;
	/// Leader window ID of the window.
	xcb_window_t leader;
	/// Cached topmost window ID of the window.
	xcb_window_t cache_leader;

	// Focus-related members
	/// Whether the window is to be considered focused.
	bool focused;

	// Blacklist related members
	/// Name of the window.
	char *name;
	/// Window instance class of the window.
	char *class_instance;
	/// Window general class of the window.
	char *class_general;
	/// <code>WM_WINDOW_ROLE</code> value of the window.
	char *role;

	/// Frame extents. Acquired from _NET_FRAME_EXTENTS.
	margin_t frame_extents;
};

/// Process pending updates/images flags on a window. Has to be called in X critical
/// section
void win_process_update_flags(session_t *ps, struct managed_win *w);
void win_process_image_flags(session_t *ps, struct managed_win *w);

// TODO(vinhowe): see if we can get rid of this step and just unmap immediately
/// Start the unmap of a window. We cannot unmap immediately since we might need to fade
/// the window out.
void unmap_win_start(struct session *, struct managed_win *);

// TODO(vinhowe): see if we can get rid of this step and just unmap immediately
/// Start the mapping of a window. We cannot map immediately since we might need to fade
/// the window in.
void map_win_start(struct session *, struct managed_win *);

/// Start the destroying of a window. Windows cannot always be destroyed immediately
/// because of fading and such.
bool must_use destroy_win_start(session_t *ps, struct win *w);

/// Release images bound with a window, set the *_NONE flags on the window. Only to be
/// used when de-initializing the backend outside of win.c
void win_release_images(struct backend_base *base, struct managed_win *w);
winmode_t attr_pure win_calc_mode(const struct managed_win *w);
/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, struct managed_win *w);
void win_on_factor_change(session_t *ps, struct managed_win *w);
/**
 * Update cache data in struct _win that depends on window size.
 */
void win_on_win_size_change(session_t *ps, struct managed_win *w);
void win_unmark_client(session_t *ps, struct managed_win *w);
void win_recheck_client(session_t *ps, struct managed_win *w);

// TODO(absolutelynothelix): rename to x_update_win_(randr_?)monitor and move to
// the x.h.
void win_update_monitor(int nmons, region_t *mons, struct managed_win *mw);

/**
 * Retrieve the bounding shape of a window.
 */
// XXX was win_border_size
void win_update_bounding_shape(session_t *ps, struct managed_win *w);
/**
 * Get a rectangular region in global coordinates a window occupies.
 */
void win_extents(const struct managed_win *w, region_t *res);
region_t win_extents_by_val(const struct managed_win *w);
/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, const struct managed_win *w);
/**
 * Get a rectangular region a window occupies, excluding frame.
 *
 * Return region in global coordinates.
 */
void win_get_region_noframe_local(const struct managed_win *w, region_t *);

/// Get the region for the frame of the window
void win_get_region_frame_local(const struct managed_win *w, region_t *res);
/// Get the region for the frame of the window, by value
region_t win_get_region_frame_local_by_val(const struct managed_win *w);
/// Insert a new window above window with id `below`, if there is no window, add to top
/// New window will be in unmapped state
struct win *add_win_above(session_t *ps, xcb_window_t id, xcb_window_t below);
/// Insert a new win entry at the top of the stack
struct win *add_win_top(session_t *ps, xcb_window_t id);
/// Query the Xorg for information about window `win`
/// `win` pointer might become invalid after this function returns
struct win *fill_win(session_t *ps, struct win *win);
/// Move window `w` to be right above `below`
void restack_above(session_t *ps, struct win *w, xcb_window_t below);
/// Move window `w` to the bottom of the stack
void restack_bottom(session_t *ps, struct win *w);
/// Move window `w` to the top of the stack
void restack_top(session_t *ps, struct win *w);

// Stop receiving events (except ConfigureNotify, XXX why?) from a window
void win_ev_stop(session_t *ps, const struct win *w);

/// Skip the current in progress fading of window,
/// transition the window straight to its end state
///
/// @return whether the window is destroyed and freed
bool must_use win_finish_transition(session_t *ps, struct managed_win *w);
/**
 * Find a managed window from window id in window linked list of the session.
 */
struct managed_win *find_managed_win(session_t *ps, xcb_window_t id);
struct win *find_win(session_t *ps, xcb_window_t id);
struct managed_win *find_toplevel(session_t *ps, xcb_window_t id);
/**
 * Find a managed window that is, or is a parent of `wid`.
 *
 * @param ps current session
 * @param wid window ID
 * @return struct _win object of the found window, NULL if not found
 */
struct managed_win *find_managed_window_or_parent(session_t *ps, xcb_window_t wid);

/**
 * Check if a window is a fullscreen window.
 */
bool attr_pure win_is_fullscreen(const session_t *ps, const struct managed_win *w);

/**
 * Check if a window is focused, without using any focus rules or forced focus settings
 */
bool attr_pure win_is_focused_raw(const session_t *ps, const struct managed_win *w);

/// check if window has ARGB visual
bool attr_pure win_has_alpha(const struct managed_win *w);

/// check if reg_ignore_valid is true for all windows above us
bool attr_pure win_is_region_ignore_valid(session_t *ps, const struct managed_win *w);

/// Whether a given window is mapped on the X server side
bool win_is_mapped_in_x(const struct managed_win *w);

// Find the managed window immediately below `w` in the window stack
struct managed_win *attr_pure win_stack_find_next_managed(const session_t *ps,
                                                          const struct list_node *w);
/// Set flags on a window. Some sanity checks are performed
void win_set_flags(struct managed_win *w, uint64_t flags);
/// Clear flags on a window. Some sanity checks are performed
void win_clear_flags(struct managed_win *w, uint64_t flags);
/// Returns true if any of the flags in `flags` is set
bool win_check_flags_any(struct managed_win *w, uint64_t flags);
/// Returns true if all of the flags in `flags` are set
bool win_check_flags_all(struct managed_win *w, uint64_t flags);
/// Mark properties as stale for a window
void win_set_properties_stale(struct managed_win *w, const xcb_atom_t *prop, int nprops);

static inline attr_unused void win_set_property_stale(struct managed_win *w, xcb_atom_t prop) {
	return win_set_properties_stale(w, (xcb_atom_t[]){prop}, 1);
}

/// Free all resources in a struct win
void free_win_res(session_t *ps, struct managed_win *w);

static inline region_t attr_unused win_get_bounding_shape_global_by_val(struct managed_win *w) {
	region_t ret;
	pixman_region32_init(&ret);
	pixman_region32_copy(&ret, &w->bounding_shape);
	pixman_region32_translate(&ret, w->g.x, w->g.y);
	return ret;
}

/**
 * Check whether a window has WM frames.
 */
static inline bool attr_pure attr_unused win_has_frame(const struct managed_win *w) {
	return w->frame_extents.top || w->frame_extents.left || w->frame_extents.right ||
	       w->frame_extents.bottom;
}
