# Release process — `ast10x0-i2c` patched QEMU

This fork carries patches that add ASPEED AST10x0 / AST1030 I²C device
emulation on top of upstream QEMU. Releases produce a self-contained
`qemu-system-arm` tarball that downstream Bazel-based consumers
(notably `clockdomain/bundle` under `--config=virt_ast10x0_i2c`) pull
via `http_archive`, pinned by sha256.

## Tag pattern

```
ast10x0-i2c-YYYY.MM[.PATCH]
```

Examples:

- `ast10x0-i2c-2026.04`
- `ast10x0-i2c-2026.04.1` (hotfix on top of the .04 cut)

The fork does not have a real semver story — it's "snapshot of upstream
QEMU + the i2c patches at date X" — so the tag carries the date rather
than a version.

## What pushing a tag does

The [release workflow](.github/workflows/release.yml) fires on any tag
matching `ast10x0-i2c-*` and runs the matrix in
[`scripts/build-release.sh`](scripts/build-release.sh):

| OS                 | Tarball                                                  |
|--------------------|----------------------------------------------------------|
| Ubuntu 22.04 amd64 | `qemu-system-arm-ast10x0-i2c-linux-x86_64.tar.xz`        |

(Other platforms can be added by extending the matrix; see "Adding a
platform" below.)

Each tarball is accompanied by a `.sha256` sidecar. Both are attached
to a GitHub Release named after the tag, with auto-generated release
notes.

## Manual / dry-run release

To produce a tarball without cutting an actual release (e.g. to verify
the build before tagging), use the manual dispatch:

```
gh workflow run release.yml -f tag=ast10x0-i2c-test01
```

Workflow artifacts are kept for 14 days; nothing is attached to a
release. You can download via `gh run download <run-id>`.

## Tarball layout

```
qemu-system-arm-ast10x0-i2c-<triple>/
├── VERSION                        # release_tag, upstream_commit, build_triple
├── bin/
│   └── qemu-system-arm            # the patched binary; RPATH=$ORIGIN/../lib
├── lib/                           # bundled non-system .so files
│   └── libglib-2.0.so.0, libpixman-1.so.0, …
└── share/qemu/                    # only the firmware blobs ast1030-evb loads
    └── …
```

The binary is configured with `--target-list=arm-softmmu` and most
optional features disabled — it is *not* a full QEMU distribution. It
is intentionally minimal so consumers download a few MB rather than
~20 MB of unused ROMs.

## Reproducibility

`SOURCE_DATE_EPOCH` is pinned to the tagged commit's authored time.
Tar input is sorted by name with numeric `0/0` ownership and a fixed
mtime. Two builds at the same commit on the same runner image should
produce the same sha256.

Bit-perfect cross-machine reproducibility is **not** a goal — different
glibc / linker versions will produce different binaries. If you need
that, build inside a pinned Docker image and use `BUILD_TRIPLE` keyed
to the image rather than the host OS.

## Consumer side

The bundle's [MODULE.bazel](https://github.com/clockdomain/bundle/blob/main/MODULE.bazel)
declares an `http_archive` keyed to a release tag and the corresponding
sha256. Bumping the QEMU version in consumers is a two-line change:
update `urls` and `sha256`. See the bundle's [plan-i2c-emu.md](https://github.com/clockdomain/bundle/blob/main/plan-i2c-emu.md)
for the consumer-side wiring.

## Smoke test

The release workflow runs three checks on every tarball before
publishing:

1. `tar -tJf` (verifies archive is well-formed).
2. `bin/qemu-system-arm --version` (verifies the bundled libs resolve).
3. `bin/qemu-system-arm -machine help | grep -Eiq 'ast103?0'` (verifies
   the patched machine model is registered — a guard against the patch
   set silently regressing or the machine getting renamed).

If you want a deeper smoke test (boot a tiny ELF and verify semihosting
output), drop a prebuilt ELF into `tests/release-smoke/` and extend the
"Smoke test" step in `release.yml` accordingly. Recommended once the
fork ships any non-trivial patch beyond machine registration.

## Adding a platform

To extend the matrix to e.g. `linux-aarch64` or `darwin-arm64`:

1. Add a `bundle-libs-<os>.sh` helper if the new triple uses a different
   loader (macOS needs `install_name_tool` instead of `patchelf`).
2. Add a matrix entry to `release.yml`'s `strategy.matrix.include`,
   pinning the runner image:
   ```yaml
   - os: macos-14
     triple: darwin-arm64
   ```
3. Make sure `Install build deps` handles the new OS (brew packages
   instead of apt).

The asset naming convention is `qemu-system-arm-ast10x0-i2c-<triple>.tar.xz`
where `<triple>` is one of the matrix entries.

## Bumping upstream QEMU

When rebasing the i2c patches on a newer upstream QEMU:

1. Rebase locally, resolve any conflicts in `hw/i2c/aspeed_i2c.c` etc.
2. Manually run `BUILD_TRIPLE=linux-x86_64 RELEASE_TAG=ast10x0-i2c-debug
   ./scripts/build-release.sh` to confirm the build still works.
3. Push the rebased branch, run a dry-run via `gh workflow run release.yml`.
4. Once green, tag and publish.
5. Notify consumers that the next release tag will require a sha256 bump.
