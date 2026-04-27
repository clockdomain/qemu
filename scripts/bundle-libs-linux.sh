#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Walk ldd output for ${BUNDLE}/bin/qemu-system-arm, copy non-system .so
# files into ${BUNDLE}/lib/, and rewrite the binary's RPATH to
# $ORIGIN/../lib so it resolves them at runtime without LD_LIBRARY_PATH.

set -euo pipefail

BUNDLE="${1:?usage: bundle-libs-linux.sh <bundle-dir>}"
BIN="${BUNDLE}/bin/qemu-system-arm"

if ! command -v patchelf >/dev/null 2>&1; then
    echo "patchelf not found; install via 'apt install patchelf'." >&2
    exit 1
fi

# System libs we leave to the consumer's loader. These have stable enough
# ABIs across distros that bundling them invites trouble (libc symbol
# version mismatches, etc.) without buying portability.
SYSTEM_LIBS_REGEX='^(linux-vdso|libc|libm|libdl|libpthread|librt|libresolv|libnsl|ld-linux)'

mkdir -p "${BUNDLE}/lib"

# `ldd` output lines look like:
#   libglib-2.0.so.0 => /lib/x86_64-linux-gnu/libglib-2.0.so.0 (0x00007f...)
ldd "${BIN}" | awk '/=>/ {print $1, $3}' | while read -r soname src; do
    [ -z "${src:-}" ] && continue
    if echo "${soname}" | grep -qE "${SYSTEM_LIBS_REGEX}"; then
        continue
    fi
    if [ -f "${src}" ]; then
        cp -L "${src}" "${BUNDLE}/lib/${soname}"
    fi
done

patchelf --set-rpath '$ORIGIN/../lib' "${BIN}"

# Sanity check: the binary must launch with no LD_LIBRARY_PATH override.
# Failure here means we forgot to bundle a transitively required .so.
(
    cd "${BUNDLE}"
    LD_LIBRARY_PATH= ./bin/qemu-system-arm --version >/dev/null
)

echo "Bundled $(ls "${BUNDLE}/lib" | wc -l) .so files into ${BUNDLE}/lib/"
