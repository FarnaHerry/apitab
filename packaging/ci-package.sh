#!/usr/bin/env bash
# ci-package.sh — assemble a Unix release from an already-built executable.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
os="${1:?usage: ci-package.sh <linux|macos> <arch> <exe> <k6>}"
arch="${2:?usage: ci-package.sh <os> <arch> <exe> <k6>}"
exe="${3:?usage: ci-package.sh <os> <arch> <exe> <k6>}"
k6="${4:?usage: ci-package.sh <os> <arch> <exe> <k6>}"
version="$(grep -m1 -E '^project\(apitab +VERSION ' "$root/CMakeLists.txt" | sed -E 's/.*VERSION +([0-9.]+).*/\1/')"
[[ "$os" == linux || "$os" == macos ]] || { echo "unsupported OS: $os" >&2; exit 1; }
test -x "$exe"
test -x "$k6"
test -d "$root/assets"

dist="$root/dist"
rm -rf "$dist"
mkdir -p "$dist/engines"
cp "$exe" "$dist/apitab"
chmod +x "$dist/apitab"
cp -r "$root/assets" "$dist/assets"
cp "$k6" "$dist/engines/k6"
chmod +x "$dist/engines/k6"

if [[ "$os" == linux ]]; then
    cat > "$dist/run.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export INTEL_FORCE_PROBE="${INTEL_FORCE_PROBE:-1}"
exec ./apitab "$@"
EOF
    chmod +x "$dist/run.sh"
fi

out="$root/apitab-v$version-$os-$arch.tar.gz"
rm -f "$out"
tar -C "$root" -czf "$out" dist
echo "produced: $out"
ls -lh "$out"
