// TSan test shim (not shipped): ThreadSanitizer does not intercept pthread_clockjoin_np, which Qt 6
// uses in QThread::wait(), so it never learns a QThread was joined, keeps it "live", and aborts with
// "CHECK failed: sanitizer_thread_registry.cpp:186" when glibc reuses the pthread_t. Route the call
// to pthread_join / pthread_timedjoin_np, which TSan does intercept.
//
//   gcc -shared -fPIC -O1 scripts/tsan-clockjoin-shim.c -o <dir>/tsan-clockjoin-shim.so
//   LD_PRELOAD=<dir>/tsan-clockjoin-shim.so <ThreadSanitizer-built Qt program>
//
// SolidTerm's TSan run (docs/evidence/solidterm/parser-thread-2026-10-13.md): a build with
// -Db_sanitize=thread against a TSan-built libvterm 0.3.3 (pkg_config_path), the solidterm meson
// tests, then SOLIDTERM_THREADSTRESS=<ms> with SHELL=scripts/solidterm-threadstress.sh, offscreen and
// on a headless sway (threaded render loop).
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
int pthread_clockjoin_np(pthread_t th, void **ret, clockid_t clk, const struct timespec *abstime)
{
    if (!abstime)
        return pthread_join(th, ret);
    struct timespec now_c, now_r, rt = *abstime;
    if (clk != CLOCK_REALTIME) { // convert the deadline to CLOCK_REALTIME
        clock_gettime(clk, &now_c);
        clock_gettime(CLOCK_REALTIME, &now_r);
        long long d = (abstime->tv_sec - now_c.tv_sec) * 1000000000LL + (abstime->tv_nsec - now_c.tv_nsec);
        long long r = now_r.tv_sec * 1000000000LL + now_r.tv_nsec + (d > 0 ? d : 0);
        rt.tv_sec = r / 1000000000LL;
        rt.tv_nsec = r % 1000000000LL;
    }
    return pthread_timedjoin_np(th, ret, &rt);
}
