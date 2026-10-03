#!/usr/bin/env bash
# IsoTab - dependency install + build script
# Supports: Debian/Ubuntu · Arch · Fedora/RHEL

set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
BOLD="\033[1m"; GREEN="\033[32m"; RED="\033[31m"; RESET="\033[0m"

info()  { echo -e "${BOLD}> $*${RESET}"; }
ok()    { echo -e "${GREEN}OK: $*${RESET}"; }
err()   { echo -e "${RED}Error: $*${RESET}" >&2; exit 1; }

# ── detect distro ────────────────────────────────────────
if   command -v apt-get &>/dev/null; then DISTRO=debian
elif command -v pacman  &>/dev/null; then DISTRO=arch
elif command -v dnf     &>/dev/null; then DISTRO=fedora
else err "Unsupported distro. Install GTK3 development tools manually."; fi

info "Detected distro family: $DISTRO"

# ── install deps ─────────────────────────────────────────
case $DISTRO in
  debian)
    info "Installing dependencies (apt)…"
    sudo apt-get update -qq
    sudo apt-get install -y libgtk-3-dev libglib2.0-dev librsvg2-common pkg-config gcc make
    ;;
  arch)
    info "Installing dependencies (pacman)…"
    sudo pacman -S --needed --noconfirm gtk3 librsvg pkgconf gcc make
    ;;
  fedora)
    info "Installing dependencies (dnf)…"
    sudo dnf install -y gtk3-devel glib2-devel librsvg2 pkgconf gcc make
    ;;
esac
ok "Dependencies installed"

# ── build ─────────────────────────────────────────────────
info "Building IsoTab…"
make clean 2>/dev/null || true
make
ok "Build complete > run with:  ./isotab"

echo ""
echo -e "${BOLD}How it works:${RESET}"
echo "  Choose an installed browser for each session."
echo "  Sessions keep separate cookies, logins, history, and cache."
echo ""
echo -e "${BOLD}Shortcuts:${RESET}"
echo "  Alt+1…0   Quick-launch session 1–10"
echo "  Ctrl+Q    Quit launcher"
echo ""
echo -e "${BOLD}Session data stored in:${RESET}  ~/.isotab/"
