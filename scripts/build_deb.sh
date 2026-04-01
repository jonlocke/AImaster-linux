#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/build_deb.sh [--version VERSION] [--skip-build]

Builds a Debian package for AImaster and includes a systemd service that runs
under a dedicated `aimaster` system user.
USAGE
}

VERSION=""
SKIP_BUILD=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      VERSION="${2:-}"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ -z "$VERSION" ]]; then
  VERSION="$(git describe --tags --always --dirty 2>/dev/null || date +%Y.%m.%d)"
  VERSION="${VERSION#v}"
  VERSION="${VERSION//-/.}"
fi
ARCH="$(dpkg --print-architecture)"
PKG_NAME="aimaster"
BUILD_ROOT="$ROOT_DIR/dist/deb"
PKG_ROOT="$BUILD_ROOT/${PKG_NAME}_${VERSION}_${ARCH}"
DEBIAN_DIR="$PKG_ROOT/DEBIAN"

rm -rf "$PKG_ROOT"
mkdir -p "$DEBIAN_DIR" \
         "$PKG_ROOT/usr/lib/aimaster" \
         "$PKG_ROOT/usr/lib/systemd/system" \
         "$PKG_ROOT/usr/share/aimaster" \
         "$PKG_ROOT/usr/share/doc/aimaster"

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  make clean
  make
fi

install -m 0755 "$ROOT_DIR/AImaster" "$PKG_ROOT/usr/lib/aimaster/AImaster"
install -m 0644 "$ROOT_DIR/packaging/systemd/aimaster.service" "$PKG_ROOT/usr/lib/systemd/system/aimaster.service"
install -m 0755 "$ROOT_DIR/packaging/systemd/aimaster-service.sh" "$PKG_ROOT/usr/lib/aimaster/aimaster-service.sh"
install -m 0644 "$ROOT_DIR/config-example.txt" "$PKG_ROOT/usr/share/aimaster/config-example.txt"
install -m 0644 "$ROOT_DIR/cmds.csv" "$PKG_ROOT/usr/share/aimaster/cmds.csv"
install -m 0644 "$ROOT_DIR/assets/welcome.txt" "$PKG_ROOT/usr/share/aimaster/welcome.txt"
install -m 0644 "$ROOT_DIR/assets/user_prompt.txt" "$PKG_ROOT/usr/share/aimaster/user_prompt.txt"
install -m 0644 "$ROOT_DIR/README.md" "$PKG_ROOT/usr/share/doc/aimaster/README.md"
install -m 0644 "$ROOT_DIR/LICENSE" "$PKG_ROOT/usr/share/doc/aimaster/LICENSE"
install -m 0755 "$ROOT_DIR/packaging/debian/postinst" "$DEBIAN_DIR/postinst"
install -m 0755 "$ROOT_DIR/packaging/debian/prerm" "$DEBIAN_DIR/prerm"
install -m 0755 "$ROOT_DIR/packaging/debian/postrm" "$DEBIAN_DIR/postrm"

cat > "$DEBIAN_DIR/control" <<CONTROL
Package: $PKG_NAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: AImaster Packaging <noreply@example.com>
Depends: adduser, systemd, util-linux
Description: AImaster CLI with systemd service support
 AImaster is an interactive CLI assistant with provider-compatible chat,
 optional local text-to-speech, and serial/RAG integrations.
CONTROL

find "$PKG_ROOT" -type d -exec chmod 0755 {} +

dpkg-deb --root-owner-group --build "$PKG_ROOT"
echo "Built package: ${PKG_ROOT}.deb"
