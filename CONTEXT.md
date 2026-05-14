# Shadow ARM64 Port — Context for Next Agent

## Goal

Run [leansim](https://github.com/kamilsa/shadow-arm) (a Rust libp2p/QUIC/gossipsub simulator) natively under [Shadow](https://github.com/shadow/shadow) (discrete-event network simulator) on **ARM64 Linux**, inside Docker on macOS (Apple Silicon), at full speed — with **QUIC transport** (not TCP fallback).

Target: 64–128 validator consensus scenarios producing signatures and aggregated proofs.

## Repos

- **Shadow fork**: `https://github.com/kamilsa/shadow-arm`, branch `arm64-port` (at `/Users/taisei/dev/shadow`)
- **leansim**: `/Users/taisei/dev/leansim` — uses libp2p 0.54, quinn (QUIC), gossipsub, tokio `current_thread`
- **Docker container**: `shadow-dd-test` — Ubuntu ARM64, bind-mounts both repos. Build with `./setup build` in `/shadow`. Shadow binary: `/shadow/build/src/main/shadow`

## What's Been Done — ARM64 Port (3 critical fixes)

Shadow originally only ran on x86-64 Linux. The ARM64 port involved:

### 1. Seccomp/SIGSYS handler (`src/lib/shim/shim_seccomp.c`)
- ARM64 register extraction from `si->si_syscall` and `uc_mcontext.regs[]`
- FPSIMD/NEON save/restore (32 Q registers + FPSR + FPCR) via inline asm
- TPIDR_EL0 (TLS register) save/restore
- `general-regs-only` attribute to prevent compiler NEON usage in signal handler
- Syscall number table via `linux_raw_sys` (ARM64 syscall numbers differ from x86-64)
- seccomp BPF fallback when `syscall_user_dispatch` is unsupported (Docker Desktop kernel 6.12)

### 2. `c_char` type mismatch
- `c_char = u8` on ARM64 vs `i8` on x86-64. Fixed across ~10 files using `core::ffi::c_char`.

### 3. Rust-side ARM64 fixes
- `clone.rs`: ARM64 `set_context` (restore x1-x30, set x0=0, br x17) and `do_clone_thread` (svc #0)
- `signals.rs`: Hardware error handler with PC/sp/x0 dump for ARM64
- `tls.rs`: Thread pointer via `mrs x0, tpidr_el0`
- `signal.rs`: ARM64 `sigaction_restorer` (mov x8, 139; svc #0)
- `ucontext.rs`: Explicit ARM64 ucontext/sigcontext struct with offset assertions
- `syscall.rs`: ARM64 uses `linux_raw_sys::general` for syscall numbers
- `injector.c`: Uses `SYS_clock_gettime` instead of `SYS_time` (doesn't exist on ARM64)

## What's Been Done — Runtime Bug Fixes

### Fix 1: `write_sockaddr_and_len` memory corruption (commit `6cd7ad798`)
**File**: `src/main/host/syscall/io.rs`
**Problem**: `memory_ref_mut` → `process_vm_writev` writes more bytes than type size on ARM64, corrupting adjacent managed process memory → SIGSEGV in NEON-using code (ring crypto, regex-automata).
**Fix**: Replaced `memory_ref_mut` read-modify-write with `mem.read(plugin_addr_len)` + `mem.write(plugin_addr_len, &from_len)`. Also reverted the getsockname → native workaround in `mod.rs`.

### Fix 2: `epoll_event` ABI mismatch (commit `c0afbd19d`)
**File**: `src/lib/linux-api/src/epoll.rs`, `src/main/host/syscall/handler/epoll.rs`
**Problem**: `epoll_event` on ARM64 is 16 bytes (`events: u32, _pad: u32, data: u64`) with data at offset 8. Shadow's generated bindings used x86-64 packed layout (12 bytes, data at offset 4). `epoll_ctl` wrote the pointer at the wrong offset, truncating upper 32 bits → SIGSEGV when tokio dereferenced the corrupted pointer.
**Fix**: ARM64-specific `#[repr(C)]` struct + `make_epoll_event()` constructor + compile-time assertions (size 16, align 8, offsets 0 and 8). Replaced `memory_ref_mut` in `write_events_to_ptr` with per-event `mem.write`.

Also added QUIC-related UDP sockopts (`IP_MTU_DISCOVER`, `IP_PKTINFO`, `IP_RECVTOS`) in `udp.rs` and `inet.rs` — needed once QUIC progresses past epoll.

### Fix 3: `recvmmsg` syscall handler (commit `ab1b3c669`)
**Files**: `src/main/host/syscall/handler/socket.rs`, `mod.rs`
**Problem**: tokio's QUIC event loop calls `recvmmsg` (syscall 243). Shadow had no handler → returned error → QUIC handshake stalled after first round-trip.
**Fix**: Simple `recvmmsg` handler that delegates to `recvmsg` once per `mmsghdr` entry. Uses `*const std::ffi::c_void` in `log_syscall!` to avoid `SyscallDisplay` trait issues.

## What Works

- **All 7 Shadow example apps**: echo, curl, python, wget2, iperf-2, jetty, http-server
- **Simple libp2p QUIC ping** (2 nodes): CONNECTED in both directions, 14 ping/pong pairs at 100ms emulated latency
- **Leansim 1-node**: PeerId produced + SigSent at ~3.6s + NodeStats
- **Leansim 128-validator**: 130 nodes, 124K `recvmsg` + 124K `sendto`, 20K `getsockname`, **zero SIGSEGV**, all 130 PeerIds produced
- **Epoll, futex, clone3, socketpair, eventfd**: all working correctly at scale

## What Doesn't Work — Leansim Multi-Node Consensus

In any multi-node leansim scenario (2 to 128 validators), every node produces a PeerId and QUIC connections are established, but **no `SigSent` events appear**. All nodes go idle and Shadow fast-forwards to the stop time.

The 1-node case works because with no reachable peers, leansim skips connection establishment and produces all signatures locally.

### Root Cause Analysis

The likely cause is in `leansim/src/network/swarm.rs:180-215`, function `dial_seeds`:

```rust
pub async fn dial_seeds(swarm: &mut Swarm<GossipsubBehaviour>, seeds: &[String]) -> Result<()> {
    for seed in seeds {
        let ma: Multiaddr = seed.parse()?;
        swarm.dial(ma.clone())?;  // initiates QUIC connections
    }

    // Warmup: wait for connections + mesh formation
    let warmup_ms = if seeds.len() > 200 { 3000 } else { 1500 };
    let warmup = tokio::time::Instant::now()
        + std::time::Duration::from_millis(warmup_ms);
    loop {
        let remaining = warmup.saturating_duration_since(tokio::time::Instant::now());
        if remaining.is_zero() { break; }
        match tokio::time::timeout(remaining, swarm.next()).await {
            Ok(Some(SwarmEvent::ConnectionEstablished { .. })) => { /* log */ }
            Ok(Some(SwarmEvent::OutgoingConnectionError { .. })) => { /* log */ }
            Ok(None) => break,
            Err(_) => break,  // timeout
            _ => {}  // ALL OTHER EVENTS SILENTLY IGNORED
        }
    }
    Ok(())
}
```

The warmup loop uses `tokio::time::Instant::now()` and `tokio::time::timeout()`. Under Shadow, time is **emulated** — `Instant::now()` returns emulated time, and tokio timers fire based on emulated time events. The concern is whether tokio's timer wheel advances correctly when all nodes are blocked in this warmup loop simultaneously, with no application data flowing to drive time forward.

Additionally, events like `IncomingConnection`, `Dialing`, `NewListenAddr`, and gossipsub `Behaviour` events are silently consumed by `_ => {}`. While `swarm.next()` processes internal state regardless, the timeout mechanism may not fire if Shadow's event scheduler has no events to process (all nodes are in the same timeout-wait state).

### Alternative Hypothesis

The `quinn-udp` patch in leansim (`leansim/patch/quinn-udp/src/fallback.rs`) replaces GSO/GRO with basic `send_to`/`recv_from_vectored`. It has an unsafe cast from `IoSliceMut` to `MaybeUninitSlice`. While this works for the simple ping example, it might cause data corruption or dropped packets in gossipsub's heavier message patterns.

### Key syscall counts (6-node leansim, generated config)
```
recvmsg: 36, sendto: 30, getsockname: 48, getrandom: 180
futex: 18, epoll_pwait: 12, clone3: 6
```
Very low — only ~6 `recvmsg` per node. Nodes establish PeerIds then go idle.

## How to Build and Test

```bash
# Build Shadow in Docker
docker exec shadow-dd-test bash -c 'export PATH="$HOME/.cargo/bin:$PATH" && cd /shadow && ./setup build'

# Run a test
docker exec shadow-dd-test bash -c '
  cd /tmp && rm -rf shadow.data shadow.log
  timeout 30 /shadow/build/src/main/shadow /tmp/ping-test.yaml
'
```

### Test configs in container
| Path | Description |
|------|-------------|
| `/tmp/ping-test.yaml` | 2-node libp2p QUIC ping (working) |
| `/tmp/ping-topology.gml` | Topology for ping test |
| `/tmp/libp2p-ping/` | Simple ping example source |
| `/tmp/smoke_lean.yaml` | 6-node leansim with manual configs |
| `/tmp/128-120/` | Generated 128-validator config (120s) |
| `/tmp/4-val/` | Generated 4-validator config |
| `/tmp/exp_*.toml` | Various leansim experiment configs |

### Leansim config generator
```bash
/leansim/target/release/leansim gen-shadow --experiment <toml> --out <output.yaml>
```
This generates `shadow.yaml`, `topology.gml`, and `configs/node_{id}.toml`. Generated YAML needs two manual fixes:
1. Binary path: `sed -i 's|path: /target/release/leansim|path: /leansim/target/release/leansim|g'`
2. CPU pinning: append `experimental:\n  use_cpu_pinning: false\n`

## Next Steps for Agent

The immediate problem to solve: **leansim multi-node consensus stalls at PeerId — no SigSent produced**.

Possible approaches:
1. **Fix the `dial_seeds` warmup loop**: Replace `tokio::time::Instant::now()` + `tokio::time::timeout()` with an event-counting approach (e.g., wait until N connections are established or M events processed, rather than relying on emulated time)
2. **Add logging**: Set `RUST_LOG=info` or `RUST_LOG=debug` in the process environment to see leansim's tracing output
3. **Investigate gossipsub mesh formation**: The warmup may need to handle gossipsub events (GRAFT/PRUNE) to allow the mesh to form
4. **Test incremental complexity**: Start with a libp2p ping + gossipsub example, then add the signature protocol
5. **Check the quinn-udp patch**: Verify the unsafe cast doesn't cause issues with larger message patterns

## Fix: UDP Socket Non-Blocking (`leansim/patch/quinn-udp/src/fallback.rs`)

**Root cause**: The fallback commented out `socket.0.set_nonblocking(true)?;` with `// SKIP`. This kept the UDP socket in blocking mode.

Without `O_NONBLOCK`, quinn's edge-triggered epoll read loop (which reads in a loop until `EAGAIN`) would block on the second `recv_from_vectored` call after consuming all available data. This prevented the tokio event loop from ever returning to the timer wheel, causing `epoll_wait` to hang forever. The process was stuck after 2 `epoll_pwait` calls.

With `set_nonblocking(true)`, `recv_from_vectored` returns `EAGAIN` when no data is available, quinn's read loop terminates correctly, epoll returns, timers fire, and the event loop runs normally.

**What Works Now (4-validator consensus)**:
- All 4 validators produce PeerIds and `SigSent` events
- Signatures propagate via gossipsub mesh
- Local aggregator receives signatures and generates `LocalProofGenerated` (3+ sigs, 75% threshold)
- Global aggregator receives `LocalProofReceived` and completes `GlobalProofCompleted`
- Syscall counts show active event loop: `epoll_pwait:676`, `recvmsg:911`, `sendto:690`

## Remaining Issues

1. **Configuration mismatch**: `run_timeout_secs=60` in experiment config exceeds `stop_time=15s` in Shadow YAML. Processes are killed with `StoppedByShadow`. Either increase stop_time or reduce run_timeout.
2. Processes exit via `.await` timeout rather than clean exit — the main event loop runs until `run_timeout_secs` expires. This is expected behavior but causes `StoppedByShadow` if Shadow's stop_time fires first.

## How to Test

```bash
# Build leansim (includes quinn-udp non-blocking fix)
docker exec shadow-dd-test bash -c 'export PATH="$HOME/.cargo/bin:$PATH" && cd /leansim && cargo build --release'

# Run 4-validator test
docker exec shadow-dd-test bash -c '
  cd /tmp/4-val && rm -rf shadow.data
  timeout 20 /shadow/build/src/main/shadow shadow.yaml 2>&1 | grep -E "SigSent|GlobalProof|LocalProof"
'

# Check per-node events
docker exec shadow-dd-test bash -c '
  for f in /tmp/4-val/shadow.data/hosts/node-*/leansim.1000.stdout; do
    n=$(echo $f | grep -o "node-[0-9]")
    echo -n "$n: "
    grep -oP "SigSent|SigReceived|LocalProofGenerated|LocalProofReceived|GlobalProofCompleted" $f | sort | uniq -c | tr "\n" " "
    echo
  done
'
