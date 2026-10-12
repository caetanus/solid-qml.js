// Minimal virtual pointer for a nested headless sway (test harness only): reads lines from stdin
//   abs X Y   (absolute motion in a W x H output)   down   up
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include "vp.h"
static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct wl_seat *seat;
static void g(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v) {
    if (!strcmp(i, zwlr_virtual_pointer_manager_v1_interface.name)) mgr = wl_registry_bind(r, n, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (!strcmp(i, wl_seat_interface.name)) seat = wl_registry_bind(r, n, &wl_seat_interface, 1);
}
static void gr(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener L = { g, gr };
static uint32_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int main(int argc, char **argv) {
    int W = 1600, H = 900;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "vptr: no display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &L, NULL);
    wl_display_roundtrip(dpy);
    if (!mgr) { fprintf(stderr, "vptr: no virtual pointer manager\n"); return 1; }
    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    wl_display_roundtrip(dpy);
    fprintf(stderr, "vptr: ready\n");
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        int x, y;
        if (sscanf(line, "abs %d %d", &x, &y) == 2) zwlr_virtual_pointer_v1_motion_absolute(p, now(), x, y, W, H);
        else if (!strncmp(line, "down", 4)) zwlr_virtual_pointer_v1_button(p, now(), 0x110, 1);
        else if (!strncmp(line, "up", 2)) zwlr_virtual_pointer_v1_button(p, now(), 0x110, 0);
        else continue;
        zwlr_virtual_pointer_v1_frame(p);
        wl_display_flush(dpy);
        wl_display_roundtrip(dpy);
    }
    zwlr_virtual_pointer_v1_destroy(p);
    wl_display_roundtrip(dpy);
    return 0;
}
