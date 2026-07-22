# Pre-deploy review 2026-07-20 — findings + fixes (accessors, nej patch, GC, runbook)

**Scope:** the user-requested "extra careful, thorough" review of everything on `angerman/2.34-v3` before the one-command infra rollout. Three passes: my line-by-line review, an adversarial GC-safety review of the Model-B accessor API, and an adversarial review of the nej patch against **Hydra's actual invocation** (source-verified against `angerman/hydra@4c4d6fe5f`). Every finding below is FIXED on this branch unless marked otherwise; gates re-run after.
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## The two deploy-killing findings (both would have shipped silently)

### H1 — Hydra never passes `--flake`; the v3 gate could never fire in production
Source-verified: `hydra-eval-jobset` invokes `nix-eval-jobs --expr 'let flake = builtins.getFlake (toString "<locked-url>"); in flake.hydraJobs or flake.checks or (throw …)' --gc-roots-dir … --meta --constituents --force-recurse --workers N --max-memory-size M`. The original patch gated on `args.flake` → **production deploy = silent no-op** (and a service-level `NIX_V3_REQUIRE` = total eval outage, since it hard-fails every legacy jobset too).
**Fix:** new `v3::evalExprRoot` — v3 evaluates the `--expr` string natively (getFlake is v3-native); gate = `--flake`(+compatible lockFlags) **or** `--expr`+empty-autoArgs. Root-shape guard throws (→ TW fallback) for lambda/functor roots (TW's `autoCallFunction` semantics). Env propagation verified: execvp + Perl `%ENV` (only `NIX_PATH` scrubbed) → env reaches the workers.

### G1 — the handle's GcRoots were invisible to the always-on minor scavenge
`Scavenger::run` (gc.cc) never walked the `GcRoot` registry (`walkCppStackRoots` was wired only into the gated-off major GC/audits) — the accessor API's central safety mechanism was decorative. Latent-only because scavenges fire at `exitDepth==0` while accessor forces run at `exitDepth==1`; the one REACHABLE window: a `--select`/`--expr` root eval runs a fresh top-level dispatch at `exitDepth==0` with `h->root` live → UAF. The brute/stress gates are structurally blind to this class (also exitDepth==0-gated).
**Fix:** the minor scavenger now walks + rewrites `gcRootStack()` and `gcRootVecStack()` entries (O(handles) — the registry is empty or a handful of long-lived entries at any scavenge point). Re-gated with the full brute + a `--select`-under-1MB-nursery smoke.

## Production-correctness fixes (silent-divergence class)

| id | finding | fix |
|---|---|---|
| F2 | `--constituents` (hydra passes it ALWAYS): v3 path emitted aggregates with `constituents: []` → hydra would create **empty aggregate builds, silently green** | `jobNeedsTreeWalker`: aggregate jobs route per-job through the ORIGINAL TW path (lazy TW root, evaluated at most once per worker); shadow-verified: aggregate carries both constituent drvPaths byte-identically |
| F4 | function-valued jobs + `__functor` attrsets silently dropped (TW auto-calls/unwraps them) | same per-job TW fallback |
| F1 | `--meta` (hydra passes it ALWAYS): no `checkMeta` filter → null/path metas included; **`meta.tests = { basic = <drv>; }` deep-forced + serialized** (TW rejects on the `outPath` probe without deep-forcing) | `checkMetaV3` mirrors TW's filter exactly; force errors now propagate (the old catch-all also swallowed Interrupted/limit errors → silent partial meta) |
| F3 | non-bool `recurseForDerivations` silently ignored (TW hard-errors); also v3 forced it under `--force-recurse` (TW skips even the force — hydra always passes `--force-recurse`) | `childAttrNames(h, recurse, forceRecurse)` mirrors both |
| A4 | v3's call-depth guard threw a bare `runtime_error` → infinite-recursion jobs lost their FATAL classification (stock nej aborts the eval) | new `v3::CallDepthError` (2 vm.cc sites) + nej maps it to `fatal=true`; BlackholeError stays per-job (matches TW's EvalError) |
| F6 | `--override-input`/`--update-input`/`--reference-lock-file` not honored by v3's internal re-lock → silent wrong-lock eval | gate refuses v3 when set (TW fallback) |
| A5 | `fromV3` field-force order differed (errors surfaced from a different field); quoted fragment segments mis-split | order = name→drvPath→outputs; quoted fragments throw → TW fallback |
| — | per-job v3 eval error could leave the handle's VMState frames indeterminate for the NEXT job | worker rebuilds the handle after any per-job v3 exception (reset-FIRST — the GcRoot dtor pops blindly LIFO); on rebuild failure the worker continues on TW |

## Deploy-config corrections (runbook updated)

- **`NIX_V3_MAX_HEAP` removed from the evaluator env** — a 2G hard cap converts previously-succeeding 2–3 GB haskell.nix evals into failures (hydra's own limit is a soft between-jobs restart, not a cap).
- **`evaluator_max_memory_size` RAISED to 3072, not lowered** — the restart check reads monotone `ru_maxrss`, and the AOT's `Shared_Clean` pages still count toward per-process RSS (sharing lowers box PSS, not the counter). At 1024 every v3 worker would restart after every big job (root re-eval each time). The earlier "lower to 768" was wrong.
- **`NIX_V3_REQUIRE` must never be set on the service** (global env would hard-fail legacy jobsets); manual canary runs only.
- Known throughput characteristic (reviewer finding, not fixed here): at accessor depth v3 never scavenges → per-worker RSS grows monotonically across jobs → workers restart on the memory check more often than TW, each restart re-evaluating the root (warm via disk/AOT caches). **Canary gate: measure jobs-per-restart.** Arming scavenge at accessor depth is future work that G1's registry walk now makes safe to attempt.

## Verified-clean (adversarial hunts that found nothing)

Accessor by-value force discipline + string copies (nothing returns arena-backed views); handle/CU lifetime + synthetic frame (matches the CLI template; exception unwind restores frames via `clearBlackMarksOnException` — the rebuild hardening is belt-and-braces); fork/concurrency (per-process state only); `escapeNixString` injection-safety; child-name ordering parity; gate-off byte-parity (TW path untouched); `registerGCRoot` (drvPath-keyed); restart protocol.

## Re-gate results (post-fix, this branch)

- Extended shadow jobset (15 jobs: aggregate+constituents, functor, defaulted/undefaulted lambdas, drv-in-meta/null-meta/path-meta, non-bool recurseForDerivations, throwing job + continuation): **parity OK in both `--force-recurse` and plain modes** vs the pristine-TW oracle (error text normalized, everything else byte-identical); aggregate constituents = the two drvPaths; worker continues after the failing job.
- `--select`-under-1MB-nursery smoke (the G1 window): green post-fix.
- Full `--brute`: see the re-gate commit/note.

## Deferred (documented, not blocking)

CA-derivation `queryOutputs(false)` fallback (zw3rk has no CA drvs); tier-1 `showErrorInfo` formatting for v3 error messages (per-job errors carry the v3 message without TW trace formatting — cosmetic); fragment prefix-fallback (`packages.<sys>.<frag>`) — engagement-failure falls back to TW which reproduces TW behavior exactly; scavenge-at-accessor-depth (future; unblocked by G1).

---

# Round-2 clean-room review (2026-07-22)

Ten reviewers re-run with EMPTY context (no lode docs, no prior findings — so
they couldn't be primed by round-1's conclusions), each an independent lens.
They found real bugs — several IN THE 2026-07-20 FIXES THEMSELVES. All fixed
+ re-gated; branches re-pushed fast-forward (nix @5ce4305fb, nej fork
2.34.1-v3 @83ef1e2, portable patch regenerated + git-am-verified, tree
identical to the branch).

## Confirmed + fixed

- **CR7-C1 (the sharpest; a self-inflicted regression):** v3 `kMaxCallDepth`
  was 5000, but the tree-walker's `max-call-depth` DEFAULTS to 10000. So jobs
  recursing 5001..10000 frames (deep haskell.nix module fixpoints) threw where
  stock nix succeeds — and the round-1 A4 change (CallDepthError→fatal) made
  that a WHOLE-EVAL abort. Raised to 10000; ceilings now coincide. Verified: a
  7000-deep job builds instead of aborting.
- **CR5-#1:** A4 converted only 2 of 6 call-depth guard sites. Did the other
  four (OP_CALL_N, OP_FORCE, forceValue, callClosureNExact).
- **CR5-#2:** `addErrorContext` erased CallDepthError to runtime_error on the
  common nixpkgs unwind path. Added the type-preserving arm.
- **CR6-D1 (silent wrong derivation):** an intermediate `__functor` attrset was
  looked up raw instead of autoCall-unwrapped → a shadowing child could be
  emitted as the WRONG drv. descendAttrPath now detects it and throws → TW
  retry. Verified: interFunctor.group.real = "correct", SHADOW-WRONG count 0.
- **CR6-D6/D8/D10:** non-attrset `.meta` throws (TW parity); checkMetaV3 depth
  guard (deep meta → clean error, no C-stack-overflow worker crash); UTF-8 meta
  force-dumped in-path (no out-of-try worker crash).
- **CR8-C1:** AOT `off+len` u64-overflow bounds checks → `off>size||len>size-off`
  (latent; AOT disabled by default).
- **CR5-#3:** `GcRoot::~GcRoot` erases its OWN entry (heap-held/out-of-order
  handle destruction is safe now that the minor scavenger walks the registry).
- **CR5-#4:** postScavengeAudit mirrors the registry walk (brute audit now
  covers that root class).

## Architectural safety net (CR7-R1 / CR8-R1) — the highest-value change

The nej worker now treats **v3 as the fast path and the tree-walker as the
correctness ORACLE**: on ANY per-job v3 exception it rebuilds the handle and
RETRIES that job on the tree-walker (unless NIX_V3_REQUIRE). So a residual
v3-vs-TW divergence can never make hydra wrong or worse-formatted — worst case
a divergent job is evaluated twice. This subsumes the whole class of
throwing divergences (numeric attr segments, CA outputs, non-attrset meta,
output-shape laxness, error-byte formatting). D1's silent-wrong-data case is
handled separately by the intermediate-functor detection above.

## Verified CLEAN by the clean-room reviewers (no change needed)

Injection (escapeNixString complete for the Nix string grammar); cross-jobset
cache poisoning (CU/EvalResult keys are content-addressed); SQLite under 20-way
concurrency (WAL + 1h busy-timeout, degrades to cache-miss); cross-DSO
CallDepthError typeinfo (default visibility → catch-by-type works across the
libnixexprv3 .so into nej); backport fidelity (subsystem ported byte-identical;
independently re-confirmed the G1 walk is correct — "2.34 has strictly more
root coverage, not less"); gate-off parity (libexpr/libstore/libmain
byte-identical to base d9ffe24ea; v3-lib static-init inventory clean; no new
stderr output gate-off).

## Latent / deferred (documented, not deploy-blocking in the production config)

- AOT C2 (no key↔blob integrity → tampered AOT = silently-wrong eval): AOT is
  disabled by default (v3AotWorkloads=[]); gate enabling it on a per-blob hash
  + moving the AOT build out of the eval user's write scope.
- NIX_TRACE_EVAL file-truncate + V3_DBG_SIGTRAP load-time read: env is clean in
  the hydra-evaluator unit; hardening candidates.
- system-nix `.#nix` doCheck: GitTest/DirectoryIterator unit tests fail in the
  linux-1 build sandbox — A/B-PROVEN identical on the deployed base d9ffe24ea
  (v3-independent, environmental). The deploy-critical hydra closure builds
  green regardless (it builds the nix-* components, not the doCheck'd
  everything-package).

---

# Round-3 clean-room adversarial review (2026-07-22)

Ten MORE reviewers, again EMPTY context (no lode docs, no prior findings) and
on the `fable` model, each a distinct lens (RR1 nej-patch-vs-upstream, RR2
GC/accessor, RR3 env/build, RR4 packaging, RR5 GC/nursery internals, RR6
accessor-semantics-vs-TW, RR7 classifier, RR8 AOT/worker, RR9 GCC/linux
portability — actually compiled all 10 TUs under GCC 14/libstdc++, RR10
infra/rollout). They CONFIRMED the round-1/round-2 fixes sound (GcRoot dtor,
accessor container discipline incl. the in-path dump, CallDepthError typing,
cross-DSO catch, backport fidelity, gate-off parity) and found a fresh batch —
several cross-corroborated by 2-4 independent reviewers.

## Confirmed + fixed (this branch)

Accessor parity (`eval_jobs_api.cc`) — every one is "throw where TW throws so
the per-job TW-retry net reproduces TW's bytes", closing SILENT (non-throwing)
divergences the retry net could not otherwise heal:
- **Numeric attr segments** (RR6-S1 = RR1-F7): `descendAttrPath` now throws when
  a segment parses as `string2Int<unsigned int>` — TW's `findAlongAttrPath`
  treats it as a list index and errors on an attrset, and nix-eval-jobs only
  ever produces numeric segments from numeric attr NAMES on attrsets, so TW
  always errors; v3 was silently emitting the job (realistic on version-matrix
  jobsets).
- **outputs() shape-laxness** (RR6-S2 / RR7-R2 / RR1-F3, three reviewers):
  rewrote `outputs()` to mirror `queryOutputs(withPaths=true)` decision-for-
  decision — throw on non-list `.outputs`, non-string / context-carrying output
  element, non-attrset output attr, and (no-`.outputs` branch) a missing
  top-level `.outPath`; keep TW's `continue` for a missing output attr / missing
  per-output outPath.  The prior silent skip + outPath-fallback could emit a job
  with wrong/partial/empty outputs (an all-empty `"outputs":{}` even trips
  Hydra's own `die unless scalar @outputNames`).
- **name()/system() context+type** (RR6-S3/S4): `name()` throws on a
  context-carrying name (TW `forceStringNoCtx`); `system()` throws on a
  present-but-non-string / context `system` instead of returning "unknown"
  (read-only-store path only; latent for the local-store deploy).
- **evalExprRoot pre-select guard** (RR6-S6 = RR9-RISK1): the lambda/functor
  root-shape guard now runs on the PRE-select root inside `makeHandleFromSource`
  (TW's releaseExprTopLevelValue auto-calls the raw root BEFORE `--select`); the
  post-select guard could engage on a different traversal root with no throw.
- **Rooted-slot reads** (RR6-L1 = RR1-F5): `descendAttrPath` / `jobNeedsTreeWalker`
  read the GC-rooted `h.jobValue` slot on every `lookup` instead of an unrooted
  snapshot (a `lookup` can realise a MapAttrs entry and allocate); corrected the
  stale "alloc-free — no GC hazard" comments.

GC / lifetime:
- **GcRootVec / GcRootRange dtors** (RR2-c5 = RR5-4): now erase their OWN
  registry entry (added `registered_`/`data_` members) instead of a blind LIFO
  pop — matches the CR5-#3 `GcRoot` hardening, safe under out-of-order /
  heap-held destruction.
- **Header/rationale comments** (RR2-c2 / RR5-8 / RR9-4): `eval_jobs_api.hh`
  one-handle-per-thread rationale rewritten (the real remaining constraint is
  thread AFFINITY, not the retired blind-pop); vm.cc `kMaxCallDepth` "5000"
  comments corrected to 10000.

nej worker (`worker.cc`, portable patch regenerated):
- **rebuildV3 only on unexpected exceptions** (RR7-C1 = RR8-R1 = RR6-obs1 =
  RR1-F4, FOUR reviewers): a routine per-job eval error leaves the VMState clean
  via the v3 dispatch loop's exception barrier (clearBlackMarksOnException
  unwinds frames/valueStack/withStack to the synthetic outer frame + reverts
  in-flight thunks), so the handle is reused as-is — no warm getFlake root-replay
  and no second live heap (under GC_DONT_GC=1) per erroring job.  Full rebuild
  kept only for a genuinely-unexpected exception type (helper `v3StateCleanAfter`).
- **Env-var value parsing** (RR1-F6 / RR3): `NIX_V3_DIRECT_EVAL`/`NIX_V3_REQUIRE`
  are value-parsed (`0`/`false`/`no`/`off` ⇒ off); `NIX_V3_DIRECT_EVAL=0` is now
  the in-place kill-switch (was: any presence engaged v3, so `=0` silently kept
  it on).
- **`#include <cstdlib>`/`<cctype>`** (RR9-RISK3): `std::getenv` no longer relies
  on `<stdlib.h>` transitively hoisting it.

Infra (`~/Projects/zw3rk/infra`):
- **linux-0 `deployment.buildOnTarget = true`** (RR10-RISK1, the actual
  "one-command" blocker): without it, `colmena apply --on linux-0` from the Mac
  builds the entire v3 nix+hydra+nej stack under Rosetta emulation (incl. nej's
  doCheck gtest), because the preflight closure is not on cache.zw3rk.com.  Now
  it builds natively on the 32 GB target (matches linux-1/idx/intelligence).
- **Eval-watchdog awk** (RR7-C3, statically verified): `hydra-eval-jobset`'s
  `comm` truncates to 15 chars ("hydra-eval-jobs"), so the exact match on the
  14-char "hydra-eval-job" NEVER fired — the 3 h stuck-eval backstop was dead.
  Now prefix-matches + also reaps orphaned nix-eval-jobs workers (PPID 1).
- **hydra-evaluator memory backstop** (RR10-RISK2): `MemoryHigh=24G` /
  `MemoryMax=28G` / `OOMScoreAdjust=500` on the evaluator cgroup (contains all
  nej workers) — 20 workers × 3 GiB = 60 GiB permissible peak on a 32 GB box had
  no cap and an inert oomd; now reclaim/kill stays inside the eval (a killed
  worker fails at most that eval; hydra retries) instead of the kernel OOM-killer
  taking postgres.  Values are a tunable starting point.
- **AOT env-file** (RR8-R4): corrected the security note — hydra:hydra 0755,
  written only by the (disabled) AOT oneshot; NOT an injection vector because
  Nix eval is pure (flake code cannot write files; IFD builds in the daemon
  sandbox).

## Documented / deferred (NOT deploy-blocking)

- **RR5-1 (forceWriteTarget write-after-scavenge)** — a PRE-EXISTING v3-core
  hazard (deepForceList `applyForceWriteback` into a nursery list vs the nursery
  reset), NOT introduced by the CI-integration work, with a documented prior
  audit conclusion (GC_AUDIT_ROUND_2 #6 = "memoization-loss only") that RR5's
  sharper static analysis disputes.  Covered by the `--brute` nursery-audit
  stress gate (1 MB nursery + V3_DBG_NURSERY_AUDIT).  Needs a dedicated repro
  (deepForceList + forced mid-element scavenge + allocation-in-window under
  V3_DBG_NURSERY_FWT) to adjudicate; TOP core follow-up, but not a regression and
  not a deploy blocker under current stress coverage.
- **RR1-F2 (v3 VM hard-crash has no fallback)** — the TW-retry net catches C++
  exceptions only; a deterministic SIGSEGV/SIGBUS or OOM-kill inside a worker
  lands in handleBrokenWorkerPipe → whole-eval failure (upstream TW had no such
  class).  Mitigation for the deploy: the staged rollout (shadow → canary →
  full) + the kill-switch (`NIX_V3_DIRECT_EVAL=0` + colmena apply).  Post-deploy
  hardening: a collector-side crash counter that re-forks with v3 disabled after
  N signal deaths.  Documented in the runbook.
- **RR4 packaging matrix edges** (C1 windows-cross eval-time failure, R1
  sanitizer lanes, R2 static/non-mingw cross) — break previously-green `nix flake
  check` / CI-matrix attrs but NOT the x86_64-linux Hydra deploy route (which
  never evaluates them).  Already an accepted limitation (v3 is
  `platforms=unix`).  Clean fix = mirror `nix-perl-bindings` (`optionalAttrs
  (!isWindows)` + null in sanitizer scopes) but needs guarding `src/nix/eval.cc`'s
  unconditional `v3::` call on Windows — deferred as off-deploy-path.
- **RR4-R3 (v3_release)** — the deployed evaluator ships with `v3_release=false`,
  i.e. always-on instrumentation (per-alloc counters, V3_STATS_INC in hot VM
  paths).  Flipping to `v3_release=true` strips it (faster single-eval; env-gated
  diagnostics still available) but changes codegen → needs its own re-gate.  A
  RECOMMENDED follow-up decision, not a blocker (the CI win is density + IFD
  visibility, not single-eval CPU).
- **RR10-RISK3 (rollback re-arms doCheck)** — reverting the deploy commit also
  reverts the `doCheck=false` override, and the old pin's everything-package
  fails its test gate when rebuilt today.  Rollback must KEEP the doCheck=false
  hunk (revert only the input pins + v3 env), or build-on-target with it in
  place.  Runbook instruction.
- Latent/dormant (single reviewer): RR5-2 cross-thread handle assert, RR5-3
  checkMetaV3 worklist (deploy runs main-thread 8 MB stacks — safe at 10000),
  RR5-5 eval-scope-handle scavenger walk (FFI dormant), RR6-S7/S8/S9 (empty
  segments / `""` attr / NUL-truncated meta — contrived), RR6-C1 = reply.dump
  UTF-8 worker crash (pre-existing, parity-equal to upstream), RR10-RISK4/5 (AOT
  probe path + masked-timer noise — dormant while AOT disabled).

## Re-gate — GREEN (2026-07-22)

- v3 lib + all 5 test binaries rebuild clean (only the 2 pre-existing
  warnings); `v3-gc-root-handles` correctly relinked for the gc_root.hh layout
  change.
- GC brute/nursery-audit suites (gc-root-handles, smoke, evalscope,
  drv-preflight): 4/4 PASS.
- Full `--brute` battery: **41/41** (transient 40/41 was the
  `lint-no-direct-tw-include` ratchet tripping on a first-cut `#include
  "nix/util/util.hh"` for string2Int; resolved by dropping the include —
  string2Int is transitively visible via ffi.hh, byte-parity-identical).
- Patched nej + a pristine-TW nej both build against the v3 prefix (`[12/12]
  Linking`); the patched binary's `otool -L` shows libnixexprv3.
- Shadow parity, PROD mode, baseline flake: core + `--meta` **byte-IDENTICAL**
  to the pristine TW oracle (6 jobs, 18 accessor-engagements, 0 root-fallback,
  0 retries).
- Shadow parity, PROD mode, EXTENDED flake (adds a numeric-named `"18"` job and
  a malformed `outputs = "out"` job): **byte-IDENTICAL** to the oracle, with
  **exactly 2 TW-retries = `18` + `badout`** — i.e. v3 throws on both new cases
  and the retry net heals each to TW's exact output.
- Under `NIX_V3_REQUIRE=1` (retry disabled) both new cases propagate v3's OWN
  throw text (`v3 attrPath: numeric segment '18'…`, `outputs… value is not a
  list`), proving the T2/T3 throws fire before the retry converts them.

Net: correctness/parity fully preserved; the new throws are exercised and
retry-healed. Safe to publish.

### Release-build re-gate (`v3_release=true`, RR4-R3 decision)

The deployed evaluator is switched to the RELEASE build (package.nix
`mesonFlags = [ (lib.mesonBool "v3_release" true) ]`) — always-on
instrumentation stripped (V3_STATS_* → no-ops), env-gated diagnostics still
available on demand.  V3_RELEASE is correctness-neutral (diagnostic counters
only), re-verified:

- Release build compiles clean (meson prints `v3: V3_RELEASE build`), no
  `-Werror` from any stripped `V3_STATS_BLOCK`.
- `--brute` clean on the release build after two follow-on fixes:
  - 6 smoke sub-tests (`testPrim{MapAttrsNames,MapAttrsNestedNames}DoNotRealize`,
    `testPrimAttrValuesMapAttrsSortsWithOneAppPerValue`,
    `testPrimMapAttrsNestedSelectUsesMappedValue`,
    `testPrim{IntersectAttrs,RemoveAttrs}MapAttrsChainCopy`) assert on
    `allocStats().pairsAllocated` App3-memoization pair counts, which
    V3_RELEASE zeroes; guarded `#ifdef V3_RELEASE` to SKIP (their primop
    behavior is covered on the release binary by the shadow-parity gate and
    fully asserted in the default build's --brute).  Default build unchanged
    (41/41).
  - lint-no-direct-tw-include: the round-3 explanatory comment in
    eval_jobs_api.cc literally contained the grep pattern `#include "nix/`;
    reworded (it also failed on the non-release committed state — fixed).
- Shadow parity on the RELEASE lib: byte-IDENTICAL to the pristine TW oracle
  on baseline + extended flakes; exactly 2 retries (18 + badout).
