/* panel_stub_win.c — Windows-only stubs for the front-panel reconcile symbols
 * that session.c links against. The real implementations live in panel.c, an
 * AF_UNIX socket server (POSIX-only) that is gated out of the Windows build (see
 * CMakeLists). The §8.7 IP e2e never drives the §11.4 reconcile path or the front
 * panel (harp-deviced runs with an empty --panel-sock), so these no-ops let the
 * daemon link and run on Windows without the panel. Compiled ONLY on _WIN32.
 *
 * Signatures mirror panel.c exactly (reconcile_post_offer/_read + device.h's
 * reconcile_set_choice) so session.c's reconcile handlers link unchanged. */
#ifdef _WIN32

void reconcile_post_offer(const char *expect, const char *live, int dirty) {
    (void)expect; (void)live; (void)dirty;
}

void reconcile_read(int *pending, char *expect12, char *live12, int *dirty, int *choice) {
    (void)expect12; (void)live12; (void)dirty;
    if (pending) *pending = 0;   /* no pending reconcile offer on the panel-less build */
    if (choice) *choice = 0;
}

int reconcile_set_choice(int n) { (void)n; return 0; }

#endif /* _WIN32 */
