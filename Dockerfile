# syntax=docker/dockerfile:1
# Shadow network simulator — multi-stage build
# https://shadow.github.io
#
# Build:     docker build -t shadow .
# Push:      docker tag shadow youruser/shadow && docker push youruser/shadow
# Run:       docker run -v /path/to/config:/config -w /config shadow shadow.yaml

FROM ubuntu:24.04 AS builder

LABEL org.opencontainers.image.source="https://github.com/shadow/shadow"
LABEL org.opencontainers.image.description="Shadow (builder stage)"

# Install build dependencies
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    build-essential cmake pkg-config \
    libglib2.0-dev libigraph-dev libpcap-dev \
    libclang-dev llvm-dev curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install Rust (pinned to match container build environment)
RUN curl --proto =https --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain 1.95.0 && \
    /root/.cargo/bin/rustup default 1.95.0
ENV PATH="/root/.cargo/bin:${PATH}" \
    CARGO_HOME="/root/.cargo" \
    RUSTUP_HOME="/root/.rustup"

# Copy source (build/ is excluded by .dockerignore)
COPY . /shadow
WORKDIR /shadow

# Build Shadow
RUN echo "Building Shadow release..." && \
    ./setup build --clean 2>&1 && \
    cp build/src/main/shadow /usr/local/bin/shadow && \
    mkdir -p /usr/local/lib/shadow && \
    cp build/src/lib/preload-injector/libshadow_injector.so /usr/local/lib/shadow/ && \
    cp build/src/lib/preload-libc/libshadow_libc.so /usr/local/lib/shadow/ && \
    cp build/src/lib/preload-openssl/libshadow_openssl_rng.so /usr/local/lib/shadow/ && \
    cp build/src/lib/preload-openssl/libshadow_openssl_crypto.so /usr/local/lib/shadow/ && \
    cp build/src/shim/target/release/libshadow_shim.so /usr/local/lib/shadow/ && \
    echo "Build complete"

# ---------- Runtime image ----------
FROM ubuntu:24.04

LABEL org.opencontainers.image.source="https://github.com/shadow/shadow"
LABEL org.opencontainers.image.description="Shadow discrete-event network simulator"
LABEL org.opencontainers.image.version="3.3.0"

# Runtime libraries only
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    libglib2.0-0t64 libigraph3t64 libpcap0.8t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/bin/shadow /usr/local/bin/shadow
COPY --from=builder /usr/local/lib/shadow/ /usr/local/lib/shadow/

# Shadow looks for its preload libraries via built-in rpath.
# Create symlinks matching the build-time paths for all libraries.
RUN mkdir -p /shadow/build/src/lib/preload-injector \
             /shadow/build/src/lib/preload-libc \
             /shadow/build/src/lib/preload-openssl \
             /shadow/build/src/shim/target/release && \
    ln -sf /usr/local/lib/shadow/libshadow_injector.so /shadow/build/src/lib/preload-injector/ && \
    ln -sf /usr/local/lib/shadow/libshadow_libc.so /shadow/build/src/lib/preload-libc/ && \
    ln -sf /usr/local/lib/shadow/libshadow_openssl_rng.so /shadow/build/src/lib/preload-openssl/ && \
    ln -sf /usr/local/lib/shadow/libshadow_openssl_crypto.so /shadow/build/src/lib/preload-openssl/ && \
    ln -sf /usr/local/lib/shadow/libshadow_shim.so /shadow/build/src/shim/target/release/

RUN shadow --help >/dev/null 2>&1 && echo "Shadow binary OK"

ENTRYPOINT ["shadow"]
CMD ["--help"]
