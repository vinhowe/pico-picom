// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stddef.h>
#include <stdio.h>
#include <xcb/xcb.h>

#include "utils.h"

struct session;
struct backend_base;

// A list of known driver quirks:
// *  NVIDIA driver doesn't like seeing the same pixmap under different
//    ids, so avoid naming the pixmap again when it didn't actually change.

/// A list of possible drivers.
/// The driver situation is a bit complicated. There are two drivers we care about: the
/// DDX, and the OpenGL driver. They are usually paired, but not always, since there is
/// also the generic modesetting driver.
/// This enum represents _both_ drivers.
enum driver {
	DRIVER_AMDGPU = 1,        // AMDGPU for DDX, radeonsi for OpenGL
	DRIVER_RADEON = 2,        // ATI for DDX, mesa r600 for OpenGL
	DRIVER_FGLRX = 4,
	DRIVER_NVIDIA = 8,
	DRIVER_NOUVEAU = 16,
	DRIVER_INTEL = 32,
	DRIVER_MODESETTING = 64,
};

/// Return a list of all drivers currently in use by the X server.
/// Note, this is a best-effort test, so no guarantee all drivers will be detected.
enum driver detect_driver(xcb_connection_t *, struct backend_base *, xcb_window_t);

/// Apply driver specified global workarounds. It's safe to call this multiple times.
void apply_driver_workarounds(struct session *ps, enum driver);