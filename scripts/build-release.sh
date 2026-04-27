#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a self-contained qemu-system-arm tarball for the
# clockdomain/qemu@ast10x0-i2c fork. Intended to be invoked from
# .github/workflows/release.yml; runnable manually for debugging.
#
# Inputs (env vars):
#   BUILD_TRIPLE   e.g. "linux-x86_64". Selects the bundle-libs-<os>.sh helper.
#   RELEASE_TAG    e.g. "ast10x0-i2c-2026.04". Recorded in the bundle's
#                  VERSION file; not used in the tarball name.
#
# Output (relative to the repo root):
#   dist/qemu-system-arm-ast10x0-i2c-<BUILD_TRIPLE>.tar.xz
#   dist/qemu-system-arm-ast10x0-i2c-<BUILD_TRIPLE>.tar.xz.sha256

set -euo pipefail

: "${BUILD_TRIPLE:?must be set, e.g. linux-x86_64}"
: "${RELEASE_TAG:?must be set, e.g. ast10x0-i2c-2026.04}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${REPO_ROOT}/dist/staging"
OUTDIR="${REPO_ROOT}/dist"
NAME="qemu-system-arm-ast10x0-i2c"
BUNDLE="${OUTDIR}/${NAME}-${BUILD_TRIPLE}"

# Reproducibility: pin file timestamps to the tip commit's authored time.
# This keeps the tarball sha256 stable across re-runs of the same tag on the
# same runner image.
export SOURCE_DATE_EPOCH="$(git -C "${REPO_ROOT}" log -1 --format=%ct)"

echo ">>> Cleaning previous build artifacts"
rm -rf "${REPO_ROOT}/build" "${OUTDIR}"
mkdir -p "${REPO_ROOT}/build" "${OUTDIR}" "${BUNDLE}/bin" "${BUNDLE}/lib" \
    "${BUNDLE}/share/qemu"

echo ">>> Configuring (target-list=arm-softmmu)"
cd "${REPO_ROOT}/build"
"${REPO_ROOT}/configure" \
    --target-list=arm-softmmu \
    --prefix=/ \
    --disable-werror \
    --disable-docs \
    --disable-guest-agent \
    --disable-gtk \
    --disable-sdl \
    --disable-vnc \
    --disable-curses \
    --disable-spice \
    --disable-bzip2 \
    --disable-lzo \
    --disable-snappy \
    --disable-glusterfs \
    --disable-rbd \
    --disable-libusb \
    --disable-usb-redir \
    --disable-tools \
    --enable-system

echo ">>> Building"
ninja

echo ">>> Installing into staging dir"
DESTDIR="${PREFIX}" ninja install

echo ">>> Assembling release bundle"
cp "${PREFIX}/bin/qemu-system-arm" "${BUNDLE}/bin/"

# Copy only firmware/ROM blobs the ast1030-evb machine actually loads.
# Listed explicitly so we notice when QEMU adds new deps the patched
# machine model picks up. If a future patch references a new blob, add
# it here AND extend the smoke test in release.yml accordingly.
SHARE_BLOBS=(
    efi-virtio.rom
    kvmvapic.bin
    pvh.bin
)
for blob in "${SHARE_BLOBS[@]}"; do
    src="${PREFIX}/share/qemu/${blob}"
    if [ -f "${src}" ]; then
        cp "${src}" "${BUNDLE}/share/qemu/"
    fi
done

# Drop a VERSION marker so consumers can sanity-check what they extracted.
cat > "${BUNDLE}/VERSION" <<EOF
release_tag=${RELEASE_TAG}
upstream_commit=$(git -C "${REPO_ROOT}" rev-parse HEAD)
build_triple=${BUILD_TRIPLE}
build_epoch=${SOURCE_DATE_EPOCH}
EOF

echo ">>> Bundling shared libraries"
"${REPO_ROOT}/scripts/bundle-libs-${BUILD_TRIPLE%%-*}.sh" "${BUNDLE}"

echo ">>> Stripping binary"
strip "${BUNDLE}/bin/qemu-system-arm"

echo ">>> Smoke test (binary launches with bundled libs)"
(cd "${BUNDLE}" && LD_LIBRARY_PATH= ./bin/qemu-system-arm --version)

echo ">>> Creating tarball"
# --sort=name + numeric owner + epoch produce a stable archive.
tar \
    --owner=0 --group=0 --numeric-owner \
    --sort=name \
    --mtime="@${SOURCE_DATE_EPOCH}" \
    -cJf "${OUTDIR}/${NAME}-${BUILD_TRIPLE}.tar.xz" \
    -C "${OUTDIR}" "${NAME}-${BUILD_TRIPLE}"

(cd "${OUTDIR}" && sha256sum "${NAME}-${BUILD_TRIPLE}.tar.xz" \
    > "${NAME}-${BUILD_TRIPLE}.tar.xz.sha256")

echo ">>> Done:"
ls -lh "${OUTDIR}/${NAME}-${BUILD_TRIPLE}.tar.xz" \
       "${OUTDIR}/${NAME}-${BUILD_TRIPLE}.tar.xz.sha256"
cat    "${OUTDIR}/${NAME}-${BUILD_TRIPLE}.tar.xz.sha256"
