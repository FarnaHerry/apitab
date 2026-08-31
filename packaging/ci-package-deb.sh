#!/usr/bin/env bash
# ci-package-deb.sh — build a .deb from the assembled dist/ tree (ci-package.sh 产物)。
# 布局：/opt/apitab（apitab + assets + engines），/usr/bin/apitab 包装器，桌面入口。
# 用法: ci-package-deb.sh <version> <arch> <dist-dir>   （在仓库根运行）
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?usage: ci-package-deb.sh <version> <arch> <dist-dir>}"
arch="${2:?usage: ci-package-deb.sh <version> <arch> <dist-dir>}"
dist="${3:?usage: ci-package-deb.sh <version> <arch> <dist-dir>}"

test -f "$dist/apitab"
test -d "$dist/assets"
test -x "$dist/engines/k6"

# deb 的 Architecture 命名与 CI 矩阵名不同
case "$arch" in
    x86_64) debarch=amd64 ;;
    arm64)  debarch=arm64 ;;
    *) debarch="$arch" ;;
esac

pkg="apitab_${version}_${debarch}"
stage="$root/dist-deb/$pkg"
rm -rf "$stage"
mkdir -p "$stage/opt/apitab" "$stage/usr/bin" "$stage/usr/share/applications" \
         "$stage/usr/share/icons/hicolor/256x256/apps" "$stage/DEBIAN"

cp -r "$dist/apitab" "$stage/opt/apitab/apitab"
chmod +x "$stage/opt/apitab/apitab"
cp -r "$dist/assets" "$stage/opt/apitab/assets"
cp -r "$dist/engines" "$stage/opt/apitab/engines"

cp "$root/packaging/apitab-wrapper.sh" "$stage/usr/bin/apitab"
chmod +x "$stage/usr/bin/apitab"

cp "$root/packaging/apitab.desktop" "$stage/usr/share/applications/apitab.desktop"
if [ -f "$stage/opt/apitab/assets/icon.png" ]; then
    cp "$stage/opt/apitab/assets/icon.png" "$stage/usr/share/icons/hicolor/256x256/apps/apitab.png"
fi

cat > "$stage/DEBIAN/control" <<EOF
Package: apitab
Version: ${version}
Section: devel
Priority: optional
Architecture: ${debarch}
Maintainer: FarnaHerry <farnaherry@users.noreply.github.com>
Depends: libx11-6, libxrandr2, libxcursor1, libxinerama1, libxi6, libgl1
Description: API testing and load testing tool
 Single-request API debugging, WebSocket/TCP clients and k6 load
 testing with SQLite-backed collections and history.
Installed-Size: $(du -sk "$stage/opt" | cut -f1)
EOF

out="$root/apitab-v${version}-linux-${arch}.deb"
rm -f "$out"
dpkg-deb --root-owner-group --build "$stage" "$out"
rm -rf "$root/dist-deb"
echo "produced: $out"
ls -lh "$out"
