#!/bin/sh
# Run the contract test suite against the pulse-rust backend in Docker.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
docker build -t cubeb-contract-pulse "$DIR/test/docker-pulse"
docker run --rm -v "$DIR":/cubeb cubeb-contract-pulse
