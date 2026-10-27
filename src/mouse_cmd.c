/**
 * @file mouse_cmd.c
 * @brief The shell's mouse command: status, speed and acceleration, kept in /MOUSE.CFG ("speed 5" and "accel on" lines).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "mouse.h"

#define CONFIG "/MOUSE.CFG"

void mouse_load_config(void) {
    FIL f;
    if (f_open(&f, CONFIG, FA_READ) != FR_OK)
        return;
    int s;
    bool a;
    mouse_get_speed(&s, &a);
    char line[40];
    while (f_gets(line, sizeof line, &f)) {
        if (!strncmp(line, "speed ", 6))
            s = atoi(line + 6);
        else if (!strncmp(line, "accel ", 6))
            a = !strncmp(line + 6, "on", 2);
    }
    f_close(&f);
    mouse_set_speed(s, a);
}

static void save(void) {
    int s;
    bool a;
    mouse_get_speed(&s, &a);
    FIL f;
    if (f_open(&f, CONFIG, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("mouse: can't save " CONFIG " (no drive?)\n");
        return;
    }
    f_printf(&f, "speed %d\naccel %s\n", s, a ? "on" : "off");
    f_close(&f);
}

void mouse_command(int argc, char **argv) {
    int s;
    bool a;
    mouse_get_speed(&s, &a);
    if (argc >= 3 && !strcmp(argv[1], "speed") && atoi(argv[2]) >= 1 && atoi(argv[2]) <= 10) {
        mouse_set_speed(atoi(argv[2]), a);
        save();
    } else if (argc >= 3 && !strcmp(argv[1], "accel") &&
               (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        mouse_set_speed(s, !strcmp(argv[2], "on"));
        save();
    } else if (argc > 1) {
        printf("usage: mouse [speed 1-10 | accel on|off]\n");
        return;
    }
    mouse_get_speed(&s, &a);
    int n = mouse_count(), x, y, b;
    mouse_get(&x, &y, &b);
    if (n)
        printf("Mouse: %d connected, at %d,%d, buttons %d. ", n, x, y, b);
    else
        printf("Mouse: none connected. ");
    printf("Speed %d (1-10), acceleration %s.\n", s, a ? "on" : "off");
}
