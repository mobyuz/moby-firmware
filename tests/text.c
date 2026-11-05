// SPDX-License-Identifier: GPL-3.0-only

// F4 host tests for display text: the UTF-8 decoder (malformed input, bounds, no
// transliteration between scripts) and the Cyrillic and turned comma glyphs.
// Pure C, no ESP-IDF header.

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "moby_face.h"
#include "moby_font.h"

enum { TURNED_COMMA = MOBY_FONT_EXTRA_BASE + 74 };

// A display cell as a char, without a constant cast that MSVC flags.
static char glyph_cell(unsigned index)
{
    return (char)(unsigned char)(MOBY_FONT_EXTRA_BASE + index);
}

static size_t decode(const char *utf8, char *cells, size_t capacity)
{
    return moby_font_decode(utf8, cells, capacity);
}

static void expect_cells(const char *utf8, const char *expected)
{
    char cells[MOBY_FACE_TEXT_MAX + 1];
    const size_t count = decode(utf8, cells, MOBY_FACE_TEXT_MAX);
    if (strcmp(cells, expected) != 0 || count != strlen(expected)) {
        fprintf(stderr, "decode mismatch for input starting %02x: got %zu cells\n", (unsigned)(uint8_t)utf8[0], count);
        assert(0);
    }
}

static void test_ascii_and_bounds(void)
{
    expect_cells("", "");
    expect_cells("Salom, dunyo 123 ~!", "Salom, dunyo 123 ~!");
    // Controls and DEL show as '?'.
    expect_cells("a\x01" "b\x7f" "c\n", "a?b?c?");

    char cells[8];
    memset(cells, 'x', sizeof cells);
    assert(decode("abcdef", cells, 3) == 3 && strcmp(cells, "abc") == 0 && cells[4] == 'x');
    assert(decode("abc", cells, 3) == 3 && strcmp(cells, "abc") == 0);
    assert(decode("ab", cells, 3) == 2 && strcmp(cells, "ab") == 0);
    // A zero capacity writes nothing, and a null buffer is refused.
    memset(cells, 'x', sizeof cells);
    assert(decode("abc", cells, 0) == 0 && cells[0] == 'x');
    assert(moby_font_decode("abc", NULL, 3) == 0);
    assert(moby_font_decode(NULL, cells, 3) == 0 && cells[0] == '\0');

    // Cut off at the capacity: a multi-byte letter is never split across it.
    assert(decode("\xd0\x96\xd0\x96\xd0\x96", cells, 2) == 2 && cells[2] == '\0');
    assert((uint8_t)cells[0] == MOBY_FONT_EXTRA_BASE + 6 && (uint8_t)cells[1] == MOBY_FONT_EXTRA_BASE + 6);

    // The decoder stops at the NUL and never reads what follows it, even a lead byte that wants more.
    const char tail[] = "ab\0\xd0\x96z";
    assert(decode(tail, cells, 7) == 2 && strcmp(cells, "ab") == 0);
    const char cut[] = "\xd0\0\x96";
    assert(decode(cut, cells, 7) == 1 && cells[0] == '?' && cells[1] == '\0');
}

static void test_russian(void)
{
    // A-YA map in order, then YO; a-ya in order, then yo.
    char cells[MOBY_FACE_TEXT_MAX + 1];
    for (unsigned cp = 0x0410; cp <= 0x042f; ++cp) {
        const char text[] = { (char)(unsigned char)(0xc0U | (cp >> 6)), (char)(unsigned char)(0x80U | (cp & 0x3fU)), 0 };
        assert(decode(text, cells, 4) == 1 && (uint8_t)cells[0] == MOBY_FONT_EXTRA_BASE + (cp - 0x0410));
    }
    for (unsigned cp = 0x0430; cp <= 0x044f; ++cp) {
        const char text[] = { (char)(unsigned char)(0xc0U | (cp >> 6)), (char)(unsigned char)(0x80U | (cp & 0x3fU)), 0 };
        assert(decode(text, cells, 4) == 1 && (uint8_t)cells[0] == MOBY_FONT_EXTRA_BASE + 33 + (cp - 0x0430));
    }
    assert(decode("\xd0\x81", cells, 4) == 1 && (uint8_t)cells[0] == MOBY_FONT_EXTRA_BASE + 32);
    assert(decode("\xd1\x91", cells, 4) == 1 && (uint8_t)cells[0] == MOBY_FONT_EXTRA_BASE + 65);
    // "Privet, mir!" in Cyrillic: six letters, comma, space, three letters, bang.
    assert(decode("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, \xd0\xbc\xd0\xb8\xd1\x80!", cells, 40) == 12);
    assert((uint8_t)cells[0] == MOBY_FONT_EXTRA_BASE + 15 && cells[6] == ',' && cells[7] == ' ' && cells[11] == '!');
}

static void test_uzbek(void)
{
    char cells[MOBY_FACE_TEXT_MAX + 1];
    // Latin: o and g with U+02BB stay Latin letters plus the turned comma.
    assert(decode("O\xca\xbbzbek g\xca\xbbisht", cells, 40) == 13);
    assert(cells[0] == 'O' && (uint8_t)cells[1] == TURNED_COMMA && cells[2] == 'z' && cells[7] == 'g' &&
           (uint8_t)cells[8] == TURNED_COMMA);
    // The marks people type instead (U+02BC, U+2018, U+2019) show the same cell. A plain ASCII quote stays ASCII.
    assert(decode("o\xca\xbc o\xe2\x80\x98 o\xe2\x80\x99 o'", cells, 40) == 11);
    assert((uint8_t)cells[1] == TURNED_COMMA && (uint8_t)cells[4] == TURNED_COMMA && (uint8_t)cells[7] == TURNED_COMMA &&
           cells[10] == '\'');
    // Cyrillic extras: O-breve, o-breve, Q-hook, q-hook, G-bar, g-bar, H-tail, h-tail.
    assert(decode("\xd0\x8e\xd1\x9e\xd2\x9a\xd2\x9b\xd2\x92\xd2\x93\xd2\xb2\xd2\xb3", cells, 40) == 8);
    for (unsigned i = 0; i < 8; ++i) {
        assert((uint8_t)cells[i] == MOBY_FONT_EXTRA_BASE + 66 + i);
    }
    // Digraphs of the Latin alphabet are plain ASCII.
    expect_cells("Sh Ch ng", "Sh Ch ng");
}

static void test_scripts_stay_apart(void)
{
    char latin[MOBY_FACE_TEXT_MAX + 1];
    char cyrillic[MOBY_FACE_TEXT_MAX + 1];
    // Latin A and Cyrillic A (and the other lookalikes) have different cells and are never folded together,
    // even where the glyph shapes agree.
    assert(decode("AaBCEHKMOPTXcepxy", latin, 40) == 17);
    assert(decode("\xd0\x90\xd0\xb0\xd0\x92\xd0\xa1\xd0\x95\xd0\x9d\xd0\x9a\xd0\x9c\xd0\x9e\xd0\xa0\xd0\xa2\xd0\xa5"
                  "\xd1\x81\xd0\xb5\xd1\x80\xd1\x85\xd1\x83",
                  cyrillic, 40) == 17);
    for (size_t i = 0; i < 17; ++i) {
        assert((uint8_t)latin[i] < 0x80 && (uint8_t)cyrillic[i] >= MOBY_FONT_EXTRA_BASE);
        assert(latin[i] != cyrillic[i]);
    }
    // A mixed line keeps each script in its own cells.
    assert(decode("Moby \xd0\x9c\xd0\xbe\xd0\xb1\xd0\xb8", latin, 40) == 9);
    assert(latin[0] == 'M' && (uint8_t)latin[5] >= MOBY_FONT_EXTRA_BASE);
    // A letter the font lacks is '?', not a near neighbour: Ukrainian I-diaeresis, Greek, CJK, an emoji,
    // and Latin letters with accents.
    expect_cells("\xd0\x87", "?");
    expect_cells("\xce\xa9", "?");
    expect_cells("\xe4\xb8\xad", "?");
    expect_cells("\xf0\x9f\x8c\x90", "?");
    expect_cells("\xc3\xa9\xc5\x9f\xc4\x9f", "???");
    // Characters just outside the mapped blocks.
    expect_cells("\xd0\x80\xd1\x90\xd1\x9f\xd2\x91", "????");
}

static void test_malformed_utf8(void)
{
    // Each ill-formed sequence is one '?', and the byte that ended it is read again.
    expect_cells("\x80", "?");
    expect_cells("\xbf\xbf", "??");
    expect_cells("\xc0\xaf", "??");
    expect_cells("\xc1\xbf", "??");
    expect_cells("\xe0\x80\x80", "???");
    expect_cells("\xe0\x9f\xbf", "???");
    expect_cells("\xed\xa0\x80", "???");
    expect_cells("\xed\xbf\xbf", "???");
    expect_cells("\xf0\x80\x80\x80", "????");
    expect_cells("\xf4\x90\x80\x80", "????");
    expect_cells("\xf5\x80\x80\x80", "????");
    expect_cells("\xf8\x88\x80\x80\x80", "?????");
    expect_cells("\xff\xfe", "??");
    // Cut off by the end of the string.
    expect_cells("\xd0", "?");
    expect_cells("\xe2\x82", "?");
    expect_cells("\xf0\x9f\x8c", "?");
    // Cut off by a letter: the letter is not lost.
    expect_cells("\xd0" "A", "?A");
    expect_cells("\xe2\x82" "z", "?z");
    expect_cells("\xf0\x9f" "\xd0\x96", "?\x86");
    // Surrounding text survives.
    expect_cells("ok\xff" "ok", "ok?ok");
    expect_cells("\xd0\x9f\xd1" "x", "\x8f?x");
}

// A strict reference check, independent of the decoder: is the string well-formed UTF-8 without controls?
static bool reference_valid(const uint8_t *s, size_t *points)
{
    *points = 0;
    while (*s) {
        uint32_t cp;
        size_t extra;
        if (*s < 0x80) {
            cp = *s;
            extra = 0;
        } else if (*s >= 0xc2 && *s <= 0xdf) {
            cp = *s & 0x1f;
            extra = 1;
        } else if (*s >= 0xe0 && *s <= 0xef) {
            cp = *s & 0x0f;
            extra = 2;
        } else if (*s >= 0xf0 && *s <= 0xf4) {
            cp = *s & 0x07;
            extra = 3;
        } else {
            return false;
        }
        ++s;
        for (size_t i = 0; i < extra; ++i, ++s) {
            if ((*s & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (*s & 0x3f);
        }
        static const uint32_t minimum[4] = { 0, 0x80, 0x800, 0x10000 };
        if (cp < minimum[extra] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
        ++*points;
    }
    return true;
}

static void test_random_inputs(void)
{
    // Deterministic noise and mutated valid text: the output is always bounded, ends with a NUL, holds only
    // cells the font knows, and for well-formed text it has one cell per code point.
    uint32_t seed = 12345;
    static const char *const seeds[] = {
        "Salom, dunyo", "O\xca\xbczbekiston", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "\xd2\x9a\xd1\x83\xd1\x88",
        "\xf0\x9f\x8c\x90 emoji", "\xe2\x80\x98quote\xe2\x80\x99",
    };
    enum { GUARD = 4, CAPACITY = 24 };
    for (unsigned round = 0; round < 200000; ++round) {
        uint8_t input[40];
        size_t length;
        if (round % 2 == 0) {
            length = (seed = seed * 1664525U + 1013904223U) % 39U;
            for (size_t i = 0; i < length; ++i) {
                input[i] = (uint8_t)((seed = seed * 1664525U + 1013904223U) >> 24);
            }
        } else {
            const char *base = seeds[round / 2 % (sizeof seeds / sizeof seeds[0])];
            length = strlen(base);
            memcpy(input, base, length);
            const unsigned flips = (seed = seed * 1664525U + 1013904223U) >> 30;
            for (unsigned f = 0; f < flips; ++f) {
                seed = seed * 1664525U + 1013904223U;
                input[seed % length] = (uint8_t)(seed >> 24);
            }
        }
        input[length] = '\0';
        for (size_t i = 0; i < length; ++i) {
            if (input[i] == 0) {
                input[i] = 0x41;
            }
        }
        char buffer[GUARD + CAPACITY + 1 + GUARD];
        memset(buffer, 0x5a, sizeof buffer);
        const size_t count = decode((const char *)input, buffer + GUARD, CAPACITY);
        assert(count <= CAPACITY && buffer[GUARD + count] == '\0');
        for (size_t i = 0; i < GUARD; ++i) {
            assert(buffer[i] == 0x5a && buffer[GUARD + CAPACITY + 1 + i] == 0x5a);
        }
        for (size_t i = 0; i < count; ++i) {
            const uint8_t cell = (uint8_t)buffer[GUARD + i];
            assert((cell >= 0x20 && cell <= 0x7e) || (cell >= MOBY_FONT_EXTRA_BASE && cell < MOBY_FONT_EXTRA_BASE + MOBY_FONT_EXTRA_CELLS));
        }
        size_t points;
        if (reference_valid(input, &points) && points <= CAPACITY) {
            bool controls = false;
            for (size_t i = 0; i < length; ++i) {
                controls = controls || input[i] < 0x20 || input[i] == 0x7f;
            }
            if (!controls) {
                assert(count == points);
            }
        }
    }
}

static void test_glyphs(void)
{
    uint8_t blank[MOBY_FONT_WIDTH] = { 0 };
    const uint8_t *question = moby_font_glyph('?');
    for (unsigned a = 0; a < MOBY_FONT_EXTRA_CELLS; ++a) {
        const uint8_t *glyph = moby_font_glyph(glyph_cell(a));
        assert(memcmp(glyph, blank, sizeof blank) != 0);
        assert(memcmp(glyph, question, MOBY_FONT_WIDTH) != 0);
        for (unsigned col = 0; col < MOBY_FONT_WIDTH; ++col) {
            assert(glyph[col] < (1U << MOBY_FONT_HEIGHT));
        }
        // Each extra glyph differs from every other extra glyph.
        for (unsigned b = a + 1; b < MOBY_FONT_EXTRA_CELLS; ++b) {
            assert(memcmp(glyph, moby_font_glyph(glyph_cell(b)), MOBY_FONT_WIDTH) != 0);
        }
    }
    // A capital and its small letter differ, for Russian and the Uzbek extras.
    for (unsigned i = 0; i < 32; ++i) {
        assert(memcmp(moby_font_glyph(glyph_cell(i)), moby_font_glyph(glyph_cell(33 + i)),
                      MOBY_FONT_WIDTH) != 0);
    }
    assert(memcmp(moby_font_glyph(glyph_cell(32)), moby_font_glyph(glyph_cell(65)), MOBY_FONT_WIDTH) != 0);
    for (unsigned i = 66; i < 74; i += 2) {
        assert(memcmp(moby_font_glyph(glyph_cell(i)), moby_font_glyph(glyph_cell(i + 1)),
                      MOBY_FONT_WIDTH) != 0);
    }
    // The turned comma sits in the top rows only, so it reads as a mark and not as a letter.
    const uint8_t *comma = moby_font_glyph(glyph_cell(74));
    for (unsigned col = 0; col < MOBY_FONT_WIDTH; ++col) {
        assert((comma[col] & 0x7c) == 0);
    }
    assert(memcmp(comma, moby_font_glyph('\''), MOBY_FONT_WIDTH) != 0);
    // Bytes past the extra block, controls and DEL give the '?' glyph.
    assert(memcmp(moby_font_glyph(glyph_cell(MOBY_FONT_EXTRA_CELLS)), question, MOBY_FONT_WIDTH) == 0);
    assert(memcmp(moby_font_glyph(glyph_cell(127)), question, MOBY_FONT_WIDTH) == 0);
    assert(memcmp(moby_font_glyph('\x7f'), question, MOBY_FONT_WIDTH) == 0);
    assert(memcmp(moby_font_glyph('\0'), question, MOBY_FONT_WIDTH) == 0);
    // The ASCII glyphs did not change.
    assert(moby_font_glyph('A')[0] == 0x7e && moby_font_glyph('a')[0] == 0x20 && moby_font_glyph(' ')[2] == 0);
}

static void test_rendering(void)
{
    // Cyrillic text draws, differs from the '?' it would otherwise become, and stays inside the frame.
    uint8_t guarded[MOBY_FACE_BYTES + 2];
    uint8_t first[MOBY_FACE_BYTES];
    uint8_t second[MOBY_FACE_BYTES];
    memset(guarded, 0xa5, sizeof guarded);
    moby_face_view_t view = { .expression = MOBY_FACE_IDLE };
    decode("\xd0\xa8", view.text, MOBY_FACE_TEXT_MAX);
    moby_face_render(guarded + 1, view, false);
    assert(guarded[0] == 0xa5 && guarded[MOBY_FACE_BYTES + 1] == 0xa5);
    memcpy(first, guarded + 1, sizeof first);
    decode("\xd0\xa9", view.text, MOBY_FACE_TEXT_MAX);
    moby_face_render(second, view, false);
    assert(memcmp(first, second, sizeof first) != 0);
    decode("\xf0\x9f\x8c\x90", view.text, MOBY_FACE_TEXT_MAX);
    moby_face_render(second, view, false);
    assert(memcmp(first, second, sizeof first) != 0);

    // The longest line of the widest letters still fits.
    char cells[MOBY_FACE_TEXT_MAX + 1];
    for (unsigned i = 0; i < MOBY_FACE_TEXT_MAX; ++i) {
        cells[i] = glyph_cell(i % MOBY_FONT_EXTRA_CELLS);
    }
    cells[MOBY_FACE_TEXT_MAX] = '\0';
    memcpy(view.text, cells, sizeof cells);
    moby_face_render(guarded + 1, view, false);
    assert(guarded[0] == 0xa5 && guarded[MOBY_FACE_BYTES + 1] == 0xa5);
}

int main(void)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } tests[] = {
        { "ascii and bounds", test_ascii_and_bounds },
        { "russian letters", test_russian },
        { "uzbek letters and turned comma", test_uzbek },
        { "scripts stay apart", test_scripts_stay_apart },
        { "malformed utf-8", test_malformed_utf8 },
        { "random inputs", test_random_inputs },
        { "glyphs", test_glyphs },
        { "rendering", test_rendering },
    };
    const size_t count = sizeof tests / sizeof tests[0];
    for (size_t i = 0; i < count; ++i) {
        printf("%s: ", tests[i].name);
        fflush(stdout);
        tests[i].run();
        puts("ok");
    }
    printf("%zu text host tests passed\n", count);
    return 0;
}
