/*
 * ui.c - Shared CLI UI helpers (see ui.h). Standard C only.
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int ui_read_line(const char *prompt, char *buf, size_t cap) {
    if (prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    if (!buf || cap == 0) return 1;
    buf[0] = '\0';

    if (!fgets(buf, (int)cap, stdin)) {
        buf[0] = '\0';
        return 1; /* EOF or error */
    }
    /* Strip trailing newline / carriage return. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

void ui_header(const char *title) {
    printf("\n=== %s ===\n", title ? title : "");
}

int ui_menu(const char *title, const char *const *options, int count) {
    char line[256];
    for (;;) {
        ui_header(title);
        for (int i = 0; i < count; i++)
            printf("%d. %s\n", i + 1, options[i]);
        if (ui_read_line(">> ", line, sizeof(line)) != 0)
            return 0; /* EOF -> cancel */

        /* Parse a plain integer. */
        char *endp = NULL;
        long v = strtol(line, &endp, 10);
        /* skip trailing spaces */
        while (endp && *endp && isspace((unsigned char)*endp)) endp++;
        if (line[0] == '\0' || endp == line || (endp && *endp != '\0') ||
            v < 1 || v > count) {
            printf("!! 1〜%d の番号を入力してください。\n", count);
            continue;
        }
        return (int)v;
    }
}

int ui_read_int(const char *prompt, int *out, int allow_empty, int def) {
    char line[128];
    for (;;) {
        if (ui_read_line(prompt, line, sizeof(line)) != 0)
            return 0; /* EOF */
        if (line[0] == '\0') {
            if (allow_empty) { if (out) *out = def; return 1; }
            printf("!! 数値を入力してください。\n");
            continue;
        }
        char *endp = NULL;
        long v = strtol(line, &endp, 10);
        while (endp && *endp && isspace((unsigned char)*endp)) endp++;
        if (endp == line || (endp && *endp != '\0')) {
            printf("!! 数値として解釈できません。もう一度入力してください。\n");
            continue;
        }
        if (out) *out = (int)v;
        return 1;
    }
}

int ui_read_yesno(const char *prompt, int def) {
    char line[64];
    for (;;) {
        if (ui_read_line(prompt, line, sizeof(line)) != 0)
            return def; /* EOF */
        if (line[0] == 'y' || line[0] == 'Y') return 1;
        if (line[0] == 'n' || line[0] == 'N') return 0;
        printf("!! y または n を入力してください。\n");
    }
}

int ui_read_string(const char *prompt, char *buf, size_t cap, int allow_empty) {
    for (;;) {
        if (ui_read_line(prompt, buf, cap) != 0)
            return 0; /* EOF */
        if (buf[0] == '\0' && !allow_empty) {
            printf("!! 空にできません。入力してください。\n");
            continue;
        }
        return 1;
    }
}
