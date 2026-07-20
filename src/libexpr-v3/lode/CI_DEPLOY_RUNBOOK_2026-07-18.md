# v3 CI rollout runbook — Hydra on linux-0 (DRAFT)

**Date:** 2026-07-18
**Target:** deploy the v3 bytecode-VM evaluator into the zw3rk Hydra CI (`~/Projects/zw3rk/infra`), so nix-eval-jobs on linux-0 evaluates jobsets on v3 — for parallel-eval density (lower per-worker RSS → more concurrent evals per box) + default-on IFD visibility.
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## 0. Artifacts (built + validated in this effort)

| artifact | what | state |
|---|---|---|
| `input-output-hk/nix#angerman/2.34-v3` (@9b853e0ac) | nix 2.34.6 + v3 VM + `nix-expr-v3` flake packaging + Model-B accessors | darwin ✅ + **x86_64-linux ✅** (--brute 41/41 both) |
| `nix-eval-jobs-modelB-v2.34.1.patch` | Model-B worker patch + `default.nix` deploy buildInput (git-am-clean on nej v2.34.1, 6 files) | shadow-parity ✅ (offline jobset, 5/5 byte-id, incl. `--meta`); deploy-route `nix build` ✅ |
| AOT cache (`build-aot-cache-ci.sh`) | shared CU bytecode mmap for the worker fleet | density ✅ (x86_64-linux, real patched nej: `Shared_Clean` across workers, `Private_Dirty=0`; ~25–35 MB/worker saved) |

**Deploy attribute:** the infra consumes `nix-pkg.packages.<sys>.nix`. On darwin, `.#nix` (everything) is red only at a **pre-existing, v3-independent** libutil unit-test gate (drvPath-proven identical to base); `.#nix-cli` is green. On x86_64-linux (the eval host) the current infra already builds `.#nix`, and subagent L confirmed the v3 nix builds + `--brute` 41/41 there — so the darwin `.#nix` libutil-test red is **darwin-only** and not on the deploy path.

---

## 1. Infra changes (`~/Projects/zw3rk/infra/ops`)

### 1.1 `flake.nix` — point nix + the evaluator at v3
```nix
# was  github:input-output-hk/nix/angerman/2.34-ifd-profiling
nix.url     = "github:input-output-hk/nix/angerman/2.34-v3";   # :25 (source; hydra follows this)
nix-pkg.url = "github:input-output-hk/nix/angerman/2.34-v3";   # :73 (system nix.package)

# NEW: the patched nix-eval-jobs (Model-B). Fork nix-eval-jobs v2.34.1 + apply
# src/libexpr-v3/lode/nix-eval-jobs-modelB-v2.34.1.patch, publish as e.g.
# github:angerman/nix-eval-jobs/2.34.1-v3, then make Hydra use it:
nix-eval-jobs.url = "github:angerman/nix-eval-jobs/2.34.1-v3";
hydra.inputs.nix-eval-jobs.follows = "nix-eval-jobs";   # CONFIRMED: hydra.inputs = {nix, nix-eval-jobs, nixpkgs, treefmt-nix}
hydra.inputs.nix.follows = "nix";                       # already present (:28)
```
`hydra.inputs.nix.follows = "nix"` already rebuilds Hydra + nix-eval-jobs against the v3-carrying libnixexpr; the new bits are (a) the v3 branch pin and (b) the patched nix-eval-jobs fork. **Rollback = revert these lines + `nix flake lock` + `colmena apply --on linux-0`.** It is a pin flip.

### 1.2 `modules/web-service-hydra.nix` — engage v3 in the evaluator
```nix
# the evaluator is spawned by hydra-evaluator.service; env propagates through
# execvp (hydra-evaluator.cc) and hydra-eval-jobset's %ENV copy (only NIX_PATH
# is scrubbed — verified in the hydra source, review 2026-07-20) into the
# nix-eval-jobs worker children.
systemd.services.hydra-evaluator.environment = {
  NIX_V3_DIRECT_EVAL    = "1";                                  # engage the v3 VM in nix-eval-jobs
  NIX_V3_AOT_CACHE_FILE = "/var/cache/hydra/v3-aot/current.aot"; # shared CU mmap across the 20 workers
  # DO NOT set NIX_V3_MAX_HEAP here (review F5, 2026-07-20): the evaluator's
  # per-worker memory is governed by hydra's evaluator_max_memory_size
  # (a soft between-jobs restart), not a hard cap.  haskell.nix-scale evals
  # legitimately exceed 2-3 GB mid-job; a hard v3 heap cap would convert
  # previously-succeeding evals into failures.  The 3-hour eval watchdog +
  # the restart check are the production guards.
  #
  # DO NOT set NIX_V3_REQUIRE on the service (review 2026-07-20): the env is
  # global to ALL jobsets, and REQUIRE hard-fails any worker where v3 cannot
  # engage — including every legacy (non-flake, --arg) jobset — taking those
  # evals down entirely.  Use NIX_V3_REQUIRE only in MANUAL shadow runs.
};
```
**How v3 engages (review-corrected):** hydra-eval-jobset invokes `nix-eval-jobs --expr 'let flake = builtins.getFlake (toString "<locked-url>"); in flake.hydraJobs or flake.checks or (throw …)' --gc-roots-dir … --meta --constituents --force-recurse --workers N --max-memory-size M` — it never passes `--flake`. The patched worker therefore engages v3 on the `--expr`+empty-autoArgs shape (`evalExprRoot`); legacy jobsets (file positional + `--arg`) stay on the tree-walker automatically. Aggregate jobs (`_hydraAggregate`, hydra always passes `--constituents`), functor attrsets, and lambda job values are routed per-job through the original tree-walker path (lazy TW root) for exact parity.

### 1.3 AOT cache builder (keyed to the jobset nixpkgs pin)
```nix
systemd.services.v3-aot-cache = {
  description = "Build the v3 AOT cache for the active jobset nixpkgs/haskell.nix pin";
  serviceConfig = { Type = "oneshot"; User = "hydra"; };
  # ExecStart: build-aot-cache-ci.sh against the jobset's locked nixpkgs rev ->
  #   /var/cache/hydra/v3-aot/<narHash>.aot ; then atomically ln -sfn current.aot.
  # Trigger on jobset input change (a stale AOT is NOT a correctness risk — v3 falls
  # back to owning any CU not in the map — only a sharing-effectiveness one).
};
systemd.tmpfiles.rules = [ "d /var/cache/hydra/v3-aot 0755 hydra hydra - -" ];
```

### 1.4 Retune the fleet — direction CORRECTED by review (2026-07-20)
```nix
# modules/web-service-hydra.nix extraConfig:
evaluator_workers = 20;             # unchanged for v1 — retune from measured box PSS later
evaluator_max_memory_size = 3072;   # was 1024 — RAISED for v3 (see below); NOT lowered
```
**Why RAISED, not lowered (the earlier "768" here was wrong):** nix-eval-jobs' restart check compares **monotone `ru_maxrss`** against this limit after every job. (a) v3's per-worker RSS is structurally higher than TW's (1.6–2.5×; haskell.nix-scale jobs peak 2–3 GB), and (b) the AOT's `Shared_Clean` pages **still count toward each worker's RSS** — sharing lowers the *box's* real memory (PSS), not the per-process RSS the check reads. At 1024 every v3 worker would cross the limit on its first big job and then restart after *every* job (`ru_maxrss` never decreases), re-evaluating the root each time — an eval-throughput regression. 3072 keeps the restart a between-big-jobs hygiene event.
**Density is PROVEN at the box level (x86_64-linux, real patched nej):** with `--workers 3` + `NIX_V3_AOT_CACHE_FILE`, all workers map the AOT `Shared_Clean = Rss, Private_Dirty = 0` — the CU bytecode is mapped **once** (page-cache-backed) and shared read-only across the fleet. On the 8-package test jobset this saved **~25–35 MB private (PSS) per worker**; at haskell.nix/HNE scale the shareable AOT is ~126 MB (WS-5 measured 105 MB Shared_Clean), so expect **~100 MB/worker real-memory saving**. Raising `evaluator_workers` beyond 20 should be driven by measured box PSS under production load, not by the per-process RSS counter.

---

## 2. Rollout (staged, each stage gated + reversible)

1. **Build + cache the artifacts.** Build `.#nix` (or `.#nix-cli`) from `angerman/2.34-v3` and the patched nix-eval-jobs on an x86_64-linux builder; push to `cache.zw3rk.com` so linux-0 substitutes rather than rebuilds. The **verified deploy-route build** (proven on darwin, the exact route the infra uses) is:
   ```bash
   # patched nej fork = nej v2.34.1 + git am nix-eval-jobs-modelB-v2.34.1.patch (6 files, incl default.nix buildInput)
   nix build 'github:angerman/nix-eval-jobs/2.34.1-v3#nix-eval-jobs' \
     --override-input nix github:input-output-hk/nix/angerman/2.34-v3
   #  -> result/bin/nix-eval-jobs links the flake-built libnixexprv3; NIX_V3_REQUIRE=1 engages v3, no fallback.
   ```
   NOTE (getFlake requires a locked flake): v3's getFlake path hard-fails on a *dirty/unlocked* flake ref (`cannot call 'getFlake' on unlocked flake reference`). Hydra jobsets are always locked flakes, so this is satisfied in production — but any manual shadow run must use a committed-clean/locked flake.
2. **Shadow (no production impact).** On linux-1, run patched (v3) + stock (TW) nix-eval-jobs over ONE real jobset's locked flake; **byte-compare the emitted drvPath set**. Gate: identical. (Offline nested-jobset shadow already 5/5 byte-id; this repeats it on a real jobset.)
3. **Canary (manual, hydra-shaped).** REVISED (review 2026-07-20): hydra's evaluator env is GLOBAL to all jobsets, so `NIX_V3_REQUIRE` cannot be scoped to one jobset on the service — a service-level REQUIRE would take down every legacy/incompatible jobset. Instead, run a MANUAL hydra-shaped invocation on linux-0/1 against a real jobset's locked URL, with REQUIRE proving engagement:
   ```bash
   NIX_V3_DIRECT_EVAL=1 NIX_V3_REQUIRE=1 nix-eval-jobs \
     --expr 'let flake = builtins.getFlake (toString "<locked-url>"); in flake.hydraJobs or flake.checks or (throw "no hydraJobs")' \
     --gc-roots-dir /tmp/canary-roots --meta --constituents --force-recurse --workers 4 --max-memory-size 3072
   ```
   (the EXACT argv hydra-eval-jobset uses, plus REQUIRE). Watch: engagement lines (one per worker), drvPath parity vs the same command without `NIX_V3_DIRECT_EVAL`, per-worker RSS, aggregate jobs carrying non-empty constituents (the per-job TW-fallback path), IFD-visibility lines.
4. **Full.** Apply the evaluator env (`NIX_V3_DIRECT_EVAL`, **no REQUIRE**) via colmena; deploy the AOT builder; keep `evaluator_max_memory_size = 3072` (§1.4). Watch the first production evals' logs for the per-worker engagement lines + parity spot-checks.

**Rollback at any stage:** revert §1.1 input pins (+ §1.2 env) → `nix flake lock` → `colmena apply --on linux-0`. No data migration; the evaluator process just reverts to the TW nix. Keep the prior `nix`/`nix-pkg`/`nix-eval-jobs` pins noted for a one-command revert.

---

## 3. Monitoring / success gates
- **Correctness:** sample eval outputs' drvPaths match a TW re-eval (the shadow discipline, ongoing on the canary jobset). Any divergence → rollback + treat as a WS-1-class bug.
- **Density:** per-worker `Private_Dirty` (smaps_rollup) down vs the no-AOT baseline; the AOT mapping shows `Shared_Clean = Rss, Private_Dirty = 0` on every worker (shared once). Measured on linux-1: ~25–35 MB/worker saved on the test jobset; expect ~100 MB/worker at haskell.nix scale. This is the "run more evals per box" win.
- **IFD visibility:** the WS-2 end-of-eval IFD lines appear in the Hydra eval log; the 3-hour `hydra-eval-watchdog` can become an informed per-IFD budget.
- **Engagement:** every production eval's log shows the per-worker `nix-eval-jobs: v3-direct engaged (--expr root via the v3 pipeline)` lines (v3 active); `v3-direct not engaged`/`falling back` lines flag jobsets that stayed on TW (legacy jobsets — expected; a flake jobset falling back is a finding). REQUIRE is manual-canary-only (§2.3).
- **Aggregates:** aggregate jobs (`_hydraAggregate`) emit non-empty `constituents` (they route via the per-job TW fallback; an EMPTY constituents list on an aggregate is the review-F2 failure mode and grounds for rollback).

---

## 4. Known caveats (carry into the deploy decision)
1. **x86_64-linux — GREEN (measured on linux-1, `9b853e0ac`).** `--brute` 41/41; patched nej engages v3 + byte-identical drvPath parity (5/5 incl `--meta`); density shared across workers. Two genuine Linux-port fixes were needed and are committed (GCC+glibc `-Werror=unused-result` on `write()` — a `(void)` cast doesn't suppress it on GCC — in the `V3_DBG_SIGTRAP` diag handler + one FFI test; both byte-id-neutral). B1 RLIMIT_AS confirmed working on Linux. **Runtime-lib note:** under the meson/PKG_CONFIG bring-up route the nej binary needs `LD_LIBRARY_PATH=<prefix>/lib` (incomplete rpath) — the `nix build --override-input` **deploy route bakes rpaths**, so production is unaffected.
2. **darwin builders' system nix**: `.#nix` is red on darwin only at the pre-existing libutil unit-test gate (v3-independent, drvPath-proven). If darwin builders rebuild their nix from the flake, either point them at `.#nix-cli`, rely on `cache.zw3rk.com`, or leave darwin on the current nix (the eval host is linux-0; darwin builders don't evaluate jobsets — the v3 eval win is entirely on linux-0). **v3 on darwin is not required for the CI eval win.**
3. **nix's own CI matrix** (Windows-cross, `buildNoGc`) will trip on v3 (`platforms=unix`, no meson `gc` option) — relevant only if the infra builds nix's full hydraJobs, not for `packages.<sys>.nix` consumption.
4. **Scope of v3 engagement:** the Model-B patch handles flake jobsets (the zw3rk shape) with drvPath/name/system/outputs/meta; `--constituents`/`--apply` and non-flake jobsets stay on TW (documented). CA-derivation `queryOutputs(false)` fallback not mirrored.

---

## 5. What v3 CI delivers (recap, measured)
- **Parallel-eval density:** the 20 evaluator workers borrow one shared AOT mmap (~105 MB CU shared once at HNE scale, WS-5) instead of each copying it — lowers per-worker RSS under `evaluator_max_memory_size`, so more concurrent evals fit per box.
- **Default-on IFD visibility** (WS-2) + **realise-context correctness** (WS-1) — the evaluator IS the IFD-heavy path (`allow_import_from_derivation=true`).
- Warm re-eval cache moat (WS-3) for the same jobset re-evaluated each push.
- **Not** single-eval speed (still ~1.8–2.5× TW CPU) — the win is fleet density + visibility, not per-eval latency.
