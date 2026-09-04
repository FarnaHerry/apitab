#!/usr/bin/env bash
# ci-package-rpm.sh — build a .rpm from the assembled dist/ tree (ci-package.sh 产物)。
# 布局与 deb 一致：/opt/apitab + /usr/bin/apitab 包装器 + 桌面入口。
# 库依赖由 rpmbuild 的 find-requires 从 ELF NEEDED 自动生成。
# 用法: ci-package-rpm.sh <version> <arch> <dist-dir>   （在仓库根运行；需 rpmbuild）
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?usage: ci-package-rpm.sh <version> <arch> <dist-dir>}"
arch="${2:?usage: ci-package-rpm.sh <version> <arch> <dist-dir>}"
dist="${3:?usage: ci-package-rpm.sh <version> <arch> <dist-dir>}"

command -v rpmbuild >/dev/null || { echo "rpmbuild not found (apt-get install rpm)" >&2; exit 1; }
test -f "$dist/apitab"
test -d "$dist/assets"
test -x "$dist/engines/k6"

case "$arch" in
    x86_64) rpmarch=x86_64 ;;
    arm64)  rpmarch=aarch64 ;;
    *) rpmarch="$arch" ;;
esac

# 允许相对（相对仓库根）或绝对路径
case "$dist" in
    /*) ;;
    *)  dist="$root/$dist" ;;
esac

top="$root/dist-rpm"
rm -rf "$top"
mkdir -p "$top"/{BUILD,RPMS,SPECS}

cat > "$top/SPECS/apitab.spec" <<EOF
Name:           apitab
Version:        ${version}
Release:        1
Summary:        API testing and load testing tool
License:        MIT
URL:            https://github.com/FarnaHerry/apitab
AutoReqProv:    yes
BuildArch:      ${rpmarch}

%description
Single-request API debugging, WebSocket/TCP clients and k6 load
testing with SQLite-backed collections and history.

%install
mkdir -p %{buildroot}/opt/apitab %{buildroot}/usr/bin %{buildroot}/usr/share/applications %{buildroot}/usr/share/icons/hicolor/256x256/apps
cp -r %{distdir}/. %{buildroot}/opt/apitab/
test ! -d %{distdir}/lib || cp -r %{distdir}/lib %{buildroot}/opt/apitab/lib
test ! -d %{distdir}/apitab.resources || cp -r %{distdir}/apitab.resources %{buildroot}/opt/apitab/apitab.resources
install -m 0755 %{srcdir}/apitab-wrapper.sh %{buildroot}/usr/bin/apitab
install -m 0644 %{srcdir}/apitab.desktop %{buildroot}/usr/share/applications/apitab.desktop
if [ -f %{buildroot}/opt/apitab/assets/icon.png ]; then
    install -m 0644 %{buildroot}/opt/apitab/assets/icon.png %{buildroot}/usr/share/icons/hicolor/256x256/apps/apitab.png
fi

%files
/opt/apitab
/usr/bin/apitab
/usr/share/applications/apitab.desktop
/usr/share/icons/hicolor/256x256/apps/apitab.png

%changelog
* $(LC_ALL=C date '+%a %b %d %Y') FarnaHerry <farnaherry@users.noreply.github.com> - ${version}-1
- First packaged release.
EOF

rpmbuild -bb \
    --define "_topdir $top" \
    --define "_arch ${rpmarch}" \
    --define "distdir $dist" \
    --define "srcdir $root/packaging" \
    "$top/SPECS/apitab.spec"

out="$root/apitab-v${version}-linux-${arch}.rpm"
rm -f "$out"
mv "$top/RPMS/${rpmarch}/apitab-${version}-1.${rpmarch}.rpm" "$out"
rm -rf "$top"
echo "produced: $out"
ls -lh "$out"
