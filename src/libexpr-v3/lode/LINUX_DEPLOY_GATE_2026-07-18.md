# x86_64-linux deploy gate — v3 on nix 2.34.6 + patched nix-eval-jobs

**Date:** 2026-07-18
**Branch under test:** `angerman/2.34-v3` @ `151aacca9` (v3 on nix 2.34.6 + flake
packaging + Model-B accessors + the portable nej patch).
**Fixes committed on:** `angerman/2.34-v3-linuxgate` (linear children of
`151aacca9`, ready to fast-forward into `angerman/2.34-v3`).
**Host:** `linux-1` — x86_64-linux, NixOS 6.12.57, 12 cores, `cache.iog.io`
configured, `/proc/<pid>/smaps_rollup` present (why density MUST run here).
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## 0. Verdict (all three gates GREEN on the real Hydra target)

| Gate | Result |
|---|---|
| **1. Linux `--brute`** | **41/41 ALL GREEN** (after 2 small Linux-port build fixes below) |
| **2. Patched nix-eval-jobs engages v3 (no fallback)** | ✅ `NIX_V3_REQUIRE=1 NIX_V3_DIRECT_EVAL=1` → "v3-direct engaged", exit 0 |
| **2. Shadow parity, drvPath/name/system/outputs + `--meta`** | **byte-identical TW == v3, 5/5 jobs** |
| **3. Density — AOT CU bytecode Shared_Clean across N workers** | ✅ **7.4 MB Shared_Clean across 3 workers, 0 Private_Dirty on the AOT** |

**DEPLOY-READY on x86_64-linux.** The 2.34-v3 backport had never run on Linux;
it does now, with only two trivial `-Werror=unused-result` build fixes (no
evaluator-logic change, byte-id-neutral on darwin). The B1 `RLIMIT_AS` fix is
present and works.

---

## 1. Gate 1 — Linux `--brute` = 41/41

Built `v3-eval` + the `nix` CLI + all v3 test binaries from `151aacca9` on
linux-1 (`meson setup -Dbuildtype=debugoptimized` + `ninja`), then
`all-v3-tests.sh --brute` → **41/41 ALL GREEN, 0 fail** (same 41-suite battery
that is green on darwin, incl. brute-audit golden byte-id, applied-cache,
ifd-provenance-cache, nonmoving-tenured, fork-worker, 815-cache, r1-verify, the
lints).

### Two genuine Linux-port build fixes (the only Linux blockers found)

Both are the same class: **GCC + glibc annotate `write()` with
`warn_unused_result`, and a `(void)` cast does NOT suppress
`-Werror=unused-result` on GCC** (clang/darwin never emitted the error, which is
why they were latent). Neither is on any eval/codegen path.

| file | site(s) | fix |
|---|---|---|
| `src/libexpr-v3/vm.cc` | 3× `(void)::write(2, …)` in the `V3_DBG_SIGTRAP` signal-diag handler + installer (inert unless `V3_DBG_SIGTRAP=1`) | capture into `[[maybe_unused]] ssize_t` |
| `src/libexpr-v3/test/evalscope-handles.cc` | 1× bare `write(fd,…)` in the FFI-readFile unit test | capture + `CHECK` the byte count |

**Byte-id-neutral:** the write itself and all evaluation behaviour are
unchanged; only the return value is now consumed. drvPaths unaffected; darwin
`--brute` stays 41/41.

### B1 (`RLIMIT_AS`) confirmed working, not re-broken by the backport

`NIX_V3_MAX_HEAP=2G v3-eval --expr 'builtins.length (builtins.genList (x:x) 1000)'`
→ `1000` (no `Arena::refill … address space exhausted`). The `limits.cc`
VmSize-baselining of `RLIMIT_AS` (so v3's cap does not fall below upstream's
16 GiB `MAP_NORESERVE` arenas) is present in the `151aacca9` source and works on
x86_64/glibc. This was the WS-5 pre-existing Linux crash; it is fixed.

---

## 2. Gate 2 — patched nix-eval-jobs on Linux: engage + shadow-parity

**Build:** installed the v3 nix to a prefix (`meson install`, all
`nix-*.pc` incl. `nix-expr-v3.pc` present), then built the Model-B-patched
nix-eval-jobs v2.34.1 (`git am` of
`lode/nix-eval-jobs-modelB-v2.34.1.patch`, clean) against that prefix via
`PKG_CONFIG_PATH` + meson — links `libnixexprv3.so`, builds clean.

**Jobset:** an offline, git-committed nested-`derivation` flake (no nixpkgs, no
network) — 5 expected jobs `foo, bar, nested.baz, nested.qux, deep.inner.deepjob`;
`hidden.shouldBeIgnored` (no `recurseForDerivations`) and `someString` (scalar)
correctly excluded by **both** paths. `foo` carries a `.meta` attr.

**Engagement (no silent fallback):** `NIX_V3_REQUIRE=1 NIX_V3_DIRECT_EVAL=1` →
`nix-eval-jobs: v3-direct engaged (flake root via builtins.getFlake)`, exit 0,
zero "fallback / did not engage / tree-walker" lines.

**Shadow parity (v3 gate-ON vs TW gate-OFF, same patched binary, same locked
flake, `jq -cS` normalised + sorted + `diff`):**
- core fields (drvPath / name / system / outputs): **BYTE_IDENTICAL** (5/5)
- `--meta`: **BYTE_IDENTICAL** (5/5); `foo.meta` = `{"broken":false,"description":"the foo job","priority":5}` on both paths.

Sample drvPaths (identical TW == v3, x86_64-linux):
```
foo                 /nix/store/qgbp36zj9j8r57lxfzq4qkdrkcdvglwf-foo.drv
nested.baz          /nix/store/glym80x8w8swcw303iynbkrncawgd1xm-baz.drv
deep.inner.deepjob  /nix/store/pd8fkrhhdg1r6k7vvqdscrrsh55j5ic8-deepjob.drv
```

---

## 3. Gate 3 — parallel-eval DENSITY with the REAL patched nix-eval-jobs

**Jobset:** a nixpkgs-backed flake (8 real packages — hello, cowsay, figlet,
gnugrep, gnused, coreutils, bash, which — via the system nixpkgs store path,
fully offline via the registry). Each descent forces stdenv/lib → nixpkgs-scale
CU volume.

**AOT build:** warmed the disk cache by running the patched nej once over the
jobset with `NIX_V3_AOT_BUILD_MODE`, then packed with `build-aot-cache.py` →
**AOT = 123 entries, 7.69 MB** (`/tmp/aot.bin`).

**Measurement:** ran the patched nej with `--workers 3` +
`NIX_V3_AOT_CACHE_FILE=/tmp/aot.bin` (fresh SQLite dir → every CU hit comes from
the AOT), sampling each worker's `/proc/<pid>/smaps` for the `/tmp/aot.bin`
mapping and `/proc/<pid>/smaps_rollup` for whole-process Private_Dirty. All 3
workers engaged v3; max-concurrent-AOT-mappers = 3.

### The AOT (CU bytecode) is Shared_Clean across all workers — GATE MET

Per-worker `/tmp/aot.bin` mapping (`r--p`), peak over the run:

| worker | AOT Rss | **AOT Shared_Clean** | AOT Private_Dirty |
|---|---|---|---|
| 1 | 7408 kB | **7408 kB** | **0** |
| 2 | 7408 kB | **7408 kB** | **0** |
| 3 | 7408 kB | **7408 kB** | **0** |

`Shared_Clean == Rss == 7408 kB` and `Private_Dirty == 0` on every worker: the
full ~7.4 MB of resident CU bytecode is mapped **once** (page-cache-backed) and
shared read-only across the whole fleet — **not copied per worker**. This is the
`evaluator_max_memory_size` density lever, now proven with the actual patched
nix-eval-jobs (the B2 canonical-symbol-table 100%-borrow working through the
real worker, not just `v3-eval`).

### Whole-process private cost (the density win, quantified)

Per-worker whole-process peak **Private_Dirty** (`smaps_rollup`), N=3:

| config | worker private dirty | note |
|---|---|---|
| **with AOT** | ~66–80 MB | 7.4 MB CU bytecode is *shared*, not in here |
| **no AOT (control)** | ~100–103 MB | CUs deserialize into each worker's *private* heap; no `/tmp/aot.bin` mapping |

The AOT + in-place borrow saves **~25–35 MB private per worker** (the 7.4 MB
shared CU mapping plus the avoided per-worker deserialization heap). At N
workers the shared CU footprint is paid **once** instead of N×.

---

## 4. Linux-port caveats (honest, non-blocking)

1. **The two `-Werror=unused-result` fixes** (§1) are the only Linux build
   deltas. Committed on `angerman/2.34-v3-linuxgate`; fast-forward into
   `angerman/2.34-v3`. Validated: rebuilt with the committed source on linux-1;
   `--brute` 41/41.
2. **nej runtime linking:** the patched nej binary needs
   `LD_LIBRARY_PATH=<prefix>/lib` at runtime (rpath is incomplete for some
   transitive `libnix*` on Linux). This is a *packaging* detail of the meson +
   PKG_CONFIG_PATH bring-up route, not a v3 bug; the `nix build`
   `--override-input` deploy route (V3_FLAKE_PACKAGING) bakes rpaths and does
   not need it.
3. **Density jobset** uses the host's system nixpkgs (offline via the registry
   store path) at x86_64-linux — representative CU volume; the sharing property
   is scale-independent and here measured on the real target OS.
4. **Broader Windows/mingw cross** (from V3_FLAKE_PACKAGING §4) is untouched and
   out of scope for the unix deploy route.
