FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make git ca-certificates curl pkg-config \
    pulseaudio pulseaudio-utils libpulse-dev \
    && rm -rf /var/lib/apt/lists/*

RUN curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal
ENV PATH=/root/.cargo/bin:$PATH

COPY entry.sh /entry.sh
RUN chmod +x /entry.sh
ENTRYPOINT ["/entry.sh"]
