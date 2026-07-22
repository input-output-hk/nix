/// @file
/// v3-native accessor surface for out-of-tree job evaluators (Model B).
/// Implementation of include/v3/eval_jobs_api.hh — see that header and
/// lode/NIX_EVAL_JOBS_PATCH_2026-07-18.md for the design.
///
/// Every accessor mirrors a tree-walker operation in nix-eval-jobs'
/// worker.cc / drv.cc (findAlongAttrPath, getDerivation,
/// collectAttrsForRecursion, PackageInfo::query{Name,DrvPath,Outputs,System},
/// queryMeta) but against a `v3::Value`.  The embedder never touches a v3
/// Value: v3's GC is MOVING, so the two live values (flake root + current job
/// value) are held in GC-rooted handle slots (`GcRoot`), forwarded across
/// every scavenge.  Accessors read those rooted slots, and follow the v3
/// force discipline — read a `Value*` into a by-value local BEFORE any force
/// that could allocate (and thus relocate the parent Bindings), and never
/// reuse a raw Bindings pointer across such a force.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/eval_jobs_api.hh"
#include "v3/run.hh"
#include "v3/vm.hh"
#include "v3/errors.hh"   // CallDepthError (meta recursion guard, review CR6-D8)
#include "v3/primop.hh"
#include "v3/print.hh"
#include "v3/alloc.hh"
#include "v3/value.hh"
#include "v3/ir.hh"
#include "v3/gc_root.hh"
// V3-NATIVE: route the EvalState fwd-decl + nix::Error through the FFI shim
// (like run.cc) rather than a direct TW include of the eval header, so this
// file stays clear of the lint-no-direct-tw-include ratchet.  We only ever
// pass `nix::EvalState &` straight through to runRootExprFromString (never
// call an EvalState method), and throw plain `nix::Error` on descent failures.
#include "v3/ffi.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// Handle
// ---------------------------------------------------------------------------

/// Per-worker evaluation handle.  Owns the compiled CU(s), a VMState with a
/// synthetic outer frame, and TWO GC-rooted Value slots (the flake root and
/// the current job value).  Never moved (held by unique_ptr), so the
/// addresses `&root` / `&jobValue` registered with GcRoot are stable.
///
/// Member order is load-bearing: `root`/`jobValue` are declared before their
/// `rootGuard`/`jobGuard`, and `rootGuard` before `jobGuard`.  GcRoot pushes
/// LIFO onto a thread-local stack; reverse-order member destruction pops
/// jobGuard then rootGuard, matching the push order (root then job).
struct EvalJobsHandle {
    std::unique_ptr<VMState> vm;
    std::unique_ptr<CompilationUnit> rootCu;
    std::unique_ptr<CompilationUnit> selectCu;   ///< only for --select
    Value root { };        ///< getFlake root, after fragment/select (WHNF)
    Value jobValue { };    ///< last descendAttrPath result (WHNF)
    std::unique_ptr<GcRoot> rootGuard;
    std::unique_ptr<GcRoot> jobGuard;
};

void EvalJobsHandleDeleter::operator()(EvalJobsHandle * h) const noexcept
{
    delete h;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace {

/// Escape a raw string into the body of a Nix double-quoted string literal.
/// A locked flakeref (git+file://…?rev=…&narHash=sha256-…) contains none of
/// these in practice, but escaping keeps the synthesized getFlake expression
/// well-formed for any input: `\` and `"` are the literal escapes, and `$` is
/// escaped so a stray `${` can never start an interpolation.
std::string escapeNixString(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"' || c == '$')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

/// Split a dot-separated attrPath fragment ("a.b.c") into segments.  Empty
/// input yields an empty vector.  Quoted segments — `a."b.c"` — are NOT
/// handled (TW's findAlongAttrPath supports them); rather than silently
/// mis-splitting, THROW so the worker's engagement catch falls back to the
/// tree-walker (review A5, 2026-07-20).  Job fragments in practice are
/// simple dotted identifiers; hydra passes no fragment at all.
std::vector<std::string> splitDot(const std::string & s)
{
    if (s.find('"') != std::string::npos)
        throw nix::Error(
            "v3 flake root: quoted attrPath fragment segments are "
            "tree-walker-only: '" + s + "'");
    std::vector<std::string> segs;
    std::string cur;
    for (char c : s) {
        if (c == '.') { if (!cur.empty()) segs.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) segs.push_back(cur);
    return segs;
}

/// Read a forced Value as a plain path/store-path string.  Accepts String
/// (the usual `.drvPath` / `.outPath` shape) and Path.  Returns "" for other
/// tags (caller decides whether that is an error).
std::string valueToPathString(const Value & v)
{
    if (v.tag() == Tag::String && v.asString()) return std::string(v.asString());
    if (v.tag() == Tag::Path && v.asPath()) return std::string(v.asPath());
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// root eval
// ---------------------------------------------------------------------------

namespace {

/// Shared core of evalFlakeRoot / evalExprRoot: build the handle, run `src`
/// through the v3 pipeline, force the root to WHNF, optionally descend a
/// dotted `fragment`, optionally apply a `--select` lambda.
EvalJobsHandlePtr makeHandleFromSource(nix::EvalState & state,
                                       const std::string & src,
                                       const std::string & fragment,
                                       const std::string & selectExpr,
                                       bool guardExprRoot = false)
{
    EvalJobsHandlePtr h(new EvalJobsHandle());
    h->vm = std::make_unique<VMState>();
    // Register the two long-lived Value slots as GC roots for the handle's
    // (== worker's) lifetime.  root/jobValue default to w==0 (a Float, no
    // pointer) so the guards are safe even before the first eval.
    h->rootGuard = std::make_unique<GcRoot>(h->root);
    h->jobGuard  = std::make_unique<GcRoot>(h->jobValue);

    RootResult rr = runRootExprFromString(state, src);
    h->rootCu = std::move(rr.cu);

    // Synthetic outer frame so dispatchLoop's `vm.frames.back().cu` is valid
    // for the descent/extraction forces (mirrors eval.cc:runV3DirectEval).
    h->vm->frames.push_back(CallFrame{
        .cu = h->rootCu.get(), .closure = nullptr, .thunk = nullptr,
        .ip = h->rootCu->entryOffset, .stackBaseOffset = 0,
        .withStackBase = 0, .flags = 0,
    });

    h->root = forceValue(*h->vm, rr.value);

    // Fragment descent (`#hydraJobs`, `#packages.aarch64-darwin`, …).
    for (const auto & seg : splitDot(fragment)) {
        Value cur = h->root;
        if (!cur.isAttrs() || !cur.asAttrs())
            throw nix::Error(
                "v3 flake root: fragment segment '" + seg + "' is not under an attrset");
        auto sid = ir::globalInternSymbol(seg);
        const Value * found = cur.asAttrs()->lookup(sid);
        if (!found)
            throw nix::Error("v3 flake root: fragment attribute '" + seg + "' not found");
        h->root = forceValue(*h->vm, *found);
    }

    // Root-shape guard for the `--expr` route — MUST run on the PRE-select
    // root (RR6-S6 / RR9-1, 2026-07-22).  The tree-walker's
    // releaseExprTopLevelValue auto-calls the RAW expression root
    // (autoCallFunction: CALLS a lambda root with all-defaulted formals,
    // unwraps a functor attrset) BEFORE it applies `--select`; if v3 applied
    // `--select` first and then inspected the post-select root, a select
    // function that tolerates a function argument would yield a DIFFERENT
    // traversal root with no error (unhealable — the divergence is in the
    // per-worker root, not a per-job throw).  Those auto-call semantics are
    // TW-only, so throw here (→ worker engagement fallback to the tree-walker)
    // for exact parity.  An attrset root (Hydra's hydraJobs) is a no-op under
    // autoCallFunction with empty autoArgs, so v3 proceeds for that shape.
    if (guardExprRoot) {
        Value root = h->root;
        bool needsTw = root.tag() == Tag::Closure;
        if (!needsTw && root.isAttrs() && root.asAttrs()) {
            static const SymbolId functorId = ir::globalInternSymbol("__functor");
            needsTw = root.asAttrs()->lookup(functorId) != nullptr;
        }
        if (needsTw)
            throw nix::Error(
                "v3 expr root is a function/functor — tree-walker "
                "autoCallFunction semantics required");
    }

    // --select: evaluate the lambda and apply it to the (post-fragment)
    // root; force the result to WHNF (mirrors initializeRootValue's
    // callFunction + forceAttrs).
    if (!selectExpr.empty()) {
        RootResult sr = runRootExprFromString(state, selectExpr);
        h->selectCu = std::move(sr.cu);
        Value selFn = forceValue(*h->vm, sr.value);
        Value applied = callClosure(*h->vm, selFn, h->root);
        h->root = forceValue(*h->vm, applied);
        // Review CR6-D5 (2026-07-22): the tree-walker's initializeRootValue
        // does `forceAttrs` on the --select result and hard-fails if it is
        // not an attrset ("'--select' must evaluate to an attrset").  Without
        // this check v3 would engage on a non-attrset select result and emit
        // an empty jobset with exit 0.  Throw so the worker's engagement
        // fallback reproduces the tree-walker's failure.
        if (!h->root.isAttrs() || !h->root.asAttrs())
            throw nix::Error(
                "'--select' must evaluate to an attrset (the traversal root)");
    }

    return h;
}

} // namespace

EvalJobsHandlePtr evalFlakeRoot(nix::EvalState & state,
                                const std::string & lockedFlakeRef,
                                const std::string & fragment,
                                const std::string & selectExpr)
{
    // getFlake — the v3-native flake root (byte-identical drvPath to TW's
    // callFlake, verified 2026-07-18).  The locked, rev-pinned ref pins
    // the content so this resolves to the same flake the caller locked.
    std::string src = "builtins.getFlake \"" + escapeNixString(lockedFlakeRef) + "\"";
    return makeHandleFromSource(state, src, fragment, selectExpr);
}

EvalJobsHandlePtr evalExprRoot(nix::EvalState & state,
                               const std::string & exprSrc,
                               const std::string & selectExpr)
{
    // The `--expr` route — THE shape Hydra actually uses (review 2026-07-20;
    // hydra-eval-jobset passes `--expr 'let flake = builtins.getFlake
    // (toString "<locked-url>"); in flake.hydraJobs or flake.checks or
    // (throw …)'`, never `--flake`).  v3 evaluates the whole expression
    // natively (getFlake is the sole v3-native impl; `or`/`throw` are
    // ordinary v3 evaluation).  Hydra pre-locks the URL via `nix flake
    // metadata`, so drvPath identity is preserved without any
    // InstallableFlake machinery.
    // The root-shape guard runs INSIDE makeHandleFromSource on the pre-select
    // root (guardExprRoot=true) — see the rationale there (RR6-S6/RR9-1).
    return makeHandleFromSource(state, exprSrc, "", selectExpr,
                                /*guardExprRoot=*/true);
}

// ---------------------------------------------------------------------------
// descend
// ---------------------------------------------------------------------------

void descendAttrPath(EvalJobsHandle & h, const std::vector<std::string> & path)
{
    VMState & vm = *h.vm;
    // Start from the (rooted) flake root; write progress into the rooted job
    // slot so every intermediate is forwarded across scavenges.
    h.jobValue = forceValue(vm, h.root);
    for (const auto & seg : path) {
        // Numeric-segment parity (RR6-S1 / RR1-F7, 2026-07-22): TW's
        // findAlongAttrPath parses every path segment with
        // `string2Int<unsigned int>` and, when it parses, treats the segment
        // as a LIST INDEX — hard-erroring "should be a list but is a set" when
        // the current value is an attrset.  nix-eval-jobs only ever produces a
        // numeric segment from a numeric ATTR NAME while recursing an attrset
        // (collectAttrsForRecursion never descends lists), so TW ALWAYS errors
        // on such a segment.  v3 would instead look the name up and silently
        // emit a job — a divergence the per-job TW-retry net (CR7-R1) cannot
        // heal because v3 doesn't throw.  Mirror TW: throw so the worker
        // retries on the tree-walker and reproduces TW's error bytes exactly.
        //
        // We call the SAME nix::string2Int<unsigned int> TW uses, so the
        // "is this a list index?" classification is byte-parity-exact.  There
        // is deliberately no explicit header include for it: the
        // lint-no-direct-tw-include V3-native ratchet forbids adding a new
        // nix/ header include to this file, and the header-only template is
        // transitively visible via ffi.hh (the same path this file already
        // relies on for nix::Error / nix::EvalState).  Do NOT add a nix/ header
        // include to "fix" this — it will fail that lint.
        if (nix::string2Int<unsigned int>(seg))
            throw nix::Error(
                "v3 attrPath: numeric segment '" + seg + "' is a list index "
                "under the tree-walker — tree-walker semantics required");
        // Re-read the GC-rooted job slot on every step.  A `lookup` below can
        // realise a MapAttrs entry and allocate; snapshotting the Bindings*
        // into an unrooted local would dangle the moment a scavenge relocates
        // it (safe today only because the scavenger never fires at accessor
        // depth — gc_root.hh's Stage-6 caveat — but the rooted slot is
        // unconditionally correct; RR6-L1 / RR1-F5, 2026-07-22).
        if (!h.jobValue.isAttrs() || !h.jobValue.asAttrs())
            // A non-attrset intermediate (a lambda, functor-less scalar, …):
            // the tree-walker's findAlongAttrPath would autoCallFunction a
            // lambda here and descend into its result.  We can't mirror that,
            // so error — the worker's per-job TW retry (review CR7-R1) then
            // re-descends correctly.
            throw nix::Error("v3 attrPath: '" + seg + "' is not under an attrset");
        // Review CR6-D1 (2026-07-22): a `__functor` attrset at an INTERMEDIATE
        // node is the silent-wrong-derivation hazard.  findAlongAttrPath
        // autoCallFunction-unwraps it and looks `seg` up in the CALL RESULT;
        // a raw lookup here would find `seg` in the functor's own attrs (a
        // DIFFERENT, possibly shadowing value) with NO error.  Detect it
        // (non-forcing presence probe) and throw so the worker retries this
        // job on the tree-walker, which unwraps correctly.
        static const SymbolId functorId = ir::globalInternSymbol("__functor");
        if (h.jobValue.asAttrs()->lookup(functorId))
            throw nix::Error(
                "v3 attrPath: intermediate node '" + seg +
                "' is a __functor attrset — tree-walker autoCall required");
        auto sid = ir::globalInternSymbol(seg);
        const Value * found = h.jobValue.asAttrs()->lookup(sid);
        if (!found)
            throw nix::Error("v3 attrPath: attribute '" + seg + "' not found");
        h.jobValue = forceValue(vm, *found);   // back into the rooted slot
    }
}

// ---------------------------------------------------------------------------
// detect / discover
// ---------------------------------------------------------------------------

bool isAttrs(EvalJobsHandle & h)
{
    // descendAttrPath already forced the job value to WHNF.
    return h.jobValue.isAttrs() && h.jobValue.asAttrs() != nullptr;
}

bool isDerivation(EvalJobsHandle & h)
{
    VMState & vm = *h.vm;
    Value v = h.jobValue;
    if (!v.isAttrs() || !v.asAttrs()) return false;
    static const SymbolId typeId = ir::globalInternSymbol("type");
    const Value * t = v.asAttrs()->lookup(typeId);
    if (!t) return false;
    // NOTE (comment corrected 2026-07-20): forcing CAN allocate here (a
    // thunked `.type`, and even `lookup` realises MapAttrs entries).  Safety
    // does not depend on "no allocation": `*t` is read into forceValue's
    // by-value argument at call setup, and neither `v` nor `t` is used
    // after the force — only the returned `tv`.
    Value tv = forceValue(vm, *t);
    return tv.tag() == Tag::String && tv.asString()
        && std::string_view(tv.asString()) == "derivation";
}

std::vector<std::string> childAttrNames(EvalJobsHandle & h, bool & recurse,
                                        bool forceRecurse)
{
    VMState & vm = *h.vm;
    std::vector<std::string> attrs;
    Value v = h.jobValue;
    if (!v.isAttrs() || !v.asAttrs()) return attrs;

    const auto & symTab = ir::globalSymbolTable();
    Bindings * b = v.asAttrs();
    // Name-only walk (no MapAttrs realisation, no allocation → `b` stays put).
    b->forEachName([&](SymbolId id) {
        attrs.emplace_back(id < symTab.size()
            ? std::string(symTab[id]) : std::string());
    });
    // TW's `attrs()->lexicographicOrder` orders by the symbol's STRING name;
    // v3 Bindings are ordered by SymbolId, so re-sort by name.
    std::sort(attrs.begin(), attrs.end());

    // recurseForDerivations (value-side part of collectAttrsForRecursion).
    // Parity (review F3, 2026-07-20): the tree-walker (a) does NOT even force
    // the attribute under --force-recurse (`!args.forceRecurse &&` guards the
    // whole read), and (b) HARD-ERRORS via forceBool when it is present but
    // not a Boolean.  Mirror both: skip entirely under forceRecurse; throw on
    // a non-bool (the worker turns it into a per-job Error, exactly like TW).
    if (!forceRecurse) {
        static const SymbolId rfdId = ir::globalInternSymbol("recurseForDerivations");
        // NOTE (comment corrected 2026-07-20): lookup is NOT always
        // alloc-free (MapAttrs realisation allocates) — but `b` was
        // re-derived from the rooted `v` above with no intervening force,
        // and `*rfd` is read into the by-value force argument at call
        // setup; nothing uses `b`/`rfd` after the force.
        const Value * rfd = b->lookup(rfdId);
        if (rfd) {
            Value rv = forceValue(vm, *rfd);
            if (!rv.isBool())
                throw nix::Error(
                    "while evaluating the `recurseForDerivations` attribute: "
                    "value is not a Boolean");
            recurse = (rv.asInt() == 1);
        }
    }
    return attrs;
}

bool jobNeedsTreeWalker(EvalJobsHandle & h, bool wantConstituents)
{
    // Review F2+F4 (2026-07-20): three job shapes whose tree-walker semantics
    // are NOT mirrored by the v3 accessors; the worker routes them through the
    // original TW path (lazy TW root) for exact parity instead of silently
    // diverging.  All checks read the GC-rooted job slot directly: `lookup`
    // can realise a MapAttrs entry and allocate, so re-read h.jobValue.asAttrs()
    // at each step rather than caching an unrooted Bindings* (RR6-L1/RR1-F5,
    // 2026-07-22 — corrects the earlier "alloc-free" claim; safe today only
    // because the scavenger never fires at accessor depth, but the rooted slot
    // is unconditionally correct if that ever changes).
    //
    //  1. lambda job values: TW's per-job autoCallFunction CALLS a lambda with
    //     all-defaulted formals (descending into its result) and hard-errors
    //     (MissingArgumentError) on non-defaulted formals; v3 would silently
    //     classify the closure as "not buildable" and drop it.
    //  2. functor attrsets (`__functor`): autoCallFunction unwraps them
    //     unconditionally (applies __functor to self, recurses on the result)
    //     even with empty autoArgs; the v3 path would treat them as a plain
    //     attrset and recurse into their attribute names.
    //  3. aggregate jobs (`_hydraAggregate`) under --constituents: constituent
    //     extraction needs coerceToString-with-context on a TW Value; emitting
    //     the aggregate WITHOUT its constituents would silently produce empty
    //     aggregate builds in Hydra — the worst failure mode.
    if (h.jobValue.tag() == Tag::Closure) return true;                 // (1)
    if (h.jobValue.isAttrs() && h.jobValue.asAttrs()) {
        static const SymbolId functorId = ir::globalInternSymbol("__functor");
        if (h.jobValue.asAttrs()->lookup(functorId)) return true;      // (2)
        if (wantConstituents) {
            static const SymbolId aggId = ir::globalInternSymbol("_hydraAggregate");
            if (h.jobValue.asAttrs()->lookup(aggId)) return true;      // (3)
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// extract
// ---------------------------------------------------------------------------

std::string drvPath(EvalJobsHandle & h)
{
    VMState & vm = *h.vm;
    Value v = h.jobValue;
    if (!v.isAttrs() || !v.asAttrs())
        throw nix::Error("v3 drvPath: value is not an attrset");
    static const SymbolId dpId = ir::globalInternSymbol("drvPath");
    const Value * dp = v.asAttrs()->lookup(dpId);
    if (!dp)
        throw nix::Error("derivation does not contain a 'drvPath' attribute");
    Value dv = forceValue(vm, *dp);
    std::string s = valueToPathString(dv);
    if (s.empty())
        throw nix::Error("v3 drvPath: 'drvPath' did not evaluate to a store path");
    return s;
}

std::string name(EvalJobsHandle & h)
{
    VMState & vm = *h.vm;
    Value v = h.jobValue;
    if (!v.isAttrs() || !v.asAttrs())
        throw nix::Error("v3 name: value is not an attrset");
    static const SymbolId nameId = ir::globalInternSymbol("name");
    const Value * n = v.asAttrs()->lookup(nameId);
    if (!n)
        throw nix::Error("derivation name missing");
    Value nv = forceValue(vm, *n);
    if (nv.tag() != Tag::String || !nv.asString())
        throw nix::Error("v3 name: 'name' did not evaluate to a string");
    // TW's queryName uses forceStringNoCtx (RR6-S3, 2026-07-22): a `name`
    // carrying string context is a hard error ("is not allowed to refer to a
    // store path").  v3 keeps context in a side table the Tag::String value
    // doesn't carry inline — consult it and throw so the TW-retry net
    // reproduces TW's rejection instead of silently emitting the job.
    if (auto * ctx = lookupStringContextEntries(nv.asString()); ctx && !ctx->empty())
        throw nix::Error(
            "v3 name: the 'name' attribute is not allowed to refer to a store path");
    return std::string(nv.asString());
}

std::string system(EvalJobsHandle & h)
{
    VMState & vm = *h.vm;
    Value v = h.jobValue;
    if (!v.isAttrs() || !v.asAttrs()) return "unknown";
    static const SymbolId sysId = ir::globalInternSymbol("system");
    const Value * s = v.asAttrs()->lookup(sysId);
    if (!s) return "unknown";                   // mirrors querySystem's default
    Value sv = forceValue(vm, *s);
    // TW's querySystem uses forceStringNoCtx when `system` is PRESENT (RR6-S4,
    // 2026-07-22): a present-but-non-string (or context-carrying) `system` is
    // a hard error, not a silent "unknown".  Only reached in read-only-store
    // mode (Hydra's local-store deploy takes the readDerivation branch and
    // never calls this), but throw for parity so the TW-retry net matches.
    if (sv.tag() != Tag::String || !sv.asString())
        throw nix::Error(
            "while evaluating the 'system' attribute of a derivation: "
            "value is not a string");
    if (auto * ctx = lookupStringContextEntries(sv.asString()); ctx && !ctx->empty())
        throw nix::Error(
            "while evaluating the 'system' attribute of a derivation: "
            "the value is not allowed to refer to a store path");
    return std::string(sv.asString());
}

std::vector<std::pair<std::string, std::string>> outputs(EvalJobsHandle & h)
{
    VMState & vm = *h.vm;
    std::vector<std::pair<std::string, std::string>> result;

    Value v = h.jobValue;
    if (!v.isAttrs() || !v.asAttrs()) return result;

    static const SymbolId outputsId = ir::globalInternSymbol("outputs");
    static const SymbolId outPathId = ir::globalInternSymbol("outPath");

    // Mirror PackageInfo::queryOutputs(withPaths=true) decision-for-decision
    // (RR6-S2 / RR1-F3, 2026-07-22).  The prior version was SHAPE-LAX where TW
    // is strict — it silently skipped malformed entries and fell back to the
    // top-level outPath — producing a job with wrong/partial/empty outputs
    // where TW hard-errors (and an all-empty `"outputs":{}` even trips Hydra's
    // own `die unless scalar @outputNames`).  A throw here is healed by the
    // worker's per-job TW-retry (CR7-R1) into byte-exact TW output.
    const Value * outsV = h.jobValue.asAttrs()->lookup(outputsId);
    if (!outsV) {
        // No `.outputs` attribute → single "out" from the top-level
        // `.outPath` (queryOutputs' else-branch → queryOutPath, which THROWS
        // "derivation does not have attribute 'outPath'" when it is absent).
        const Value * opV = h.jobValue.asAttrs()->lookup(outPathId);
        if (!opV)
            throw nix::Error("derivation does not have attribute 'outPath'");
        Value op = forceValue(vm, *opV);
        std::string s = valueToPathString(op);
        if (s.empty())
            throw nix::Error(
                "while evaluating the output path of a derivation: "
                "value is not coercible to a store path");
        result.emplace_back("out", std::move(s));
        return result;
    }

    // `.outputs` present: TW's forceList THROWS if it is not a list.  Register
    // the by-value list holder so a scavenge fired inside a per-element force
    // rewrites `outs` in place (the minor scavenger walks the GcRoot registry),
    // and RE-READ asList() each iteration rather than caching the ListVec*.
    Value outs = forceValue(vm, *outsV);
    V3_GC_ROOT(outs);
    if (outs.tag() != Tag::List || !outs.asList())
        throw nix::Error(
            "while evaluating the 'outputs' attribute of a derivation: "
            "value is not a list");

    // Collect output NAMES.  TW's forceStringNoCtx THROWS on a non-string
    // element and on a context-carrying string; mirror both (into std::strings
    // so we hold no v3 pointer across the per-output forces below).
    std::vector<std::string> names;
    for (uint32_t i = 0; i < outs.asList()->size; ++i) {
        Value e = forceValue(vm, outs.asList()->elems[i]);
        if (e.tag() != Tag::String || !e.asString())
            throw nix::Error(
                "while evaluating the name of an output of a derivation: "
                "value is not a string");
        if (auto * ctx = lookupStringContextEntries(e.asString()); ctx && !ctx->empty())
            throw nix::Error(
                "while evaluating the name of an output of a derivation: "
                "the name is not allowed to refer to a store path");
        names.emplace_back(e.asString());
    }

    for (const auto & o : names) {
        // Re-read the (rooted, forwarded) job value each iteration; the prior
        // force may have relocated its Bindings.
        auto oid = ir::globalInternSymbol(o);
        const Value * subV = h.jobValue.asAttrs()->lookup(oid);
        if (!subV) continue;                    // TW: `continue` (missing output)
        Value sub = forceValue(vm, *subV);
        // TW's forceAttrs THROWS if the output is not an attrset.
        if (!sub.isAttrs() || !sub.asAttrs())
            throw nix::Error(
                "while evaluating an output of a derivation: value is not an attrset");
        const Value * opV = sub.asAttrs()->lookup(outPathId);
        if (!opV) continue;                     // TW: `continue` (missing outPath)
        Value op = forceValue(vm, *opV);
        std::string s = valueToPathString(op);
        // TW's coerceToStorePath THROWS on a non-coercible outPath.  The
        // common String/Path shape coerces directly here; the rarer
        // coercible-attrset shape is healed by the TW-retry net.
        if (s.empty())
            throw nix::Error(
                "while evaluating an output path of a derivation: "
                "value is not coercible to a store path");
        result.emplace_back(o, std::move(s));
    }
    return result;
}

namespace {

/// Mirror of `PackageInfo::checkMeta` (libexpr get-drvs.cc): force `v`, then
/// accept ONLY Int/Bool/String/Float scalars, lists whose every element
/// passes, and attrsets (recursively) that do NOT carry an `outPath`
/// attribute.  Everything else — null, paths, functions, externals, and
/// especially DERIVATION attrsets in meta (`meta.tests = { basic = <drv>; }`,
/// common in nixpkgs/haskell.nix; the `outPath` rejection catches them
/// BEFORE any deep force) — is filtered out, exactly like the tree-walker.
/// Force errors PROPAGATE (TW's checkMeta force does too → the whole job
/// becomes a per-job eval Error).
///
/// GC discipline (corrected by the 2026-07-20 accessor audit): there is NO
/// conservative C-stack pinning in v3's minor scavenge — safety comes from
/// (a) the exitDepth gate (accessor forces run at exitDepth==1, where the
/// scavenger currently never fires) and, robustly, (b) V3_GC_ROOT
/// registration + the minor scavenger's GcRoot-registry walk (added in the
/// same review), which REWRITES the registered holder in place; we then
/// re-read `v.asList()` / `v.asAttrs()` per iteration.  Attr ids are
/// collected first, then re-looked-up per id after each recursive force.
bool checkMetaV3(VMState & vm, Value v, unsigned depth = 0)
{
    // Review CR6-D8 (2026-07-22): the tree-walker's checkMeta recurses under
    // `addCallDepth`, so pathologically-deep meta (e.g. `meta.x = foldl' (a: _:
    // { inherit a; }) {} (range 1 200000)`) throws a StackOverflowError and
    // nix-eval-jobs reports a deterministic per-job (fatal) error.  Unbounded
    // C++ recursion here would instead overflow the native stack and CRASH the
    // whole worker.  Bound it with the same ceiling as the VM's call-depth
    // guard and throw the same type so the classification matches TW.
    if (depth >= 10000)
        throw CallDepthError(
            "v3 checkMeta: stack overflow; meta nesting depth exceeded 10000");
    v = forceValue(vm, v);
    // Register the container holder for the recursive branches below: the
    // recursive checks force (and may scavenge — the minor scavenger walks
    // the GcRoot registry, review 2026-07-20), and we re-read
    // v.asList()/v.asAttrs() per iteration so a forwarded container is
    // picked up from the rewritten slot.
    V3_GC_ROOT(v);
    if (v.tag() == Tag::Int || v.tag() == Tag::Bool
        || v.tag() == Tag::String || v.tag() == Tag::Float)
        return true;
    if (v.tag() == Tag::List && v.asList()) {
        for (uint32_t i = 0; i < v.asList()->size; ++i)
            if (!checkMetaV3(vm, v.asList()->elems[i], depth + 1)) return false;
        return true;
    }
    if (v.isAttrs() && v.asAttrs()) {
        static const SymbolId outPathId = ir::globalInternSymbol("outPath");
        if (v.asAttrs()->lookup(outPathId)) return false;   // drv-in-meta
        std::vector<SymbolId> ids;
        v.asAttrs()->forEachName([&](SymbolId id) { ids.push_back(id); });
        for (SymbolId id : ids) {
            const Value * p = v.asAttrs()->lookup(id);      // re-read post-force
            if (!p || !checkMetaV3(vm, *p, depth + 1)) return false;
        }
        return true;
    }
    return false;   // Null / Path / Closure / PrimOp / External / …
}

} // namespace

std::optional<nlohmann::json> meta(EvalJobsHandle & h)
{
    // Mirror PackageInfo::queryMeta EXACTLY: it ALWAYS returns an (engaged)
    // json — a JSON `null` when there is no serialisable meta, an object
    // otherwise.  fromPackageInfo then always sets `result.meta` under
    // --meta, so the tree-walker emits `"meta":null` for a derivation with no
    // `.meta` (or an unserialisable one).  We start from `null` and let the
    // first `res[name] = …` promote it to an object, matching queryMeta's
    // `nlohmann::json meta_;` accumulation.  Returning std::nullopt here would
    // OMIT the field and diverge from TW; we never do.
    //
    // Review F1 (2026-07-20): every attribute is gated through checkMetaV3
    // (TW's checkMeta filter) BEFORE serialisation.  The previous
    // catch-and-skip around toJsonValue was NOT equivalent: it serialised
    // null metas (TW skips them) and deep-forced + serialised derivation
    // attrsets in meta (TW rejects them on the `outPath` probe without deep
    // forcing).  With the filter in front, toJsonValue only ever sees
    // checkMeta-approved shapes, which serialise without throwing — so any
    // unexpected throw now propagates as a per-job error, exactly like TW.
    VMState & vm = *h.vm;
    Value v = h.jobValue;
    nlohmann::json res = nlohmann::json(nullptr);   // JSON null
    if (!v.isAttrs() || !v.asAttrs()) return res;

    static const SymbolId metaId = ir::globalInternSymbol("meta");
    const Value * mV = v.asAttrs()->lookup(metaId);
    if (!mV) return res;                            // no `.meta` → getMeta() null

    Value m = forceValue(vm, *mV);
    // Root the meta attrset: checkMetaV3/toJsonValue below deep-force and may
    // relocate it, and we re-look-up each entry from `m` after each force.
    V3_GC_ROOT(m);
    // Review CR6-D6 (2026-07-22): the tree-walker's PackageInfo::getMeta does
    // `forceAttrs(meta)`, which THROWS on a non-attrset `.meta` (→ per-job
    // error).  Returning null here instead diverged silently; throw so the
    // worker's TW retry reproduces TW's exact behavior.
    if (!m.isAttrs() || !m.asAttrs())
        throw nix::Error("v3 meta: the `meta` attribute is not an attribute set");

    const auto & symTab = ir::globalSymbolTable();
    std::vector<std::pair<std::string, SymbolId>> metaNames;
    m.asAttrs()->forEachName([&](SymbolId id) {
        if (id < symTab.size()) metaNames.emplace_back(symTab[id], id);
    });

    for (const auto & [nm, id] : metaNames) {
        {
            const Value * mvp = m.asAttrs()->lookup(id);   // re-read rooted `m`
            if (!mvp || !checkMetaV3(vm, *mvp)) continue;  // TW checkMeta filter
        }
        // checkMetaV3 forced through the value; re-read from the rooted `m`
        // (the forces may have relocated the previous lookup's referent).
        const Value * mvp = m.asAttrs()->lookup(id);
        if (!mvp) continue;
        res[nm] = toJsonValue(vm, *mvp, symTab);           // null → object on 1st add
    }
    // Review CR6-D10 (2026-07-22): a meta string with invalid UTF-8
    // (e.g. `meta.description = readFile ./latin1-file`) makes nlohmann's
    // dump() throw type_error.316.  The tree-walker serialises meta INSIDE
    // its per-job try (printValueAsJSON) so that surfaces as a per-job error;
    // v3 built the json here but the worker's first dump was `reply.dump()`
    // OUTSIDE the per-job try → uncaught → worker CRASH.  Force the dump here,
    // inside the per-job call path, converting the crash into a per-job error
    // (which the worker's TW retry then reproduces exactly).
    (void) res.dump();
    return res;
}

} // namespace nix::v3
