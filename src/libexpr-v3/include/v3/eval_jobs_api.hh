#pragma once
/// @file
/// v3-native accessor surface for out-of-tree job evaluators (Model B).
///
/// This is the embedder API that lets nix-eval-jobs / the Hydra evaluator
/// drive the v3 bytecode VM directly (see
/// `lode/NIX_EVAL_JOBS_PATCH_2026-07-18.md`).  The worker's four coupled
/// operations — DESCEND an attrPath, DETECT a derivation, EXTRACT
/// name/drvPath/outputs/system/meta, and DISCOVER children to recurse into —
/// are exposed here as small v3-native functions, so v3 owns the *heavy*
/// nixpkgs/haskell.nix descent while nix-eval-jobs keeps all of its
/// store-side logic (`readDerivation`, `queryMissing`, JSON serialization)
/// byte-for-byte unchanged.  Everything here mirrors what the tree-walker
/// path does (`worker.cc`/`drv.cc`, `getDerivation`/`PackageInfo`), but
/// against a `v3::Value` rather than a `nix::Value`.
///
/// Model B (NOT a Value bridge): no v3->TW marshalling of the job value.
/// The only cross-boundary payloads are plain std::string / std::vector
/// (store-path strings, attr names) and — for `--meta` only — an
/// nlohmann::json built from v3's own `toJsonValue` printer.  In particular
/// the embedder never holds a raw `v3::Value`: v3 uses a *moving* GC, so a
/// `v3::Value` held in an embedder C++ local across a force could dangle when
/// a scavenge relocates its referent.  Instead the handle owns TWO GC-rooted
/// slots — the flake root and the "current job value" — and every accessor
/// reads through the rooted job slot that `descendAttrPath` last set.  The
/// embedder only ever passes the opaque handle around.
///
/// Lifetime: `evalFlakeRoot` returns a handle that owns the compiled
/// CompilationUnit(s), a `VMState`, and the GC-rooted root + job Values.
/// Keep it alive for the whole worker's lifetime.
///
/// CONSTRAINT — at most ONE live handle per thread, destroy-before-rebuild
/// (review A6, 2026-07-20): the handle's two GcRoot registrations live on a
/// strictly-LIFO thread-local stack whose RAII destructor pops the TOP entry
/// blindly.  A second concurrent handle, or constructing a replacement
/// handle before destroying the old one, interleaves push/pop and silently
/// unroots the other handle's slots.  nix-eval-jobs' worker satisfies this
/// (one handle; rebuild does `reset()` first).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nix {
class EvalState;
}

namespace nix::v3 {

/// Opaque per-worker evaluation handle.  Defined in eval_jobs_api.cc; holds
/// the flake root CU + VMState + the GC-rooted root/job Values.  The embedder
/// treats it as an opaque owned resource (via EvalJobsHandlePtr below) and
/// never dereferences a v3 Value itself.
struct EvalJobsHandle;

/// Custom deleter so `std::unique_ptr<EvalJobsHandle>` works with the
/// incomplete type in this header (the dtor is defined in the .cc where the
/// struct is complete).
struct EvalJobsHandleDeleter {
    void operator()(EvalJobsHandle * h) const noexcept;
};
using EvalJobsHandlePtr = std::unique_ptr<EvalJobsHandle, EvalJobsHandleDeleter>;

/// Root eval (the `initializeRootValue`/`evaluateFlake` replacement).
///
/// Runs `(builtins.getFlake "<lockedFlakeRef>")` through the v3 pipeline
/// (`runRootExprFromString`), optionally descends the `fragment` attrPath
/// (dot-separated, e.g. "hydraJobs" or "packages.aarch64-darwin"), and
/// optionally applies a `--select` function (a Nix lambda expression string
/// evaluated and applied to the fragment value).  Empty `fragment` /
/// `selectExpr` skip those steps.
///
/// `lockedFlakeRef` MUST be the SAME locked, rev-pinned flakeref the caller
/// resolved (nix-eval-jobs' `getLockedFlake()->flake.lockedRef.to_string()`),
/// or the drvPaths diverge from the tree-walker's `callFlake`.
///
/// The resulting root Value is in WHNF and registered as a GC root for the
/// handle's lifetime, so it survives the many force re-entries the worker
/// drives over subsequent job requests.
EvalJobsHandlePtr evalFlakeRoot(nix::EvalState & state,
                                const std::string & lockedFlakeRef,
                                const std::string & fragment,
                                const std::string & selectExpr);

/// Root eval for the `--expr` route — THE invocation shape Hydra actually
/// uses (hydra-eval-jobset passes `--expr 'let flake = builtins.getFlake
/// (toString "<locked-url>"); in flake.hydraJobs or flake.checks or
/// (throw …)'`, never `--flake`).  Runs the expression through the v3
/// pipeline (native getFlake included) and forces the root to WHNF.
///
/// THROWS when the resulting root is a lambda or a functor attrset — the
/// tree-walker's non-flake route auto-calls such roots
/// (releaseExprTopLevelValue → autoCallFunction), which v3 does not mirror;
/// the caller treats the throw as "v3 did not engage" and falls back to the
/// tree-walker for exact parity.  Only call this with EMPTY autoArgs
/// (`--arg`/`--argstr` are tree-walker-only).
EvalJobsHandlePtr evalExprRoot(nix::EvalState & state,
                               const std::string & exprSrc,
                               const std::string & selectExpr);

/// Descend a job attrPath from the handle's root (the `findAlongAttrPath`
/// replacement).  Forces each intermediate segment to WHNF and stores the
/// final value into the handle's GC-rooted "current job value" slot, which
/// every subsequent accessor reads.  Throws `nix::EvalError` on a missing
/// attribute or a non-attrset mid-path (same failure surface the tree-walker
/// path reports back to the collector).
void descendAttrPath(EvalJobsHandle & h, const std::vector<std::string> & path);

/// True iff the current job value is an attrset (forced).  The worker uses
/// this to decide whether the value can possibly be a derivation / recursable
/// attrset (mirrors `value->type() == nAttrs`).
bool isAttrs(EvalJobsHandle & h);

/// True iff the current job value is a derivation (the `getDerivation`
/// replacement): checks it is an attrset with `type == "derivation"`.
/// Mirrors `EvalState::isDerivation`.
bool isDerivation(EvalJobsHandle & h);

/// Lexicographically-ordered child attribute names of the current job value
/// (the `collectAttrsForRecursion` replacement).  Always returns every child
/// name.  `recurse` is IN/OUT: the caller sets its default (e.g.
/// `forceRecurse || path.empty()`); if the attrset carries a
/// `recurseForDerivations` attribute, `recurse` is overwritten with that
/// boolean (forced; THROWS on a non-bool, mirroring TW's forceBool).  When
/// `forceRecurse` is true the attribute is not read at all — the tree-walker
/// skips even the force in that mode (`!args.forceRecurse &&` guard), so a
/// throwing/non-bool `recurseForDerivations` must not error under
/// --force-recurse.
std::vector<std::string> childAttrNames(EvalJobsHandle & h, bool & recurse,
                                        bool forceRecurse);

/// True iff the current job value needs the TREE-WALKER path for exact
/// parity (the worker then routes this one job through the original
/// findAlongAttrPath/processDerivation with a lazily-created TW root):
///   - lambda job values (TW's per-job autoCallFunction calls all-defaulted
///     formals / hard-errors on missing arguments),
///   - functor attrsets (`__functor` — autoCallFunction unwraps them),
///   - aggregate jobs (`_hydraAggregate`) when `wantConstituents` (the
///     constituent extraction needs coerceToString-with-context on a TW
///     Value; emitting an aggregate with EMPTY constituents would silently
///     produce empty aggregate builds in Hydra).
/// All checks are non-forcing presence/tag tests (alloc-free).
bool jobNeedsTreeWalker(EvalJobsHandle & h, bool wantConstituents);

/// Force `.drvPath` of the current job value and return it as a store-path
/// string (the `requireDrvPath` replacement — the proven byte-identical
/// path).  Throws if absent or not a string/path.
std::string drvPath(EvalJobsHandle & h);

/// Force `.name` of the current job value and return it (the `queryName`
/// replacement).
std::string name(EvalJobsHandle & h);

/// Force `.system` of the current job value, or "unknown" if absent (the
/// `querySystem` replacement — only consulted on the read-only/remote-store
/// fallback; otherwise `readDerivation(...).platform` wins store-side).
std::string system(EvalJobsHandle & h);

/// Output name -> output store-path string, in the value's `.outputs` order
/// (the `queryOutputs(true)` replacement).  For each output name `o`, reads
/// `.${o}.outPath`; when there is no `.outputs` list, falls back to a single
/// `"out"` = top-level `.outPath` (mirrors `PackageInfo::queryOutputs`).
/// The caller parses each string into a `nix::StorePath`.
std::vector<std::pair<std::string, std::string>> outputs(EvalJobsHandle & h);

/// `.meta` of the current job value -> JSON (the `queryMeta` replacement).
/// Mirrors `PackageInfo::queryMeta`: ALWAYS returns an engaged optional whose
/// value is a JSON `null` when there is no serialisable meta, or an object of
/// the serialisable meta attributes otherwise.  Each attribute is rendered via
/// v3's native `toJsonValue`; non-JSON-serialisable entries (e.g. functions)
/// are skipped, matching the tree-walker's `checkMeta` filtering.  (Returning
/// std::nullopt would omit the field and diverge from TW's `"meta":null`.)
/// Only called when `--meta` is requested.
std::optional<nlohmann::json> meta(EvalJobsHandle & h);

} // namespace nix::v3
