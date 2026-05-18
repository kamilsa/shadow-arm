# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
# Build (release)
./setup build

# Build with tests and extra dependencies
./setup build --test --extra

# Debug build
./setup build --debug --test

# Clean rebuild
./setup build --clean --test

# Run all tests
./setup test

# Run specific test by regex
./setup test <name-regex>

# Run tests with extra (requires --extra at build time)
./setup test --extra

# Run Rust unit tests
(cd src && cargo test)

# Run clippy
(cd src && cargo clippy --all-targets)

# Format Rust
(cd src && cargo fmt)

# Check Rust format
(cd src && cargo fmt -- --check)
```

## Architecture

Shadow is a discrete-event network simulator that runs real applications in a simulated network environment.

### How it works
1. Creates **managed processes** via `vfork()` + `execvpe()`
2. Injects a **shim library** via `LD_PRELOAD` (`src/lib/shim/`)
3. Establishes **IPC** via shared memory and semaphores (`src/lib/shmem/`)
4. Intercepts **syscalls** via two tiers:
   - Preloading (overriding libc function symbols at link time)
   - Seccomp (trapping all non-shim syscalls)

### Key crates (from `src/Cargo.toml` workspace)
| Crate | Path | Purpose |
|---|---|---|
| `shadow-rs` | `src/main/` | Core simulator binary |
| `shadow-shim` | `src/lib/shim/` | LD_PRELOAD shim (intercepts syscalls in managed processes) |
| `shadow-shim-helper-rs` | `src/lib/shadow-shim-helper-rs/` | Shared data structures between shim and simulator |
| `shadow_shmem` | `src/lib/shmem/` | Shared memory IPC |
| `linux-api` | `src/lib/linux-api/` | Linux kernel header bindings (no_std, via bindgen) |
| `scheduler` | `src/lib/scheduler/` | Work-stealing thread scheduler |
| `tcp` | `src/lib/tcp/` | TCP implementation |
| `logger` | `src/lib/logger/` | Logging framework |
| `asm-util` | `src/lib/asm-util/` | Assembly utilities (TSC, CPUID) |
| `shadow-test` | `src/test/` | Syscall tests and integration tests |

### C code (still migrating to Rust) in `src/main/`
- `core/` — CPU affinity, worker management, controller, manager
- `host/descriptor/` — descriptor.c, socket.c, epoll.c, tcp.c
- `host/process.c`, `futex.c`
- `network/` — legacy packet types

## Platform

- **Supported**: Linux (x86-64, ARM64 with the `arm64-port` branch)
- **Minimum kernel**: 5.10
- **Docker**: requires `--shim-size=1024g --security-opt seccomp=unconfined`
- **Toolchain**: cargo 1.75+ (1.95 in CI), cmake >= 3.18.4, glib-2.0 >= 2.58

## Code conventions

From `docs/coding.md`:
- New code should be in **Rust** (migration from C is ongoing)
- Use `#![deny(unsafe_op_in_unsafe_fn)]` in Rust crates
- Syscall handler code: prefer `linux-api` crate over `libc` or `nix`
- Shim code: prefer `rustix` over `libc`
- General Shadow code: prefer `std`
- Tests: use `libc` (managed programs use libc)
- The `nix` crate is being phased out

From `docs/coding_style.md`:
- Format Rust with `cargo fmt`, lint with `clippy --all-targets`
- Format C with `clang-format` (LLVM style, 100 cols, 4-space indent)
- Quote-includes for project headers: `#include "main/utility/byte_queue.h"`
- Angle-brackets for external: `<glib.h>`

## ARM64 port status

The `arm64-port` branch includes changes for ARM64 Linux support. Key differences:
- `c_char` is `u8` (ARM64) vs `i8` (x86-64) — use `core::ffi::c_char`
- ARM64 uses `svc #0` (4 bytes) vs `syscall` (2 bytes) — update `SIZEOF_SYSCALL_INSN`
- ARM64 VDSO symbols use `__kernel_` prefix instead of `__vdso_`
- ARM64 ucontext layout differences (uc_mcontext at offset 176)
- ARM64 seccomp uses `si_syscall` (not gregs[REG_RAX])
- No `SYS_open`, `SYS_fork`, `SYS_time`, `SYS_creat` on ARM64 — use `openat(AT_FDCWD)`, `clone(SIGCHLD)`, `setitimer` fallbacks

## Examples

Simulation configs in `examples/` — run with `shadow examples/apps/curl/shadow.yaml`.
