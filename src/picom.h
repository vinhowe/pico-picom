// SPDX-License-Identifier: MIT
// Copyright (c)

// Throw everything in here.
// !!! DON'T !!!

// === Includes ===

#include <locale.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xcb/xproto.h>

#include <X11/Xutil.h>
#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "log.h"        // XXX clean up
#include "region.h"
#include "render.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "x.h"

enum root_flags {
	ROOT_FLAGS_SCREEN_CHANGE = 1,        // Received RandR screen change notify, we
	                                     // use this to track refresh rate changes
	ROOT_FLAGS_CONFIGURED = 2        // Received configure notify on the root window
};

// == Functions ==
// TODO(yshui) move static inline functions that are only used in picom.c, into picom.c

void add_damage(session_t *ps, const region_t *damage);

uint32_t determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode);

void root_damaged(session_t *ps);

void queue_redraw(session_t *ps);

void discard_pending(session_t *ps, uint32_t sequence);

void set_root_flags(session_t *ps, uint64_t flags);

void quit(session_t *ps);

xcb_window_t session_get_target_window(session_t *);

uint8_t session_redirection_mode(session_t *ps);