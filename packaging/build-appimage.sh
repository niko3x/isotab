#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
version=$(cat VERSION)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p dist
out=$PWD/dist
make install PREFIX=/usr DESTDIR="$work/AppDir"
cd "$work"
# Fixed digest fails closed if the upstream rolling asset changes.
curl -fL --retry 3 https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage -o linuxdeploy.AppImage
printf '%s  %s\n' 8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1 linuxdeploy.AppImage | sha256sum -c
curl -fL --retry 3 https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/7a3fbc31a9e5075073ff8790f26effbac5f84453/linuxdeploy-plugin-gtk.sh -o linuxdeploy-plugin-gtk.sh
chmod +x linuxdeploy.AppImage linuxdeploy-plugin-gtk.sh
export APPIMAGE_EXTRACT_AND_RUN=1 DEPLOY_GTK_VERSION=3
./linuxdeploy.AppImage --appdir AppDir --plugin gtk
# Retain the generated launcher/hook runner, putting the host snapshot first.
mv AppDir/AppRun AppDir/AppRun.wrapped
cp "$OLDPWD/packaging/AppRun" AppDir/AppRun
# GTK3 supports Wayland; do not force every user onto XWayland.
sed -i '/^export GDK_BACKEND=x11 /d' AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh
export OUTPUT="$out/IsoTab-$version-x86_64.AppImage" VERSION="$version"
# Do not run linuxdeploy again here: it may replace our AppRun.
./linuxdeploy.AppImage --appimage-extract >/dev/null
find squashfs-root -name '*appimage*' -maxdepth 4 -type f
plugin=$(find squashfs-root -type f -name 'linuxdeploy-plugin-appimage*' | head -1)
test -n "$plugin"
"$plugin" --appdir "$work/AppDir"
# Collect the exact distro source packages and notices for bundled libraries.
mkdir -p sources notices
for file in $(find AppDir/usr/lib -type f -name '*.so*' -printf '%f\n' | sort -u); do
    dpkg-query -S "*/$file" 2>/dev/null || true
done | sed 's/: \/.*//' | sort -u > packages.txt
while IFS= read -r package; do
    source=$(dpkg-query -W -f='${source:Package}=${source:Version}' "$package")
    printf '%s\n' "$source"
    cp -a "/usr/share/doc/${package%%:*}/copyright" "notices/${package//:/_}.copyright" 2>/dev/null || true
done < packages.txt | sort -u > source-packages.txt
(cd sources; while IFS= read -r package; do apt-get source --download-only "$package"; done < ../source-packages.txt)
cp source-packages.txt sources/
cp -a notices sources/
tar -czf "$out/IsoTab-$version-bundled-library-sources.tar.gz" sources
chmod +x "$OUTPUT"
