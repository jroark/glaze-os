#!/bin/sh
# Build Glaze inside the mips-cross-dev container (be300/docker-compose.yml).
set -e
cd "$(dirname "$0")"
exec docker run --rm -v "$PWD":/work -w /work mips-cross-dev:latest make -j8 "$@"
