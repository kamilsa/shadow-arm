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

void shim_seccomp_init() {
    // Install signal sigsys signal handler, which will receive syscalls that
    // get stopped by the seccomp filter. Shadow's emulation of signal-related
    // system calls will prevent this action from later being overridden by the
    // virtual process.
    struct sigaction old_action;
    if (sigaction(SIGSYS,
                  &(struct sigaction){
                      .sa_sigaction = _shim_seccomp_handle_sigsys,
                      // SA_NODEFER: Allow recursive signal handling, to handle a syscall
                      // being made during the handling of another. For example, we need this
                      // to properly handle the case that we end up logging from the syscall
                      // handler, and the IO syscalls themselves are trapped.
                      // SA_SIGINFO: Required because we're specifying sa_sigaction.
                      // SA_ONSTACK: Use the alternate signal handling stack, to avoid interfering
                      // with userspace thread stacks.
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

    // Ensure that SIGSYS isn't blocked. This code runs in the process's first
    // thread, so the resulting mask will be inherited by subsequent threads.
    // Shadow's emulation of signal-related system calls will prevent it from
    // later becoming blocked.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGSYS);
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL)) {
        panic("sigprocmask: %s", strerror(errno));
    }

    // Setting PR_SET_NO_NEW_PRIVS allows us to install a seccomp filter without
    // CAP_SYS_ADMIN.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        panic("prctl: %s", strerror(errno));
    }

    // Find the region of memory containing this function. That should be
    // the `.text` section of the shim, and contain all of the code in the shim.
    _getSectionContaining((void*)shim_seccomp_init, &TEXT_START, &TEXT_END);
    trace("text start:%p end:%p", TEXT_START, TEXT_END);
    dprintf(STDERR_FILENO, "SECCOMP_TEXT_BOUNDS start=%p end=%p\\n", TEXT_START, TEXT_END);
    if (TEXT_START == NULL) {
        panic("Couldn't find memory region containing `shim_seccomp_init`");
    }
    if (TEXT_END == NULL) {
        panic("bad end");
    }

    // We break text start and end addresses into high and low 32 bit
    // values for use in 32 bit seccomp filter operations.
    uint32_t text_start_high = (uintptr_t)TEXT_START >> 32;
    uint32_t text_start_low = (uintptr_t)TEXT_START;
    uint32_t text_end_high = (uintptr_t)TEXT_END >> 32;
    uint32_t text_end_low = (uintptr_t)TEXT_END;

    /* A bpf program to be loaded as a `seccomp` filter. Unfortunately the
     * documentation for how to write this is pretty sparse. There's a useful
     * example in samples/seccomp/bpf-direct.c of the Linux kernel source tree.
     * The best reference I've been able to find is a BSD man page:
     * https://www.freebsd.org/cgi/man.cgi?query=bpf&sektion=4&manpath=FreeBSD+4.7-RELEASE
     *
     * CAUTION: while that page annotates `k` as a `u_long`, `k` is only 32 bits.
     *
     * TODO: Consider moving filter generation into Rust, where we might be able
     * to avoid some footguns (like implicit 64->32 bit conversions), and potentially
     * into the Shadow process where we might be able to use some 3rd party libraries
     * such as `libseccomp` (which unfortunately doesn't support address range filters
     * https://github.com/seccomp/libseccomp/issues/113)
     *
     * Better yet, consider migrating to `PR_SET_SYSCALL_USER_DISPATCH`, which implements
     * *almost* the seccomp filter we're creating here. It requires minimum kernel
     * version 5.11, though.
     * https://www.kernel.org/doc./html/latest/admin-guide/syscall-user-dispatch.html
     */
    struct sock_filter filter[] = {
        /* accumulator := syscall number */
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, nr)),

        /* Always allow sigreturn; otherwise we'd crash returning from our signal handler. */
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, SYS_rt_sigreturn, /*true-skip=*/0, /*false-skip=*/1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

    /* This block was intended to whitelist reads and writes to a socket
     * used to communicate with Shadow. It turns out to be unnecessary though,
     * because the functions we're using are already wrapped, and so go through
     * shim_native_syscallv, and so end up already being whitelisted above based on that.
     * (Also ended up switching back to shared-mem-based IPC instead of a socket).
     *
     * Keeping the code around for now in case we end up needing it or something similar.
     */
#if 0
        /* check_socket: Allow reads and writes to shadow socket */
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_read, 0, 2/*check_fd*/),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_write, 0, 1/*check_fd*/),
        /* Skip to instruction pointer check */
        BPF_JUMP(BPF_JMP+BPF_JA, 3/* check_ip */, 0, 0),
        /* check_fd */
        /* accumulator := arg1 */
        BPF_STMT(BPF_LD+BPF_W+BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, toShadowFd, 0, 1/* check_ip */),
        BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
#endif

        /* Allow syscalls made from the `.text` section.
         * We allow-list native syscalls made from this region both for correctness
         * (to avoid recursing in our syscall handling) and performance (avoid the
         * interception overhead in internal synchronization primitives).
         *
         * We need to compare a 64-bit instruction pointer to 64-bit addresses.
         * Unfortunately seccomp's BPF only supports 32-bit values, so we need
         * to compare the high and low 32-bits separately.
         *
         * According to seccomp(2), the instruction pointer we load here should
         * be the address of the `syscall` instruction itself, *not* the address
         * of the *next* instruction, which is what we get in the signal handler.
         *
         * In comments below we use:
         * IP_high: high 32 bits of instruction pointer
         * IP_low: low 32 bits of instruction pointer
         */

        /* if (IP_high > text_end_high) trap; */
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JGT + BPF_K, text_end_high,
                 /*true-skip=*/0, /*false-skip=*/1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        /* if (IP_high == text_end_high && IP_low >= text_end_low) trap; */
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, text_end_high,
                 /*true-skip=*/0, /*false-skip=*/3),
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, text_end_low,
                 /*true-skip=*/0, /*false-skip=*/1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        /* if (IP_high < text_start_high) trap; */
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, text_start_high, /*true-skip=*/1, /*false-skip=*/0),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        /* if (IP_high == text_start_high && IP_low < text_start_low) trap; */
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + sizeof(int32_t)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, text_start_high,
                 /*true-skip=*/0, /*false-skip=*/3),
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, text_start_low,
                 /*true-skip=*/1, /*false-skip=*/0),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),

        /* Allow  */
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    // Re SECCOMP_FILTER_FLAG_SPEC_ALLOW: Without this flag, installing a
    // seccomp filter sets the PR_SPEC_FORCE_DISABLE bit (see prctl(2)). This
    // results in a significant performance penalty. Meanwhile Shadow is
    // semi-cooperative with its virtual processes; it doesn't try to protect
    // itself or the system from malicious code. Hence, it isn't worth paying
    // this overhead.
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_SPEC_ALLOW, &prog)) {
        panic("seccomp: %s", strerror(errno));
    }
}
