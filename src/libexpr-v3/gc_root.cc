/// @file
/// Stage 5 of the precise-root foundation — implementation.
///
/// See include/v3/gc_root.hh for the API + design rationale.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/gc_root.hh"
#include "v3/precise_root.hh"
#include "v3/value.hh"

namespace nix::v3 {

std::vector<Value *> & gcRootStack() noexcept
{
    thread_local std::vector<Value *> s;
    return s;
}

// S1.2: registry of growing Value-vector accumulators (GcRootVec).  Walked with
// CURRENT data()/size() each GC → realloc-safe + covers post-construct growth.
std::vector<std::vector<Value> *> & gcRootVecStack() noexcept
{
    thread_local std::vector<std::vector<Value> *> s;
    return s;
}

void walkCppStackRoots(RootVisitor & visitor) noexcept
{
    // Vector iteration is bottom-to-top of the LIFO push history;
    // for non-moving GC the order is irrelevant.  For a future
    // moving GC, walk-then-rewrite is correct either way because
    // each visitor.visitValue() call rewrites in place.
    for (Value * p : gcRootStack()) {
        if (p) visitor.visitValue(*p);
    }
    // GcRootVec: re-read each registered vector's CURRENT elements (data()/size()
    // at THIS instant — realloc-safe) and rewrite them in place.
    for (std::vector<Value> * v : gcRootVecStack()) {
        if (!v) continue;
        for (Value & e : *v) visitor.visitValue(e);
    }
}

GcRoot::GcRoot(Value & v) noexcept
    : registered_(&v)
{
    gcRootStack().push_back(&v);
}

GcRoot::GcRoot(Value * p) noexcept
    : registered_(p)
{
    gcRootStack().push_back(p);
}

GcRootRange::GcRootRange(Value * data, size_t n) noexcept
    : n_(data ? n : 0)
{
    auto & s = gcRootStack();
    s.reserve(s.size() + n_);
    for (size_t i = 0; i < n_; ++i) s.push_back(&data[i]);
}

GcRootRange::~GcRootRange() noexcept
{
    // LIFO: pop exactly the n_ entries we pushed (defensive against an empty
    // stack, mirroring GcRoot::~GcRoot).
    auto & s = gcRootStack();
    for (size_t i = 0; i < n_ && !s.empty(); ++i) s.pop_back();
}

GcRootVec::GcRootVec(std::vector<Value> & v) noexcept
{
    gcRootVecStack().push_back(&v);
}

GcRootVec::~GcRootVec() noexcept
{
    auto & s = gcRootVecStack();
    if (!s.empty()) s.pop_back();
}

GcRoot::~GcRoot() noexcept
{
    // Pop OUR OWN entry (review CR5-#3, 2026-07-22).  The common case — a
    // single stack-scoped GcRoot, or a handle's root/job guards destructed in
    // reverse construction order — is the stack top, an O(1) pop.  The
    // safety-critical case is out-of-order / heap-held destruction (two live
    // EvalJobsHandles, or replace-before-destroy): blindly popping the top
    // there would drop SOMEONE ELSE's live slot and leave ours dangling for
    // the minor scavenger's registry walk to dereference.  Find-and-erase our
    // own `registered_` handles every ordering.  The registry is tiny (a
    // handful of long-lived + a few scoped temporaries), so the rare
    // not-on-top search is negligible.
    auto & s = gcRootStack();
    if (s.empty()) return;
    if (s.back() == registered_) { s.pop_back(); return; }
    for (auto it = s.end(); it != s.begin(); ) {
        --it;
        if (*it == registered_) { s.erase(it); return; }
    }
    // Not found: our entry was already removed (double-pop upstream) — nothing
    // to do; never blindly pop a stranger's slot.
}

} // namespace nix::v3
