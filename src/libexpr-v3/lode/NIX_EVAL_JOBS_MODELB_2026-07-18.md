# nix-eval-jobs → v3 (Model B): what landed

**Date:** 2026-07-18
**Branch (v3 nix):** `2.34-v3-nej` (base `54466e916`)
**nix-eval-jobs:** v2.34.1 (`65ebf5b7cd453a27af09cf02b1fc57b3568cc4b7`)
**Platform:** aarch64-darwin
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

Implements **Model B** from `NIX_EVAL_JOBS_PATCH_2026-07-18.md`: a small
v3-native accessor library in the v3 nix + worker wiring in nix-eval-jobs, so
the Hydra evaluator drives the v3 bytecode VM for the *heavy* flake descent
while **all store-side logic stays byte-for-byte unchanged**.

---

## 0. Gates (all green)

| Gate | Result |
|---|---|
| C1: v3 lib `--brute` after adding the accessors | **41/41 ALL GREEN** |
| C1: lint-no-direct-tw-include | clean (accessors route through `v3/ffi.hh`, no direct TW include) |
| C3: `NIX_V3_REQUIRE=1 NIX_V3_DIRECT_EVAL=1 nix-eval-jobs --flake …` forces v3, no fallback | ✅ 15× "v3-direct engaged", exit 0 |
| C3: `NIX_V3_REQUIRE=1` on a non-flake hard-fails | ✅ exit 1, "v3-direct did not engage" |
| D: shadow parity, core fields (drvPath/name/system/outputs) | **byte-identical, 5/5 jobs** |
| D: shadow parity, `--meta` | **byte-identical, 5/5 jobs** |
| D: gate-OFF fallback == TW oracle | **byte-identical, 5/5 jobs** |

---

## 1. What landed

### C1 — v3 accessor library (commit `dbbb0abd8` on `2.34-v3-nej`)
New files in the v3 nix, wired into `libnixexprv3` + `install_headers` +
`nix-expr-v3.pc`:
- `src/libexpr-v3/include/v3/eval_jobs_api.hh`
- `src/libexpr-v3/eval_jobs_api.cc`

`namespace nix::v3` API (opaque `EvalJobsHandle`, owned via `EvalJobsHandlePtr`):

| accessor | mirrors (tree-walker) |
|---|---|
| `evalFlakeRoot(state, lockedRef, fragment, selectExpr)` | `initializeRootValue` / `evaluateFlake` — runs `(builtins.getFlake "<lockedRef>")` via `runRootExprFromString`, descends the fragment, applies `--select` |
| `descendAttrPath(h, path)` | `findAlongAttrPath` |
| `isAttrs(h)` | `value->type() == nAttrs` |
| `isDerivation(h)` | `getDerivation` / `EvalState::isDerivation` (`type == "derivation"`) |
| `childAttrNames(h, recurse)` | `collectAttrsForRecursion` (lexicographic names + `recurseForDerivations`) |
| `drvPath(h)` | `PackageInfo::requireDrvPath` |
| `name(h)` | `PackageInfo::queryName` |
| `system(h)` | `PackageInfo::querySystem` |
| `outputs(h)` | `PackageInfo::queryOutputs(true)` (name → `.${o}.outPath`) |
| `meta(h)` | `queryMeta` (JSON `null` when no serialisable meta, never `nullopt`) |

**Moving-GC safety.** The embedder never holds a raw `v3::Value`. The handle
owns two `v3::GcRoot`-registered slots (flake root + current job value),
forwarded across every scavenge; accessors read those slots and only ever pass
a `Value*` into a by-value local *before* a force that could relocate the
parent `Bindings`. This is the load-bearing difference from a naive
"return a Value" API.

**V3-NATIVE.** `EvalState`/`nix::Error` are routed through `v3/ffi.hh` exactly
like `run.cc`; no direct TW include — `lint-no-direct-tw-include` stays clean.

### C2 — nix-eval-jobs worker patch (commit `6cf76b2` in the n-e-j clone)
Patch file: **`src/libexpr-v3/lode/nix-eval-jobs-modelB-v2.34.1.patch`**
(portable to the deploy fork; `git am` on top of v2.34.1). Touches 5 files:
- `meson.build` — `dependency('nix-expr-v3')` + `cpp_std=c++23` (match v3 headers)
- `src/meson.build` — add `nix_expr_v3_dep` to `common_deps`
- `src/worker.cc` — the `NIX_V3_DIRECT_EVAL` gate, `evaluateFlakeV3`
  (locks the flake on TW to get the rev-pinned ref, hands it to v3 getFlake),
  `processDerivationV3`, `setFlakeSettings` wiring
- `src/drv.cc` / `src/drv.hh` — `Drv::fromV3` (value-side via accessors; the
  store-side block is byte-for-byte identical to `fromPackageInfo`)

The gate mirrors the `nix` CLI: `NIX_V3_DIRECT_EVAL` engages v3 for flake
jobsets (unset → the unchanged tree-walker path); `NIX_V3_REQUIRE` hard-fails
if v3 doesn't engage (CI proof of no silent fallback).

---

## 2. Shadow-parity numbers

Offline, self-contained, git-committed flake (`nej-parity-flake`, no nixpkgs /
network) — a nested `derivation {…}` tree exercising discovery, detection,
extraction, recursion (`recurseForDerivations = true` **and** absent), and
non-derivation attrs to ignore. Expected job set: `foo, bar, nested.baz,
nested.qux, deep.inner.deepjob` — `hidden.shouldBeIgnored` (no
`recurseForDerivations`) and `someString` (scalar) correctly excluded by **both**
paths.

Run: unpatched **TW oracle** (pristine v2.34.1 build) vs patched **v3**
(`NIX_V3_REQUIRE=1 NIX_V3_DIRECT_EVAL=1`), same locked flake. Full per-job JSON
(attr, drvPath, name, system, outputs, [meta]) normalised (`jq -cS` + sort) and
`diff`ed → **identical**.

Sample drvPaths (identical TW == v3):

```
foo                 /nix/store/1q54v18bj15w5q3qqh31jh303mcpj7bw-foo.drv
nested.baz          /nix/store/g4ws17cl4rfaqqw07xljv0x4vwbmx88z-baz.drv
deep.inner.deepjob  /nix/store/m23jbypsyy6y76i777n76i573lzidq08-deepjob.drv
```

`foo` carries a `.meta` attr (`{ broken=false; description="the foo job";
priority=5; }`) — under `--meta` both paths emit the identical `meta` object,
and both emit `"meta":null` for the meta-less jobs (the `queryMeta` semantics
`meta()` mirrors).

**nixpkgs stretch check:** not run. The offline tree already exercises the full
discovery/detection/extraction/recursion surface with real drvPaths; a
nixpkgs-backed jobset adds evaluator scale but no new *worker* code path, and
the drvPath-parity of v3 getFlake-through-a-flake is already retired
(`NIX_EVAL_JOBS_PATCH_2026-07-18.md` §3). Noted honestly as the one deferred
check.

---

## 3. Known gaps

- **`--constituents` / `--apply`** operate on a tree-walker `nix::Value`
  (`coerceToString` / `callFunction`) and are **not** wired on the v3 path; a v3
  job carries no constituents / extraValue. Guarded in `processDerivationV3`.
- **Only flake jobsets** use v3 (`--expr` / `--flake=false` fall through to TW).
  `NIX_V3_REQUIRE` hard-fails on a non-flake, as intended.
- **`autoCallFunction`** is skipped on the v3 path (flake `autoArgs` is empty
  and jobs are attrsets, not defaulted-arg functions). A job that is a
  defaulted-arg *function* would be ignored by v3 where TW would auto-call it.
- **CA derivations:** `outputs()` reads `.${o}.outPath` (the value side); the
  TW `queryOutputs` CA-fallback (`queryOutputs(false)`) is not mirrored. The
  offline test + typical input-addressed jobs are unaffected.
- **Deploy packaging gap** (from the build recipe, unchanged): the flake
  `--override-input` route needs `src/libexpr-v3/package.nix` +
  `nix-expr-v3` in `packaging/components.nix` before the patched n-e-j can be
  built via `nix build`. The meson + `PKG_CONFIG_PATH` route (below) is the
  proven build.

---

## 4. Reproduction (meson + PKG_CONFIG_PATH — the fast/proven route)

```bash
PREFIX=<v3-prefix>          # v3 nix installed with nix-expr-v3.pc (Phase A)
NEJ=<nix-eval-jobs v2.34.1 checkout, with the .patch applied>
NEJBUILD=<build dir>
FLK=<offline nej-parity-flake, git-committed>

# build the patched n-e-j against the v3 nix
nix develop -c bash -c "
  export PKG_CONFIG_PATH='$PREFIX/lib/pkgconfig':\$PKG_CONFIG_PATH
  meson setup '$NEJBUILD' '$NEJ' -Dbuildtype=debugoptimized
  ninja  -C  '$NEJBUILD' src/nix-eval-jobs"

# force the v3 path (no fallback)
export DYLD_FALLBACK_LIBRARY_PATH="$PREFIX/lib"
NIX_V3_REQUIRE=1 NIX_V3_DIRECT_EVAL=1 \
  "$NEJBUILD/src/nix-eval-jobs" --gc-roots-dir /tmp/gcr --flake "$FLK#hydraJobs"
```
