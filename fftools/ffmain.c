/*
 * Multi-call entry point combining ffmpeg and ffprobe into a single binary.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Single multi-call binary that provides both the ffmpeg and ffprobe
 * command line tools.
 *
 * Which tool runs is decided as follows:
 *  1. If the binary is invoked under a name ending in "ffprobe" (e.g. via a
 *     symlink), ffprobe is selected. This keeps drop-in compatibility for
 *     scripts that call ffprobe directly.
 *  2. Otherwise the command line is inspected: if it describes an output
 *     file (the ffmpeg pattern, e.g. "-i in.mp4 out.mkv") ffmpeg is run; if
 *     it only describes an input with no output (the ffprobe pattern, e.g.
 *     "-i in.mp4" or just "in.mp4") ffprobe is run.
 *
 * The shared identity symbols program_name/program_birth_year and the
 * show_help_default() callback (used by the common cmdutils/opt_common code)
 * are defined here and pointed at the selected tool before dispatching.
 */

#include "config.h"

#include <string.h>

#include "cmdutils.h"
#include "ffmain.h"
#include "libavutil/avstring.h"

/* Identity of the currently selected tool. Defined here (rather than in
 * ffmpeg.c / ffprobe.c, where they are compiled out under FF_MULTICALL) so
 * that there is a single definition shared by the merged objects. */
const char *program_name      = "ffmpeg";
int         program_birth_year = 2000;

/* ffmpeg's option table (defined in ffmpeg_opt.c). */
extern const OptionDef options[];

/* The active show_help_default() implementation, selected at dispatch time.
 * cmdutils/opt_common call show_help_default() through this indirection. */
static void (*active_show_help_default)(const char *opt, const char *arg);

void show_help_default(const char *opt, const char *arg)
{
    if (active_show_help_default)
        active_show_help_default(opt, arg);
}

static int opt_takes_arg(const OptionDef *o)
{
    if (o->type == OPT_TYPE_BOOL)
        return 0;
    if (o->type == OPT_TYPE_FUNC)
        return !!(o->flags & OPT_FUNC_ARG);
    return 1;
}

/* Mirror of cmdutils' find_option(): locate an option by name in a table,
 * allowing a trailing stream specifier (":..."). Returns NULL if not found. */
static const OptionDef *lookup_option(const OptionDef *po, const char *name)
{
    if (*name == '/')
        name++;

    while (po->name) {
        const char *end;
        if (av_strstart(name, po->name, &end) && (!*end || *end == ':'))
            return po;
        po++;
    }
    return NULL;
}

/* Determine whether the option named "name" (without leading '-') consumes a
 * following argument, consulting both the ffmpeg and ffprobe option tables so
 * that tool-specific flags are classified correctly. Unknown options are
 * assumed to take an argument, matching cmdutils' locate_option() behaviour. */
static int option_consumes_arg(const char *name)
{
    const OptionDef *po;

    po = lookup_option(options, name);
    if (!po && name[0] == 'n' && name[1] == 'o')
        po = lookup_option(options, name + 2);
    if (po)
        return opt_takes_arg(po);

    po = lookup_option(ffprobe_get_options(), name);
    if (!po && name[0] == 'n' && name[1] == 'o')
        po = lookup_option(ffprobe_get_options(), name + 2);
    if (po)
        return opt_takes_arg(po);

    return 1;
}

/* Classify a command line as an ffmpeg invocation (returns 1) or an ffprobe
 * invocation (returns 0).
 *
 * The discriminator is whether an output file is present:
 *  - ffmpeg always has at least one positional output file (and inputs given
 *    via -i), e.g. "-i in.mp4 out.mkv".
 *  - ffprobe never has an output: its single input is given either via -i or
 *    as a lone positional argument, e.g. "-i in.mp4" or "in.mp4". */
static int command_is_ffmpeg(int argc, char **argv)
{
    int positional = 0;
    int has_input  = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        /* A lone "-" is the stdin/stdout pseudo-filename, not an option. */
        if (arg[0] == '-' && arg[1]) {
            const char *name = arg + 1;

            if (!strcmp(name, "i"))
                has_input = 1;

            if (option_consumes_arg(name) && i + 1 < argc)
                i++;   /* skip the option's argument */
        } else {
            positional++;
        }
    }

    /* With an -i input, any leftover positional is an output -> ffmpeg.
     * Without -i, a single positional is an ffprobe input; anything else
     * (info queries with no files, or multiple files) defaults to ffmpeg. */
    if (has_input)
        return positional >= 1;
    return positional != 1;
}

int main(int argc, char **argv)
{
    const char *base = argv[0] ? argv[0] : "";
    const char *p;
    int run_ffmpeg;

    /* basename(argv[0]) */
    for (p = base; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;

    /* An explicit "ffprobe" invocation name (e.g. an installed symlink) forces
     * ffprobe, for drop-in compatibility with scripts. Any other name -
     * including "ffmpeg" and the combined binary's own name - falls back to
     * argument-based dispatch, so the same binary can act as either tool. */
    if (av_stristr(base, "ffprobe"))
        run_ffmpeg = 0;
    else
        run_ffmpeg = command_is_ffmpeg(argc, argv);

    if (run_ffmpeg) {
        program_name             = "ffmpeg";
        program_birth_year       = 2000;
        active_show_help_default = ffmpeg_show_help_default;
        return ffmpeg_main(argc, argv);
    } else {
        program_name             = "ffprobe";
        program_birth_year       = 2007;
        active_show_help_default = ffprobe_show_help_default;
        return ffprobe_main(argc, argv);
    }
}
