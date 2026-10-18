#!/usr/bin/env bash

source common.sh

# shellcheck disable=SC1111
needLocalStore "“min-free” and “max-free” are daemon options"

TODO_NixOS

clearStore

# shellcheck disable=SC2034
garbage1=$(nix store add-path --name garbage1 ./nar-access.sh)
# shellcheck disable=SC2034
garbage2=$(nix store add-path --name garbage2 ./nar-access.sh)
# shellcheck disable=SC2034
garbage3=$(nix store add-path --name garbage3 ./nar-access.sh)

ls -l "$garbage3"
POSIXLY_CORRECT=1 du "$garbage3"

fake_free=$TEST_ROOT/fake-free
export _NIX_TEST_FREE_SPACE_FILE=$fake_free
echo 1100 > "$fake_free"

fifoLock=$TEST_ROOT/fifoLock
mkfifo "$fifoLock"

expr=$(cat <<EOF
with import ${config_nix}; mkDerivation {
  name = "gc-A";
  buildCommand = ''
    set -x
    [[ \$(ls \$NIX_STORE/*-garbage? | wc -l) = 3 ]]

    mkdir \$out
    echo foo > \$out/bar

    # Pretend that we run out of space
    echo 100 > ${fake_free}.tmp1
    mv ${fake_free}.tmp1 $fake_free

    # Wait for the GC to run
    for i in {1..20}; do
        echo ''\${i}...
        if [[ \$(ls \$NIX_STORE/*-garbage? | wc -l) = 1 ]]; then
            exit 0
        fi
        sleep 1
    done
    exit 1
  '';
}
EOF
)

expr2=$(cat <<EOF
with import ${config_nix}; mkDerivation {
  name = "gc-B";
  buildCommand = ''
    set -x
    mkdir \$out
    echo foo > \$out/bar

    # Wait for the first build to finish
    cat "$fifoLock"
  '';
}
EOF
)

nix build --impure -v -o "$TEST_ROOT"/result-A -L --expr "$expr" \
    --min-free 1K --max-free 2K --min-free-check-interval 1 &
pid1=$!

nix build --impure -v -o "$TEST_ROOT"/result-B -L --expr "$expr2" \
    --min-free 1K --max-free 2K --min-free-check-interval 1 &
pid2=$!

# Once the first build is done, unblock the second one.
# If the first build fails, we need to postpone the failure to still allow
# the second one to finish
wait "$pid1" || FIRSTBUILDSTATUS=$?
echo "unlock" > "$fifoLock"
( exit "${FIRSTBUILDSTATUS:-0}" )
wait "$pid2"

[[ foo = $(cat "$TEST_ROOT"/result-A/bar) ]]
[[ foo = $(cat "$TEST_ROOT"/result-B/bar) ]]

# A queued auto-GC must recheck available space after it acquires the global
# GC lock. Otherwise, a request that waited behind another collector can use
# a stale low-space snapshot and delete paths after space has been restored.
clearStore
nix-store --init

staleGarbage1=$(nix store add-path --name stale-garbage1 ./nar-access.sh)
staleGarbage2=$(nix store add-path --name stale-garbage2 ./nar-access.sh)
echo 1100 > "$fake_free"

gcSync=$TEST_ROOT/stale-gc-sync
gcRelease=$TEST_ROOT/stale-gc-release
gcWriterReady=$TEST_ROOT/stale-gc-writer-ready
mkfifo "$gcSync"
mkfifo "$gcRelease"
gcHolderLog=$TEST_ROOT/stale-gc-holder.log
(_NIX_TEST_GC_SYNC_2=$gcSync nix-store --gc --print-dead >"$gcHolderLog" 2>&1) &
gcHolderPid=$!
(
    exec 9>"$gcSync"
    touch "$gcWriterReady"
    cat "$gcRelease" >&9
) &
gcWriterPid=$!
staleBuildPid=
releaseStaleGC() {
    if [[ -n ${gcWriterPid:-} ]]; then
        printf 'continue\n' >"$gcRelease" || true
        wait "$gcWriterPid" || true
        gcWriterPid=
    fi
    if [[ -n ${gcHolderPid:-} ]]; then
        wait "$gcHolderPid" || true
        gcHolderPid=
    fi
}
cleanupStaleGC() {
    if [[ -n ${gcWriterPid:-} && ! -e $gcWriterReady ]]; then
        kill "$gcWriterPid" 2>/dev/null || true
        wait "$gcWriterPid" 2>/dev/null || true
        gcWriterPid=
        kill "$gcHolderPid" 2>/dev/null || true
        wait "$gcHolderPid" 2>/dev/null || true
        gcHolderPid=
    else
        releaseStaleGC
    fi
    if [[ -n ${staleBuildPid:-} ]]; then
        kill "$staleBuildPid" 2>/dev/null || true
        wait "$staleBuildPid" 2>/dev/null || true
        staleBuildPid=
    fi
}
trap cleanupStaleGC EXIT

# The writer's open completes only after the collector has acquired the lock,
# started its roots server, and opened the hook FIFO. SYNC_2 lets concurrent
# builds register roots while the collector is parked.
for _ in {1..80}; do
    if [[ -e $gcWriterReady ]]; then
        break
    fi
    if ! kill -0 "$gcHolderPid" 2>/dev/null || ! kill -0 "$gcWriterPid" 2>/dev/null; then
        cat "$gcHolderLog"
        fail "could not establish the lock-held GC synchronization point"
    fi
    sleep 0.25
done
[[ -e $gcWriterReady ]] || fail "timed out waiting for the lock-held GC synchronization point"

staleExpr=$(cat <<EOF
with import ${config_nix}; mkDerivation {
  name = "gc-stale-request";
  buildCommand = ''
    for i in {1..120}; do
      if [[ \$(cat ${fake_free}) -ge 1500 ]]; then
        break
      fi
      sleep 0.25
    done
    [[ \$(cat ${fake_free}) -ge 1500 ]] || exit 1
    mkdir \$out
  '';
}
EOF
)

staleGCLog=$TEST_ROOT/stale-gc-build.log
echo 100 > "$fake_free"
nix build --impure -vvvvv -o "$TEST_ROOT"/stale-gc-result -L --expr "$staleExpr" \
    --min-free 1K --max-free 2K --min-free-check-interval 1 >"$staleGCLog" 2>&1 &
staleBuildPid=$!

for _ in {1..80}; do
    if grep -q 'waiting for the big garbage collector lock' "$staleGCLog"; then
        break
    fi
    if ! kill -0 "$staleBuildPid" 2>/dev/null; then
        cat "$staleGCLog"
        fail "stale auto-GC test build exited before queueing on the GC lock"
    fi
    sleep 0.25
done
grep -q 'waiting for the big garbage collector lock' "$staleGCLog" || {
    cat "$staleGCLog"
    fail "stale auto-GC request did not queue on the held GC lock"
}

# The collector is now queued with its original 100-byte snapshot. Restore
# space above min-free before releasing the holder.
echo 1500 > "$fake_free.tmp"
mv "$fake_free.tmp" "$fake_free"
releaseStaleGC
wait "$staleBuildPid"
staleBuildPid=

nix-store --check-validity "$staleGarbage1" "$staleGarbage2"

# Empty auto-GC results are shared between local-store processes. A new garbage
# path must survive a second process's recent empty result, while a manual GC
# and a full-space-band drop still collect it.
clearStore
nix-store --init
emptyGCCache=$NIX_STATE_DIR/auto-gc-empty-cache.json
rm -f "$emptyGCCache"
echo 100 > "$fake_free"

addCachePath() {
    local name=$1 log=$2 maxFree=${3:-2K} pathFile=$TEST_ROOT/$1.path
    NIX_DEBUG_AUTO_GC=1 nix \
        --option min-free 1K \
        --option max-free "$maxFree" \
        --option min-free-check-interval 0 \
        store add-path --name "$name" ./nar-access.sh >"$pathFile" 2>"$log"
    cat "$pathFile"
}

cacheFirstLog=$TEST_ROOT/auto-gc-empty-first.log
cacheFirst=$(addCachePath auto-gc-empty-first "$cacheFirstLog")
grep -q 'event=completed .*bytes_freed=0 paths=0' "$cacheFirstLog" || {
    cat "$cacheFirstLog"
    fail "first low-space auto-GC did not complete an empty scan"
}

# This invocation is a different process. It adds a distinct, unrooted path;
# verify that the first path survives the shared recent-empty shortcut.
cacheSecondLog=$TEST_ROOT/auto-gc-empty-second.log
cacheSecond=$(addCachePath auto-gc-empty-second "$cacheSecondLog")
grep -q 'event=cache-skip skip_reason=recent-empty' "$cacheSecondLog" || {
    cat "$cacheSecondLog"
    fail "second local-store process did not reuse the recent empty result"
}
nix-store --check-validity "$cacheFirst"

# Explicit GC remains authoritative even while the empty-result record is fresh.
nix-store --gc
if nix-store --check-validity "$cacheFirst" >/dev/null 2>&1; then
    fail "manual GC incorrectly honored the auto-GC empty-result cache"
fi

# Equality at 97 percent is outside the cooldown band and must trigger a scan.
cacheThresholdSeed=$(addCachePath auto-gc-empty-threshold-seed "$TEST_ROOT/auto-gc-empty-threshold-seed.log")
echo 97 > "$fake_free"
cacheThresholdLog=$TEST_ROOT/auto-gc-empty-threshold.log
cacheThreshold=$(addCachePath auto-gc-empty-threshold "$cacheThresholdLog")
grep -q 'event=completed .*paths=[1-9]' "$cacheThresholdLog" || {
    cat "$cacheThresholdLog"
    fail "auto-GC did not scan at exactly 97 percent of the recorded free space"
}
if nix-store --check-validity "$cacheThresholdSeed" >/dev/null 2>&1; then
    fail "the 97-percent threshold scan did not collect known garbage"
fi

# A changed GC target invalidates the cache even when the free-space value is
# inside the reuse band.
clearStore
nix-store --init
rm -f "$emptyGCCache"
echo 100 > "$fake_free"
cacheConfigSeed=$(addCachePath auto-gc-empty-config-seed "$TEST_ROOT/auto-gc-empty-config-seed.log")
cacheConfigLog=$TEST_ROOT/auto-gc-empty-config.log
cacheConfig=$(addCachePath auto-gc-empty-config-changed "$cacheConfigLog" 3K)
grep -q 'event=completed .*paths=[1-9]' "$cacheConfigLog" || {
    cat "$cacheConfigLog"
    fail "changed max-free configuration reused an incompatible empty result"
}
if nix-store --check-validity "$cacheConfigSeed" >/dev/null 2>&1; then
    fail "changed max-free scan did not collect known garbage"
fi

# Corrupt cache contents are treated as a miss, not as a reason to skip GC.
clearStore
nix-store --init
rm -f "$emptyGCCache"
echo 100 > "$fake_free"
cacheCorruptSeed=$(addCachePath auto-gc-empty-corrupt-seed "$TEST_ROOT/auto-gc-empty-corrupt-seed.log")
echo 'not a valid cache record' > "$emptyGCCache"
cacheCorruptLog=$TEST_ROOT/auto-gc-empty-corrupt.log
cacheCorrupt=$(addCachePath auto-gc-empty-corrupt-trigger "$cacheCorruptLog")
grep -q 'event=completed .*paths=[1-9]' "$cacheCorruptLog" || {
    cat "$cacheCorruptLog"
    fail "corrupt empty-result cache prevented a real scan"
}
if nix-store --check-validity "$cacheCorruptSeed" >/dev/null 2>&1; then
    fail "cache-miss scan did not collect known garbage"
fi

# Valid JSON with the wrong numeric types/schema is also a cache miss. In
# particular, floating-point and oversized schema values must not be coerced
# into integers by the parser.
clearStore
nix-store --init
rm -f "$emptyGCCache"
echo 100 > "$fake_free"
cacheMalformedSeed=$(addCachePath auto-gc-empty-malformed-seed "$TEST_ROOT/auto-gc-empty-malformed-seed.log")
jq '.free_after = 100.5' "$emptyGCCache" > "$TEST_ROOT/auto-gc-empty-malformed-cache.json"
mv "$TEST_ROOT/auto-gc-empty-malformed-cache.json" "$emptyGCCache"
cacheMalformedLog=$TEST_ROOT/auto-gc-empty-malformed.log
cacheMalformed=$(addCachePath auto-gc-empty-malformed-trigger "$cacheMalformedLog")
grep -q 'event=completed .*paths=[1-9]' "$cacheMalformedLog" || {
    cat "$cacheMalformedLog"
    fail "valid JSON with a floating-point free_after did not trigger a scan"
}
if nix-store --check-validity "$cacheMalformedSeed" >/dev/null 2>&1; then
    fail "floating-point cache miss did not collect known garbage"
fi

clearStore
nix-store --init
rm -f "$emptyGCCache"
echo 100 > "$fake_free"
cacheSchemaSeed=$(addCachePath auto-gc-empty-schema-seed "$TEST_ROOT/auto-gc-empty-schema-seed.log")
jq '.schema = 4294967297' "$emptyGCCache" > "$TEST_ROOT/auto-gc-empty-schema-cache.json"
mv "$TEST_ROOT/auto-gc-empty-schema-cache.json" "$emptyGCCache"
cacheSchemaLog=$TEST_ROOT/auto-gc-empty-schema.log
cacheSchema=$(addCachePath auto-gc-empty-schema-trigger "$cacheSchemaLog")
grep -q 'event=completed .*paths=[1-9]' "$cacheSchemaLog" || {
    cat "$cacheSchemaLog"
    fail "unknown oversized cache schema did not trigger a scan"
}
if nix-store --check-validity "$cacheSchemaSeed" >/dev/null 2>&1; then
    fail "unknown-schema cache miss did not collect known garbage"
fi
