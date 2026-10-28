/**
 * @file swarm_art.h
 * @brief Swarm's own pixel art and lettering: the fighter, the five kinds of alien and a 5x7 font.
 *
 * All of it was drawn for this game. Each sprite is 16x16 and the same on
 * both sides, so only its left half is written here, one 8-character row a
 * line, and swarm.c mirrors it: '.' is see-through and every other letter a
 * colour (see art_colour in swarm.c). The sprites face up, the way they sit
 * in the formation; swarm.c makes the turned copies (16 directions) when the
 * game starts. `swarm sprites` writes them out as a .SPR file for the
 * sprite editor, and a /SWARM.SPR found at start is used instead.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

/// The sprites in the art, in .SPR frame order.
enum {
    ART_SHIP,     ///< the player's fighter
    ART_DRONE_A,  ///< drone, wings up
    ART_DRONE_B,  ///< drone, wings down
    ART_MOTH_A,   ///< moth, wings open
    ART_MOTH_B,   ///< moth, wings closed
    ART_BOSS_A,   ///< flagship, wings in
    ART_BOSS_B,   ///< flagship, wings out
    ART_DART_A,   ///< dart (challenge stages)
    ART_DART_B,   ///< dart, its flame flickering
    ART_DISC_A,   ///< spinning disc (challenge stages)
    ART_DISC_B,   ///< the disc half a turn on
    ART_COUNT     ///< how many
};

/// The left halves of the sprites: ART_COUNT x 16 rows of 8.
static const char *const art_rows[ART_COUNT][16] = {
    {   // ART_SHIP
        ".......W", ".......W", "......WR", "......WR", ".....WWB", "..R..WWB", "..R..WWW", "..W.WWWW",
        "..WWWBWW", ".WWWBBWW", "WWW.BBWR", "WW..BB.R", "W...B..R", ".......R", "......R.", "........",
    },
    {   // ART_DRONE_A
        "........", ".....Y..", "......Y.", "......CC", "BB...CRC", "BBB..CCC", ".BBBBCCC", "..BBBCYY",
        "...BBCYY", "..BBBCCC", ".BBB.CYY", "BBB...YY", "BB....CC", "......C.", "........", "........",
    },
    {   // ART_DRONE_B
        "........", ".....Y..", "......Y.", "......CC", ".....CRC", ".....CCC", "....BCCC", "...BBCYY",
        "..BBBCYY", ".BBBBCCC", "BBB..CYY", "BB....YY", "B.....CC", "......C.", "........", "........",
    },
    {   // ART_MOTH_A
        "........", "....P...", ".....P..", "......WW", "RRR..WWW", "RWRR.WRR", "RWWRRWRR", ".RWWRRWW",
        "..RRRRWW", ".RWWRRWW", "RWWRRWRR", "RWRR.WRR", "RRR..WWW", "......W.", "........", "........",
    },
    {   // ART_MOTH_B
        "........", "....P...", ".....P..", "......WW", ".....WWW", "..RR.WRR", ".RWRRWRR", ".RWWRRWW",
        "..RRRRWW", ".RWWRRWW", ".RWRRWRR", "..RR.WRR", ".....WWW", "......W.", "........", "........",
    },
    {   // ART_BOSS_A
        "........", "......Y.", ".....Y..", "....GGGG", "...GYGGG", "...GGGgg", "G...GGgg", "GG..GGGG",
        "GgG.gGGG", "GggGgGGG", ".GggGGgG", "..GgGGgG", "...GG.gG", ".....g.g", "........", "........",
    },
    {   // ART_BOSS_B
        "........", "......Y.", ".....Y..", "....GGGG", "...GYGGG", "...GGGgg", "GG..GGgg", "GGG.GGGG",
        "GgGGgGGG", ".GgggGGG", "..GGGGgG", "...G.GgG", "......gG", ".....g.g", "........", "........",
    },
    {   // ART_DART_A
        ".......O", "......OY", "......OY", ".....OYY", ".....OYW", "....OOYW", "...O.OYY", "..O..OYY",
        ".O...OOY", "O....OOO", "O...O.OO", ".....O.O", "....O..R", "......R.", ".......R", "........",
    },
    {   // ART_DART_B
        ".......O", "......OY", "......OY", ".....OYY", ".....OYW", "....OOYW", "...O.OYY", "..O..OYY",
        ".O...OOY", "O....OOO", "O...O.OO", ".....O.O", "....O..Y", "......Y.", ".......R", ".......Y",
    },
    {   // ART_DISC_A
        "........", "........", "...L....", ".....LLL", "...LLCCC", "..LCCWWW", "..LCWBBB", ".LCWBbbb",
        ".LCWBbbb", "..LCWBBB", "..LCCWWW", "...LLCCC", ".....LLL", "...L....", "........", "........",
    },
    {   // ART_DISC_B
        "........", ".....L..", "........", ".....LLL", "...LLWWW", "..LWWCCC", "L.LWCBBB", ".LWCBbbb",
        ".LWCBbbb", "L.LWCBBB", "..LWWCCC", "...LLWWW", ".....LLL", "........", ".....L..", "........",
    },
};

/// The font's characters, in this order; anything else is drawn as a space.
static const char font_chars[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.!?:/'=%,()";

/// The font: 5x7 for each of font_chars, a byte a row from the top, bit 4 the left column.
static const uint8_t font_rows[sizeof font_chars - 1][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // space
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},  // 0
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},  // 1
    {0x0e, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1f},  // 2
    {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},  // 3
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},  // 4
    {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},  // 5
    {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},  // 6
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},  // 8
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},  // 9
    {0x04, 0x0a, 0x11, 0x11, 0x1f, 0x11, 0x11},  // A
    {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},  // B
    {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e},  // C
    {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c},  // D
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},  // E
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},  // F
    {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f},  // G
    {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},  // H
    {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e},  // I
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c},  // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},  // L
    {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},  // N
    {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},  // O
    {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},  // P
    {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d},  // Q
    {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},  // R
    {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e},  // S
    {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},  // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04},  // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a},  // W
    {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11},  // X
    {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04},  // Y
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f},  // Z
    {0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00},  // -
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c},  // .
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},  // !
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},  // ?
    {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00},  // :
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00},  // /
    {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},  // '
    {0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00},  // =
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},  // %
    {0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08},  // ,
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},  // (
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},  // )
};
