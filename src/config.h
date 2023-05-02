// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

/// Common functions and definitions for configuration parsing
/// Used for command line arguments

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include "uthash_extra.h"

#include "compiler.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "win_defs.h"

typedef struct session session_t;

/// @brief Possible backends
enum backend {
	BKEND_GLX,
	NUM_BKEND,
};

typedef struct win_option_mask {
	bool focus : 1;
	bool redir_ignore : 1;
} win_option_mask_t;

typedef struct win_option {
	bool focus;
	bool redir_ignore;
} win_option_t;

/// Structure representing all options.
typedef struct options {
	// === General ===
	/// The backend in use.
	enum backend backend;
	/// Whether to sync X drawing with X Sync fence to avoid certain delay
	/// issues with GLX backend.
	bool xrender_sync_fence;
	/// Whether to avoid using stencil buffer under GLX backend. Might be
	/// unsafe.
	bool glx_no_stencil;
	/// Whether to avoid rebinding pixmap on window damage.
	bool glx_no_rebind_pixmap;
	/// Path to log file.
	char *logpath;
	/// Whether to show all X errors.
	bool show_all_xerrors;
	/// Window type option override.
	win_option_t wintype_option[NUM_WINTYPES];

	// === VSync & software optimization ===
	// TODO(vinhowe): move this to session type to be disabled only when
	// GLX_EXT_buffer_age is not supported
	/// Whether use damage information to help limit the area to paint
	bool use_damage;
} options_t;

extern const char *const BACKEND_STRS[NUM_BKEND + 1];

bool must_use parse_long(const char *, long *);
bool must_use parse_int(const char *, int *);

void set_default_winopts(options_t *, win_option_mask_t *);

/**
 * Parse a backend option argument.
 */
static inline attr_pure enum backend parse_backend(const char *str) {
	for (enum backend i = 0; BACKEND_STRS[i]; ++i) {
		if (!strcasecmp(str, BACKEND_STRS[i])) {
			return i;
		}
	}
	log_error("Invalid backend argument: %s", str);
	return NUM_BKEND;
}

// vim: set noet sw=8 ts=8 :
