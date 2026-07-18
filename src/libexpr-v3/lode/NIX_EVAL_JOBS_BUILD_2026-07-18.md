# Building nix-eval-jobs against the v3 nix (Phase A + B)

**Date:** 2026-07-18
**Branch:** `angerman/2.34-v3` (Phase-A commit sits on this branch, on top of `36bcbda12`)
**Platform verified:** aarch64-darwin (the laptop)
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

This is the executable, deploy-shaped recipe for the first two steps of the
nix-eval-jobs → v3 integration (NIX_EVAL_JOBS_PATCH_2026-07-18.md §3, Phase 1):
make the v3 nix expose `nix-expr-v3` via pkg-config (Phase A), then build the
**unpatched** nix-eval-jobs v2.34.1 against that v3 nix to establish the
tree-walker (TW) baseline oracle (Phase B). v3 is **not** engaged yet — that is
Phase C.

---

## 0. TL;DR (both gates GREEN)

| Gate | Result |
|---|---|
| Phase A: `PKG_CONFIG_PATH=<prefix>/lib/pkgconfig pkg-config --exists nix-expr-v3` | ✅ resolves; `--cflags` = `-std=c++23 -I<prefix>/include`, `--libs` = `-L<prefix>/lib -lnixexprv3` |
| Phase A: in-tree `nix` CLI still builds | ✅ `ninja src/nix/nix` links `@rpath/libnixexprv3.dylib`; `nix --version` → `2.34.6` |
| Phase B (a): unpatched nix-eval-jobs builds against v3 nix + runs | ✅ emits job JSON on the offline smoke flake |
| Phase B (b): `pkg-config --exists nix-expr-v3` in the n-e-j build env | ✅ OK |

**Phase-A change:** one additive block in `src/libexpr-v3/meson.build`
(`import('pkgconfig').generate(this_library, filebase:'nix-expr-v3', …)`),
committed as `fff5be408`.

---

## 1. Phase A — the v3 nix ships `nix-expr-v3.pc`

The v3 library was already `install: true` + `install_headers(…)` + an in-tree
`meson.override_dependency('nix-expr-v3', …)`, but generated no `.pc` — so an
out-of-tree build could not discover it. The fix mirrors the shared
`nix-meson-build-support/export/meson.build` helper's `pkgconfig.generate()`
call (but does not `subdir()` the helper, because that helper also re-issues
`meson.override_dependency`, which would collide with the existing
full-link-surface override the in-tree `nix` CLI depends on).

### Build + install the v3 nix to a prefix

```bash
# from the v3 nix checkout (branch angerman/2.34-v3, with the Phase-A commit)
PREFIX=/path/to/v3-prefix            # any writable dir
nix develop -c meson setup   build --prefix="$PREFIX" -Dbuildtype=debugoptimized
nix develop -c ninja  -C     build
nix develop -c meson install -C     build
```

`-Dbuildtype=debugoptimized` is **required** for anything that exercises eval:
a `-O0` build ~2× the eval wall-time and times out the `--brute` battery
(BACKPORT_2_34 §3.1 flag 1). It does not matter for Phase A/B correctness, but
keep it so the same `build/` is reusable for the brute gate.

### Verify (Phase-A gate)

```bash
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" nix develop -c bash -c '
  pkg-config --exists nix-expr-v3 && echo EXISTS-OK
  pkg-config --cflags nix-expr-v3
  pkg-config --libs   nix-expr-v3'
```

Observed:

```
EXISTS-OK
-std=c++23 -I<prefix>/include
-L<prefix>/lib -lnixexprv3
```

Generated `nix-expr-v3.pc`:

```
Name: Nix expr v3
Description: Nix v3 bytecode VM evaluator (libnixexprv3)
Version: 2.34.6
Requires.private: nix-util, nix-store, nix-expr, nix-fetchers, nix-main, nix-flake, libarchive >= 3.1.2, nlohmann_json >= 3.9, bdw-gc, sqlite3 >= 3.6.19
Libs: -L${libdir} -lnixexprv3
Cflags: -I${includedir} -std=c++23
```

(meson auto-appends the transitive pkg-config `Requires.private` — libarchive /
nlohmann_json / bdw-gc / sqlite3 / nix-fetchers — beyond the five nix-* modules
listed explicitly in `requires_private`. All resolve: the nix-* ones from the
same prefix; the external ones from the build's own deps.)

The in-tree `nix` CLI is unaffected (`dependency('nix-expr-v3')` in
`src/nix/meson.build` still resolves via the retained manual override, not the
`.pc`): `ninja src/nix/nix` links `@rpath/libnixexprv3.dylib`, `nix --version`
prints `2.34.6`.

---

## 2. Phase B — build UNPATCHED nix-eval-jobs against the v3 nix

nix-eval-jobs v2.34.1 (`65ebf5b7cd453a27af09cf02b1fc57b3568cc4b7`) depends on
`nix-store / nix-fetchers / nix-expr / nix-flake / nix-main / nix-cmd` via
pkg-config (`src/meson.build`). It does **not** yet depend on `nix-expr-v3`
(that is the Phase-C worker patch). So the Phase-B build only needs the v3
nix's installed nix-* modules on `PKG_CONFIG_PATH`.

### 2.1 Recipe used + verified here — meson against the installed v3 prefix

This is the fast iteration path and directly proves both Phase-B gates. Point
`PKG_CONFIG_PATH` at the Phase-A prefix (prepended before the dev-shell's own,
so nlohmann_json / libcurl / gtest / bdw-gc / libarchive / sqlite3 still
resolve from the dev shell):

```bash
PREFIX=/path/to/v3-prefix                # the Phase-A install prefix
NEJ=/path/to/nix-eval-jobs               # v2.34.1 checkout
NEJBUILD=/path/to/nej-build

nix develop -c bash -c "
  export PKG_CONFIG_PATH='$PREFIX/lib/pkgconfig':\$PKG_CONFIG_PATH
  # Gate (b): the v3 .pc is consumable by the external build env
  pkg-config --exists nix-expr-v3 && echo 'GATE-B: nix-expr-v3 OK'
  meson setup '$NEJBUILD' '$NEJ' -Dbuildtype=debugoptimized
  ninja -C '$NEJBUILD' src/nix-eval-jobs
"
```

meson resolves every nix-* dep at `2.34.6` from the prefix; the build links
`src/nix-eval-jobs` (only `-Wmissing-designated-field-initializers` warnings
from the nix headers — no errors). `otool -L` confirms the binary links the
prefix dylibs: `libnixstore.2.34.6.dylib`, `libnixexpr.2.34.6.dylib`,
`libnixmain.2.34.6.dylib`, `libnixcmd.2.34.6.dylib`.

### 2.2 Run it — the TW baseline oracle (Phase-B gate a)

A fully self-contained offline flake (no nixpkgs, no network) — `derivation`
is a builtin, so jobs evaluate to real drvPaths:

```nix
# v3-smoke-flake/flake.nix  (git-committed so pure flake eval is clean)
{
  outputs = { self, ... }:
    let
      system = "aarch64-darwin";
      mkJob = name: text: derivation {
        inherit name system;
        builder = "/bin/sh";
        args = [ "-c" "echo '${text}' > $out" ];
      };
    in { hydraJobs = { foo = mkJob "foo" "hello-from-v3-smoke"; bar = mkJob "bar" "second-job"; }; };
}
```

```bash
FLK=/path/to/v3-smoke-flake
nix develop -c bash -c "
  export DYLD_FALLBACK_LIBRARY_PATH='$PREFIX/lib'   # darwin: find the prefix dylibs at runtime
  '$NEJBUILD/src/nix-eval-jobs' --gc-roots-dir /tmp/nej-gcroots --flake '$FLK#hydraJobs'
"
```

Output (one JSON object per job; this is the **TW baseline** — `NIX_V3_DIRECT_EVAL`
unset, so the tree-walker owns eval):

```
bar  drvPath=/nix/store/4fjmdrmpafg0bswq9w598ps3m4xzry4s-bar.drv  out=/nix/store/qmaq6gjg28ni7y8iij4nj94pi6k23vlq-bar
foo  drvPath=/nix/store/jskz9nhqb2q0gprxkb0yk07iglbkwz1n-foo.drv  out=/nix/store/f40v3lvy78yr75cs9ih3bhhdk4zd2wv8-foo
```

Deterministic across re-runs. `--expr '{ baz = derivation { … }; }'` works the
same way (offline). **These drvPaths are the oracle Phase-C shadow-parity
byte-compares the v3 output against.**

### 2.3 Deploy-shaped route — the flake `--override-input` (as infra will do)

The infra (`ops/flake.nix`) overrides nix-eval-jobs' `nix` input to the v3 nix
and `nix build`s. nix-eval-jobs' `nix` input is `flake = false` (a plain
source); its own flake builds `nixComponents` from that source's
`packaging/components.nix`:

```bash
nix build \
  'github:nix-community/nix-eval-jobs/v2.34.1' \
  --override-input nix github:input-output-hk/nix/angerman/2.34-v3
# or with a local checkout:
#   --override-input nix path:/path/to/nix-2.34-v3
```

**Status of this route — conceptually confirmed for the UNPATCHED build, with a
packaging gap that blocks the PATCHED (Phase-C) build:**

- The **unpatched** n-e-j only needs `nix-store / nix-fetchers / nix-expr /
  nix-flake / nix-main / nix-cmd` components. None of those depend on
  libexpr-v3, and the v3 source's `packaging/components.nix` builds all of
  them — so the unpatched deploy build is sound. (It was not run to completion
  here because the n-e-j flake pins `nixpkgs-unstable`, a heavy sandboxed
  rebuild + network; the meson route in §2.1 is the equivalent, faster proof.)

- **GAP for the deploy route once Phase C lands:** `packaging/components.nix`
  has **no `nix-expr-v3` component**, and `src/nix/package.nix`'s `buildInputs`
  do **not** list `nix-expr-v3`. There is no `src/libexpr-v3/package.nix`. So:
  - building the v3 **`nix` CLI** via the flake packaging currently fails at
    meson config (`dependency('nix-expr-v3')` in `src/nix/meson.build` is
    unresolved — it only resolves in the top-level dev-shell meson where
    libexpr-v3 is a *subproject*); and
  - the Phase-C **patched** nix-eval-jobs (which adds `nix-expr-v3` to its
    `buildInputs`) cannot get a `nixComponents.nix-expr-v3`.

  **Fix (Phase-C build prerequisite, ranked #1 for the deploy route):**
  1. add `src/libexpr-v3/package.nix` (a `mkMesonLibrary` mirroring
     `src/libexpr/package.nix`, carrying the call-flake.nix generated header +
     the v3 parser + deps nix-util/nix-store/nix-expr/nix-main/nix-flake +
     toml11 + sqlite);
  2. add `nix-expr-v3 = callPackage ../src/libexpr-v3/package.nix { };` to
     `packaging/components.nix`;
  3. add `nix-expr-v3` to `src/nix/package.nix` and (Phase C) nix-eval-jobs'
     `default.nix` `buildInputs`.

  This is additive Nix packaging (not evaluator work) and is validated by a
  real `nix build` of the v3 `nix` CLI — deferred here to keep Phase A/B
  scoped and because that build is heavy + network-bound.

---

## 3. What is proven vs. what remains

**Proven (this session):**
- The v3 nix exposes `nix-expr-v3` via pkg-config; external `pkg-config
  --exists / --cflags / --libs` resolve to the installed lib + headers.
- The in-tree `nix` CLI is undisturbed.
- Unpatched nix-eval-jobs v2.34.1 **builds and runs** against the v3 nix and
  produces the TW baseline drvPath oracle on an offline jobset.
- `pkg-config --exists nix-expr-v3` succeeds inside the nix-eval-jobs build env
  — the concrete thing the Phase-C patch's `dependency('nix-expr-v3')` needs.

**Remaining (Phase C and its deploy packaging):**
- The Model-B accessor library + worker wiring (the actual patch).
- The `packaging/components.nix` + `src/libexpr-v3/package.nix` wiring so the
  deploy `nix build --override-input` route builds the v3 `nix` CLI and the
  patched nix-eval-jobs (see §2.3 gap).
