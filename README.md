# The Shadow Simulator (ARM64 Port)

This is an **ARM64 Linux port** of [Shadow](https://github.com/shadow/shadow), the discrete-event network simulator. The original project only supports x86-64 Linux. This port enables Shadow to run natively on ARM64 platforms — Apple Silicon (via Docker, Lima, OrbStack), AWS Graviton, Raspberry Pi, and other ARM64 hardware.

The port was done by [kamilsa](https://github.com/kamilsa/leansim) and lives in the `arm64-port` branch. It is used by [LeanSim](https://github.com/kamilsa/leansim), a lightweight simulation framework targeting an ARM64 Docker environment for local development on Apple Silicon Macs.

## Port Status

**236 tests total, 224 passed (95%), 12 failed.**

### Failed Tests

| Test | Issue |
|---|---|
| `clone_leader_exits_early-shadow` | Timeout |
| `clone_leader_exits_early_nomm-shadow` | Timeout |
| `clone-shadow` | Timeout |
| `determinism1-compare-shadow` | Failed |
| `bash-example-shadow` | Failed |
| `intercept_golang_time-shadow` | Failed |
| `pipe-shadow` | Failed |
| `socket-shadow` | Failed |
| `socket-new-tcp-shadow` | Failed |
| `stat-shadow` | Failed |
| `time-shadow` | Failed |
| `shadowtools-tests` | Failed |

Example apps that pass: `curl`, `http-server`, `wget2`, `iperf-2`, `jetty`.

Known limitations:
- **etcd**: Go runtime crash under Shadow (known LD_PRELOAD limitation for statically-linked Go binaries)
- **nginx**: SIGSEGV during HTTP request processing under Shadow on ARM64 (works without Shadow)

## Building and Testing

After installing the [dependencies](https://shadow.github.io/docs/guide/install_dependencies.html): build, test, and install Shadow into `~/.local`:

```
$ ./setup build --clean --test
$ ./setup test
$ ./setup install
```

Read the [usage guide](https://shadow.github.io/docs/guide) or get started with some [example simulations](https://shadow.github.io/docs/guide/getting_started_basic.html).

<!--- ANCHOR: body (for mdbook) -->

## What is Shadow?

Shadow is a discrete-event network simulator that directly executes real
application code, enabling you to simulate distributed systems with thousands of
network-connected processes in **realistic** and **scalable** private network
experiments using your laptop, desktop, or server running Linux.

Shadow experiments can be scientifically **controlled** and deterministically
**replicated**, making it easier for you to reproduce bugs and eliminate
confounding factors in your experiments.

## How Does Shadow Work?

Shadow directly executes **real applications**:

- Shadow directly executes unmodified, real application code using native OS
  (Linux) processes.
- Shadow co-opts the native processes into a discrete-event simulation by
  interposing at the system call API.
- The necessary system calls are emulated such that the applications need not
  be aware that they are running in a Shadow simulation.

Shadow connects the applications in a **simulated network**:

- Shadow constructs a private, virtual network through which the managed
  processes can communicate.
- Shadow internally implements simulated versions of common network protocols
  (e.g., TCP and UDP).
- Shadow internally models network routing characteristics (e.g., path latency
  and packet loss) using a configurable network graph.

## Why is Shadow Needed?

Network emulators (e.g., [mininet](http://mininet.org)) run real application
code on top of real OS kernels in real time, but are non-determinsitic and have
limited scalability: time distortion can occur if emulated processes exceed an
unknown computational threshold, leading to undefined behavior.

Network simulators (e.g., [ns-3](https://www.nsnam.org)) offer more experimental
control and scalability, but have limited application-layer realism because they
run application abstractions in place of real application code.

Shadow offers a novel, hybrid emulation/simulation architecture: it directly
executes real applications as native OS processes in order to faithfully
reproduce application-layer behavior while also co-opting the processes into a
high-performance network simulation that can scale to large distributed systems
with hundreds of thousands of processes.

## Caveats

Shadow implements **over 150 functions from the system call API**, but does not
yet fully support all API features. Although applications that make _basic_ use
of the supported system calls should work out of the box, those that use more
_complex_ features or functions may not yet function correctly when running in
Shadow. Extending support for the API is a work-in-progress.

That being said, we are particularly motivated to run large-scale [Tor
Network](https://www.torproject.org) simulations. This use-case is already
fairly well-supported and we are eager to continue extending support for it.

## More Information

Homepage:
- <https://shadow.github.io>

Documentation:
- [User documentation](https://shadow.github.io/docs/guide)
- [Developer documentation](https://shadow.github.io/docs/rust)

Community Support:
- <https://github.com/shadow/shadow/discussions>

Bug Reports:
- <https://github.com/shadow/shadow/issues>

<!--- ANCHOR_END: body (for mdbook) -->
