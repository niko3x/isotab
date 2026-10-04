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
mv AppDir/AppRun AppDir/AppRun.hooks
cp "$OLDPWD/packaging/AppRun" AppDir/AppRun
# GTK3 supports Wayland; do not force every user onto XWayland.
sed -i '/^export GDK_BACKEND=x11 /d' AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh
export OUTPUT="$out/IsoTab-$version-x86_64.AppImage" VERSION="$version"
./linuxdeploy.AppImage --appimage-extract >/dev/null
# Pin the runtime as well as the bundler, and provide its source and license.
curl -fL --retry 3 https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 -o runtime-x86_64
printf '%s  %s\n' 156f4bdbde9c52d01814600013e0a273f0118dc2de98975f3c8c63427ec79074 runtime-x86_64 | sha256sum -c
# Collect the exact distro source packages and notices for bundled libraries.
mkdir -p sources notices
for file in $(find AppDir/usr/lib -type f -name '*.so*' -printf '%f\n' | sort -u); do
    dpkg-query -S "*/$file" 2>/dev/null || true
done > owners.txt
dpkg-query -S '/usr/share/glib-2.0/schemas/*' >> owners.txt 2>/dev/null || true
sed 's/: \/.*//' owners.txt | sort -u > packages.txt
while IFS= read -r package; do
    source=$(dpkg-query -W -f='${source:Package}=${source:Version}' "$package")
    printf '%s\n' "$source"
    cp -L "/usr/share/doc/${package%%:*}/copyright" "notices/${package//:/_}.copyright"
done < packages.txt | sort -u > source-packages.txt
(cd sources; while IFS= read -r package; do apt-get source --download-only "$package"; done < ../source-packages.txt)
curl -fL --retry 3 https://github.com/AppImage/type2-runtime/archive/8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa.tar.gz -o sources/appimage-runtime.tar.gz
curl -fL --retry 3 https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz -o sources/fuse-3.15.0.tar.xz
curl -fL --retry 3 https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz -o sources/squashfuse-0.5.2.tar.gz
printf '%s  %s\n' 70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0 sources/fuse-3.15.0.tar.xz db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3 sources/squashfuse-0.5.2.tar.gz | sha256sum -c
curl -fL --retry 3 https://raw.githubusercontent.com/AppImage/type2-runtime/8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa/LICENSE -o notices/appimage-runtime.LICENSE
cp source-packages.txt sources/
cp -a notices sources/
tar -czf "$out/IsoTab-$version-bundled-library-sources.tar.gz" sources
mkdir -p AppDir/usr/share/doc/isotab
cp -a notices AppDir/usr/share/doc/isotab/bundled-licenses
# Invoke appimagetool directly to preserve the custom AppRun and fixed runtime.
ARCH=x86_64 squashfs-root/plugins/linuxdeploy-plugin-appimage/usr/bin/appimagetool \
    --runtime-file "$work/runtime-x86_64" "$work/AppDir" "$OUTPUT"
chmod +x "$OUTPUT"
