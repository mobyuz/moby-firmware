// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_FONT_H
#define MOBY_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MOBY_FONT_WIDTH = 5,
    MOBY_FONT_HEIGHT = 7,
    // Display text is a string of one-byte cells: printable ASCII as itself, and cells from
    // MOBY_FONT_EXTRA_BASE up for Russian and Uzbek Cyrillic and the Uzbek turned comma.
    MOBY_FONT_EXTRA_BASE = 0x80,
    MOBY_FONT_EXTRA_CELLS = 75,
};

// Five columns (bit 0 is the top row) for a cell. Any other byte gives the '?' glyph.
const uint8_t *moby_font_glyph(char ch);

// Turns UTF-8 into cells, at most max_cells of them, and ends them with a NUL (cells needs max_cells + 1
// bytes). Returns the number of cells. Reads up to the NUL only and never past it. Each malformed
// sequence (stray or missing continuation, overlong form, surrogate, past U+10FFFF, a cut-off end) is
// one '?'; so is a control character and a code point the font lacks. Latin stays Latin and Cyrillic
// stays Cyrillic. U+02BB, U+02BC, U+2018 and U+2019 all show as the Uzbek turned comma.
size_t moby_font_decode(const char *utf8, char *cells, size_t max_cells);

#endif
