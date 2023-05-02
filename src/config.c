// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "common.h"
#include "compiler.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "win.h"

#include "config.h"

/**
 * Parse a long number.
 */
bool parse_long(const char *s, long *dest) {
	const char *endptr = NULL;
	long val = strtol(s, (char **)&endptr, 0);
	if (!endptr || endptr == s) {
		log_error("Invalid number: %s", s);
		return false;
	}
	while (isspace((unsigned char)*endptr))
		++endptr;
	if (*endptr) {
		log_error("Trailing characters: %s", s);
		return false;
	}
	*dest = val;
	return true;
}

/**
 * Parse an int  number.
 */
bool parse_int(const char *s, int *dest) {
	long val;
	if (!parse_long(s, &val)) {
		return false;
	}
	if (val > INT_MAX || val < INT_MIN) {
		log_error("Number exceeded int limits: %ld", val);
		return false;
	}
	*dest = (int)val;
	return true;
}

void set_default_winopts(options_t *opt, win_option_mask_t *mask) {
	// Focused/unfocused state only apply to a few window types, all other windows
	// are always considered focused.
	const wintype_t nofocus_type[] = {WINTYPE_UNKNOWN, WINTYPE_NORMAL, WINTYPE_UTILITY};
	for (unsigned long i = 0; i < ARR_SIZE(nofocus_type); i++) {
		if (!mask[nofocus_type[i]].focus) {
			mask[nofocus_type[i]].focus = true;
			opt->wintype_option[nofocus_type[i]].focus = false;
		}
	}
	for (unsigned long i = 0; i < NUM_WINTYPES; i++) {
		if (!mask[i].focus) {
			mask[i].focus = true;
			opt->wintype_option[i].focus = true;
		}
		if (!mask[i].redir_ignore) {
			mask[i].redir_ignore = true;
			opt->wintype_option[i].redir_ignore = false;
		}
	}
}