#include "lib/asm-util/tsc_internal.h"

// ARM64 stubs: there is no TSC (timestamp counter) on ARM64.
// These functions are never called at runtime (shim_insn_emu is gated on ARM64).

uint64_t TscC_nativeCyclesPerSecond() {
    return 0;
}

// The exported Rust functions (via cbindgen) that the main process may link against.
// Provide empty stubs.
struct Tsc {
    uint64_t cyclesPerSecond;
};

struct Tsc Tsc_create(uint64_t cycles_per_second) {
    return (struct Tsc){.cyclesPerSecond = cycles_per_second};
}

void Tsc_emulateRdtsc(const struct Tsc *tsc, uint64_t *rax, uint64_t *rdx,
                      uint64_t *rip, uint64_t nanos) {
    (void)tsc; (void)rax; (void)rdx; (void)rip; (void)nanos;
}

void Tsc_emulateRdtscp(const struct Tsc *tsc, uint64_t *rax, uint64_t *rdx,
                       uint64_t *rcx, uint64_t *rip, uint64_t nanos) {
    (void)tsc; (void)rax; (void)rdx; (void)rcx; (void)rip; (void)nanos;
}
