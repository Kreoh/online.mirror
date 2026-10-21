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
export SRCDIR="$repo_root/docker/from-source"

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
  export CACHE_ROOT="$case_dir" BUILDDIR="$case_dir" mtime_work="$case_dir/mtime"
  mkdir "$mtime_work"
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


# Exercise the production source-validation block between real checkout calls.
# All inputs and the PO parser are tiny synthetic files; no native command runs.
awk '/^# BEGIN SOURCE VALIDATION/ {copy=1; next} /^# END SOURCE VALIDATION/ {copy=0; found=1} copy {print} END {if (!found) exit 1}' "$build" > "$fixture/source.sh"
mkdir -p "$fixture/origin/common" "$fixture/origin/engine/static/emscripten" "$fixture/origin/browser/po" "$fixture/origin/browser/util"
printf 'Collabora Online\n' > "$fixture/origin/common/shared.hpp"
printf 'neutral\n' > "$fixture/origin/engine/static/emscripten/favicon.ico"
printf 'old\n' > "$fixture/origin/favicon.ico"
printf 'msgid "hello"\nmsgstr "hello"\n' > "$fixture/origin/browser/po/en.po"
printf 'import sys\nfrom pathlib import Path\nassert all("INVALID" not in Path(p).read_text() for p in sys.argv[2:])\n' > "$fixture/origin/browser/util/po2json.py"
"$real_git" -C "$fixture/origin" add .
"$real_git" -C "$fixture/origin" commit -qm 'source fixture'
source_tip="$("$real_git" -C "$fixture/origin" rev-parse HEAD)"
new_cache timestamp-round-trip
checkout_pin "$source_tip"
bash "$fixture/source.sh" > "$case_dir/source.log" 2>&1
test -f "$case_dir/source-mtimes.json"
before_mtime="$(stat -c %y "$case_dir/online/common/shared.hpp")"
mkdir "$mtime_work"
checkout_pin "$source_tip"
bash "$fixture/source.sh" >> "$case_dir/source.log" 2>&1
test "$(stat -c %y "$case_dir/online/common/shared.hpp")" = "$before_mtime"
grep -Fq 'pre-build debranding scan passed' "$case_dir/source.log"
grep -Fq 'browser PO catalogues passed' "$case_dir/source.log"
grep -Fq 'Source timestamps: restored 1 files.' "$case_dir/source.log"
# A failed real validation must prevent completion and record publication.
mkdir "$mtime_work"
checkout_pin "$source_tip"
printf 'INVALID\n' >> "$case_dir/online/browser/po/en.po"
if bash "$fixture/source.sh" >> "$case_dir/source.log" 2>&1; then
  echo 'Invalid PO source passed preparation' >&2; exit 1
fi
test ! -e "$case_dir/source-mtimes.json"

# Exercise the exact export here-document with text placeholders, never native code.
export ONLINE_OFFICE_SOURCE_REVISION="$tip"
export QUEUE_SECCOMP="$repo_root/docker/cool-seccomp-profile.json"
export QUEUE_REPORT="$fixture/report" QUEUE_BUILD="$fixture/runtime"
mkdir -p "$QUEUE_REPORT" "$QUEUE_BUILD/online"
runtime="$QUEUE_BUILD/online"
for resource in coolwsd coolmount coolforkit-caps coolforkit-ns coolwsd.xml test/run_unit.sh test/.libs/unit-agent-control.so test/.libs/unit-agent-control-kit.so; do
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
for module in unit-agent-control unit-agent-control-kit; do
  tar -tzf "$QUEUE_REPORT/debug-runtime.tar.gz" | grep -Fx "./${runtime#/}/test/.libs/$module.so"
  rm "$runtime/test/.libs/$module.so"
  if package_fixture; then echo "Missing $module module passed export" >&2; exit 1; fi
  grep -Fq "Missing queue export resource: $runtime/test/.libs/$module.so" "$QUEUE_REPORT/result.log"
  test ! -e "$QUEUE_STAGE"
  printf 'fixture\n' > "$runtime/test/.libs/$module.so"
done
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
# Verify the exported policy and reject tampering before any container is started.
(cd "$QUEUE_REPORT" && sha256sum -c seccomp.sha256)
cmp "$QUEUE_SECCOMP" "$QUEUE_REPORT/cool-seccomp-profile.json"
printf '\n' >> "$QUEUE_REPORT/cool-seccomp-profile.json"
if (cd "$QUEUE_REPORT" && sha256sum -c seccomp.sha256) > "$fixture/policy.log" 2>&1; then
  echo 'Modified seccomp policy passed verification' >&2; exit 1
fi

# Exercise the exact resolved-model validator, including absent and extra policies.
awk '/docker compose .* -f .*config --format json.*node -e/ {copy=1; next} copy && /^});/ {print "});"; found=1; exit} copy {print} END {if (!found) exit 1}' "$build" > "$fixture/policy.js"
export OFFICE_NATIVE_ARTIFACTS="$fixture/artifacts" OFFICE_NATIVE_BUILDER_ID=fixture
export OFFICE_NATIVE_UID=1000 OFFICE_NATIVE_GID=1000
for policy_case in exact absent wrong extra unconfined privileged capabilities; do
  node - "$policy_case" <<'MODEL' > "$fixture/model.json"
const which=process.argv[2], service={image:"fixture",user:"1000:1000",entrypoint:["/bin/bash","/queue-validation/run-queue-unit.sh"],security_opt:["seccomp="+process.env.OFFICE_NATIVE_ARTIFACTS+"/cool-seccomp-profile.json"]};
if(which==="absent") delete service.security_opt;
if(which==="wrong") service.security_opt=["seccomp=/other/profile.json"];
if(which==="extra") service.security_opt.push("no-new-privileges:true");
if(which==="unconfined") service.security_opt=["seccomp=unconfined"];
if(which==="privileged") service.privileged=true;
if(which==="capabilities") service.cap_add=["SYS_ADMIN"];
process.stdout.write(JSON.stringify({services:{"code-uno-experimental":service}}));
MODEL
  if node "$fixture/policy.js" < "$fixture/model.json" > "$fixture/model.log" 2>&1; then
    test "$policy_case" = exact
  else
    test "$policy_case" != exact
  fi
done

# Execute the production ordinary runner and release restoration on a real timeout.
# Only shorten the clock; retain GNU timeout's process-group and exit semantics.
awk '/^      : > .*unit-agent-control.trs/ {copy=1} copy && /^    \) > / {found=1; exit} copy {print} END {if (!found) exit 1}' "$build" > "$fixture/unit.sh"
awk '/^    restore_release\(\)/ {copy=1} copy {print} copy && /^    }/ {found=1; exit} END {if (!found) exit 1}' "$build" > "$fixture/restore.sh"
mkdir -p "$fixture/deadline/test" "$fixture/deadline/report"
cat > "$fixture/deadline/test/run_unit.sh" <<'RUNNER'
#!/usr/bin/env bash
trap '' TERM
sleep 30
RUNNER
chmod +x "$fixture/deadline/test/run_unit.sh"
export REAL_TIMEOUT="$(command -v timeout)"
cat > "$fixture/bin/timeout" <<'TIMEOUT'
#!/usr/bin/env bash
test "$1" = --signal=TERM
test "$2" = --kill-after=10s
test "$3" = 600s
shift 3
exec "$REAL_TIMEOUT" --signal=TERM --kill-after=0.1s 0.1s "$@"
TIMEOUT
chmod +x "$fixture/bin/timeout"
export report_dir="$fixture/deadline/report"
printf ':test-result: PASS stale\n' > "$report_dir/unit-agent-control.trs"
printf 'unchanged\n' > "$report_dir/release-before.sha256"
if (cd "$fixture/deadline" && PATH="$fixture/bin:$PATH" bash -e -c '
  source "$1"
  configure_release() { printf "restored\n"; }
  hash_installation() { printf "unchanged\n"; }
  trap restore_release EXIT
  bash "$2"
' bash "$fixture/restore.sh" "$fixture/unit.sh"); then
  echo 'Timed-out unit passed validation' >&2; exit 1
fi
test "$(cat "$report_dir/runner-exit.txt")" = 137
test "$(cat "$report_dir/validation-exit.txt")" = 1
grep -qx restored "$report_dir/release-restore.log"
cmp "$report_dir/release-before.sha256" "$report_dir/release-after.sha256"
test ! -s "$report_dir/unit-agent-control.trs"
rm "$fixture/bin/timeout"

# The exported runner must record its own timeout and discard stale results too.
awk '/^cd \/workspace-cache\/build\/builddir\/online\/test$/ {copy=1; next} copy && /^QUEUE_UNIT$/ {found=1; exit} copy {print} END {if (!found) exit 1}' "$build" | sed "s|/queue-state|$report_dir|g" > "$fixture/queue-unit.sh"
cat > "$fixture/bin/timeout" <<'TIMEOUT'
#!/usr/bin/env bash
test "$1" = --signal=TERM
test "$2" = --kill-after=10s
test "$3" = 360s
shift 3
exec "$REAL_TIMEOUT" --signal=TERM --kill-after=0.1s 0.1s "$@"
TIMEOUT
chmod +x "$fixture/bin/timeout"
printf '0\n' > "$report_dir/unit.exit"
printf ':test-result: PASS stale\n' > "$report_dir/unit-agent-control.trs"
if (cd "$fixture/deadline/test" && PATH="$fixture/bin:$PATH" bash -e "$fixture/queue-unit.sh"); then
  echo 'Timed-out exported unit passed validation' >&2; exit 1
fi
test "$(cat "$report_dir/unit.exit")" = 137
test ! -s "$report_dir/unit-agent-control.trs"
rm "$fixture/bin/timeout"

# A denied namespace probe fails immediately, using the exact production block.
awk '/^# Probe the user namespace/ {copy=1} copy {print} copy && /^}/ {found=1; exit} END {if (!found) exit 1}' "$build" > "$fixture/preflight.sh"
cat > "$fixture/bin/unshare" <<'UNSHARE'
#!/usr/bin/env bash
test "$*" = '--user --map-root-user unshare --mount --propagation private true'
exit 1
UNSHARE
chmod +x "$fixture/bin/unshare"
if PATH="$fixture/bin:$PATH" bash "$fixture/preflight.sh" > "$fixture/preflight.log" 2>&1; then
  echo 'Denied namespace probe passed validation' >&2; exit 1
fi
grep -Fq 'Native validation requires user and private mount namespaces' "$fixture/preflight.log"

echo 'Build preparation checks passed (Git recovery, export, systemplate, policy, deadline; no compilation).'
