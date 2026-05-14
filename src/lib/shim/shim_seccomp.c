/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */
#include "shim_seccomp.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "lib/logger/logger.h"
#include "lib/shim/shim_syscall.h"
#include "lib/shim/shim_tls.h"

// Per-thread FPSIMD save buffer for the SIGSYS handler.
// On ARM64, callees of the handler may use NEON/SIMD registers.
// We save all Q registers + FPSR/FPCR at handler entry and restore at exit
// to prevent the managed process from seeing corrupted NEON state.
#ifdef __aarch64__
struct fpsimd_save {
    __uint128_t vregs[32];
    __uint32_t fpsr;
    __uint32_t fpcr;
};
static _Thread_local struct fpsimd_save fpsimd_save_buf;

static void fpsimd_save(struct fpsimd_save* buf) {
    __asm__ volatile(
        "stp q0, q1, [%0, #0]\n\t"
        "stp q2, q3, [%0, #32]\n\t"
        "stp q4, q5, [%0, #64]\n\t"
        "stp q6, q7, [%0, #96]\n\t"
        "stp q8, q9, [%0, #128]\n\t"
        "stp q10, q11, [%0, #160]\n\t"
        "stp q12, q13, [%0, #192]\n\t"
        "stp q14, q15, [%0, #224]\n\t"
        "stp q16, q17, [%0, #256]\n\t"
        "stp q18, q19, [%0, #288]\n\t"
        "stp q20, q21, [%0, #320]\n\t"
        "stp q22, q23, [%0, #352]\n\t"
        "stp q24, q25, [%0, #384]\n\t"
        "stp q26, q27, [%0, #416]\n\t"
        "stp q28, q29, [%0, #448]\n\t"
        "stp q30, q31, [%0, #480]\n\t"
        "mrs %x1, fpsr\n\t"
        "mrs %x2, fpcr\n\t"
        "str w1, [%0, #512]\n\t"
        "str w2, [%0, #516]\n\t"
        :
        : "r"(buf->vregs), "r"(&buf->fpsr), "r"(&buf->fpcr)
        : "memory"
    );
}

static void fpsimd_restore(const struct fpsimd_save* buf) {
    __asm__ volatile(
        "ldp q0, q1, [%0, #0]\n\t"
        "ldp q2, q3, [%0, #32]\n\t"
        "ldp q4, q5, [%0, #64]\n\t"
        "ldp q6, q7, [%0, #96]\n\t"
        "ldp q8, q9, [%0, #128]\n\t"
        "ldp q10, q11, [%0, #160]\n\t"
        "ldp q12, q13, [%0, #192]\n\t"
        "ldp q14, q15, [%0, #224]\n\t"
        "ldp q16, q17, [%0, #256]\n\t"
        "ldp q18, q19, [%0, #288]\n\t"
        "ldp q20, q21, [%0, #320]\n\t"
        "ldp q22, q23, [%0, #352]\n\t"
        "ldp q24, q25, [%0, #384]\n\t"
        "ldp q26, q27, [%0, #416]\n\t"
        "ldp q28, q29, [%0, #448]\n\t"
        "ldp q30, q31, [%0, #480]\n\t"
        "ldr w1, [%0, #512]\n\t"
        "ldr w2, [%0, #516]\n\t"
        "msr fpsr, x1\n\t"
        "msr fpcr, x2\n\t"
        :
        : "r"(buf->vregs), "r"(&buf->fpsr), "r"(&buf->fpcr)
        : "memory"
    );
}
#endif

// Start of shim's text (code) segment. Inclusive.
// Immutable after global initialization, which should be done exactly once by
// one thread.
static void* TEXT_START = NULL;
// End of shim's text (code) segment. Exclusive.
// Immutable after global initialization, which should be done exactly once by
// one thread.
static void* TEXT_END = NULL;

#ifdef __aarch64__
__attribute__((target("general-regs-only")))
#endif
static void _debug_sigsys(const char* label, long syscall_num, const void* pc,
                          const void* syscall_insn_addr, long rv) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "SIGSYS_DEBUG %s n=%ld pc=%p insn=%p rv=%ld\n", label,
                       syscall_num, pc, syscall_insn_addr, rv);
    if (len < 0) {
        return;
    }
    if (len > (int)sizeof(buf)) {
        len = sizeof(buf);
    }
    shim_native_syscall(NULL, SYS_write, STDERR_FILENO, buf, (long)len);
}

// Handler function that receives syscalls that are stopped by the seccomp filter.
#ifdef __aarch64__
__attribute__((target("general-regs-only")))
#endif
static void _shim_seccomp_handle_sigsys(int sig, siginfo_t* info, void* voidUcontext) {
#ifdef __aarch64__
    // Save NEON/SIMD state before any callee clobbers it.
    // The kernel saves FPSIMD on the signal stack, but callees of this handler
    // (shim_syscall, Rust code, libc) may use NEON and modify registers.
    // We save to a per-thread buffer to avoid touching the signal stack.
    fpsimd_save(&fpsimd_save_buf);
#endif
    ExecutionContext prev_ctx = shim_swapExecutionContext(EXECUTION_CONTEXT_SHADOW);
    ucontext_t* ctx = (ucontext_t*)(voidUcontext);
    if (sig != SIGSYS) {
        abort();
    }

#ifdef __x86_64__
    #define SIZEOF_SYSCALL_INSN 2
    greg_t* regs = ctx->uc_mcontext.gregs;
    const long syscall_num = regs[REG_RAX];
    const long arg1 = regs[REG_RDI];
    const long arg2 = regs[REG_RSI];
    const long arg3 = regs[REG_RDX];
    const long arg4 = regs[REG_R10];
    const long arg5 = regs[REG_R8];
    const long arg6 = regs[REG_R9];
    const void* pc = (void*)regs[REG_RIP];
    // pc points to instruction *after* the syscall instruction, which is 2 bytes long.
    const void* syscall_insn_addr = (void*)regs[REG_RIP] - SIZEOF_SYSCALL_INSN;
    unsigned long long* return_reg = &ctx->uc_mcontext.gregs[REG_RAX];
#else
    // ARM64: svc #0 instruction is 4 bytes.
    #define SIZEOF_SYSCALL_INSN 4
    // Syscall number from siginfo_t (x8 may be clobbered in the signal context).
    const long syscall_num = info->si_syscall;
    const long arg1 = ctx->uc_mcontext.regs[0];
    const long arg2 = ctx->uc_mcontext.regs[1];
    const long arg3 = ctx->uc_mcontext.regs[2];
    const long arg4 = ctx->uc_mcontext.regs[3];
    const long arg5 = ctx->uc_mcontext.regs[4];
    const long arg6 = ctx->uc_mcontext.regs[5];
    // pc (saved in sigcontext) points to the instruction after svc.
    const void* pc = (void*)ctx->uc_mcontext.pc;
    const void* syscall_insn_addr = (void*)(ctx->uc_mcontext.pc - SIZEOF_SYSCALL_INSN);
    unsigned long long* return_reg = &ctx->uc_mcontext.regs[0];
#endif

    trace("Trapped syscall %ld at %p", syscall_num, syscall_insn_addr);
    _debug_sigsys("entry", syscall_num, pc, syscall_insn_addr, 0);
    if (syscall_insn_addr >= TEXT_START && syscall_insn_addr < TEXT_END) {
        panic("seccomp filter blocked syscall from %p, which is within %p-%p", syscall_insn_addr,
              TEXT_START, TEXT_END);
    }

    // Make the syscall via the *the shim's* syscall function (which overrides
    // libc's).  It in turn will either emulate it or (if interposition is
    // disabled), make the call natively. In the latter case, the syscall
    // will be permitted to execute by the seccomp filter.
    // Make the syscall via the *the shim's* syscall function (which overrides
    // libc's).  It in turn will either emulate it or (if interposition is
    // disabled), make the call natively. In the latter case, the syscall
    // will be permitted to execute by the seccomp filter.
    long rv = shim_syscall(ctx, prev_ctx, syscall_num, arg1, arg2,
                           arg3, arg4, arg5, arg6);
    trace("Trapped syscall %ld returning %ld", syscall_num, rv);
    _debug_sigsys("return", syscall_num, pc, syscall_insn_addr, rv);
    *return_reg = rv;
    shim_swapExecutionContext(prev_ctx);
#ifdef __aarch64__
    // Restore NEON/SIMD state that callees may have clobbered.
    fpsimd_restore(&fpsimd_save_buf);
#endif
#undef SIZEOF_SYSCALL_INSN
}

// TODO: dedupe this with `maps` parsing in `patch_vdso.c` and `proc_maps.rs`,
// ideally into something that doesn't allocate or use libc.
static void _getSectionContaining(const void* target, void** start, void** end) {
    assert(start);
    *start = NULL;
    assert(end);
    *end = NULL;

    FILE* maps = fopen("/proc/self/maps", "r");

    size_t n = 100;
    // `line` has to be `malloc`'d for compatibility with `getline`, below.
    char* line = malloc(n);

    while (true) {
        ssize_t rv = getline(&line, &n, maps);
        if (rv < 0) {
            break;
        }
        if (sscanf(line, "%p-%p", start, end) != 2) {
            warning("Couldn't parse maps line: %s", line);
            // Ensure both are still NULL.
            *start = NULL;
            *end = NULL;
            // Might as well keep going and see if another line matches and parses.
            continue;
        }
        if (target >= *start && target < *end) {
            // Success
            break;
        }
    }

    free(line);
    fclose(maps);
}

// Thread-local selector for syscall_user_dispatch on ARM64.
// Set to BLOCK normally to intercept syscalls from outside the shim.
// Callees that need to make native syscalls can set it to ALLOW temporarily.
#ifdef __aarch64__
static _Thread_local char syscall_dispatch_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
#endif

void shim_seccomp_init() {
    // Install signal sigsys signal handler, which will receive syscalls that
    // get intercepted. Shadow's emulation of signal-related system calls will
    // prevent this action from later being overridden by the virtual process.
    struct sigaction old_action;
    if (sigaction(SIGSYS,
                  &(struct sigaction){
                      .sa_sigaction = _shim_seccomp_handle_sigsys,
                      // SA_NODEFER: Allow recursive signal handling, to handle a syscall
                      // being made during the handling of another.
                      // SA_SIGINFO: Required because we're specifying sa_sigaction.
                      // SA_ONSTACK: Use the alternate signal handling stack.
                      .sa_flags = SA_NODEFER | SA_SIGINFO | SA_ONSTACK,
                  },
                  &old_action) < 0) {
        panic("sigaction: %s", strerror(errno));
    }
    if (old_action.sa_handler || old_action.sa_sigaction) {
        warning("Overwrite handler for SIGSYS (%p)", old_action.sa_handler
                                                         ? (void*)old_action.sa_handler
                                                         : (void*)old_action.sa_sigaction);
    }

    // Ensure that SIGSYS isn't blocked.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGSYS);
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL)) {
        panic("sigprocmask: %s", strerror(errno));
    }

    // Setting PR_SET_NO_NEW_PRIVS is required for both seccomp and syscall_user_dispatch.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        panic("prctl: %s", strerror(errno));
    }

    // Find the shim's .text section for the allowed region.
    _getSectionContaining((void*)shim_seccomp_init, &TEXT_START, &TEXT_END);
    trace("text start:%p end:%p", TEXT_START, TEXT_END);
    if (TEXT_START == NULL || TEXT_END == NULL) {
        panic("Couldn't find shim .text section");
    }

#ifdef __aarch64__
    // Use PR_SET_SYSCALL_USER_DISPATCH instead of seccomp BPF on ARM64.
    // This avoids the seccomp signal delivery path which corrupts FPSIMD
    // state on virtualized ARM64 (Docker on macOS). Requires kernel >=5.11.
    //
    // syscall_user_dispatch intercepts any syscall instruction outside
    // the allowed region and delivers SIGSYS with si_code=SYS_USER_DISPATCH.
    // Syscalls from within the region go straight to the kernel.
    //
    // The thread-local selector can be set to ALLOW to temporarily permit
    // syscalls from outside the region (useful for shim_native_syscall).
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH,
              PR_SYS_DISPATCH_ON,
              TEXT_START,
              (uintptr_t)TEXT_END - (uintptr_t)TEXT_START,
              &syscall_dispatch_selector) < 0) {
        panic("prctl(PR_SET_SYSCALL_USER_DISPATCH): %s", strerror(errno));
    }
    trace("syscall_user_dispatch enabled: region %p-%p", TEXT_START, TEXT_END);
#else
    // x86-64: use seccomp BPF filter (original implementation).
    // We break text start and end addresses into high and low 32 bit
    // values for use in 32 bit seccomp filter operations.
    uint32_t text_start_high = (uintptr_t)TEXT_START >> 32;
    uint32_t text_start_low = (uintptr_t)TEXT_START;
    uint32_t text_end_high = (uintptr_t)TEXT_END >> 32;
    uint32_t text_end_low = (uintptr_t)TEXT_END;

    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, SYS_rt_sigreturn, /*true-skip=*/0, /*false-skip=*/1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

#if 0
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_read, 0, 2),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_write, 0, 1),
        BPF_JUMP(BPF_JMP+BPF_JA, 3, 0, 0),
        BPF_STMT(BPF_LD+BPF_W+BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, toShadowFd, 0, 1),
        BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
#endif

        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JGT + BPF_K, text_end_high, /*true-skip=*/0, /*false-skip=*/1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, text_end_high, /*true-skip=*/0, /*false-skip=*/3),
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, text_end_low, /*true-skip=*/0, /*false-skip=*/1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, text_start_high, /*true-skip=*/1, /*false-skip=*/0),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, text_start_high, /*true-skip=*/0, /*false-skip=*/3),
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, text_start_low, /*true-skip=*/1, /*false-skip=*/0),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_SPEC_ALLOW, &prog)) {
        panic("seccomp: %s", strerror(errno));
    }
#endif
}
