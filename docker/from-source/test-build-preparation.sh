#!/usr/bin/env bash
# Non-compiling checks: synthetic Git repositories and shell export fixtures only.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$repo_root/docker/from-source/build.sh"
fixture="$(mktemp -d)"
trap 'rm -rf -- "$fixture"' EXIT
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME=Fixture GIT_AUTHOR_EMAIL=fixture@example.invalid
export GIT_COMMITTER_NAME=Fixture GIT_COMMITTER_EMAIL=fixture@example.invalid
real_git="$(command -v git)"
export REAL_GIT="$real_git"

# Extract the production code, never a second implementation of the checkout rules.
awk '/^# BEGIN PINNED CHECKOUT/ {copy=1; next} /^# END PINNED CHECKOUT/ {copy=0; found=1} copy {print} END {if (!found) exit 1}' "$build" > "$fixture/checkout.sh"
test -s "$fixture/checkout.sh"
awk '/^QUEUE_PACKAGE$/ {copy=0; found=1} copy {print} /<<.QUEUE_PACKAGE./ {copy=1} END {if (!found) exit 1}' "$build" > "$fixture/package.sh"
test -s "$fixture/package.sh"
mkdir "$fixture/bin"
cat > "$fixture/bin/git" <<'GIT'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$FETCH_LOG"
if [ "$1" = fetch ]; then
  case "${FAIL_FETCH:-}" in
    all) exit 23;;
    unshallow) case " $* " in *' --unshallow '*) exit 23;; esac;;
  esac
fi
exec "$REAL_GIT" "$@"
GIT
chmod +x "$fixture/bin/git"
"$real_git" init -q -b main "$fixture/origin"
for value in first second third; do
  printf '%s\n' "$value" > "$fixture/origin/value"
  "$real_git" -C "$fixture/origin" add value
  "$real_git" -C "$fixture/origin" commit -qm "$value"
done
old="$("$real_git" -C "$fixture/origin" rev-parse HEAD~2)"
tip="$("$real_git" -C "$fixture/origin" rev-parse HEAD)"
"$real_git" -C "$fixture/origin" checkout -qb divergent "$old"
printf 'divergent\n' > "$fixture/origin/value"
"$real_git" -C "$fixture/origin" commit -qam divergent
divergent="$("$real_git" -C "$fixture/origin" rev-parse HEAD)"
"$real_git" -C "$fixture/origin" checkout -q main
export ONLINE_OFFICE_SOURCE_REPOSITORY="file://$fixture/origin"
export ONLINE_OFFICE_SOURCE_BRANCH=main
new_cache()
{
  case_dir="$fixture/$1"
  mkdir "$case_dir"
  "$real_git" clone -q --depth=1 --branch main "$ONLINE_OFFICE_SOURCE_REPOSITORY" "$case_dir/online"
  export FETCH_LOG="$case_dir/git.log"
  : > "$FETCH_LOG"
}
checkout_pin()
{
  (cd "$case_dir" && PATH="$fixture/bin:$PATH" ONLINE_OFFICE_SOURCE_REVISION="$1" bash "$fixture/checkout.sh") > "$case_dir/result.log" 2>&1
}
assert_head()
{
  test "$("$real_git" -C "$case_dir/online" rev-parse HEAD)" = "$1"
  test -z "$("$real_git" -C "$case_dir/online" symbolic-ref -q HEAD || true)"
}
reject_pin()
{
  local before
  before="$("$real_git" -C "$case_dir/online" rev-parse HEAD)"
  if checkout_pin "$1"; then echo "Unexpected checkout success: $2" >&2; exit 1; fi
  grep -Fq "$2" "$case_dir/result.log"
  test "$("$real_git" -C "$case_dir/online" rev-parse HEAD)" = "$before"
}
new_cache current-tip
checkout_pin "$tip"
assert_head "$tip"
if grep -q -- --unshallow "$FETCH_LOG"; then
  echo "Unexpected full-history fetch: $case_dir" >&2; exit 1
fi
test "$("$real_git" -C "$case_dir/online" rev-parse --is-shallow-repository)" = true

new_cache missing-old-pin
if "$real_git" -C "$case_dir/online" cat-file -e "$old^{commit}" 2>/dev/null; then
  echo "Older pin unexpectedly exists before history recovery" >&2; exit 1
fi
checkout_pin "$old"
assert_head "$old"
grep -q -- --unshallow "$FETCH_LOG"

new_cache present-old-pin
"$real_git" -C "$case_dir/online" fetch -q --depth=1 origin "$old"
"$real_git" -C "$case_dir/online" cat-file -e "$old^{commit}"
if "$real_git" -C "$case_dir/online" merge-base --is-ancestor "$old" origin/main; then
  echo "Older pin ancestry unexpectedly resolves before history recovery" >&2; exit 1
fi
checkout_pin "$old"
assert_head "$old"
grep -q -- --unshallow "$FETCH_LOG"

new_cache divergent-pin
"$real_git" -C "$case_dir/online" fetch -q --depth=1 origin "$divergent"
reject_pin "$divergent" 'not an ancestor'
grep -q -- --unshallow "$FETCH_LOG"

new_cache malformed-pin
reject_pin main 'full lower-case Git revision'
if grep -q '^fetch ' "$FETCH_LOG"; then
  echo 'Malformed pin unexpectedly triggered a fetch' >&2; exit 1
fi

new_cache unknown-pin
reject_pin 0000000000000000000000000000000000000000 'revision is missing'
grep -q -- --unshallow "$FETCH_LOG"

new_cache fetch-failure
export FAIL_FETCH=all
reject_pin "$tip" 'Could not fetch the configured source branch'
unset FAIL_FETCH

new_cache history-fetch-failure
export FAIL_FETCH=unshallow
reject_pin "$old" 'Could not fetch full source history'
unset FAIL_FETCH

new_cache full-history-invalid
"$real_git" -C "$case_dir/online" fetch -q --unshallow origin
reject_pin 0000000000000000000000000000000000000000 'revision is missing'
if grep -q -- --unshallow "$FETCH_LOG"; then
  echo "Unexpected full-history fetch: $case_dir" >&2; exit 1
fi
if grep -q -- --depth "$FETCH_LOG"; then
  echo "Full-history cache unexpectedly fetched with a depth limit" >&2; exit 1
fi
test "$("$real_git" -C "$case_dir/online" rev-parse --is-shallow-repository)" = false
checkout_pin "$old"
assert_head "$old"

# Exercise the exact export here-document with text placeholders, never native code.
export ONLINE_OFFICE_SOURCE_REVISION="$tip"
export QUEUE_REPORT="$fixture/report" QUEUE_BUILD="$fixture/runtime"
mkdir -p "$QUEUE_REPORT" "$QUEUE_BUILD/online"
runtime="$QUEUE_BUILD/online"
for resource in coolwsd coolmount coolforkit-caps coolforkit-ns coolwsd.xml test/run_unit.sh test/.libs/unit-agent-control.so; do
  mkdir -p "$runtime/$(dirname "$resource")"
  printf 'fixture\n' > "$runtime/$resource"
done
mkdir -p "$runtime/test/data/presets" "$runtime/browser/dist" "$runtime/etc" "$runtime/systemplate" "$runtime/engine/instdir"
package_fixture()
{
  export QUEUE_STAGE
  QUEUE_STAGE="$(mktemp -d "$QUEUE_REPORT/.queue-root.XXXXXX")"
  (cd "$runtime" && bash -Eeuo pipefail "$fixture/package.sh") > "$QUEUE_REPORT/result.log" 2>&1
}
package_fixture
test -s "$QUEUE_REPORT/debug-runtime.tar.gz"
test -f "$QUEUE_REPORT/debug-dependencies.sha256"
(cd "$QUEUE_REPORT" && sha256sum -c debug-runtime.sha256)
test ! -d "$runtime/.libs"
test ! -e "$QUEUE_STAGE"
rm "$runtime/test/.libs/unit-agent-control.so"
if package_fixture; then echo 'Missing unit module passed export' >&2; exit 1; fi
grep -Fq "Missing queue export resource: $runtime/test/.libs/unit-agent-control.so" "$QUEUE_REPORT/result.log"
test ! -e "$QUEUE_STAGE"
printf 'fixture\n' > "$runtime/test/.libs/unit-agent-control.so"
# A copy failure must name the resource and clean the partial stage.
cat > "$fixture/bin/cp" <<'CP'
#!/usr/bin/env bash
exit 19
CP
chmod +x "$fixture/bin/cp"
if PATH="$fixture/bin:$PATH" package_fixture; then echo 'Failed copy passed export' >&2; exit 1; fi
grep -Fq "Could not copy queue export resource: $runtime/coolwsd" "$QUEUE_REPORT/result.log"
test ! -e "$QUEUE_STAGE"
rm "$fixture/bin/cp"
# A reported missing dependency must fail, even when ldd itself returns zero.
cat > "$fixture/bin/file" <<'FILE'
#!/bin/sh
printf 'ELF fixture\n'
FILE
cat > "$fixture/bin/ldd" <<'LDD'
#!/bin/sh
printf 'libmissing.so => not found\n'
LDD
chmod +x "$fixture/bin/file" "$fixture/bin/ldd"
if PATH="$fixture/bin:$PATH" package_fixture; then echo 'Missing dependency passed export' >&2; exit 1; fi
grep -Fq 'Missing queue export dependencies for' "$QUEUE_REPORT/result.log"
grep -Fq 'libmissing.so => not found' "$QUEUE_REPORT/result.log"
test ! -e "$QUEUE_STAGE"
rm "$fixture/bin/file" "$fixture/bin/ldd"
cat > "$fixture/bin/tar" <<'TAR'
#!/usr/bin/env bash
exit 17
TAR
chmod +x "$fixture/bin/tar"
if PATH="$fixture/bin:$PATH" package_fixture; then echo 'Failed archive passed export' >&2; exit 1; fi
grep -Eq 'Queue export failed at line [0-9]+ \(exit 17\)' "$QUEUE_REPORT/result.log"
test ! -e "$QUEUE_STAGE"

# A missing cpio fails before the script creates the template; a failed cpio
# must never reach later template steps or the recipe's success stamp.
mkdir "$fixture/empty-bin" "$fixture/engine"
if PATH="$fixture/empty-bin" /bin/bash "$repo_root/coolwsd-systemplate-setup" "$fixture/plate" "$fixture/engine" > "$fixture/plate.log" 2>&1; then
  echo 'Missing cpio passed systemplate setup' >&2; exit 1
fi
grep -Fq 'Missing required utility: cpio' "$fixture/plate.log"
test ! -e "$fixture/plate"
for utility in find fc-cache; do
  printf '#!/bin/sh\nexit 0\n' > "$fixture/bin/$utility"
  chmod +x "$fixture/bin/$utility"
done
printf '#!/bin/sh\nexit 19\n' > "$fixture/bin/cpio"
chmod +x "$fixture/bin/cpio"
if PATH="$fixture/bin:$PATH" bash "$repo_root/coolwsd-systemplate-setup" "$fixture/plate" "$fixture/engine" > "$fixture/plate.log" 2>&1; then
  echo 'Failed cpio passed systemplate setup' >&2; exit 1
fi
grep -Fq 'Failed to copy systemplate files with cpio' "$fixture/plate.log"
test ! -e "$fixture/plate/etc"
test ! -e "$fixture/plate/system_stamp"
# The production preflight invalidates only the stamp, preserving unrelated cache.
awk '/^prepare_systemplate\(\)/ {copy=1} copy {print} copy && /^}/ {found=1; exit} END {if (!found) exit 1}' "$build" > "$fixture/prepare.sh"
mkdir -p "$fixture/cache/online/systemplate"
touch "$fixture/cache/online/systemplate/system_stamp" "$fixture/cache/online/systemplate/retained"
PATH="$fixture/bin:$PATH" BUILDDIR="$fixture/cache" bash -c 'source "$1"; prepare_systemplate' bash "$fixture/prepare.sh"
test ! -e "$fixture/cache/online/systemplate/system_stamp"
test -e "$fixture/cache/online/systemplate/retained"
grep -Eq '^[[:space:]]+cpio \\$' "$repo_root/docker/from-source/Builder.Dockerfile"
echo 'Build preparation checks passed (Git recovery, export, systemplate; no compilation).'
