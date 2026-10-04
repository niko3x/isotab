#!/usr/bin/env bash
# Run as a regular user inside a disposable container, never the real HOME.
set -euo pipefail
app=${1:-/usr/bin/isotab}
"$app" --version
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -m700 "$fixture/home"
set +e
HOME="$fixture/home" XDG_CONFIG_HOME="$fixture/config" XDG_CACHE_HOME="$fixture/cache" NO_AT_BRIDGE=1 \
    timeout 8s xvfb-run -a "$app" > "$fixture/log" 2>&1
status=$?
set -e
cat "$fixture/log"
test "$status" = 124
test -f "$fixture/home/.isotab/launcher.lock"
! grep -Ei 'error while loading|symbol lookup error|segmentation|failed to load|CRITICAL' "$fixture/log"
echo 'Packaged application started with a fresh temporary profile store.'
