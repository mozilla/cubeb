FROM rust:1-trixie

RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake libpulse-dev pkg-config pulseaudio \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# Initialize the test, build, and Rust PulseAudio submodules used on Linux.
RUN git submodule update --init --recursive -- \
        cmake/sanitizers-cmake googletest src/cubeb-pulse-rs \
    && cargo fetch --manifest-path src/cubeb-pulse-rs/Cargo.toml

ENV CARGO_NET_OFFLINE=true
RUN --network=none cmake -S . -B build -DBUILD_RUST_LIBS=ON \
    && cmake --build build --parallel "$(nproc)" \
    && .oss-scanner/test.sh
