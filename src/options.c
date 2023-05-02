// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <getopt.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "backend/backend.h"
#include "common.h"
#include "log.h"
#include "options.h"
#include "utils.h"
#include "win.h"

#pragma GCC diagnostic error "-Wunused-parameter"

struct picom_option {
	const char *long_name;
	int has_arg;
	int val;
	const char *arg_name;
	const char *help;
};

// clang-format off
static const struct option *longopts = NULL;
static const struct picom_option picom_options[] = {
    {"help"                        , no_argument      , 'h', NULL          , "Print this help message and exit."},
    {"daemon"                      , no_argument      , 'b', NULL          , "Daemonize process."},
    {"backend"                     , required_argument, 290, NULL          , "Backend. Only possible value is `glx`"},
    {"glx-no-stencil"              , no_argument      , 291, NULL          , NULL},
    {"glx-no-rebind-pixmap"        , no_argument      , 298, NULL          , NULL},
    {"xrender-sync-fence"          , no_argument      , 313, NULL          , "Additionally use X Sync fence to sync clients' draw calls. Needed on "
                                                                             "nvidia-drivers with GLX backend for some users."},
    {"show-all-xerrors"            , no_argument      , 314, NULL          , NULL},
    {"version"                     , no_argument      , 318, NULL          , "Print version number and exit."},
    {"log-level"                   , required_argument, 321, NULL          , "Log level, possible values are: trace, debug, info, warn, error"},
    {"log-file"                    , required_argument, 322, NULL          , "Path to the log file."},
};
// clang-format on

static void setup_longopts(void) {
	auto opts = ccalloc(ARR_SIZE(picom_options) + 1, struct option);
	for (size_t i = 0; i < ARR_SIZE(picom_options); i++) {
		opts[i].name = picom_options[i].long_name;
		opts[i].has_arg = picom_options[i].has_arg;
		opts[i].flag = NULL;
		opts[i].val = picom_options[i].val;
	}
	longopts = opts;
}

void print_help(const char *help, size_t indent, size_t curr_indent, size_t line_wrap,
                FILE *f) {
	if (curr_indent > indent) {
		fputs("\n", f);
		curr_indent = 0;
	}

	if (line_wrap - indent <= 1) {
		line_wrap = indent + 2;
	}

	size_t pos = 0;
	size_t len = strlen(help);
	while (pos < len) {
		fprintf(f, "%*s", (int)(indent - curr_indent), "");
		curr_indent = 0;
		size_t towrite = line_wrap - indent;
		while (help[pos] == ' ') {
			pos++;
		}
		if (pos + towrite > len) {
			towrite = len - pos;
			fwrite(help + pos, 1, towrite, f);
		} else {
			auto space_break = towrite;
			while (space_break > 0 && help[pos + space_break - 1] != ' ') {
				space_break--;
			}

			bool print_hyphen = false;
			if (space_break == 0) {
				print_hyphen = true;
				towrite--;
			} else {
				towrite = space_break;
			}

			fwrite(help + pos, 1, towrite, f);

			if (print_hyphen) {
				fputs("-", f);
			}
		}

		fputs("\n", f);
		pos += towrite;
	}
}

/**
 * Print usage text.
 */
static void usage(const char *argv0, int ret) {
	FILE *f = (ret ? stderr : stdout);
	fprintf(f, "picom (%s)\n", PICOM_VERSION);
	fprintf(f, "Standalone X11 compositor\n");

	fprintf(f, "Usage: %s [OPTION]...\n\n", argv0);
	fprintf(f, "OPTIONS:\n");

	int line_wrap = 80;
	struct winsize window_size = {0};
	if (ioctl(fileno(f), TIOCGWINSZ, &window_size) != -1) {
		line_wrap = window_size.ws_col;
	}

	size_t help_indent = 0;
	for (size_t i = 0; i < ARR_SIZE(picom_options); i++) {
		if (picom_options[i].help == NULL) {
			// Hide options with no help message.
			continue;
		}
		auto option_len = strlen(picom_options[i].long_name) + 2 + 4;
		if (picom_options[i].arg_name) {
			option_len += strlen(picom_options[i].arg_name) + 1;
		}
		if (option_len > help_indent && option_len < 30) {
			help_indent = option_len;
		}
	}
	help_indent += 6;

	for (size_t i = 0; i < ARR_SIZE(picom_options); i++) {
		if (picom_options[i].help == NULL) {
			continue;
		}
		size_t option_len = 8;
		fprintf(f, "    ");
		if ((picom_options[i].val > 'a' && picom_options[i].val < 'z') ||
		    (picom_options[i].val > 'A' && picom_options[i].val < 'Z')) {
			fprintf(f, "-%c, ", picom_options[i].val);
		} else {
			fprintf(f, "    ");
		}
		fprintf(f, "--%s", picom_options[i].long_name);
		option_len += strlen(picom_options[i].long_name) + 2;
		if (picom_options[i].arg_name) {
			fprintf(f, "=%s", picom_options[i].arg_name);
			option_len += strlen(picom_options[i].arg_name) + 1;
		}
		fprintf(f, "  ");
		option_len += 2;
		print_help(picom_options[i].help, help_indent, option_len,
		           (size_t)line_wrap, f);
	}
}

static const char *shortopts = "D:I:O:r:o:m:l:t:i:e:hscnfFCazGb";

/// Get config options that are needed to parse the rest of the options
/// Return true if we should quit
bool get_early_config(int argc, char *const *argv, bool *all_xerrors, bool *fork,
                      int *exit_code) {
	setup_longopts();

	int o = 0, longopt_idx = -1;

	// Pre-parse the commandline arguments to check for --config and invalid
	// switches
	// Must reset optind to 0 here in case we reread the commandline
	// arguments
	optind = 1;
	*exit_code = 0;
	while (-1 != (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
		if (o == 'h') {
			usage(argv[0], 0);
			return true;

		} else if (o == 'b') {
			*fork = true;
		} else if (o == 314) {
			*all_xerrors = true;
		} else if (o == 318) {
			printf("%s\n", PICOM_VERSION);
			return true;
		} else if (o == '?' || o == ':') {
			usage(argv[0], 1);
			goto err;
		}
	}

	// Check for abundant positional arguments
	if (optind < argc) {
		// log is not initialized here yet
		fprintf(stderr, "picom doesn't accept positional arguments.\n");
		goto err;
	}

	return false;
err:
	*exit_code = 1;
	return true;
}

/**
 * Process arguments and configuration files.
 */
bool get_cfg(options_t *opt, int argc, char *const *argv, win_option_mask_t *winopt_mask) {
	int o = 0, longopt_idx = -1;

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));

	// Enforce LC_NUMERIC locale "C" here to make sure dots are recognized
	// instead of commas in atof().
	setlocale(LC_NUMERIC, "C");

	// Parse commandline arguments. Range checking will be done later.

	optind = 1;
	while (-1 != (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
		switch (o) {
#define P_CASEBOOL(idx, option)                                                          \
	case idx:                                                                        \
		opt->option = true;                                                      \
		break
#define P_CASELONG(idx, option)                                                          \
	case idx:                                                                        \
		if (!parse_long(optarg, &opt->option)) {                                 \
			exit(1);                                                         \
		}                                                                        \
		break
#define P_CASEINT(idx, option)                                                           \
	case idx:                                                                        \
		if (!parse_int(optarg, &opt->option)) {                                  \
			exit(1);                                                         \
		}                                                                        \
		break

		// clang-format off
		// Short options
		case 318:
		case 'h':
			// These options should cause us to exit early,
			// so assert(false) here
			assert(false);
			break;
		case 'b':
		case 314:
		case 320:
			// These options are handled by get_early_config()
			break;
		case 322:
			// --log-file
			free(opt->logpath);
			opt->logpath = strdup(optarg);
			break;
		case 290:
			// --backend
			opt->backend = parse_backend(optarg);
			if (opt->backend >= NUM_BKEND)
				exit(1);
			break;
		P_CASEBOOL(291, glx_no_stencil);
		P_CASEBOOL(298, glx_no_rebind_pixmap);
		P_CASEBOOL(313, xrender_sync_fence);
		case 321: {
			enum log_level tmp_level = string_to_log_level(optarg);
			if (tmp_level == LOG_LEVEL_INVALID) {
				log_warn("Invalid log level, defaults to WARN");
			} else {
				log_set_level_tls(tmp_level);
			}
			break;
		}
		default: usage(argv[0], 1); break;
#undef P_CASEBOOL
		}
		// clang-format on
	}

	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	// Apply default wintype options that are dependent on global options
	set_default_winopts(opt, winopt_mask);

	return true;
}

// vim: set noet sw=8 ts=8 :
