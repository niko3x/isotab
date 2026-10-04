#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
version=$(cat VERSION)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
top=$(mktemp -d)
trap 'rm -rf -- "$top"' EXIT
mkdir -p "$top"/{SOURCES,SPECS,BUILD,RPMS,SRPMS} dist
git archive --prefix="isotab-$version/" HEAD | gzip -n > "$top/SOURCES/isotab-$version.tar.gz"
rpmbuild -ba --define "_topdir $top" --define "isotab_version $version" packaging/isotab.spec
cp "$top"/RPMS/x86_64/isotab-[0-9]*.rpm "$top"/SRPMS/*.rpm dist/
