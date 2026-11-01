#!/bin/sh
# Build Glaze inside the mips-cross-dev container (be300/docker-compose.yml).
set -e
cd "$(dirname "$0")"
# git isn't usable inside the container, so describe the build here.
build_id=$(git describe --always --dirty 2>/dev/null || echo dev)
exec docker run --rm -v "$PWD":/work -w /work mips-cross-dev:latest make -j8 BUILD_ID="$build_id" "$@"
