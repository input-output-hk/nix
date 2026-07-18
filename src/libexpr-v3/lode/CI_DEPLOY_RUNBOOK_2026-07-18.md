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
# the evaluator is spawned by hydra-evaluator.service; env propagates to the
# hydra-eval-jobset -> nix-eval-jobs worker children.
systemd.services.hydra-evaluator.environment = {
  NIX_V3_DIRECT_EVAL    = "1";                                  # engage the v3 VM in nix-eval-jobs
  NIX_V3_AOT_CACHE_FILE = "/var/cache/hydra/v3-aot/current.aot"; # shared CU mmap across the 20 workers
  NIX_V3_MAX_HEAP       = "2G";                                 # typed OOM, RLIMIT_AS-safe post-B1
  # CANARY ONLY: NIX_V3_REQUIRE = "1";  # hard-fail if v3 doesn't engage (catch silent TW fallback)
};
```

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

### 1.4 Retune the fleet (AFTER measuring per-worker RSS under the shared AOT on the REAL jobset)
```nix
# modules/web-service-hydra.nix extraConfig — set FROM the measured production numbers, not guessed:
evaluator_workers = 30;            # was 20 — more concurrent evals per box
evaluator_max_memory_size = 768;   # was 1024 — tighter cap now that CU is shared
```
**Density is PROVEN (x86_64-linux, subagent L, real patched nej):** with `--workers 3` + `NIX_V3_AOT_CACHE_FILE`, all workers map the AOT `Shared_Clean = Rss, Private_Dirty = 0` — the CU bytecode is mapped **once** (page-cache-backed) and shared read-only across the fleet, not copied. On the 8-package test jobset this saved **~25–35 MB private per worker** (66–80 MB with AOT vs 100–103 MB without); at haskell.nix/HNE scale the shareable AOT is ~126 MB (WS-5 measured 105 MB Shared_Clean), so expect **~100 MB/worker private saving** on real jobsets. The exact `workers`/`memory` retune is jobset-dependent — measure per-worker `Private_Dirty` on the production jobset under the shared AOT, then set these.

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
3. **Canary (one jobset).** Point a single low-stakes jobset's eval at v3 with `NIX_V3_DIRECT_EVAL=1` **and** `NIX_V3_REQUIRE=1` (so a silent TW fallback hard-fails and is visible). Watch: eval success, drvPath parity vs the prior TW eval, per-worker RSS, IFD-visibility log lines, no `NIX_V3_REQUIRE` failures. Hold ≥ a few eval cycles.
4. **Full.** Remove `NIX_V3_REQUIRE`, apply the evaluator env globally, deploy the AOT builder, retune `evaluator_workers`/`evaluator_max_memory_size` from the measured RSS. `colmena apply --on linux-0`.

**Rollback at any stage:** revert §1.1 input pins (+ §1.2 env) → `nix flake lock` → `colmena apply --on linux-0`. No data migration; the evaluator process just reverts to the TW nix. Keep the prior `nix`/`nix-pkg`/`nix-eval-jobs` pins noted for a one-command revert.

---

## 3. Monitoring / success gates
- **Correctness:** sample eval outputs' drvPaths match a TW re-eval (the shadow discipline, ongoing on the canary jobset). Any divergence → rollback + treat as a WS-1-class bug.
- **Density:** per-worker `Private_Dirty` (smaps_rollup) down vs the no-AOT baseline; the AOT mapping shows `Shared_Clean = Rss, Private_Dirty = 0` on every worker (shared once). Measured on linux-1: ~25–35 MB/worker saved on the test jobset; expect ~100 MB/worker at haskell.nix scale. This is the "run more evals per box" win.
- **IFD visibility:** the WS-2 end-of-eval IFD lines appear in the Hydra eval log; the 3-hour `hydra-eval-watchdog` can become an informed per-IFD budget.
- **No fallbacks:** during canary, zero `NIX_V3_REQUIRE=1 … did not engage` errors.

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
