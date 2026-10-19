/* Renders the palette into a virtual terminal and checks that every row of the
   popup has the same width (borders line up).  Treats every code point as 1 cell. */
#include "../core/palette.h"
#include "../core/theme.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define R 60
#define C 200
static unsigned screen[R + 2][C + 2];

static void emulate(const unsigned char *s, size_t n) {
    int row = 1, col = 1;
    memset(screen, 0, sizeof screen);
    for (size_t i = 0; i < n;) {
        if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2; int p[8] = {0}, np = 0;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == '?')) {
                if (s[j] == ';') { if (np < 7) np++; }
                else if (s[j] >= '0' && s[j] <= '9' && p[np] < 100000) p[np] = p[np] * 10 + s[j] - '0';
                j++;
            }
            if (j < n && s[j] == 'H') { row = p[0] ? p[0] : 1; col = p[1] ? p[1] : 1; }
            i = j + 1; continue;
        }
        unsigned cp = s[i]; int len = 1;
        if (cp >= 0xF0) { cp &= 7; len = 4; } else if (cp >= 0xE0) { cp &= 15; len = 3; } else if (cp >= 0xC0) { cp &= 31; len = 2; }
        for (int k = 1; k < len; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        if (row >= 1 && row <= R && col >= 1 && col <= C) screen[row][col] = cp;
        col++; i += len;
    }
}

static void check(PaletteState *ps, ForgeTheme *t, int W, int H, const char *label) {
    char path[] = "/tmp/pal_XXXXXX"; int fd = mkstemp(path);
    int saved = dup(1); fflush(stdout); dup2(fd, 1);
    palette_render(ps, t, W, H);
    dup2(saved, 1); close(saved);
    off_t n = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
    unsigned char *buf = malloc(n); assert(read(fd, buf, n) == n); close(fd); unlink(path);
    emulate(buf, n); free(buf);

    int popup_w = 80, lim = W - 20; if (popup_w > lim) popup_w = lim; if (popup_w < 40) popup_w = 40; if (popup_w > W) popup_w = W;
    int col = (W - popup_w) / 2 + 1, row = H / 4; if (row < 1) row = 1;
    int bad = 0, rows = 0;
    for (int y = row; y < R; y++) {
        unsigned first = screen[y][col];
        if (!first) break;
        rows++;
        unsigned last = screen[y][col + popup_w - 1], after = screen[y][col + popup_w];
        int ok = (first == 0x250C || first == 0x2502 || first == 0x251C || first == 0x2514) &&
                 (last  == 0x2510 || last  == 0x2502 || last  == 0x2524 || last  == 0x2518) && after == 0;
        if (!ok) { printf("  %s: row %d misaligned (first=%x last=%x after=%x)\n", label, y - row, first, last, after); bad++; }
    }
    printf("%-28s rows=%d %s\n", label, rows, bad ? "FAIL" : "ok");
    assert(rows >= 4 && !bad);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ForgeTheme t; theme_load(&t, "catppuccin");
    PaletteState ps; palette_init(&ps);
    palette_add_command(&ps, "Save File", "Save current file", "Ctrl+S", NULL, NULL);
    palette_add_command(&ps, "Go to Line", "Jump to a line number", ":", NULL, NULL);
    palette_add_command(&ps, "A very long command name that must be truncated to fit the box nicely", "and a long description that also has to be cut off so it never spills over the border", "Ctrl+Shift+Alt+X", NULL, NULL);
    for (int i = 0; i < 20; i++) palette_add_command(&ps, "Filler", "desc", "", NULL, NULL);
    for (int i = 0; i < 30; i++) { snprintf(ps.files[i].path, 512, "dir/sub/file_%d.c", i); strcpy(ps.files[i].basename, "file.c"); }
    ps.file_count = 30; ps.files_loaded = true;
    int widths[] = { 200, 120, 100, 80, 60 };
    for (int w = 0; w < 5; w++) {
        char l[64];
        palette_show(&ps); snprintf(l, sizeof l, "commands W=%d", widths[w]); check(&ps, &t, widths[w], 40, l);
        palette_handle_key(&ps, 'g'); palette_handle_key(&ps, 'o'); snprintf(l, sizeof l, "commands+input W=%d", widths[w]); check(&ps, &t, widths[w], 40, l);
        palette_show(&ps); ps.mode = PALETTE_FILES; ps.filtered_count = 30; for (int i = 0; i < 30; i++) ps.filtered[i] = i;
        snprintf(l, sizeof l, "files W=%d", widths[w]); check(&ps, &t, widths[w], 40, l);
        palette_show(&ps); ps.mode = PALETTE_GOTO_LINE; snprintf(l, sizeof l, "goto W=%d", widths[w]); check(&ps, &t, widths[w], 40, l);
        palette_show(&ps); for (int i = 0; i < 200; i++) palette_handle_key(&ps, 'x'); snprintf(l, sizeof l, "long input W=%d", widths[w]); check(&ps, &t, widths[w], 40, l);
    }
    puts("palette render ok"); return 0;
}
