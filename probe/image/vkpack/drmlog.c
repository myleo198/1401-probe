// drmlog.so - 1401 Probe: records every ioctl a process makes on /dev/dri (the GPU's kernel driver), with the argument
// block before and after the call, into $DRMLOG (capped at $DRMLOG_MAX bytes, default 16 MiB). Preloaded only into
// gputest during the GPU test step. The sequence is the kernel interface Mesa's Vulkan driver needs for this GPU, which
// a macOS kernel driver has to offer the same driver. AMD command submissions (DRM_IOCTL_AMDGPU_CS) also log each chunk.
// Text lines, one per call:  <ms> fd<path> ioctl <req> dir <r|w|rw|-> nr <nr> size <n> -> rc <rc> [errno <e>]
//                             in  <hex of the argument before the call>   out <hex after>
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

// musl's sys/ioctl.h has no _IOC_* decoders; these are the kernel's asm-generic layout, which x86 uses
#ifndef _IOC_NR
#define _IOC_NR(r)   ((r) & 0xffu)
#define _IOC_TYPE(r) (((r) >> 8) & 0xffu)
#define _IOC_SIZE(r) (((r) >> 16) & 0x3fffu)
#define _IOC_DIR(r)  (((r) >> 30) & 0x3u)
#endif
#ifndef _IOC_WRITE
#define _IOC_WRITE 1u
#define _IOC_READ  2u
#endif

#define CAP_ARG 512  // bytes of an argument block kept; DRM argument structs are smaller than this

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *out;
static long budget = -1;
static int (*real_ioctl)(int, int, ...);

static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int is_dri(int fd, char *path, size_t n) {
    char l[64]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    ssize_t k = readlink(l, path, n - 1); if (k <= 0) return 0;
    path[k] = 0;
    return strncmp(path, "/dev/dri/", 9) == 0;
}
static void hex(const char *tag, const void *p, size_t n) {
    if (!p || !n) return;
    if (n > CAP_ARG) n = CAP_ARG;
    const unsigned char *b = p;
    int w = fprintf(out, "  %s ", tag);
    for (size_t i = 0; i < n; i++) w += fprintf(out, "%02x", b[i]);
    w += fprintf(out, "\n");
    budget -= w > 0 ? w : 0;
}
static void amdgpu_chunks(const void *arg) {
    // struct drm_amdgpu_cs_in { u32 ctx_id, bo_list_handle, num_chunks, flags; u64 chunks; } -> u64[] of chunk pointers,
    // each struct drm_amdgpu_cs_chunk { u32 chunk_id, length_dw; u64 chunk_data; }
    const uint32_t *in = arg; uint32_t nc = in[2]; const uint64_t *list = (const uint64_t *)(uintptr_t)*(const uint64_t *)(in + 4);
    if (!list || nc > 64) return;
    for (uint32_t i = 0; i < nc; i++) {
        const uint32_t *ch = (const uint32_t *)(uintptr_t)list[i]; if (!ch) continue;
        budget -= fprintf(out, "  chunk %u id %u dwords %u\n", i, ch[0], ch[1]);
        hex("  data", (const void *)(uintptr_t)*(const uint64_t *)(ch + 2), (size_t)ch[1] * 4);
    }
}

int ioctl(int fd, int req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
    char path[128];
    // DRMLOG_ANY=1 (the build check only): every fd, so the logger can be proven in a container that has no GPU
    if (!getenv("DRMLOG") || (!is_dri(fd, path, sizeof path) && !(getenv("DRMLOG_ANY") && (snprintf(path, sizeof path, "fd"), 1))))
        return real_ioctl(fd, req, arg);
    unsigned ureq = (unsigned)req, dir = _IOC_DIR(ureq), size = _IOC_SIZE(ureq), type = _IOC_TYPE(ureq), nr = _IOC_NR(ureq);
    unsigned char before[CAP_ARG];
    size_t keep = size < CAP_ARG ? size : CAP_ARG;
    if (arg && (dir & _IOC_WRITE) && keep) memcpy(before, arg, keep);
    int rc = real_ioctl(fd, req, arg); int e = errno;
    pthread_mutex_lock(&mu);
    if (!out) {
        out = fopen(getenv("DRMLOG"), "w");
        const char *m = getenv("DRMLOG_MAX"); budget = m ? atol(m) : 16l << 20;
    }
    if (out && budget > 0) {
        budget -= fprintf(out, "%.3f fd%d %s ioctl %08x type %c nr 0x%02x dir %s size %u -> rc %d%s", ms(), fd, path, ureq,
                          type >= 0x20 && type < 0x7f ? type : '?', nr, dir == 3 ? "rw" : dir == 2 ? "r" : dir == 1 ? "w" : "-",
                          size, rc, rc < 0 ? "" : "\n");
        if (rc < 0) budget -= fprintf(out, " errno %d\n", e);
        if (arg && (dir & _IOC_WRITE)) hex("in ", before, keep);
        if (arg && (dir & _IOC_READ)) hex("out", arg, keep);
        if (type == 'd' && nr == 0x40 + 0x04 && arg && size >= 24) amdgpu_chunks(before);  // DRM_COMMAND_BASE + DRM_AMDGPU_CS
        if (budget <= 0) fprintf(out, "DRMLOG budget reached - later calls not recorded\n");
        fflush(out);
    }
    pthread_mutex_unlock(&mu);
    errno = e;
    return rc;
}
