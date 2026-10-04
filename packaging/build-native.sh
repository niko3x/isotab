#!/usr/bin/env bash
# Build on Ubuntu 22.04, never on the developer's rolling-release host.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
version=$(cat VERSION)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
test "$(uname -m)" = x86_64
mkdir -p dist
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
make clean
make
make test
NO_AT_BRIDGE=1 xvfb-run -a make ui-test
make install PREFIX=/usr DESTDIR="$stage"
strip "$stage/usr/bin/isotab"
mkdir -p "$stage/DEBIAN"
# Resolve actual ELF requirements instead of guessing libc symbol versions.
mkdir -p "$stage/shlibs/debian"
printf 'Source: isotab\nSection: utils\nPriority: optional\nMaintainer: niko3x <193813187+niko3x@users.noreply.github.com>\nStandards-Version: 4.6.0\n\nPackage: isotab\nArchitecture: amd64\nDescription: Browser session manager\n' > "$stage/shlibs/debian/control"
deps=$(cd "$stage/shlibs"; dpkg-shlibdeps -O -e"$stage/usr/bin/isotab" | sed -n 's/^shlibs:Depends=//p')
rm -rf "$stage/shlibs"
cat > "$stage/DEBIAN/control" <<CONTROL
Package: isotab
Version: $version
Architecture: amd64
Maintainer: niko3x <193813187+niko3x@users.noreply.github.com>
Section: web
Priority: optional
Depends: $deps, librsvg2-common
Homepage: https://github.com/niko3x/isotab
Installed-Size: $(du -sk "$stage/usr" | cut -f1)
Description: Manage separate browser profiles
 A native GTK3 session manager for installed browsers.
CONTROL
# No scripts touch HOME or profile data, including on uninstall.
dpkg-deb --root-owner-group --build "$stage" "dist/isotab_${version}_amd64.deb"
rm -rf "$stage/DEBIAN"
# A pacman package containing the same older-baseline ELF avoids a fresh
# Arch glibc requirement on Manjaro. Rebuild from source using the PKGBUILD.
cat > "$stage/.PKGINFO" <<PKGINFO
pkgname = isotab
pkgbase = isotab
pkgver = $version-1
pkgdesc = Manage separate browser profiles
url = https://github.com/niko3x/isotab
builddate = $(date +%s)
packager = niko3x
size = $(du -sb "$stage/usr" | cut -f1)
arch = x86_64
license = GPL-3.0-only
depend = glibc>=2.35
depend = glib2>=2.72
depend = gtk3>=3.24
depend = librsvg
PKGINFO
bsdtar --uid 0 --gid 0 --zstd -cf "dist/isotab-${version}-1-x86_64.pkg.tar.zst" -C "$stage" .PKGINFO usr
