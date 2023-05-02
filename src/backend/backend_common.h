// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <stdbool.h>

#include "backend.h"
#include "region.h"

typedef struct session session_t;
typedef struct win win;
typedef struct backend_base backend_t;
struct backend_operations;

struct backend_image_inner_base {
	int refcount;
	bool has_alpha;
};

struct backend_image {
	// Backend dependent inner image data
	struct backend_image_inner_base *inner;
	// Effective size of the image
	int ewidth, eheight;
};

void init_backend_base(struct backend_base *base, session_t *ps);

void *default_clone_image(backend_t *base, const void *image_data, const region_t *reg);
bool default_is_image_transparent(backend_t *base attr_unused, void *image_data);
struct backend_image *default_new_backend_image(int w, int h);
