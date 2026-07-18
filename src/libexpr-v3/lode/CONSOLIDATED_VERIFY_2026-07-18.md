# Consolidated v3 CI-integration tree — verification + deploy-route proof

**Date:** 2026-07-18
**Branch:** `angerman/2.34-v3` (tip `151aacca9` = merge of `2.34-v3-pkg`
flake-packaging + `2.34-v3-nej` Model-B accessors/patch)
**Platform:** aarch64-darwin (the laptop)
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

This validates the CONSOLIDATED tree (both the flake packaging AND the Model-B
accessors + the portable nej patch on one branch) and proves the **`nix build`
deploy-route** build of the patched nix-eval-jobs against the v3 nix.

---

## 0. Gates (all green)

| Gate | Result |
|---|---|
| **T1** — consolidated `--brute` (full 41-suite battery, moving-GC stress) | **41/41 ALL GREEN** on `151aacca9` |
| **T2** — DEPLOY route: `nix build` of patched nej with `nix` input overridden to the v3 tree | **BUILT** — `result/bin/nix-eval-jobs` links the flake-built v3 nix (`nix-expr-v3` component) |
| **T3** — shadow parity on the consolidated tree (v3 vs pristine TW oracle) | **byte-identical, 5/5 jobs** (core + `--meta`); v3 engaged 15×, no fallback |

---

## 1. T1 — consolidated `--brute`

Built the v3 nix optimized and ran the full pre-merge battery:

```bash
nix develop -c meson setup build -Dbuildtype=debugoptimized
nix develop -c ninja -C build src/nix/nix src/libexpr-v3/v3-eval \
  src/libexpr-v3/v3-smoke src/libexpr-v3/v3-drv-preflight \
  src/libexpr-v3/v3-gc-root-handles src/libexpr-v3/v3-evalscope-handles
nix develop -c bash src/libexpr-v3/test/all-v3-tests.sh --brute
```

Result:

```
==================== summary ====================
  mode:   brute
  total:  41
  pass:   41
  fail:   0
ALL GREEN
```

**Gotcha (recorded for the next runner):** the `--brute` battery's first four
suites (`smoke`, `drv-preflight`, `evalscope`, `gc-root-handles`) run compiled
test binaries (`v3-smoke`, `v3-drv-preflight`, `v3-gc-root-handles`,
`v3-evalscope-handles`) that are **separate meson targets** — building only
`src/nix/nix` + `src/libexpr-v3/v3-eval` leaves them missing and the battery
reports `37/41` with those four failing on "No such file or directory". They
must be built before the gate. With all six targets built, the accessors +
packaging merge is regression-free: **41/41**.

---

## 2. T2 — the `nix build` deploy route (the exact infra route)

The infra (`ops/flake.nix`) overrides nix-eval-jobs' `nix` input to the v3 nix
and `nix build`s. Now that the flake packaging exists (`src/libexpr-v3/package.nix`
+ `nix-expr-v3` in `packaging/components.nix`), the deploy route converges.

### 2.1 The copy-pasteable deploy artifact

```bash
# patched nix-eval-jobs v2.34.1 (git am of the portable patch), nix input -> v3 nix
nix build 'path:/path/to/nix-eval-jobs#nix-eval-jobs' \
  --override-input nix 'git+file:///path/to/nix?ref=refs/heads/angerman/2.34-v3&rev=151aacca9934f817761a7c4dfbfc348ac3a59882'
# for the deploy fork the override is the usual github form, e.g.
#   --override-input nix github:input-output-hk/nix/angerman/2.34-v3
```

The nej `nix` input is `flake = false` (a plain source); nej's own flake builds
`nixComponents` from that source's `packaging/components.nix`. The override
makes those components the **v3** nix's — including the new `nix-expr-v3`
component, which the patched nej's `dependency('nix-expr-v3')` +
`buildInputs += nixComponents.nix-expr-v3` resolve against.

### 2.2 Eval-phase proof (the packaging closes the §2.3 gap)

With the override, the deploy flake evaluates to a build plan that **includes
the v3 component and the patched nej**:

```
these 12 derivations will be built:
  …-nix-util-2.34.6.drv  …-nix-store-2.34.6.drv  …-nix-expr-2.34.6.drv
  …-nix-flake-2.34.6.drv …-nix-main-2.34.6.drv   …-nix-cmd-2.34.6.drv
  …-nix-expr-v3-2.34.6.drv         <-- the v3 library, built by the flake packaging
  …-nix-eval-jobs-2.34.1.drv       <-- the patched nej, depends on nix-expr-v3
```

Before the packaging work (`NIX_EVAL_JOBS_BUILD_2026-07-18.md` §2.3) this eval
failed: `packaging/components.nix` had no `nix-expr-v3`, so nej's
`dependency('nix-expr-v3')` was unresolvable in the granular component build.
The merge's `package.nix` + `components.nix` wiring is what makes the plan
resolve.

### 2.3 Build outcome — GREEN

The build ran to completion (exit 0). It fetched 177 cached paths (330 MiB) and
built the 12 derivations from source in the sandbox — `nix-util`, `nix-store`,
`nix-fetchers`, `nix-expr`, `nix-flake`, `nix-main`, `nix-cmd`, **`nix-expr-v3`**
(via the flake's `src/libexpr-v3/package.nix`), `nix-util-c`,
`nix-util-test-support`, `curl`, and the patched **`nix-eval-jobs`** (its
`doCheck = true` gtest unit tests passed). Result:

```
result -> /nix/store/…-nix-eval-jobs-2.34.1
result/bin/nix-eval-jobs   (788 KB)
```

`otool -L result/bin/nix-eval-jobs` confirms it links the **flake-built** v3 nix:

```
…-nix-store-2.34.6/lib/libnixstore.2.34.6.dylib
…-nix-expr-2.34.6/lib/libnixexpr.2.34.6.dylib
…-nix-main-2.34.6/lib/libnixmain.2.34.6.dylib
…-nix-cmd-2.34.6/lib/libnixcmd.2.34.6.dylib
…-nix-expr-v3-2.34.6/lib/libnixexprv3.dylib     <-- the v3 library, deploy-route built
```

End-to-end engagement check on the deploy binary (not the meson-route one):

```bash
NIX_V3_REQUIRE=1 NIX_V3_DIRECT_EVAL=1 \
  result/bin/nix-eval-jobs --gc-roots-dir /tmp/gcr --flake <parity-flake>#hydraJobs
#   -> exit 0, 5 jobs, "v3-direct engaged" ×15, no fallback
#   -> drvPaths byte-identical to the meson-route v3 output AND the TW oracle
```

So the **exact infra route** (`nix build` with `nix` overridden to the v3 tree)
now builds the patched nix-eval-jobs against the flake-packaged v3 nix and the
resulting binary drives the v3 bytecode VM.

### 2.4 The one nej-side tweak (folded into the portable patch)

The `nix build` deploy route needs nej's `default.nix` to add the v3 component
to `buildInputs` (so `nixComponents.nix-expr-v3` flows into the derivation):

```nix
buildInputs = with pkgs; [
  …
  nixComponents.nix-expr
  nixComponents.nix-expr-v3   # <-- added: the meson dependency('nix-expr-v3')
  nixComponents.nix-flake
  …
];
```

This is now **folded into the committed portable patch**
(`src/libexpr-v3/lode/nix-eval-jobs-modelB-v2.34.1.patch`, 6 files:
`meson.build`, `src/meson.build`, `src/drv.{cc,hh}`, `src/worker.cc`, **and now
`default.nix`**). The patch still `git am`-applies clean on pristine
NixOS/nix-eval-jobs v2.34.1 (`65ebf5b7cd453a27af09cf02b1fc57b3568cc4b7`) —
verified by `git am` on a fresh checkout.

---

## 3. T3 — shadow parity on the consolidated tree

Built the patched nej (Model B) **and** an independent pristine unpatched nej
against the freshly-installed consolidated v3 prefix (meson route,
`PKG_CONFIG_PATH=<prefix>/lib/pkgconfig`), and byte-compared the v3 path against
the tree-walker oracle on the offline nested jobset (`nej-parity-flake`, rev
`8b989cb`, `system=aarch64-darwin`, 5 jobs).

```
# jobs  oracle=5  v3=5  fallback=5
'v3-direct engaged' lines: 15        (no "falling back" line — v3 owned every job)
DIFF v3(gate-ON) vs pristine TW oracle  [drvPath/name/system/outputs] :  IDENTICAL
DIFF v3(gate-ON) vs patched gate-OFF fallback                          :  IDENTICAL
DIFF v3(gate-ON,--meta) vs oracle(--meta) [full JSON incl meta]        :  IDENTICAL
```

Engagement: `NIX_V3_REQUIRE=1 NIX_V3_DIRECT_EVAL=1` forced v3 with **no silent
fallback** (15 "v3-direct engaged" lines, one per worker; zero fallback lines,
exit 0).

Sample drvPaths (v3 gate-ON == pristine TW oracle == the drvPaths recorded in
`NIX_EVAL_JOBS_MODELB_2026-07-18.md` §2):

```
foo                 /nix/store/1q54v18bj15w5q3qqh31jh303mcpj7bw-foo.drv
nested.baz          /nix/store/g4ws17cl4rfaqqw07xljv0x4vwbmx88z-baz.drv
deep.inner.deepjob  /nix/store/m23jbypsyy6y76i777n76i573lzidq08-deepjob.drv
```

**Gotcha (recorded):** the offline parity flake must be **committed clean** — a
dirty git tree makes v3's `evaluateFlakeV3` derive an *unlocked* flake ref, and
native `builtins.getFlake` then hard-fails with
`cannot call 'getFlake' on unlocked flake reference … (use --impure to
override)`. A stale uncommitted `system=` edit in the scratchpad flake tripped
this; reverting to the committed rev fixed it. (The TW oracle path tolerates a
dirty tree; the v3 getFlake path does not — it needs the rev-pin.)

---

## 4. Files changed on `angerman/2.34-v3`

| File | Change |
|---|---|
| `src/libexpr-v3/lode/nix-eval-jobs-modelB-v2.34.1.patch` | regenerated to also carry the `default.nix` `buildInputs += nix-expr-v3` deploy-route tweak (still git-am-clean on v2.34.1) |
| `src/libexpr-v3/lode/CONSOLIDATED_VERIFY_2026-07-18.md` | **NEW** — this doc |

No evaluator/packaging source needed changing: the consolidated merge already
carried the working flake packaging (`package.nix`, `components.nix`,
`everything.nix`, `hydra.nix`, `flake.nix`) and the Model-B accessors. The only
fix required for the *deploy route* was the nej-side `default.nix` one-liner,
which lives in the portable patch (nej is a separate repo).
