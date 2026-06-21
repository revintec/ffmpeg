/*
 * Declarations shared by the ffmpeg/ffprobe multi-call binary.
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

#ifndef FFTOOLS_FFMAIN_H
#define FFTOOLS_FFMAIN_H

#include "cmdutils.h"

/* Tool entry points (each tool's main() is renamed under FF_MULTICALL). */
int ffmpeg_main(int argc, char **argv);
int ffprobe_main(int argc, char **argv);

/* Per-tool show_help_default() implementations, dispatched at runtime. */
void ffmpeg_show_help_default(const char *opt, const char *arg);
void ffprobe_show_help_default(const char *opt, const char *arg);

/* Accessor for ffprobe's (otherwise file-local) option table. */
const OptionDef *ffprobe_get_options(void);

#endif /* FFTOOLS_FFMAIN_H */
