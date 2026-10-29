/**
 * @file lancer_art.h
 * @brief Lancer's own pixel art and lettering: the ship, its pod, the enemies, the capsules and a 5x7 font.
 *
 * All of it was drawn for this game. Each sprite is 16x16, written a row a
 * line: '.' is see-through and every other letter a colour (see art_colour
 * in lancer.c). 'X' and 'x' are the main colour and its dark shade, chosen
 * when the pictures are made: the pod and the capsules come in red, blue
 * and yellow. The ship faces right and the enemies left; things that sit on
 * the ground are drawn standing on the bottom row, and lancer.c turns them
 * upside down for the ceiling. `lancer sprites` writes them out as a .SPR
 * file for the sprite editor, and a /LANCER.SPR found at start is used
 * instead.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

/// The sprites in the art, in .SPR frame order.
enum {
    ART_SHIP,        ///< the player's ship
    ART_SHIP_B,      ///< the ship, its flame flickering
    ART_ORB_A,       ///< the pod
    ART_ORB_B,       ///< the pod, its core pulsing
    ART_DART_A,      ///< dart: flies in waves
    ART_DART_B,      ///< dart, fins flapping
    ART_SWOOP_A,     ///< swooper: turns at the ship
    ART_SWOOP_B,     ///< swooper, wings back
    ART_TURRET,      ///< gun dome on the ground
    ART_HOPPER_A,    ///< walker
    ART_HOPPER_B,    ///< walker, the other step
    ART_HAULER_A,    ///< hauler: carries a capsule
    ART_HAULER_B,    ///< hauler, its thruster flickering
    ART_MINE_A,      ///< mine
    ART_MINE_B,      ///< mine, turned
    ART_COIL,        ///< a coil's body ball
    ART_COIL_HEAD,   ///< a coil's head
    ART_HATCH,       ///< spore hatch, shut
    ART_HATCH_OPEN,  ///< spore hatch, open
    ART_CAPSULE,     ///< a power capsule (its letter is drawn on it)
    ART_PLATE,       ///< an armour plate (the first boss)
    ART_COUNT        ///< how many
};

/// The sprites: ART_COUNT x 16 rows of 16.
static const char *const art_rows[ART_COUNT][16] = {
    {   // ART_SHIP
        "................", "................", "................", "...KKK..........",
        "...KwwKK........", "....KbbwKKK.....", "..rKKBBBBBwKK...", ".OrKBBBCCWwBBKK.",
        "YOrKBBBBCCBBBBKW", ".OrKBBBBBBBBBKK.", "..rKKKBBBBKKK...", "....KbbwKK......",
        "...KwwKK........", "...KKK..........", "................", "................",
    },
    {   // ART_SHIP_B
        "................", "................", "................", "...KKK..........",
        "...KwwKK........", "....KbbwKKK.....", "..rKKBBBBBwKK...", "YOrKBBBCCWwBBKK.",
        "WYrKBBBBCCBBBBKW", "YOrKBBBBBBBBBKK.", "..rKKKBBBBKKK...", "....KbbwKK......",
        "...KwwKK........", "...KKK..........", "................", "................",
    },
    {   // ART_ORB_A
        "................", ".....kKKKKk.....", "...kKxxxxxxKk...", "..kKxXXXXXXxKk..",
        "..KxXXYYYYXXxK..", ".kxXXYWWWWYXXxk.", ".KxXYWWWWWWYXxK.", "KKxXYWWWWWWYXxKK",
        "KKxXYWWWWWWYXxKK", ".KxXYWWWWWWYXxK.", ".kxXXYWWWWYXXxk.", "..KxXXYYYYXXxK..",
        "..kKxXXXXXXxKk..", "...kKxxxxxxKk...", ".....kKKKKk.....", "................",
    },
    {   // ART_ORB_B
        "................", ".....kKKKKk.....", "...kKxxxxxxKk...", "..kKxXXXXXXxKk..",
        "..KxXXWWWWXXxK..", ".kxXXWYYYYWXXxk.", ".KxXWYYWWYYWXxK.", "KKxXWYWWWWYWXxKK",
        "KKxXWYWWWWYWXxKK", ".KxXWYYWWYYWXxK.", ".kxXXWYYYYWXXxk.", "..KxXXWWWWXXxK..",
        "..kKxXXXXXXxKk..", "...kKxxxxxxKk...", ".....kKKKKk.....", "................",
    },
    {   // ART_DART_A
        "................", "................", "................", "..........rrr...",
        "........rRRRRr..", ".....kKKRRYYRRr.", "...kKWKRRRRRRRr.", ".CWWKKKRRRRRRRRr",
        "...kKWKRRRRRRRr.", ".....kKKRRYYRRr.", "........rRRRRr..", "..........rrr...",
        "................", "................", "................", "................",
    },
    {   // ART_DART_B
        "................", "................", "................", "............rr..",
        "........rRRRRRr.", ".....kKKRRYYRRr.", "...kKWKRRRRRRRr.", ".CWWKKKRRRRRRRRr",
        "...kKWKRRRRRRRr.", ".....kKKRRYYRRr.", "........rRRRRRr.", "............rr..",
        "................", "................", "................", "................",
    },
    {   // ART_SWOOP_A
        "................", "................", ".......gg.......", "......gGGg......",
        ".....gGLLGg.....", "...ggGGLLGGg....", ".YWgGGGGGGGGgg..", "YRWGGGGGGGGGGGg.",
        ".YWgGGGGGGGGgg..", "...ggGGLLGGg....", ".....gGLLGg.....", "......gGGg......",
        ".......gg.......", "................", "................", "................",
    },
    {   // ART_SWOOP_B
        "................", "................", ".........gg.....", ".......gGGgg....",
        ".....gGLLGg.....", "...ggGGLLGGg....", ".YWgGGGGGGGGgg..", "YRWGGGGGGGGGGGg.",
        ".YWgGGGGGGGGgg..", "...ggGGLLGGg....", ".....gGLLGg.....", ".......gGGgg....",
        ".........gg.....", "................", "................", "................",
    },
    {   // ART_TURRET
        "................", "................", "................", "................",
        "................", "................", "................", "......kKKk......",
        "....kKwWWwKk....", "...kKwPPPPwKk...", "..kKwPPMMPPwKk..", "..KwPPMWWMPPwK..",
        ".kKKKKKKKKKKKKk.", ".KwwwwwwwwwwwwK.", "kKkKkKkKkKkKkKkK", "KKKKKKKKKKKKKKKK",
    },
    {   // ART_HOPPER_A
        "................", "................", "................", "................",
        "................", ".....nTTTTn.....", "...nTTYYTTTTn...", "..nTCWTTTTTTTn..",
        "..TCCWTTTTTTTT..", ".nTTTTTTTTTTTTn.", ".nnTTTTTTTTTTnn.", "..nnnnnnnnnnnn..",
        "...k..k..k..k...", "..k..k..k..k....", "..k..k..k..k....", ".kk.kk.kk.kk....",
    },
    {   // ART_HOPPER_B
        "................", "................", "................", "................",
        "................", ".....nTTTTn.....", "...nTTYYTTTTn...", "..nTCWTTTTTTTn..",
        "..TCCWTTTTTTTT..", ".nTTTTTTTTTTTTn.", ".nnTTTTTTTTTTnn.", "..nnnnnnnnnnnn..",
        "....k..k..k..k..", "....k..k..k..k..", "...k..k..k..k...", "..kk.kk.kk.kk...",
    },
    {   // ART_HAULER_A
        "................", "...kKKKKKKKKk...", "..kKwwwwwwwwKk..", ".kKwPPPPPPPPwKk.",
        ".KwPPpppppPPPwK.", "CWwPpYYYYYpPPwKO", "CWwPpYWWWYpPPwKY", ".KwPpYYYYYpPPwKO",
        ".kKwPPPPPPPPwKk.", "..kKwwwwwwwwKk..", "...kKKKKKKKKk...", "....k..k..k.....",
        "...kk.kk.kk.....", "................", "................", "................",
    },
    {   // ART_HAULER_B
        "................", "...kKKKKKKKKk...", "..kKwwwwwwwwKk..", ".kKwPPPPPPPPwKk.",
        ".KwPPpppppPPPwK.", "CWwPpYYYYYpPPwKY", "CWwPpYWWWYpPPwKW", ".KwPpYYYYYpPPwKY",
        ".kKwPPPPPPPPwKk.", "..kKwwwwwwwwKk..", "...kKKKKKKKKk...", "....k..k..k.....",
        "...kk.kk.kk.....", "................", "................", "................",
    },
    {   // ART_MINE_A
        ".......W........", ".......K........", "...K...K...K....", "....K.kKk.K.....",
        ".....kRRRk......", "....kRRMRRk.....", "...kRRMWMRRk....", "WKKKRMWWWMRKKKW.",
        "...kRRMWMRRk....", "....kRRMRRk.....", ".....kRRRk......", "....K.kKk.K.....",
        "...K...K...K....", ".......K........", ".......W........", "................",
    },
    {   // ART_MINE_B
        "................", ".W...........W..", "..K.........K...", "...K...K...K....",
        "....KkRRRkK.....", "....kRRMRRk.....", "...kRRMWMRRk....", "..KkRMWWWMRkK...",
        "...kRRMWMRRk....", "....kRRMRRk.....", "....KkRRRkK.....", "...K...K...K....",
        "..K.........K...", ".W...........W..", "................", "................",
    },
    {   // ART_COIL
        "................", "................", ".....cccccc.....", "...ccCCCCCCcc...",
        "..cCCWWCCCCCCc..", "..cCWWCCCCCCCc..", ".cCCWCCCCCCCCCc.", ".cCCCCCCCCCCCgc.",
        ".cCCCCCCCCCCggc.", ".cCCCCCCCCCgggc.", "..cCCCCCCCgggc..", "..cgCCCCgggggc..",
        "...ccgggggggc...", ".....cccccc.....", "................", "................",
    },
    {   // ART_COIL_HEAD
        "................", "......cccccc....", "....ccCCCCCCcc..", "...cCCCCCCCCCCc.",
        "..cCCYYCCCCCCCCc", "..cCYRRYCCCCCCCc", ".WcCCYYCCCCCCCgc", "W.WccCCCCCCCCggc",
        ".....cCCCCCCgggc", "W.WccCCCCCCCgggc", ".WcCCCCCCCCggggc", "..cgCCCCCCgggggc",
        "...cgggggggggcc.", "....cccccccccc..", "................", "................",
    },
    {   // ART_HATCH
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        ".....kkkkkk.....", "...kkKggggKkk...", "..kKgGGGGGGgKk..", ".kKgGGLLLLGGgKk.",
        ".KgGGLLLLLLGGgK.", "kKkKkKkKkKkKkKkK", "KwwwwwwwwwwwwwwK", "KKKKKKKKKKKKKKKK",
    },
    {   // ART_HATCH_OPEN
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "..kk........kk..", ".kKg........gKk.", ".kKgr......rgKk.", ".kKgrRRRRRRrgKk.",
        ".KgrRRMMMMRRrgK.", "kKkKkKkKkKkKkKkK", "KwwwwwwwwwwwwwwK", "KKKKKKKKKKKKKKKK",
    },
    {   // ART_CAPSULE
        "................", "................", "................", "....kKKKKKKk....",
        "..kKWWWWWWWWKk..", ".kKWxxxxxxxxWKk.", ".KWxXXXXXXXXxWK.", ".KWxXXXXXXXXxWK.",
        ".KWxXXXXXXXXxWK.", ".KWxXXXXXXXXxWK.", ".kKWxxxxxxxxWKk.", "..kKWWWWWWWWKk..",
        "....kKKKKKKk....", "................", "................", "................",
    },
    {   // ART_PLATE
        "................", "................", ".....kKKKKk.....", "....kKwwwwKk....",
        "...kKwWWWWwKk...", "..kKwWPPPPWwKk..", "..KwWPpppPPWwK..", "..KwWPpMMpPWwK..",
        "..KwWPpMMpPWwK..", "..KwWPpppPPWwK..", "..kKwWPPPPWwKk..", "...kKwWWWWwKk...",
        "....kKwwwwKk....", ".....kKKKKk.....", "................", "................",
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
