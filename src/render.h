// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <stdbool.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include "backend/gl/glx.h"
#include "region.h"

typedef struct _glx_texture glx_texture_t;
typedef struct glx_prog_main glx_prog_main_t;
typedef struct session session_t;

struct managed_win;

typedef struct paint {
	xcb_pixmap_t pixmap;
	xcb_render_picture_t pict;
	glx_texture_t *ptex;
	struct glx_fbconfig_info *fbcfg;
} paint_t;

void render(session_t *ps, int x, int y, int dx, int dy, int w, int h, bool argb,
            glx_texture_t *ptex, const region_t *reg_paint, const glx_prog_main_t *pprogram);
void paint_one(session_t *ps, struct managed_win *w, const region_t *reg_paint);

void paint_all(session_t *ps, struct managed_win *const t, bool ignore_damage);

void free_picture(xcb_connection_t *c, xcb_render_picture_t *p);

void free_paint(session_t *ps, paint_t *ppaint);
void free_root_tile(session_t *ps);

bool init_render(session_t *ps);