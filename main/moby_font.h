// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_FONT_H
#define MOBY_FONT_H

#include <stdint.h>

enum { MOBY_FONT_WIDTH = 5, MOBY_FONT_HEIGHT = 7 };

// Five columns for a printable ASCII character; any other byte gives the '?' glyph.
const uint8_t *moby_font_glyph(char ch);

#endif
