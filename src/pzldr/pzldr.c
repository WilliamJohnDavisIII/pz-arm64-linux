// pzldr: a small ELF loader for the game's Android arm64 libraries on a 16 KB-page kernel.
// glibc's ld.so refuses them (their segments are only 4 KB aligned). We map each library
// into one anonymous 64 KB-aligned region, copy the segments in, apply relocations
// ourselves, then give each 16 KB page the union of the permissions of the segments in it.
// Imports resolve like DF_SYMBOLIC: the library's own definitions first, then the global
// glibc namespace (libpzshim etc.), then libraries loaded earlier by pzldr.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "pzldr.h"

extern char **environ;
#ifndef DT_ANDROID_REL
#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELA 0x60000011
#endif

#define MAXMOD 32
struct pzmod {
    char name[PATH_MAX];
    uintptr_t base;
    size_t span;
    const ElfW(Phdr) *phdr;
    int phnum;
    const ElfW(Sym) *symtab;
    const char *strtab;
    size_t nsyms;
};
static struct pzmod mods[MAXMOD];
static int nmods;
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int verbose = -1;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[pzldr] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
#define VLOG(...) do { if (verbose > 0) logf_(__VA_ARGS__); } while (0)

// ---- overrides handed to loaded libraries ---------------------------------------------
// dl_iterate_phdr: glibc's modules, then ours (their unwinders find .eh_frame this way).
static int my_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
    int r = dl_iterate_phdr(cb, data);
    if (r) return r;
    for (int i = 0; i < nmods; i++) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = mods[i].base;
        info.dlpi_name = mods[i].name;
        info.dlpi_phdr = mods[i].phdr;
        info.dlpi_phnum = mods[i].phnum;
        r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}

static void missing_import(void) {
    logf_("called an unresolved import; aborting");
    abort();
}

// ---- symbol lookup -----------------------------------------------------------------------
static size_t gnu_hash_nsyms(const uint32_t *h) {
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    const uint64_t *bloom = (const uint64_t *)(h + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t b = 0; b < nbuckets; b++) if (buckets[b] > last) last = buckets[b];
    if (last < symoffset) return symoffset;
    while (!(chain[last - symoffset] & 1)) last++;
    return last + 1;
}

static void *mod_sym(const struct pzmod *m, const char *name) {
    for (size_t i = 1; i < m->nsyms; i++) {
        const ElfW(Sym) *s = &m->symtab[i];
        if (s->st_shndx == SHN_UNDEF || !s->st_name) continue;
        int t = ELF64_ST_TYPE(s->st_info);
        if (t != STT_FUNC && t != STT_OBJECT && t != STT_NOTYPE && t != STT_COMMON) continue;
        if (strcmp(m->strtab + s->st_name, name) == 0) return (void *)(m->base + s->st_value);
    }
    return NULL;
}

static void *resolve_global(const char *name) {
    if (strcmp(name, "dl_iterate_phdr") == 0) return (void *)my_dl_iterate_phdr;
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    for (int i = 0; i < nmods; i++) if ((p = mod_sym(&mods[i], name))) return p;
    return NULL;
}

// ---- loading -------------------------------------------------------------------------------
static int read_at(int fd, void *buf, size_t n, off_t off) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, off + got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int relocate(struct pzmod *m, const ElfW(Rela) *rel, size_t n, int *unresolved) {
    for (size_t i = 0; i < n; i++) {
        uint32_t type = ELF64_R_TYPE(rel[i].r_info), si = ELF64_R_SYM(rel[i].r_info);
        uint64_t *where = (uint64_t *)(m->base + rel[i].r_offset);
        if (type == R_AARCH64_RELATIVE) { *where = m->base + rel[i].r_addend; continue; }
        if (type == R_AARCH64_NONE) continue;
        uint64_t S = 0;
        if (si) {
            const ElfW(Sym) *s = &m->symtab[si];
            const char *nm = m->strtab + s->st_name;
            if (s->st_shndx != SHN_UNDEF) {
                S = m->base + s->st_value;              // DF_SYMBOLIC: own definition wins
            } else {
                void *p = resolve_global(nm);
                if (!p && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
                    if ((*unresolved)++ < 20) logf_("%s: unresolved import %s", m->name, nm);
                    p = (void *)missing_import;
                }
                S = (uint64_t)p;
            }
        }
        switch (type) {
        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
            *where = S + rel[i].r_addend;
            break;
        default:
            logf_("%s: unsupported relocation type %u at 0x%lx", m->name, type, (unsigned long)rel[i].r_offset);
            return -1;
        }
    }
    return 0;
}

void *pzldr_open(const char *path_in) {
    if (verbose < 0) verbose = getenv("PZLDR_VERBOSE") != NULL;
    char pathbuf[PATH_MAX];   // canonical path: the same library reached two ways loads once
    const char *path = realpath(path_in, pathbuf) ? pathbuf : path_in;
    pthread_mutex_lock(&lock);
    void *ret = NULL;
    int fd = -1;
    for (int i = 0; i < nmods; i++)
        if (strcmp(mods[i].name, path) == 0) { ret = &mods[i]; goto out; }
    if (nmods == MAXMOD) { logf_("too many modules"); goto out; }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { logf_("%s: %s", path, strerror(errno)); goto out; }
    ElfW(Ehdr) eh;
    if (read_at(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_machine != EM_AARCH64
        || eh.e_type != ET_DYN || eh.e_phentsize != sizeof(ElfW(Phdr))) {
        logf_("%s: not an arm64 shared object", path); goto out;
    }
    ElfW(Phdr) ph[64];
    if (eh.e_phnum > 64 || read_at(fd, ph, eh.e_phnum * sizeof ph[0], eh.e_phoff)) { logf_("%s: bad phdrs", path); goto out; }

    uintptr_t lo = UINTPTR_MAX, hi = 0, align = 0x10000;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_align > align) align = ph[i].p_align;
    }
    if (lo != 0) { logf_("%s: first LOAD not at 0", path); goto out; }
    size_t pg = sysconf(_SC_PAGESIZE);
    size_t span = (hi + pg - 1) & ~(pg - 1);
    char *res = mmap(NULL, span + align, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { logf_("%s: reserve: %s", path, strerror(errno)); goto out; }
    uintptr_t base = ((uintptr_t)res + align - 1) & ~(align - 1);
    if (base > (uintptr_t)res) munmap(res, base - (uintptr_t)res);
    munmap((void *)(base + span), (uintptr_t)res + span + align - (base + span));
    if (mmap((void *)base, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        logf_("%s: map: %s", path, strerror(errno)); goto out;
    }
    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_filesz
            && read_at(fd, (void *)(base + ph[i].p_vaddr), ph[i].p_filesz, ph[i].p_offset)) {
            logf_("%s: read segment %d failed", path, i); goto out;
        }

    struct pzmod *m = &mods[nmods];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", path);
    m->base = base; m->span = span;
    m->phnum = eh.e_phnum;
    m->phdr = (const ElfW(Phdr) *)(base + eh.e_phoff);   // first LOAD covers the headers

    const ElfW(Dyn) *dyn = NULL;
    for (int i = 0; i < eh.e_phnum; i++) if (ph[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn) *)(base + ph[i].p_vaddr);
    if (!dyn) { logf_("%s: no PT_DYNAMIC", path); goto out; }
    const ElfW(Rela) *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, jmprelsz = 0, initarrsz = 0;
    uintptr_t init = 0, initarr = 0;
    const uint32_t *gnuhash = NULL, *sysvhash = NULL;
    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const ElfW(Sym) *)(base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(base + d->d_un.d_ptr); break;
        case DT_RELA: rela = (const ElfW(Rela) *)(base + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (const ElfW(Rela) *)(base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
        case DT_INIT: init = base + d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: initarr = base + d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: initarrsz = d->d_un.d_val; break;
        case DT_GNU_HASH: gnuhash = (const uint32_t *)(base + d->d_un.d_ptr); break;
        case DT_HASH: sysvhash = (const uint32_t *)(base + d->d_un.d_ptr); break;
        case DT_REL: case DT_RELR: case DT_ANDROID_REL: case DT_ANDROID_RELA:
            logf_("%s: unsupported relocation table (dyn tag 0x%lx)", path, (long)d->d_tag); goto out;
        }
    }
    m->nsyms = sysvhash ? sysvhash[1] : gnuhash ? gnu_hash_nsyms(gnuhash) : 0;

    // Dependencies go into the global namespace (they are normal glibc-loadable libraries).
    char dir[PATH_MAX];
    if (!realpath(path, dir)) snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/'); if (slash) *slash = 0; else strcpy(dir, ".");
    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        const char *need = m->strtab + d->d_un.d_val;
        char full[PATH_MAX + 256]; snprintf(full, sizeof full, "%s/%s", dir, need);
        int local = access(full, F_OK) == 0;
        void *h = local ? dlopen(full, RTLD_NOW | RTLD_GLOBAL) : dlopen(need, RTLD_NOW | RTLD_GLOBAL);
        if (!h && local) {   // another 4 KB-aligned Android library: load it ourselves
            VLOG("%s: needed %s not loadable by ld.so (%s), using pzldr", path, need, dlerror());
            h = pzldr_open(full);
        }
        if (!h) { logf_("%s: needed %s: %s", path, need, dlerror()); goto out; }
        VLOG("%s: needed %s ok", path, need);
    }

    int unresolved = 0;
    if (rela && relocate(m, rela, relasz / sizeof *rela, &unresolved)) goto out;
    if (jmprel && relocate(m, jmprel, jmprelsz / sizeof *jmprel, &unresolved)) goto out;
    if (unresolved) logf_("%s: %d unresolved imports (they abort if called)", path, unresolved);

    // Page permissions: union of the flags of every segment touching each page.
    size_t npages = span / pg;
    unsigned char *perm = calloc(npages, 1);
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        int f = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0) | (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
        for (size_t p = ph[i].p_vaddr / pg; p <= (ph[i].p_vaddr + ph[i].p_memsz - 1) / pg; p++) perm[p] |= f;
    }
    int rwx = 0;
    for (size_t p = 0; p < npages;) {
        size_t q = p;
        while (q < npages && perm[q] == perm[p]) q++;
        int f = perm[p] ? perm[p] : PROT_NONE;
        if ((f & PROT_WRITE) && (f & PROT_EXEC)) rwx += q - p;
        if (f & PROT_EXEC) __builtin___clear_cache((char *)(base + p * pg), (char *)(base + q * pg));
        if (mprotect((void *)(base + p * pg), (q - p) * pg, f)) { logf_("%s: mprotect: %s", path, strerror(errno)); free(perm); goto out; }
        p = q;
    }
    free(perm);
    nmods++;
    VLOG("%s: loaded at %#lx, %zu KB, %zu symbols, %d RWX pages", path, (unsigned long)base, span >> 10, m->nsyms, rwx);

    // Initializers (bionic passes argc/argv/envp too).
    typedef void (*initfn)(int, char **, char **);
    if (init) ((initfn)init)(0, NULL, environ);
    for (size_t i = 0; i < initarrsz / sizeof(uintptr_t); i++) {
        uintptr_t f = ((uintptr_t *)initarr)[i];
        if (f && f != (uintptr_t)-1) ((initfn)f)(0, NULL, environ);
    }
    ret = m;
out:
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&lock);
    return ret;
}

void *pzldr_sym(void *handle, const char *name) {
    return handle ? mod_sym((struct pzmod *)handle, name) : NULL;
}
