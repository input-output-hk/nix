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
