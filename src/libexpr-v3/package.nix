# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0.
#
# Flake-packaging derivation for the v3 bytecode-VM evaluator library
# (`nix-expr-v3`, libnixexprv3).  This mirrors `src/libexpr/package.nix`
# (libexpr-v3 is a sibling library that *wraps* nix-expr) and is what makes
# the component discoverable to `packaging/components.nix` — i.e. buildable
# via `nix build`, the deploy route Hydra/CI uses.
{
  lib,
  stdenv,
  mkMesonLibrary,

  bison,
  flex,
  cmake, # for resolving toml11 dep (dependency(..., method : 'cmake'))

  nix-util,
  nix-store,
  nix-expr,
  nix-main,
  nix-flake,
  boost,
  boehmgc,
  nlohmann_json,
  toml11,
  sqlite,

  # Configuration Options

  version,

  # v3 always runs on the generational moving GC (nursery + Phase-D
  # write-barriers + gen-major collection are SHIPPED and DEFAULT-ON — see
  # src/libexpr-v3/CLAUDE.md constraint 0).  There is no no-GC v3 build, so
  # unlike nix-expr there is no meson `gc` option to toggle; this flag only
  # governs whether boehmgc is propagated, and drops it on Windows where v3
  # is not built.
  enableGC ? !stdenv.hostPlatform.isWindows,
}:

let
  inherit (lib) fileset;
in

mkMesonLibrary (finalAttrs: {
  pname = "nix-expr-v3";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    ../../.version
    ./.version
    ./meson.build
    ./meson.options

    # #698: the v3 evaluator compiles + runs libflake's call-flake.nix
    # natively via a generated header (meson.build gen_header.process of
    # '../libflake/call-flake.nix').  That source lives OUTSIDE workDir, so it
    # must be pulled in explicitly or `meson setup` fails to find the input.
    ../libflake/call-flake.nix

    # v3-native parser: bison/flex grammar inputs.  These are `.y`/`.l`, so the
    # `.cc`/`.hh` globs below do not cover them.
    ./parser/v3-parser.y
    ./parser/v3-parser.l

    # The monolithic meson.build declares every v3 test() inline, and each
    # references its driver script/fixture via `files(...)`, which meson
    # resolves at CONFIGURE time (a missing file fails `meson setup`).  Ship
    # the whole test/ + bench/ trees so configuration succeeds.  The suites
    # themselves are NOT executed by the packaging build (doCheck = false);
    # they are the `all-v3-tests.sh --brute` battery, run separately.
    ./test
    ./bench

    # Source globs — new accessor / opt-pass / primop `.cc`/`.hh` files (e.g.
    # the Model-B accessor sources added on a sibling branch) are picked up
    # automatically, including those under cli/, parser/, and include/v3/.
    (fileset.fileFilter (file: file.hasExt "cc") ./.)
    (fileset.fileFilter (file: file.hasExt "hh") ./.)
  ];

  nativeBuildInputs = [
    bison
    flex
    cmake
  ];

  # deps_other in meson.build: not consumed by any installed header, only in
  # `.cc` (toml11 for builtins.fromTOML; sqlite3 for disk_cache.cc raw-BLOB
  # column reads).
  buildInputs = [
    toml11
    sqlite
  ];

  # deps_private_maybe_subproject in meson.build (nix-util / nix-store /
  # nix-expr / nix-main / nix-flake) plus the header deps libexpr propagates
  # (boost / nlohmann_json / boehmgc).  Propagated because the nix CLI that
  # links libnixexprv3 needs the whole chain at its own link time, and because
  # the installed nix-expr-v3.pc lists them as Requires.private.
  propagatedBuildInputs = [
    nix-util
    nix-store
    nix-expr
    nix-main
    nix-flake
    boost
    nlohmann_json
  ]
  ++ lib.optional enableGC boehmgc;

  # The v3 test() blocks (the --brute battery, lang tests, drv/chain parity
  # harnesses) require nixpkgs + a built `nix` on PATH and would hang the
  # sandbox.  They are run by `all-v3-tests.sh`, NOT by the packaging build,
  # which only needs meson to CONFIGURE them (files() resolution, above).
  doCheck = false;
  doInstallCheck = false;

  meta = {
    # v3 is unix-only: the v3-eval stack-size link flag and the moving-GC
    # substrate are not exercised on Windows.  (The nix CLI still lists
    # windows in its own platforms; a Windows v3 build is a separate, untried
    # port — see the packaging doc's caveats.)
    platforms = lib.platforms.unix;
  };

})
