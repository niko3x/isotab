#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
# A container checks actual old headers/libraries, but shares the host kernel.
# Keep the source mount read-only; all build outputs stay inside the container.
docker run --rm -v "$PWD:/source:ro" debian:bullseye bash -euxc '
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y gcc make pkg-config libgtk-3-dev libglib2.0-dev librsvg2-common xvfb xauth
    useradd -m builder
    mkdir /build
    cp -a /source/. /build/
    chown -R builder:builder /build
    runuser -u builder -- bash -euxc '\''
        cd /build
        make clean
        gcc --version
        test "$(pkg-config --modversion glib-2.0 | cut -d. -f1-2)" = 2.66
        pkg-config --modversion gtk+-3.0 glib-2.0
        make
        make test
        NO_AT_BRIDGE=1 xvfb-run -a make ui-test
    '\''
'
