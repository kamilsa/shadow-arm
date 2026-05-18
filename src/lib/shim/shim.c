#include "lib/shim/shim.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <search.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "lib/log-c2rust/log-c2rust.h"
#include "lib/log-c2rust/rustlogger.h"
#include "lib/logger/logger.h"
#include "lib/shadow-shim-helper-rs/shim_helper.h"
#include "lib/shim/patch_vdso.h"
#include "lib/shim/shim_api.h"
#include "lib/shim/shim_insn_emu.h"
#include "lib/shim/shim_seccomp.h"
#include "lib/shim/shim_sys.h"
#include "lib/shim/shim_syscall.h"

static void _shim_parent_init_logging() {
    int level = shimshmem_getLogLevel(shim_hostSharedMem());

    // Route C logging through Rust's `log`
    logger_setDefault(rustlogger_new());
    // Install our `log` backend.
    shimlogger_install(level);
}

static void _shim_init_death_signal() {
    // Ensure that the child process exits when Shadow does. This is to avoid
    // confusing behavior or a "stalled out" process in the case that Shadow
    // exits abnormally. Shadow normally ensures all managed processes have
    // exited before exiting itself.
    //
    // TODO: This would be better to do in between (v)fork and exec, e.g. in
    // case the shim is never initialized properly, but isn't currently an
    // operation supported by posix_spawn.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
        warning("prctl: %s", strerror(errno));
    }

    // Exit now if Shadow has already exited before we made the above `prctl`
    // call.
    if (getppid() != shimshmem_getShadowPid(shim_hostSharedMem())) {
        error("Shadow exited.");
        exit(EXIT_FAILURE);
    }
}

static void _shim_parent_init_memory_manager_internal() {
    syscall(SHADOW_SYSCALL_NUM_INIT_MEMORY_MANAGER);
}

// Tell Shadow to initialize the MemoryManager, which includes remapping the
// stack.
static void _shim_parent_init_memory_manager() {
    if (shim_getExecutionContext() != EXECUTION_CONTEXT_SHADOW) {
        panic("Unexpectedly called from non-shadow context");
    }

    // Temporarily allocate some memory for a separate stack. The MemoryManager
    // is going to remap the original stack, and we can't actively use it while
    // it does so.
    const size_t stack_sz = 4096*10;
    void *stack = mmap(NULL, stack_sz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        panic("mmap: %s", strerror(errno));
    }

    ucontext_t remap_ctx, orig_ctx;
    if (getcontext(&remap_ctx) != 0) {
        panic("getcontext: %s", strerror(errno));
    }

    // Run on our temporary stack.
    remap_ctx.uc_stack.ss_sp = stack;
    remap_ctx.uc_stack.ss_size = stack_sz;

    // Return to the original ctx (which is initialized by swapcontext, below).
    remap_ctx.uc_link = &orig_ctx;

    makecontext(&remap_ctx, _shim_parent_init_memory_manager_internal, 0);

    // Call _shim_parent_init_memory_manager_internal on the configured stack.
    // Returning from _shim_parent_init_memory_manager_internal will return to
    // here.
    if (swapcontext(&orig_ctx, &remap_ctx) != 0) {
        panic("swapcontext: %s", strerror(errno));
    }

    if (munmap(stack, stack_sz) != 0) {
        panic("munmap: %s", strerror(errno));
    }
}

static void _shim_parent_init_seccomp() {
    shim_seccomp_init();
}

static void _shim_parent_init_insn_emu() {
    shim_insn_emu_init();
}

void _shim_parent_init_preload() {
    if (shim_getExecutionContext() != EXECUTION_CONTEXT_SHADOW) {
        panic("Unexpectedly called from non-shadow context");
    }

    dprintf(STDERR_FILENO, "SHIM: IPC init...\n");
    _shim_parent_init_ipc();
    dprintf(STDERR_FILENO, "SHIM: IPC done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_ipc_wait_for_start_event...\n");
    _shim_ipc_wait_for_start_event();
    dprintf(STDERR_FILENO, "SHIM: _shim_ipc_wait_for_start_event done\n");

    dprintf(STDERR_FILENO, "SHIM: shim_install_hardware_error_handlers...\n");
    shim_install_hardware_error_handlers();
    dprintf(STDERR_FILENO, "SHIM: shim_install_hardware_error_handlers done\n");
    patch_vdso((void*)getauxval(AT_SYSINFO_EHDR));
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_host_shm...\n");
    _shim_parent_init_host_shm();
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_host_shm done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_manager_shm...\n");
    _shim_parent_init_manager_shm();
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_manager_shm done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_logging...\n");
    _shim_parent_init_logging();
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_logging done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_init_signal_stack...\n");
    _shim_init_signal_stack();
    dprintf(STDERR_FILENO, "SHIM: _shim_init_signal_stack done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_init_death_signal...\n");
    _shim_init_death_signal();
    dprintf(STDERR_FILENO, "SHIM: _shim_init_death_signal done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_memory_manager...\n");
    _shim_parent_init_memory_manager();
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_memory_manager done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_insn_emu...\n");
    _shim_parent_init_insn_emu();
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_insn_emu done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_seccomp...\n");
    _shim_parent_init_seccomp();
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_init_seccomp done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_close_stdin...\n");
    _shim_parent_close_stdin();
    dprintf(STDERR_FILENO, "SHIM: _shim_parent_close_stdin done\n");
    dprintf(STDERR_FILENO, "SHIM: preempt_process_init...\n");
    preempt_process_init();
    dprintf(STDERR_FILENO, "SHIM: preempt_process_init done\n");
}

void _shim_child_thread_init_preload() {
    if (shim_getExecutionContext() != EXECUTION_CONTEXT_SHADOW) {
        panic("Unexpectedly called from non-shadow context");
    }

    _shim_preload_only_child_ipc_wait_for_start_event();

    dprintf(STDERR_FILENO, "SHIM: _shim_init_signal_stack...\n");
    _shim_init_signal_stack();
    dprintf(STDERR_FILENO, "SHIM: _shim_init_signal_stack done\n");
}

void _shim_child_process_init_preload() {
    if (shim_getExecutionContext() != EXECUTION_CONTEXT_SHADOW) {
        panic("Unexpectedly called from non-shadow context");
    }

    _shim_preload_only_child_ipc_wait_for_start_event();
    dprintf(STDERR_FILENO, "SHIM: _shim_init_signal_stack...\n");
    _shim_init_signal_stack();
    dprintf(STDERR_FILENO, "SHIM: _shim_init_signal_stack done\n");
    dprintf(STDERR_FILENO, "SHIM: _shim_init_death_signal...\n");
    _shim_init_death_signal();
    dprintf(STDERR_FILENO, "SHIM: _shim_init_death_signal done\n");
}

void shim_ensure_init() { _shim_load(); }
