// Preloaded first so its static TLS block starts at tp+16 on aarch64.
// Bionic code reads its stack canary from tp+40 (TLS_SLOT_STACK_GUARD); this
// reserves that slot with a constant so the canary never changes under it.
#include <stdio.h>
#include <stdint.h>
__attribute__((tls_model("initial-exec"), used, aligned(16)))
__thread uint64_t pz_bionic_tls_slots[8];
__attribute__((constructor)) static void check(void) {
    char *tp; __asm__("mrs %0, tpidr_el0" : "=r"(tp));
    long off = (char *)&pz_bionic_tls_slots[0] - tp;
    if (off > 40 || off + (long)sizeof pz_bionic_tls_slots <= 48)
        fprintf(stderr, "[pztls] WARNING: TLS block at tp+%ld does not cover the bionic canary slot\n", off);
}
