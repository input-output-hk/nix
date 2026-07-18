# Wiring nix-expr-v3 into the flake packaging (the `nix build` deploy route)

**Date:** 2026-07-18
**Branch:** `2.34-v3-pkg` (base `54466e916` = `angerman/2.34-v3` tip, which
already ships the Phase-A `nix-expr-v3.pc` meson export)
**Platform verified:** aarch64-darwin (the laptop)
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

This closes the packaging gap named in `NIX_EVAL_JOBS_BUILD_2026-07-18.md`
§2.3: the v3 subsystem builds fine under the top-level dev-shell meson (where
`libexpr-v3` is a *subproject*), but the **flake packaging** did not build
`libexpr-v3` at all — so `nix build` of the v3 `nix` CLI (the deploy route
Hydra/CI uses, and the route the infra takes when it points its `nix` /
`nix-eval-jobs` inputs at the v3 nix flake) failed at meson config.

---

## 0. TL;DR (gate GREEN)

| Gate | Result |
|---|---|
| `nix build .#nix-cli` (the nix CLI, links nix-expr-v3) | ✅ builds |
| `result/bin/nix --version` | ✅ `nix (Nix) 2.34.6` |
| `otool -L result/bin/nix` links libnixexprv3 | ✅ `…/nix-expr-v3-2.34.6/lib/libnixexprv3.dylib` |
| `NIX_V3_DIRECT_EVAL=1 nix eval --impure --expr '1+1'` | ✅ `2` (v3 engaged) |
| `nix build .#nix-expr-v3` → `libnixexprv3.dylib` in lib output | ✅ (6.1 MB) |
| `nix build .#nix-expr-v3^dev` → `nix-expr-v3.pc` in dev output | ✅ (+ 44 `v3/` headers incl. `ast/`) |
| `nix eval .#hydraJobs.listingIsComplete` | ✅ `{}` (no throw) |
| `nix build .#nix` (combined nix-everything, `doCheck=true`) | ⚠️ red — but on **pre-existing** libutil unit-test failures unrelated to v3 (see §3.1) |

The `Failed to increase stack size …` line printed before the version is a
**harmless** warning: `src/nix/meson.build` requests a 64 MiB pthread stack
(`-Wl,-stack_size`) and the ambient rlimit refuses to grow it; the binary
continues normally. Unrelated to this packaging work.

---

## 1. What was missing

`src/nix/meson.build` already declares `dependency('nix-expr-v3')` (the v3 CLI
links libnixexprv3). But the *flake* packaging had no way to supply it:

- **no `nix-expr-v3` component** in `packaging/components.nix`;
- **no `src/libexpr-v3/package.nix`** (every other `src/lib*` has one);
- **`src/nix/package.nix` buildInputs did not list `nix-expr-v3`**.

Result: `nix build .#nix-cli` failed at meson config — `dependency('nix-expr-v3')`
is unresolvable in the granular per-component build (it only resolves in the
top-level dev-shell meson, where libexpr-v3 is a `subproject`, not a pkg-config
module).

A **second, latent** gap surfaced only once the component built: the v3
library's `install_headers` list was a hand-curated subset, so the installed
(dev-output) header set was **not self-contained**. The in-tree build hides
this because `include_directories('include')` exposes every header; but an
out-of-tree consumer that compiles against `-I<dev>/include` hits
`fatal error: 'v3/…' file not found` on any transitively-included header that
was omitted.

---

## 2. What was added

### 2.1 `src/libexpr-v3/package.nix` (NEW)

`mkMesonLibrary` derivation mirroring `src/libexpr/package.nix`:

- `pname = "nix-expr-v3"`, `workDir = ./.`.
- **fileset** — the shared build-support + `.version` + `meson.build`/`meson.options`,
  plus the pieces the monolithic v3 `meson.build` needs at configure that the
  `.cc`/`.hh` globs don't cover:
  - `../libflake/call-flake.nix` — the generated-header input
    (`gen_header.process` of it; lives outside workDir);
  - `parser/v3-parser.y` + `parser/v3-parser.l` — bison/flex grammar inputs;
  - the whole `test/` and `bench/` trees — every inline `test()` block resolves
    its driver via `files(...)`, which meson checks **at configure time** (a
    missing file fails `meson setup`); the suites themselves are **not run**
    here;
  - `fileFilter .cc` / `.hh` over `./.` — so new accessor / opt-pass / primop
    sources (incl. under `cli/`, `parser/`, `include/v3/`) are picked up
    automatically.
- **propagatedBuildInputs** = `nix-util nix-store nix-expr nix-main nix-flake`
  (+ `boost nlohmann_json` and `boehmgc` when `enableGC`) — the meson
  `deps_private_maybe_subproject` set, propagated because the CLI that links
  libnixexprv3 needs the whole chain at its own link time (and the `.pc` lists
  them as `Requires.private`).
- **buildInputs** = `toml11 sqlite` (meson `deps_other`: `fromTOML` + the
  `disk_cache.cc` raw-BLOB sqlite reads).
- **nativeBuildInputs** = `bison flex cmake` (cmake resolves the toml11
  `method:'cmake'` dep).
- **`doCheck = false`** — the v3 `test()` battery (`--brute`, lang, drv/chain
  parity) needs nixpkgs + a built `nix` on PATH and is run by
  `all-v3-tests.sh`, not by the packaging build.
- `meta.platforms = lib.platforms.unix` (v3 is unix-only; see caveats).

### 2.2 `src/libexpr-v3/meson.build` — install the whole `include/v3` tree

Replaced the curated `install_headers(files(...))` with

```meson
install_subdir('include/v3', install_dir : get_option('includedir'))
```

`include/v3/` holds only public `.hh` (44 files incl. `ast/expr.hh`), so this
is correct **and** future-proof — a sibling branch adding a header no longer
re-breaks the out-of-tree build. The three holes it plugged (found by iterative
flake builds of the CLI):

| missing header | pulled by | reached from |
|---|---|---|
| `owned_or_borrowed.hh` | `bytecode.hh` | `run.hh` → `eval.cc` |
| `nursery.hh` | `alloc.hh` | `alloc.hh` → many |
| `heap_trace.hh` | — | included directly by `src/nix/main.cc` |

### 2.3 The wiring (mirror the `nix-expr` entries)

- `packaging/components.nix`: `nix-expr-v3 = callPackage ../src/libexpr-v3/package.nix { };`
- `src/nix/package.nix`: function arg + `buildInputs += nix-expr-v3`.
- `packaging/everything.nix`: function arg + `libs += nix-expr-v3` +
  `meta.pkgConfigModules += "nix-expr-v3"` — so `nix-expr-v3.pc` and the dev
  headers flow into the **combined** `nix` dev output (via the `lndir` of each
  lib's dev), and its `.pc` is discoverable there.
- `packaging/hydra.nix`: add `"nix-expr-v3"` to the `forAllPackages'` genAttrs
  list. **Required**: without it, the `listingIsComplete` check throws
  `nix-expr-v3: missing?` (it diffs the hardcoded list against every `nix-*`
  attr in `nixComponents2`, which now includes `nix-expr-v3`).
- `flake.nix`: add `"nix-expr-v3" = { }` to the `flatMapAttrs` component
  enumeration (exposes `packages.<system>.nix-expr-v3`; keeps the list
  consistent).

---

## 3. Validated invocations

```bash
# on the 2.34-v3-pkg checkout, aarch64-darwin
nix build .#nix-expr-v3 .#nix-expr-v3^dev   # the v3 library component
#   -> result/lib/libnixexprv3.dylib
#   -> result-1-dev/lib/pkgconfig/nix-expr-v3.pc + include/v3/*.hh (44, incl ast/)

nix build .#nix-cli                          # the nix CLI (the real deploy gate)
result/bin/nix --version                     # nix (Nix) 2.34.6
otool -L result/bin/nix | grep nixexprv3     # links .../libnixexprv3.dylib
NIX_V3_DIRECT_EVAL=1 result/bin/nix eval --impure --expr '1+1'   # -> 2

nix eval .#hydraJobs.listingIsComplete       # {}  (component listing consistent)
```

`.#nix-cli` is the correct attr for the nix CLI package and is the essential
deploy gate — it is what links libnixexprv3, and it is fully GREEN. `.#nix`
(= `nix-everything`) is the *combined* package; building it additionally runs
the full unit + functional test suite (`doCheck = true` there, `checkInputs` =
all `*-tests.run` + `nix-functional-tests`), which is orthogonal to v3
packaging and much heavier.

### 3.1 `.#nix` (combined) is red on a PRE-EXISTING, non-v3 test failure

`nix build .#nix` fails at the `checkInputs` gate on
`nix-util-tests.tests.run`: **26 gtest failures in libutil** (NarTest/
InvalidNarTest, HashJSON, BLAKE3HashJSON, MemorySourceAccessorJSON,
NarListingJSON `from_json`/`to_json` golden cases) on this aarch64-darwin
environment. This is **not** caused by — and not related to — the v3
packaging:

- `nix-util-tests` is the **lowest layer** of the dependency graph; its
  derivation (`…-nix-util-tests-run.drv`) input closure contains **no
  reference to nix-expr-v3** (verified with `nix-store --query --requisites`);
- my changes never touch libutil, so that drvPath is **byte-identical** to
  what base `54466e916` produces — i.e. these unit tests fail on the base
  branch too, independent of this work.

So `.#nix` would be red on the base branch on this host regardless. Per the
project testing rule I did **not** alter those tests. The combined `nix` dev
output's aggregation of `nix-expr-v3.pc` is wired via `everything.nix` `libs`
(evaluated clean); the `.pc`/dylib/header presence is proven directly on the
`nix-expr-v3` component outputs and the fully-green `.#nix-cli` build. The
libutil unit-test breakage is a separate, pre-existing item for whoever owns
the 2.34 darwin test environment.

---

## 4. Caveats — is the darwin build representative of the x86_64-linux Hydra target?

**Mostly yes, with two honest gaps:**

1. **Platform.** This gate ran on **aarch64-darwin**; Hydra's primary target is
   **x86_64-linux**. The packaging is platform-neutral Nix (fileset + deps),
   and the only darwin-specific thing in play is the `otool -L` check
   (linux: `ldd` / `patchelf --print-needed`) and the harmless
   `-Wl,-stack_size` warning. libexpr-v3 already builds on x86_64-linux under
   the dev-shell meson, so the same components should build under the flake
   there. **Not yet run on linux in this session** — it should be confirmed on
   an x86_64-linux builder (or in Hydra CI) before calling the deploy route
   100% green on that target.

2. **Windows cross.** `nix-expr-v3` is declared `meta.platforms = unix`, but
   `src/nix/meson.build` links `dependency('nix-expr-v3')` unconditionally, and
   `nix-cli` claims `unix ++ windows`. The Hydra `buildCross`
   `x86_64-w64-mingw32` job will now try to cross-build nix-expr-v3 and likely
   fail (v3's moving-GC substrate + the darwin/win stack flags are untried on
   Windows). This is a **pre-existing tension** introduced by the backport
   adding the unconditional `dependency('nix-expr-v3')` to the CLI, not by this
   packaging change; it does not affect the unix (linux/darwin) deploy route.
   If the mingw cross job must stay green, either gate the CLI's v3 dep on
   `!windows` or port v3 to Windows — out of scope here.

3. **GC option.** Unlike `nix-expr`, v3 has **no meson `gc` option** (the
   generational moving GC is SHIPPED + default-on, per
   `src/libexpr-v3/CLAUDE.md` constraint 0), so `package.nix` only conditionally
   *propagates* boehmgc; it never passes a `-Dgc=` flag. The Hydra `buildNoGc`
   job (`nix-expr.override { enableGC = false }`) does not disable v3's GC —
   another reason v3 is not part of the no-GC configuration.

4. **`v3_release`.** The build uses the meson default `v3_release=false` (dev
   instrumentation compiled in but env-gated-off). A production deploy that
   wants the instrumentation *stripped* would pass `-Dv3_release=true`; left at
   the default here to match the in-tree dev/brute build behavior.

---

## 5. Files changed

| File | Change |
|---|---|
| `src/libexpr-v3/package.nix` | **NEW** — `mkMesonLibrary` for `nix-expr-v3` |
| `src/libexpr-v3/meson.build` | `install_headers(files…)` → `install_subdir('include/v3', …)` |
| `packaging/components.nix` | `nix-expr-v3 = callPackage …` |
| `src/nix/package.nix` | function arg + `buildInputs += nix-expr-v3` |
| `packaging/everything.nix` | arg + `libs += nix-expr-v3` + `pkgConfigModules += "nix-expr-v3"` |
| `packaging/hydra.nix` | `forAllPackages'` list += `"nix-expr-v3"` (listingIsComplete) |
| `flake.nix` | component enumeration += `"nix-expr-v3"` |

Committed on `2.34-v3-pkg` as two Rule-0 commits:
`libexpr-v3: install the whole include/v3 tree, not a curated subset` and
`packaging: build nix-expr-v3 via the flake (deploy route)`.
