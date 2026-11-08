// Bionic -> glibc compatibility shim for Project Zomboid's Android arm64 natives.
// Symbols glibc lacks are defined directly; symbols whose ABI differs are
// renamed in the patched libs to pzsh_<name> and implemented here.
#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static void warn(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[pzshim] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

// ---- symbols missing from glibc ----
EXPORT int *__errno(void) { return &errno; }
EXPORT char __sF[3 * 152];  // bionic legacy FILE array (&__sF[1] == stdout)
// Map a bionic __sF pointer to the matching glibc stream.
static FILE *fx(FILE *f) {
    char *p = (char *)f;
    if (p >= __sF && p < __sF + sizeof __sF) {
        switch ((p - __sF) / 152) { case 0: return stdin; case 1: return stdout; default: return stderr; }
    }
    return f;
}
EXPORT void android_set_abort_message(const char *msg) { if (msg) warn("abort: %s", msg); }

EXPORT size_t __strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
EXPORT char *__strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) {
    (void)dl; (void)sl; return strncpy(d, s, n);
}
EXPORT size_t __fwrite_chk(const void *b, size_t sz, size_t c, FILE *f, size_t bl) { f = fx(f);
    (void)bl; return fwrite(b, sz, c, f);
}
EXPORT ssize_t __write_chk(int fd, const void *b, size_t c, size_t bl) { (void)bl; return write(fd, b, c); }
EXPORT ssize_t __sendto_chk(int fd, const void *b, size_t len, size_t bl, int fl,
                            const struct sockaddr *a, socklen_t al) {
    (void)bl; return sendto(fd, b, len, fl, a, al);
}
EXPORT void __FD_SET_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_SET(fd, s); }
EXPORT void __FD_CLR_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_CLR(fd, s); }
EXPORT int __FD_ISSET_chk(int fd, const fd_set *s, size_t sz) { (void)sz; return FD_ISSET(fd, s); }

// ---- ABI-incompatible functions (imports renamed to pzsh_*) ----

// bionic: (buf, size, count, stream, buf_size); glibc order differs
EXPORT size_t pzsh___fread_chk(void *b, size_t sz, size_t c, FILE *f, size_t bl) { f = fx(f);
    (void)bl; return fread(b, sz, c, f);
}

// bionic sysconf names differ from glibc's
EXPORT long pzsh_sysconf(int name) {
    switch (name) {
    case 0x00: return sysconf(_SC_ARG_MAX);
    case 0x05: return sysconf(_SC_CHILD_MAX);
    case 0x06: return sysconf(_SC_CLK_TCK);
    case 0x0b: return sysconf(_SC_OPEN_MAX);
    case 0x27: case 0x28: return sysconf(_SC_PAGESIZE);
    case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
    case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
    case 0x62: return sysconf(_SC_PHYS_PAGES);
    case 0x63: return sysconf(_SC_AVPHYS_PAGES);
    }
    warn("unmapped bionic sysconf(0x%x)", name);
    errno = EINVAL; return -1;
}

// bionic pthread_attr_t is 56 bytes, glibc's is 64; glibc only uses the first 56.
EXPORT int pzsh_pthread_attr_init(void *attr) {
    pthread_attr_t tmp;
    int r = pthread_attr_init(&tmp);
    memcpy(attr, &tmp, 56);
    return r;
}

// bionic termios is the 36-byte kernel struct; glibc's is larger.
struct btermios { tcflag_t i, o, c, l; cc_t line; cc_t cc[19]; };
EXPORT int pzsh_tcgetattr(int fd, struct btermios *t) {
    struct termios g; int r = tcgetattr(fd, &g);
    if (r == 0) { t->i = g.c_iflag; t->o = g.c_oflag; t->c = g.c_cflag; t->l = g.c_lflag;
                  t->line = g.c_line; memcpy(t->cc, g.c_cc, 19); }
    return r;
}
EXPORT int pzsh_tcsetattr(int fd, int act, const struct btermios *t) {
    struct termios g; if (tcgetattr(fd, &g) != 0) return -1;
    g.c_iflag = t->i; g.c_oflag = t->o; g.c_cflag = t->c; g.c_lflag = t->l;
    g.c_line = t->line; memcpy(g.c_cc, t->cc, 19);
    return tcsetattr(fd, act, &g);
}

// bionic addrinfo swaps ai_canonname/ai_addr, and AI_*/NI_* flag values differ.
struct baddrinfo {
    int flags, family, socktype, protocol; socklen_t addrlen;
    char *canonname; struct sockaddr *addr; struct baddrinfo *next;
};
static int ai_flags_b2g(int b) {
    int g = b & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
    if (b & 0x8) g |= AI_NUMERICSERV;
    if (b & 0x100) g |= AI_ALL;
    if (b & 0x400) g |= AI_ADDRCONFIG;
    if (b & 0x800) g |= AI_V4MAPPED;
    return g;
}
EXPORT int pzsh_getaddrinfo(const char *node, const char *svc,
                            const struct baddrinfo *bh, struct baddrinfo **res) {
    struct addrinfo h, *gr = NULL;
    if (bh) { memset(&h, 0, sizeof h); h.ai_flags = ai_flags_b2g(bh->flags);
              h.ai_family = bh->family; h.ai_socktype = bh->socktype; h.ai_protocol = bh->protocol; }
    int r = getaddrinfo(node, svc, bh ? &h : NULL, &gr);
    if (r != 0) { *res = NULL; return r < 0 ? -r : r; }
    struct baddrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *g = gr; g; g = g->ai_next) {
        struct baddrinfo *b = calloc(1, sizeof *b + g->ai_addrlen);
        b->flags = g->ai_flags; b->family = g->ai_family; b->socktype = g->ai_socktype;
        b->protocol = g->ai_protocol; b->addrlen = g->ai_addrlen;
        b->addr = (struct sockaddr *)(b + 1); memcpy(b->addr, g->ai_addr, g->ai_addrlen);
        b->canonname = g->ai_canonname ? strdup(g->ai_canonname) : NULL;
        *tail = b; tail = &b->next;
    }
    freeaddrinfo(gr);
    *res = head; return 0;
}
EXPORT void pzsh_freeaddrinfo(struct baddrinfo *b) {
    while (b) { struct baddrinfo *n = b->next; free(b->canonname); free(b); b = n; }
}
EXPORT int pzsh_getnameinfo(const struct sockaddr *sa, socklen_t sl, char *host, socklen_t hl,
                            char *serv, socklen_t svl, int bf) {
    int g = 0;
    if (bf & 1) g |= NI_NOFQDN;
    if (bf & 2) g |= NI_NUMERICHOST;
    if (bf & 4) g |= NI_NAMEREQD;
    if (bf & 8) g |= NI_NUMERICSERV;
    if (bf & 16) g |= NI_DGRAM;
    int r = getnameinfo(sa, sl, host, hl, serv, svl, g);
    return r < 0 ? -r : r;
}

// bionic pthread_mutex_t is 40 bytes, glibc's is 48 (init clears all 48;
// the other mutex ops only touch the first 40 for non-robust mutexes).
EXPORT int pzsh_pthread_mutex_init(void *m, const pthread_mutexattr_t *a) {
    pthread_mutex_t tmp;
    int r = pthread_mutex_init(&tmp, a);
    memcpy(m, &tmp, 40);
    return r;
}

// stdio wrappers so __sF-based streams reach glibc's real FILEs
EXPORT int pzsh_fflush(FILE *f) { return fflush(f ? fx(f) : NULL); }
EXPORT int pzsh_fclose(FILE *f) { return fclose(fx(f)); }
EXPORT int pzsh_feof(FILE *f) { return feof(fx(f)); }
EXPORT char *pzsh_fgets(char *s, int n, FILE *f) { return fgets(s, n, fx(f)); }
EXPORT int pzsh_fputc(int c, FILE *f) { return fputc(c, fx(f)); }
EXPORT int pzsh_fputs(const char *s, FILE *f) { return fputs(s, fx(f)); }
EXPORT size_t pzsh_fread(void *b, size_t sz, size_t n, FILE *f) { return fread(b, sz, n, fx(f)); }
EXPORT size_t pzsh_fwrite(const void *b, size_t sz, size_t n, FILE *f) { return fwrite(b, sz, n, fx(f)); }
EXPORT int pzsh_fseek(FILE *f, long o, int w) { return fseek(fx(f), o, w); }
EXPORT int pzsh_fseeko(FILE *f, off_t o, int w) { return fseeko(fx(f), o, w); }
EXPORT long pzsh_ftell(FILE *f) { return ftell(fx(f)); }
EXPORT off_t pzsh_ftello(FILE *f) { return ftello(fx(f)); }
EXPORT int pzsh_getc(FILE *f) { return getc(fx(f)); }
EXPORT int pzsh_ungetc(int c, FILE *f) { return ungetc(c, fx(f)); }
EXPORT void pzsh_rewind(FILE *f) { rewind(fx(f)); }
EXPORT int pzsh_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(fx(f), fmt, ap); }
EXPORT int pzsh_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfprintf(fx(f), fmt, ap); va_end(ap); return r;
}

// PZ's FMOD integration demands the exact FMOD release it was built against
// (PZ_FMOD_VERSION, detected by install.sh) and rejects any other patch release.
// FMOD is binary compatible within a minor series, so report that version.
#ifndef PZ_FMOD_VERSION
#define PZ_FMOD_VERSION 0x20309
#endif
#include <dlfcn.h>
EXPORT int pzsh_FMOD_System_GetVersion(void *sys, unsigned int *ver, unsigned int *build) {
    static int (*real)(void *, unsigned int *, unsigned int *);
    if (!real) {
        const char *names[] = { "libfmod.so.14", "libfmodL.so.14", "libfmod.so" };
        for (int i = 0; i < 3 && !real; i++) {
            void *h = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD);
            if (h) real = (int (*)(void *, unsigned int *, unsigned int *))dlsym(h, "FMOD_System_GetVersion");
        }
        if (!real) { warn("FMOD_System_GetVersion not found"); return 1; }
    }
    int r = real(sys, ver, build);
    if (r == 0 && ver && (*ver >> 8) == (PZ_FMOD_VERSION >> 8)) *ver = PZ_FMOD_VERSION;
    return r;
}

// bionic pthread_cond_t only guarantees 4-byte alignment, but glibc's condvar
// does 64-bit atomics on it (SIGBUS/BUS_ADRALN when it lands on a 4-aligned
// address). Keep a properly aligned glibc condvar out of line: the bionic
// storage holds {u32 magic, pthread_cond_t *real} (zeroed = lazily created).
#define PZ_CMAGIC 0x50a7c0ddu
static pthread_mutex_t pz_cond_lazy = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t *pz_cond(void *c, int create) {
    pthread_cond_t *r;
    if (__atomic_load_n((uint32_t *)c, __ATOMIC_ACQUIRE) == PZ_CMAGIC) { memcpy(&r, (char *)c + 4, 8); return r; }
    if (!create) return NULL;
    pthread_mutex_lock(&pz_cond_lazy);
    if (__atomic_load_n((uint32_t *)c, __ATOMIC_ACQUIRE) == PZ_CMAGIC) memcpy(&r, (char *)c + 4, 8);
    else {
        r = aligned_alloc(16, (sizeof(pthread_cond_t) + 15) & ~15ul);
        pthread_cond_init(r, NULL);
        memcpy((char *)c + 4, &r, 8);
        __atomic_store_n((uint32_t *)c, PZ_CMAGIC, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&pz_cond_lazy);
    return r;
}
EXPORT int pzsh_pthread_cond_init(void *c, const void *attr) {
    (void)attr;
    memset(c, 0, 12);
    pz_cond(c, 1);
    return 0;
}
EXPORT int pzsh_pthread_cond_destroy(void *c) {
    pthread_cond_t *r = pz_cond(c, 0);
    if (r) { __atomic_store_n((uint32_t *)c, 0, __ATOMIC_RELEASE); pthread_cond_destroy(r); free(r); }
    return 0;
}
EXPORT int pzsh_pthread_cond_wait(void *c, pthread_mutex_t *m) { return pthread_cond_wait(pz_cond(c, 1), m); }
EXPORT int pzsh_pthread_cond_timedwait(void *c, pthread_mutex_t *m, const struct timespec *t) { return pthread_cond_timedwait(pz_cond(c, 1), m, t); }
EXPORT int pzsh_pthread_cond_signal(void *c) { return pthread_cond_signal(pz_cond(c, 1)); }
EXPORT int pzsh_pthread_cond_broadcast(void *c) { return pthread_cond_broadcast(pz_cond(c, 1)); }

// PZ asks FMOD for 32 kHz 5.1 output. On Apple Silicon that unusual stream,
// remixed by PipeWire, coincided with speakersafetyd crashing at launch. Ask
// for plain 48 kHz stereo instead; FMOD does the downmix itself.
// PZ_FMOD_NATIVE_FORMAT=1 keeps the game's own format.
EXPORT int pzsh_FMOD_System_SetSoftwareFormat(void *sys, int rate, int mode, int raw) {
    static int (*real)(void *, int, int, int);
    if (!real) {
        const char *names[] = { "libfmod.so.14", "libfmodL.so.14", "libfmod.so" };
        for (int i = 0; i < 3 && !real; i++) {
            void *h = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD);
            if (h) real = (int (*)(void *, int, int, int))dlsym(h, "FMOD_System_SetSoftwareFormat");
        }
        if (!real) { warn("FMOD_System_SetSoftwareFormat not found"); return 1; }
    }
    if (!getenv("PZ_FMOD_NATIVE_FORMAT")) { rate = 48000; mode = 3 /* FMOD_SPEAKERMODE_STEREO */; raw = 0; }
    return real(sys, rate, mode, raw);
}
