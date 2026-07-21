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
