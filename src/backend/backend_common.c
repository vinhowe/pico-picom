// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <math.h>
#include <string.h>
#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"

void *default_clone_image(backend_t *base attr_unused, const void *image_data,
                          const region_t *reg_visible attr_unused) {
	auto new_img = ccalloc(1, struct backend_image);
	*new_img = *(struct backend_image *)image_data;
	new_img->inner->refcount++;
	return new_img;
}

bool default_is_image_transparent(backend_t *base attr_unused, void *image_data) {
	struct backend_image *img = image_data;
	return img->inner->has_alpha;
}

struct backend_image *default_new_backend_image(int w, int h) {
	auto ret = ccalloc(1, struct backend_image);
	ret->eheight = h;
	ret->ewidth = w;
	return ret;
}

void init_backend_base(struct backend_base *base, session_t *ps) {
	base->c = ps->c;
	base->loop = ps->loop;
	base->root = ps->root;
	base->busy = false;
	base->ops = NULL;
}
