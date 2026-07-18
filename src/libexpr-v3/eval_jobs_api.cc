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
/// input yields an empty vector.  (Quoted segments — `a."b.c"` — are not
/// handled; job fragments in practice are simple dotted identifiers.)
std::vector<std::string> splitDot(const std::string & s)
{
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

EvalJobsHandlePtr evalFlakeRoot(nix::EvalState & state,
                                const std::string & lockedFlakeRef,
                                const std::string & fragment,
                                const std::string & selectExpr)
{
    EvalJobsHandlePtr h(new EvalJobsHandle());
    h->vm = std::make_unique<VMState>();
    // Register the two long-lived Value slots as GC roots for the handle's
    // (== worker's) lifetime.  root/jobValue default to w==0 (a Float, no
    // pointer) so the guards are safe even before the first eval.
    h->rootGuard = std::make_unique<GcRoot>(h->root);
    h->jobGuard  = std::make_unique<GcRoot>(h->jobValue);

    // 1. getFlake — the v3-native flake root (byte-identical drvPath to TW's
    //    callFlake, verified 2026-07-18).  The locked, rev-pinned ref pins
    //    the content so this resolves to the same flake the caller locked.
    std::string src = "builtins.getFlake \"" + escapeNixString(lockedFlakeRef) + "\"";
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

    // 2. Fragment descent (`#hydraJobs`, `#packages.aarch64-darwin`, …).
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

    // 3. --select: evaluate the lambda and apply it to the (post-fragment)
    //    root; force the result to WHNF (mirrors initializeRootValue's
    //    callFunction + forceAttrs).
    if (!selectExpr.empty()) {
        RootResult sr = runRootExprFromString(state, selectExpr);
        h->selectCu = std::move(sr.cu);
        Value selFn = forceValue(*h->vm, sr.value);
        Value applied = callClosure(*h->vm, selFn, h->root);
        h->root = forceValue(*h->vm, applied);
    }

    return h;
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
        Value cur = h.jobValue;                // rooted-slot snapshot
        if (!cur.isAttrs() || !cur.asAttrs())
            throw nix::Error("v3 attrPath: '" + seg + "' is not under an attrset");
        auto sid = ir::globalInternSymbol(seg);
        const Value * found = cur.asAttrs()->lookup(sid);
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
    // `.type` is a literal string in a derivation attrset — forcing it does
    // not allocate, so `v` cannot move here.
    Value tv = forceValue(vm, *t);
    return tv.tag() == Tag::String && tv.asString()
        && std::string_view(tv.asString()) == "derivation";
}

std::vector<std::string> childAttrNames(EvalJobsHandle & h, bool & recurse)
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
    static const SymbolId rfdId = ir::globalInternSymbol("recurseForDerivations");
    const Value * rfd = b->lookup(rfdId);       // alloc-free; `b` still valid
    if (rfd) {
        Value rv = forceValue(vm, *rfd);
        if (rv.isBool()) recurse = (rv.asInt() == 1);
    }
    return attrs;
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
    if (sv.tag() != Tag::String || !sv.asString()) return "unknown";
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

    // First collect the output NAMES (into std::strings, so we hold no v3
    // pointer across the per-output forces below).
    std::vector<std::string> names;
    {
        const Value * outsV = h.jobValue.asAttrs()->lookup(outputsId);
        if (outsV) {
            Value outs = forceValue(vm, *outsV);
            if (outs.tag() == Tag::List && outs.asList()) {
                ListVec * lv = outs.asList();
                for (uint32_t i = 0; i < lv->size; ++i) {
                    Value e = forceValue(vm, lv->elems[i]);
                    if (e.tag() == Tag::String && e.asString())
                        names.emplace_back(e.asString());
                }
            }
        }
    }

    if (names.empty()) {
        // No `.outputs` list → single "out" whose path is the top-level
        // `.outPath` (mirrors queryOutputs' else-branch → queryOutPath).
        const Value * opV = h.jobValue.asAttrs()->lookup(outPathId);
        if (opV) {
            Value op = forceValue(vm, *opV);
            std::string s = valueToPathString(op);
            if (!s.empty()) result.emplace_back("out", std::move(s));
        }
        return result;
    }

    for (const auto & o : names) {
        // Re-read the (rooted, forwarded) job value each iteration; the prior
        // force may have relocated its Bindings.
        auto oid = ir::globalInternSymbol(o);
        const Value * subV = h.jobValue.asAttrs()->lookup(oid);
        if (!subV) continue;                    // mirrors queryOutputs' `continue`
        Value sub = forceValue(vm, *subV);
        if (!sub.isAttrs() || !sub.asAttrs()) continue;
        const Value * opV = sub.asAttrs()->lookup(outPathId);
        if (!opV) continue;
        Value op = forceValue(vm, *opV);
        std::string s = valueToPathString(op);
        if (s.empty()) continue;
        result.emplace_back(o, std::move(s));
    }
    return result;
}

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
    VMState & vm = *h.vm;
    Value v = h.jobValue;
    nlohmann::json res = nlohmann::json(nullptr);   // JSON null
    if (!v.isAttrs() || !v.asAttrs()) return res;

    static const SymbolId metaId = ir::globalInternSymbol("meta");
    const Value * mV = v.asAttrs()->lookup(metaId);
    if (!mV) return res;                            // no `.meta` → getMeta() null

    Value m = forceValue(vm, *mV);
    // Root the meta attrset: toJsonValue below deep-forces and may relocate
    // it, and we re-look-up each entry from `m` after each serialisation.
    V3_GC_ROOT(m);
    if (!m.isAttrs() || !m.asAttrs()) return res;   // .meta not an attrset → null

    const auto & symTab = ir::globalSymbolTable();
    std::vector<std::pair<std::string, SymbolId>> metaNames;
    m.asAttrs()->forEachName([&](SymbolId id) {
        if (id < symTab.size()) metaNames.emplace_back(symTab[id], id);
    });

    for (const auto & [nm, id] : metaNames) {
        const Value * mvp = m.asAttrs()->lookup(id);   // re-read rooted `m`
        if (!mvp) continue;
        try {
            res[nm] = toJsonValue(vm, *mvp, symTab);   // null → object on 1st add
        } catch (const std::exception &) {
            // Non-serialisable (e.g. a function) — skip, as queryMeta's
            // checkMeta filter does.
        }
    }
    return res;
}

} // namespace nix::v3
