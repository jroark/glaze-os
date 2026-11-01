#!/bin/sh
# Build Glaze inside the mips-cross-dev container (be300/docker-compose.yml).
set -e
cd "$(dirname "$0")"
# git isn't usable inside the container, so describe the build here.
build_id=$(git describe --always --dirty 2>/dev/null || echo dev)
# Sibling app repositories (their out/pkg feeds `make store`) appear read-only
# at /<name>, so ../<name> resolves from /work as it does on the host.
siblings=
for repo in glaze-emulators glaze-apps; do
    [ -d "../$repo" ] && siblings="$siblings -v $(cd "../$repo" && pwd):/$repo:ro"
done
exec docker run --rm -v "$PWD":/work $siblings -w /work mips-cross-dev:latest make -j8 BUILD_ID="$build_id" "$@"
