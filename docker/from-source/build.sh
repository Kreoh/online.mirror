#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# -- Available env vars --
# * DOCKER_HUB_REPO - which Docker Hub repo to use
# * DOCKER_HUB_TAG  - exact revision-specific Docker Hub tag to create
# * ONLINE_OFFICE_SOURCE_REPOSITORY - which Git repository to clone
# * ONLINE_OFFICE_SOURCE_BRANCH - which branch to build
# * ONLINE_OFFICE_SOURCE_REVISION - exact full Kreoh source revision to build
# * ONLINE_OFFICE_FINAL_DOCKERFILE - explicit final packaging route, Distroless or Debian
# * ONLINE_OFFICE_SOURCE_CACHE_DIR - persistent build cache root
# * ONLINE_OFFICE_FINAL_DOCKER_NO_CACHE - if true, build the final Docker image without cache
# * ENGINE_BUILD_TARGET - which make target to run for the engine (when building from source)
# * ONLINE_EXTRA_BUILD_OPTIONS - extra build options for online
# * NO_DOCKER_IMAGE - if set, don't build the docker image itself, just do all the preps

if [[ "${ONLINE_OFFICE_RUN_AGENT_CONTROL:-}" == 1 ]]; then
  # Probe the user namespace, then the private mount namespace used by JailUtil.
  # Host namespace restrictions must fail before compilation or first-child startup.
  timeout --signal=TERM --kill-after=5s 10s \
    unshare --user --map-root-user unshare --mount --propagation private true || {
    echo "Native validation requires user and private mount namespaces under the supplied seccomp policy." >&2
    exit 1
  }
fi

# Check env variables
if [ -z "$DOCKER_HUB_REPO" ]; then
  DOCKER_HUB_REPO="mydomain/online-office"
fi;

if [[ ! "${ONLINE_OFFICE_SOURCE_REVISION:-}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "ONLINE_OFFICE_SOURCE_REVISION must be an explicit full lower-case Git revision." >&2
  exit 1
fi

if [ -n "${ONLINE_OFFICE_SOURCE_REPOSITORY:-}" ] && [ "$ONLINE_OFFICE_SOURCE_REPOSITORY" != "https://github.com/Kreoh/online.mirror.git" ]; then
  echo "ONLINE_OFFICE_SOURCE_REPOSITORY must be https://github.com/Kreoh/online.mirror.git" >&2
  exit 1
fi
if [ -z "${ONLINE_OFFICE_SOURCE_REPOSITORY:-}" ]; then
  ONLINE_OFFICE_SOURCE_REPOSITORY="https://github.com/Kreoh/online.mirror.git"
fi;
if [ -n "${ONLINE_OFFICE_SOURCE_BRANCH:-}" ] && [ "$ONLINE_OFFICE_SOURCE_BRANCH" != "main" ]; then
  echo "ONLINE_OFFICE_SOURCE_BRANCH must be main" >&2
  exit 1
fi
if [ -z "${ONLINE_OFFICE_SOURCE_BRANCH:-}" ]; then
  ONLINE_OFFICE_SOURCE_BRANCH="main"
fi;
echo "Building exact revision '$ONLINE_OFFICE_SOURCE_REVISION' from '$ONLINE_OFFICE_SOURCE_REPOSITORY'"

if [ -n "${ENGINE_ASSETS:-}" ]; then
  echo "ENGINE_ASSETS is prohibited: build the document engine from Kreoh source." >&2
  exit 1
fi;
echo "Building engine from source"

if [ -z "$ENGINE_BUILD_TARGET" ]; then
  ENGINE_BUILD_TARGET=""
fi;
echo "Engine build target: '$ENGINE_BUILD_TARGET'"


SRCDIR=$(realpath `dirname $0`)
INSTDIR="$SRCDIR/instdir"
CACHE_ROOT="${ONLINE_OFFICE_SOURCE_CACHE_DIR:-${HOME:-$SRCDIR}/.cache/online-office-source-builder}"

FINAL_DOCKERFILE="${ONLINE_OFFICE_FINAL_DOCKERFILE:-Distroless}"
case "$FINAL_DOCKERFILE" in
  Distroless) ;;
  Debian)
    echo "WARNING: using the diagnostic Debian image instead of the publication route." >&2
    ;;
  *)
    echo "ONLINE_OFFICE_FINAL_DOCKERFILE must be Distroless or Debian." >&2
    exit 1
    ;;
esac
BUILDDIR="$CACHE_ROOT/builddir"
if [ -z "${CCACHE_DIR:-}" ]; then
  export CCACHE_DIR="$CACHE_ROOT/ccache"
fi

mkdir -p "$BUILDDIR" "$CCACHE_DIR"
cd "$BUILDDIR"

rm -rf "$INSTDIR" || true
mkdir -p "$INSTDIR"

# POCO is built as part of the engine (engine/external/poco) and picked up from
# its workdir automatically by the cool configure below; no separate build.


##### cloning & updating #####

# Clone the online monorepo (engine/ contains the rendering engine)
if test ! -d online ; then
  git clone --depth=1 --branch "$ONLINE_OFFICE_SOURCE_BRANCH" "$ONLINE_OFFICE_SOURCE_REPOSITORY" online || exit 1
fi

# BEGIN PINNED CHECKOUT (also exercised by test-build-preparation.sh)
(
  cd online || exit 1
  test "$(git config --get remote.origin.url)" = "$ONLINE_OFFICE_SOURCE_REPOSITORY" || {
    echo "Source cache origin does not match the configured repository." >&2
    exit 1
  }
  [[ "$ONLINE_OFFICE_SOURCE_REVISION" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Source revision must be a full lower-case Git revision." >&2
    exit 1
  }
  branch_ref="refs/remotes/origin/$ONLINE_OFFICE_SOURCE_BRANCH"
  branch_refspec="+refs/heads/$ONLINE_OFFICE_SOURCE_BRANCH:$branch_ref"
  fetch_options=(--force)
  if [ "$(git rev-parse --is-shallow-repository)" = true ]; then
    fetch_options+=(--depth=1)
  fi
  git fetch "${fetch_options[@]}" origin "$branch_refspec" || {
    echo "Could not fetch the configured source branch." >&2
    exit 1
  }
  if ! git cat-file -e "$ONLINE_OFFICE_SOURCE_REVISION^{commit}" 2>/dev/null ||
      ! git merge-base --is-ancestor "$ONLINE_OFFICE_SOURCE_REVISION" "$branch_ref"; then
    if [ "$(git rev-parse --is-shallow-repository)" = true ]; then
      echo "Fetching full configured branch history to verify the pinned revision."
      git fetch --unshallow --force origin "$branch_refspec" || {
        echo "Could not fetch full source history for the pinned revision." >&2
        exit 1
      }
    fi
  fi
  git cat-file -e "$ONLINE_OFFICE_SOURCE_REVISION^{commit}" 2>/dev/null || {
    echo "Pinned source revision is missing after fetching the configured branch." >&2
    exit 1
  }
  git merge-base --is-ancestor "$ONLINE_OFFICE_SOURCE_REVISION" "$branch_ref" || {
    echo "Pinned source revision is not an ancestor of the configured branch." >&2
    exit 1
  }
  git checkout --detach -f "$ONLINE_OFFICE_SOURCE_REVISION" || exit 1
  git clean -f -d || exit 1
  test "$(git rev-parse HEAD)" = "$ONLINE_OFFICE_SOURCE_REVISION"
) || exit 1
# END PINNED CHECKOUT

source_version=$(awk -F'[][]' '$2 == "coolwsd" { print $4; exit }' online/configure.ac) || exit 1
if [ -z "$source_version" ]; then
  echo "Could not read the source version from configure.ac" >&2
  exit 1
fi
EXPECTED_DOCKER_HUB_TAG="${source_version}-agent-save-${ONLINE_OFFICE_SOURCE_REVISION:0:12}"
if [ -z "${DOCKER_HUB_TAG:-}" ]; then
  DOCKER_HUB_TAG="$EXPECTED_DOCKER_HUB_TAG"
fi;
if [ "$DOCKER_HUB_TAG" != "$EXPECTED_DOCKER_HUB_TAG" ]; then
  echo "DOCKER_HUB_TAG must be $EXPECTED_DOCKER_HUB_TAG" >&2
  exit 1
fi
echo "Using Docker Hub Repository: '$DOCKER_HUB_REPO' with tag '$DOCKER_HUB_TAG'."

python3 "$SRCDIR/debrand.py" source "$BUILDDIR/online" || exit 1
python3 "$SRCDIR/debrand.py" scan-source "$BUILDDIR/online" || exit 1
python3 "$SRCDIR/debrand.py" validate-po "$BUILDDIR/online" || exit 1

bash "$SRCDIR/check-builder.sh" "$BUILDDIR/online/configure.ac" || exit 1
( cd online && ./autogen.sh ) || exit 1

##### engine #####

( cd online/engine && ./autogen.sh --with-distro=OnlineLinux-LOKit --disable-epm --without-package-format --disable-symbols ) || exit 1
( cd online/engine && make $ENGINE_BUILD_TARGET ) || exit 1

# Run native JavaScript regression tests only on the Jenkins builder.
if [[ "${ONLINE_OFFICE_RUN_JSUNO:-}" == "1" ]]; then
  (
    set -o pipefail
    test -z "${SUPPRESS_TESTS:-}" || exit 1
    report_dir="${ONLINE_OFFICE_AGENT_CONTROL_REPORT_DIR:?Missing Jenkins report directory}"
    mkdir -p "$report_dir" || exit 1
    printf '%s\n' "$ONLINE_OFFICE_SOURCE_REVISION" > "$report_dir/jsuno-source-revision.txt" || exit 1
    cd online/engine || exit 1
    make CppunitTest_jsuno_execute CppunitTest_jsuno_testuno 2>&1 | tee "$report_dir/jsuno.log" || exit 1
  ) || exit 1
fi

# copy stuff
mkdir -p "$INSTDIR"/opt/
cp -a online/engine/instdir "$INSTDIR"/opt/online-office

##### coolwsd & cool #####

# build
configure_release()
{
( cd online && ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-tests --with-lokit-path="$BUILDDIR"/online/engine/include --with-lo-builddir="$BUILDDIR"/online/engine --with-lo-path=/opt/online-office --without-app-branding --with-app-name='Online Office' --with-app-package-name=org.onlineoffice.app --without-vendor --with-info-url=about:blank $ONLINE_EXTRA_BUILD_OPTIONS)
}
# The existing recipe removes and recreates systemplate after its stamp is removed.
# Cached stamps from builders without cpio may describe incomplete templates.
prepare_systemplate()
{
  command -v cpio >/dev/null || {
    echo "Missing required systemplate utility: cpio. Rebuild the Jenkins builder image." >&2
    return 1
  }
  rm -f -- "$BUILDDIR/online/systemplate/system_stamp"
}
prepare_systemplate || exit 1
configure_release || exit 1
( cd online && make -j $(nproc)) || exit 1

# copy stuff
( cd online && DESTDIR="$INSTDIR" make install ) || exit 1

# Exercise the existing native suite only in the existing Jenkins builder.
if [[ "${ONLINE_OFFICE_RUN_AGENT_CONTROL:-}" == "1" ]]; then
  (
    set -e -o pipefail
    report_dir="${ONLINE_OFFICE_AGENT_CONTROL_REPORT_DIR:?Missing Jenkins report directory}"
    mkdir -p "$report_dir" || exit 1
    test -z "${SUPPRESS_TESTS:-}" || exit 1
    hash_installation()
    {
      find "$INSTDIR" -type f -print0 | sort -z | xargs -0 sha256sum
    }
    hash_installation > "$report_dir/release-before.sha256" || exit 1
    restore_release()
    {
      status=$?
      trap - EXIT
      configure_release > "$report_dir/release-restore.log" 2>&1 || status=1
      hash_installation > "$report_dir/release-after.sha256" || status=1
      cmp "$report_dir/release-before.sha256" "$report_dir/release-after.sha256" || status=1
      printf '%s\n' "$status" > "$report_dir/validation-exit.txt"
      exit "$status"
    }
    trap restore_release EXIT
    # Keep Calc geometry and chart bindings in the existing Jenkins validation report.
    (
      cd online/engine || exit 1
      make CppunitTest_sc_subsequent_export_test2 CppunitTest_desktop_lib CppunitTest_sc_tiledrendering
    ) 2>&1 | tee "$report_dir/calc-subsequent-export-test2.log" || exit $?
    (
      cd online || exit 1
      ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --enable-tests --enable-debug \
        --with-lokit-path="$BUILDDIR/online/engine/include" \
        --with-lo-builddir="$BUILDDIR/online/engine" \
        --with-lo-path="$BUILDDIR/online/engine/instdir" \
        --without-app-branding --with-app-name='Online Office' \
        --with-app-package-name=org.onlineoffice.app --without-vendor \
        --with-info-url=about:blank $ONLINE_EXTRA_BUILD_OPTIONS || exit 1
      make -j "$(nproc)" coolwsd coolmount coolforkit-caps coolforkit-ns || exit 1
      prepare_systemplate || exit 1
      make systemplate "$BUILDDIR/online/jails" "$BUILDDIR/online/cache" presets-dir || exit 1
      make -C test unit-agent-control.la unit-agent-script-control.la unit-tiletest.la unittest || exit 1

      if [ "$ONLINE_OFFICE_EXPORT_QUEUE_VALIDATION" = true ]; then
        test "$BUILDDIR" = /workspace-cache/build/builddir || {
          echo "Queue export requires build root /workspace-cache/build/builddir; got $BUILDDIR" >&2
          exit 1
        }
        queue_stage="$(mktemp -d "$report_dir/.queue-root.XXXXXX")" || exit 1
        QUEUE_STAGE="$queue_stage" QUEUE_REPORT="$report_dir" QUEUE_BUILD="$BUILDDIR" QUEUE_SECCOMP="$SRCDIR/../cool-seccomp-profile.json" bash -Eeuo pipefail <<'QUEUE_PACKAGE'
          queue_stage="$QUEUE_STAGE"
          report_dir="$QUEUE_REPORT"
          BUILDDIR="$QUEUE_BUILD"
          trap 'rm -rf -- "$queue_stage"' EXIT
          trap 'status=$?; printf "Queue export failed at line %s (exit %s).\n" "$LINENO" "$status" >&2; exit "$status"' ERR
          # Preserve configured absolute paths, without copying a source checkout.
          test -d "$PWD/test/data/presets" || {
            echo "Missing queue export presets: $PWD/test/data/presets" >&2
            exit 1
          }
          for resource in coolwsd coolmount coolforkit-caps coolforkit-ns coolwsd.xml \
              test/run_unit.sh test/.libs/unit-agent-control.so \
              test/.libs/unit-agent-control-kit.so test/data \
              browser/dist etc systemplate engine/instdir; do
            test -e "$PWD/$resource" || {
              echo "Missing queue export resource: $PWD/$resource" >&2
              exit 1
            }
            cp -a --parents "$PWD/$resource" "$queue_stage/" || {
              echo "Could not copy queue export resource: $PWD/$resource" >&2
              exit 1
            }
          done
          # Include transitive binary/unit dependencies at their recorded absolute paths.
          : > "$report_dir/debug-dependencies.txt"
          : > "$report_dir/debug-dependencies.sha256"
          while IFS= read -r -d '' executable; do
            if file -b "$executable" | grep -q ELF; then
              runtime_executable="${executable#"$queue_stage"}"
              dependencies="$(ldd "$runtime_executable" 2>&1)" || {
                case "$dependencies" in
                  *"not a dynamic executable"*|*"statically linked"*) continue;;
                  *)
                    echo "Could not inspect queue export dependencies: $runtime_executable" >&2
                    exit 1
                    ;;
                esac
              }
              if printf '%s\n' "$dependencies" | grep -q 'not found'; then
                printf 'Missing queue export dependencies for %s:\n%s\n' "$runtime_executable" "$dependencies" >&2
                exit 1
              fi
              printf '%s\n%s\n' "$runtime_executable" "$dependencies" >> "$report_dir/debug-dependencies.txt"
              while IFS= read -r dependency; do
                test -f "$dependency"
                sha256sum "$dependency" >> "$report_dir/debug-dependencies.sha256"
                cp -L --parents "$dependency" "$queue_stage/"
              done < <(printf '%s\n' "$dependencies" | awk '$2 == "=>" && substr($3,1,1)=="/" {print $3} substr($1,1,1)=="/" {print $1}')
            fi
          done < <(find "$queue_stage" -type f -print0)
          test "$(du -sm "$queue_stage" | cut -f1)" -le 20480
          printf '%s\n' "$ONLINE_OFFICE_SOURCE_REVISION" > "$report_dir/debug-source.revision"
          printf '%s\n' "$(id -u)" > "$report_dir/debug.uid"
          printf '%s\n' "$(id -g)" > "$report_dir/debug.gid"
          printf '%s\n' "$BUILDDIR/online" > "$report_dir/debug-root.path"
          (cd "$queue_stage" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$report_dir/debug-files.sha256"
          (ulimit -f 20971520; tar --xattrs -czf "$report_dir/debug-runtime.tar.gz" -C "$queue_stage" .)
          (cd "$report_dir" && sha256sum debug-runtime.tar.gz > debug-runtime.sha256)
          cp -- "$QUEUE_SECCOMP" "$report_dir/cool-seccomp-profile.json"
          (cd "$report_dir" && sha256sum cool-seccomp-profile.json > seccomp.sha256)
QUEUE_PACKAGE
        test "$?" = 0 || exit 1
        cat > "$report_dir/run-queue-unit.sh" <<'QUEUE_UNIT'
#!/usr/bin/env bash
set -euo pipefail
# Probe the user namespace, then the private mount namespace used by JailUtil.
# Host namespace restrictions must fail before compilation or first-child startup.
timeout --signal=TERM --kill-after=5s 10s \
  unshare --user --map-root-user unshare --mount --propagation private true || {
  echo "Native validation requires user and private mount namespaces under the supplied seccomp policy." >&2
  exit 1
}
test "$(id -u)" != 0
test "$(id -u)" = "$(cat /queue-validation/debug.uid)"
test "$(id -g)" = "$(cat /queue-validation/debug.gid)"
test "$(cat /queue-validation/debug-root.path)" = /workspace-cache/build/builddir/online
test -d /queue-state/home
test -w /workspace-cache/build/builddir/online/jails
test -w /workspace-cache/build/builddir/online/cache
sha256sum -c /queue-validation/debug-dependencies.sha256 > /queue-state/dependency-verification.log
cd /workspace-cache/build/builddir/online/test
rm -f /queue-state/unit.exit
: > /queue-state/unit-agent-control.trs
# Include first-child startup in the 300s external unit budget, with 60s headroom.
set +e
timeout --signal=TERM --kill-after=10s 360s ./run_unit.sh --test-name unit-agent-control.la --port 9980 \
  --log-file /queue-state/unit-agent-control.log \
  --trs-file /queue-state/unit-agent-control.trs --fail-fast
result=$?
set -e
printf '%s\n' "$result" > /queue-state/unit.exit
test "$result" = 0
grep -q '^:test-result: PASS' /queue-state/unit-agent-control.trs
! grep -Eq '^:test-result: (FAIL|SKIP|ERROR)' /queue-state/unit-agent-control.trs
QUEUE_UNIT
        cat > "$report_dir/run-queue-validation.sh" <<'QUEUE_LAUNCH'
#!/usr/bin/env bash
# Run from the existing config workspace with the same explicitly supplied Compose inputs.
# Usage: run-queue-validation.sh /absolute/artifacts /absolute/fresh-state -- -f base.yaml -f ...
set -euo pipefail
artifacts="$(realpath "$1")"
state="$2"
shift 2
test "$1" = --
shift
test "$#" -gt 0
compose_files=0
probe_input=0
selenium_input=0
previous=""
for argument in "$@"; do
  if [ "$previous" = -f ]; then
    compose_files=$((compose_files + 1))
    case "$argument" in
      */compose.new-image-probe.yaml) probe_input=1;;
      */compose.selenium-shm.yaml) selenium_input=1;;
    esac
  fi
  previous="$argument"
done
test "$compose_files" = 3
test "$probe_input" = 1
test "$selenium_input" = 1
command -v node >/dev/null
command -v timeout >/dev/null
test ! -e "$state"
mkdir -m 700 "$state"
state="$(realpath "$state")"
test "$(id -u)" != 0
test "$(id -u)" = "$(cat "$artifacts/debug.uid")"
test "$(id -g)" = "$(cat "$artifacts/debug.gid")"
(cd "$artifacts" && sha256sum -c builder-image.sha256 && sha256sum -c debug-runtime.sha256 && sha256sum -c seccomp.sha256)
builder="$(cat "$artifacts/builder-image.id")"
[[ "$builder" =~ ^sha256:[0-9a-f]{64}$ ]]
docker image load -i "$artifacts/builder-image.tar.gz" > "$state/image-load.log"
test "$(docker image inspect --format '{{.Id}}' "$builder")" = "$builder"
mkdir "$state/root" "$state/jails" "$state/cache" "$state/home"
tar --no-same-owner --xattrs -xzf "$artifacts/debug-runtime.tar.gz" -C "$state/root"
(cd "$state/root" && sha256sum -c "$artifacts/debug-files.sha256" > "$state/bundle-verification.log")
export OFFICE_NATIVE_BUILDER_ID="$builder" OFFICE_NATIVE_UID="$(id -u)" OFFICE_NATIVE_GID="$(id -g)"
export OFFICE_NATIVE_ROOT="$state/root" OFFICE_NATIVE_STATE="$state" OFFICE_NATIVE_ARTIFACTS="$artifacts"
export OFFICE_NATIVE_RUN_ID="$(cat /proc/sys/kernel/random/uuid)"
# Process the resolved model in memory; write only the bounded override, never other service secrets.
docker compose "$@" --profile office-uno-experimental config --format json | node -e '
let data=""; process.stdin.on("data", x=>data+=x); process.stdin.on("end",()=>{
 const model=JSON.parse(data), override=model["x-native-retirement"];
 if(!override || Object.keys(override.services).sort().join(",")!=="code-uno-experimental,office-uno-go-no-go") throw Error("Missing bounded native override");
 override.services["code-uno-experimental"].security_opt=["seccomp="+process.env.OFFICE_NATIVE_ARTIFACTS+"/cool-seccomp-profile.json"];
 override.services["code-uno-experimental"].labels={"io.kreoh.native-queue-run":process.env.OFFICE_NATIVE_RUN_ID};
 const font=model.services["code-uno-experimental"].volumes?.find(v=>v.target==="/opt/online-office/share/registry/data/impress-font-control.xcu");
 if(font) {
   if(font.type!=="bind" || !font.read_only) throw Error("Unsafe font control mount");
   override.services["code-uno-experimental"].volumes.push({...font,target:"/workspace-cache/build/builddir/online/engine/instdir/share/registry/data/impress-font-control.xcu"});
 }
 override["x-native-font-control"]=font?"retained":"absent-in-active-configuration";
 process.stdout.write(JSON.stringify(override));
});' > "$state/native-retirement.override.json"
override="$state/native-retirement.override.json"
docker compose "$@" -f "$override" --profile office-uno-experimental config --quiet
docker compose "$@" -f "$override" --profile office-uno-experimental config --format json | node -e '
let data=""; process.stdin.on("data",x=>data+=x); process.stdin.on("end",()=>{
 const m=JSON.parse(data), s=m.services["code-uno-experimental"];
 if(s.image!==process.env.OFFICE_NATIVE_BUILDER_ID || s.user!==process.env.OFFICE_NATIVE_UID+":"+process.env.OFFICE_NATIVE_GID) throw Error("Wrong native image/user");
 if(JSON.stringify(s.entrypoint)!==JSON.stringify(["/bin/bash","/queue-validation/run-queue-unit.sh"])) throw Error("Wrong runner");
 if(s.privileged || s.cap_add?.length || s.security_opt?.some(x=>x.includes("unconfined"))) throw Error("Sandbox bypass");
 if(JSON.stringify(s.security_opt)!==JSON.stringify(["seccomp="+process.env.OFFICE_NATIVE_ARTIFACTS+"/cool-seccomp-profile.json"])) throw Error("Wrong native seccomp policy");
 const aliases=Object.entries(m.services).filter(([name,v])=>name==="code-uno-experimental" || v.networks?.["office-uno-internal"]?.aliases?.includes("code"));
 if(aliases.length!==1) throw Error("Multiple isolated code providers");
});'
# Refuse to replace any pre-existing engine or runner, including stopped containers.
existing="$(docker compose "$@" --profile office-uno-experimental ps --all -q code-uno-experimental office-uno-go-no-go)"
test -z "$existing"
unit_id=""
creation_attempted=0
cleanup_queue_validation()
{
  local status="$1" cleanup_failed=0 owned_id="" discovery_status=0 attempt=0
  trap - EXIT
  trap '' INT TERM
  set +e
  printf '%s\n' "$status" > "$state/launcher.exit"
  # Recover ownership if creation failed or was interrupted before returning its ID.
  if [ -z "$unit_id" ] && [ "$creation_attempted" = 1 ]; then
    # Docker may finish a submitted create after the client times out. Reconcile boundedly.
    for attempt in 1 2 3 4 5 6; do
      owned_id="$(timeout --kill-after=5s 10s docker ps --all --quiet --filter "label=io.kreoh.native-queue-run=$OFFICE_NATIVE_RUN_ID")"
      discovery_status=$?
      printf '%s %s\n' "$attempt" "$discovery_status" >> "$state/ownership-discovery.status"
      if [ "$discovery_status" != 0 ]; then
        cleanup_failed=1
      elif [[ "$owned_id" =~ ^[0-9a-f]{12,64}$ ]]; then
        unit_id="$owned_id"
        break
      elif [ -n "$owned_id" ]; then
        cleanup_failed=1
        break
      fi
      if [ "$attempt" != 6 ]; then sleep 2; fi
    done
    if [ -z "$unit_id" ]; then
      cleanup_failed=1
      printf '%s\n' "Creation ownership unresolved; no unowned container removed." > "$state/ownership-unresolved.txt"
    fi
  fi
  if [ -n "$unit_id" ]; then
    owned_id="$(timeout --kill-after=5s 10s docker inspect --format '{{ index .Config.Labels "io.kreoh.native-queue-run" }}' "$unit_id")"
    if [ "$owned_id" = "$OFFICE_NATIVE_RUN_ID" ]; then
      timeout --kill-after=5s 10s docker inspect --format '{{json .State}}' "$unit_id" > "$state/container-state-before-cleanup.json" || cleanup_failed=1
      timeout --kill-after=5s 15s docker stop --time 5 "$unit_id" > "$state/container-stop.log" 2>&1 || cleanup_failed=1
      timeout --kill-after=5s 10s docker logs "$unit_id" > "$state/container.log" 2>&1 || cleanup_failed=1
      timeout --kill-after=5s 10s docker inspect --format '{{.State.ExitCode}}' "$unit_id" > "$state/container.exit" || cleanup_failed=1
      timeout --kill-after=5s 10s docker rm "$unit_id" > "$state/container-remove.log" 2>&1 || cleanup_failed=1
    else
      cleanup_failed=1
    fi
  fi
  printf '%s\n' "$cleanup_failed" > "$state/cleanup.exit"
  # Preserve the original failure or signal; failed cleanup cannot turn success into PASS.
  if [ "$status" = 0 ] && [ "$cleanup_failed" != 0 ]; then status=1; fi
  exit "$status"
}
trap 'cleanup_queue_validation "$?"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# Create without replacement or starting dependencies. Docker's service container name
# provides the atomic collision boundary if another invocation wins after the precheck.
creation_attempted=1
timeout --kill-after=5s 60s docker compose "$@" -f "$override" --profile office-uno-experimental up --no-start --no-recreate --no-deps --no-build code-uno-experimental
unit_id="$(timeout --kill-after=5s 10s docker ps --all --quiet --filter "label=io.kreoh.native-queue-run=$OFFICE_NATIVE_RUN_ID")"
[[ "$unit_id" =~ ^[0-9a-f]{12,64}$ ]]
test "$(timeout --kill-after=5s 10s docker inspect --format '{{ index .Config.Labels "io.kreoh.native-queue-run" }}' "$unit_id")" = "$OFFICE_NATIVE_RUN_ID"
# Start only the exact verified container, never a service-level target.
timeout --kill-after=5s 30s docker start "$unit_id" > "$state/container-start.log"
deadline=$((SECONDS + 180))
until [ "$(timeout --kill-after=5s 10s docker inspect --format '{{.State.Health.Status}}' "$unit_id")" = healthy ]; do
  test "$SECONDS" -lt "$deadline"
  test "$(timeout --kill-after=5s 10s docker inspect --format '{{.State.Running}}' "$unit_id")" = true
  sleep 1
done
set +e
timeout --signal=TERM --kill-after=10s 420s docker compose "$@" -f "$override" --profile office-uno-experimental run --rm --no-deps --use-aliases office-uno-go-no-go > "$state/java.log" 2>&1
java_result=$?
set -e
printf '%s\n' "$java_result" > "$state/java.exit"
# Unit mode has its own bounded timeout. Capture both outcomes before teardown.
timeout --kill-after=5s 330s docker wait "$unit_id" > "$state/container.exit"
test "$java_result" = 0
test "$(cat "$state/container.exit")" = 0
test "$(cat "$state/unit.exit")" = 0
grep -q '^:test-result: PASS' "$state/unit-agent-control.trs"
# EXIT collects logs and removes only the container bearing this invocation's ownership label.
QUEUE_LAUNCH
        chmod 755 "$report_dir/run-queue-unit.sh" "$report_dir/run-queue-validation.sh"
      fi
      # Exercise the dormant cancellation reader through real local descriptors.
      (cd test && CPPUNIT_TEST_NAME='^ScriptCancellationTests::' \
        timeout --signal=TERM --kill-after=10s 120s ./unittest --verbose --cert-path ../etc) \
        > "$report_dir/script-cancellation.log" 2>&1 || exit $?
      for cancellation_test in testIdentityAndLateStop testStopBeforeStart testUnknownAndBound \
          testMalformedAndDisconnect testShutdownAndDeliveryFailure testBackpressure \
          testSequentialReuseAndReplay testRemoteReservationAndStop testReplyValidation \
          testLostAcknowledgement; do
        grep -q "START ScriptCancellationTests::$cancellation_test" "$report_dir/script-cancellation.log" || exit 1
        grep -q "END ScriptCancellationTests::$cancellation_test" "$report_dir/script-cancellation.log" || exit 1
      done
      # Exercise both KitQueue absorption branches and existing invalidation cases.
      (cd test && CPPUNIT_TEST_NAME='^KitQueueTests::testCallbackInvalidation' \
        timeout --signal=TERM --kill-after=10s 120s ./unittest --verbose --cert-path ../etc) \
        > "$report_dir/kit-queue-invalidation.log" 2>&1 || exit $?
      grep -q 'START KitQueueTests::testCallbackInvalidationOrigin' "$report_dir/kit-queue-invalidation.log" || exit 1
      grep -q 'END KitQueueTests::testCallbackInvalidationOrigin' "$report_dir/kit-queue-invalidation.log" || exit 1
      # Select the wire parser regression through the existing CppUnit runner.
      : > "$report_dir/tile-invalidation-parser.trs" || exit 1
      set +e
      (cd test && CPPUNIT_TEST_NAME='^TileCacheTests::testInvalidateMessageParsing$' \
        COOL_TEST_OPTIONS='' timeout --signal=TERM --kill-after=10s 120s ./run_unit.sh \
        --test-name unit-tiletest.la --log-file "$report_dir/tile-invalidation-parser.log" \
        --trs-file "$report_dir/tile-invalidation-parser.trs" --fail-fast)
      parser_runner_status=$?
      set -e
      printf '%s\n' "$parser_runner_status" > "$report_dir/tile-invalidation-parser-exit.txt"
      test "$parser_runner_status" -eq 0 || exit 1
      grep -q '^:test-result: PASS' "$report_dir/tile-invalidation-parser.trs" || exit 1
      ! grep -Eq '^:test-result: (FAIL|SKIP|ERROR)' "$report_dir/tile-invalidation-parser.trs" || exit 1
      # An unmatched CppUnit filter can report success without running a test.
      grep -q 'START TileCacheTests::testInvalidateMessageParsing' "$report_dir/tile-invalidation-parser.log" || exit 1
      grep -q 'END TileCacheTests::testInvalidateMessageParsing' "$report_dir/tile-invalidation-parser.log" || exit 1
      : > "$report_dir/unit-agent-script-control.trs" || exit 1
      (cd test && timeout --signal=TERM --kill-after=10s 2800s ./run_unit.sh \
        --test-name unit-agent-script-control.la \
        --log-file "$report_dir/unit-agent-script-control.log" \
        --trs-file "$report_dir/unit-agent-script-control.trs" --fail-fast) || exit 1
      grep -q '^:test-result: PASS' "$report_dir/unit-agent-script-control.trs" || exit 1
      ! grep -Eq '^:test-result: (FAIL|SKIP|ERROR)' "$report_dir/unit-agent-script-control.trs" || exit 1
      ! grep -Eq 'private-controlled-(error|source)-marker' "$report_dir/unit-agent-script-control.log" || exit 1
      for script_case in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21; do
        grep -Eq "AgentScriptControlTests${script_case} .*SUCCESS: exitTest:" "$report_dir/unit-agent-script-control.log" || exit 1
      done
      : > "$report_dir/unit-agent-control.trs" || exit 1
      set +e
      # Seventeen units at 30s each, plus 90s for startup and teardown.
      (cd test && timeout --signal=TERM --kill-after=10s 600s ./run_unit.sh --test-name unit-agent-control.la \
        --log-file "$report_dir/unit-agent-control.log" \
        --trs-file "$report_dir/unit-agent-control.trs" --fail-fast)
      runner_status=$?
      set -e
      printf '%s\n' "$runner_status" > "$report_dir/runner-exit.txt"
      test "$runner_status" -eq 0 || exit 1
      grep -q '^:test-result: PASS' "$report_dir/unit-agent-control.trs" || exit 1
      ! grep -Eq '^:test-result: (FAIL|SKIP|ERROR)' "$report_dir/unit-agent-control.trs"
    ) > "$report_dir/build-and-test.log" 2>&1
  ) || exit 1
fi

python3 "$SRCDIR/debrand.py" rootfs "$INSTDIR" "$BUILDDIR/online" || exit 1

# Create new docker image
if [ -z "$NO_DOCKER_IMAGE" ]; then
  cd "$SRCDIR"
  docker_build_args=(
    --build-arg "ONLINE_OFFICE_SOURCE_REVISION=$ONLINE_OFFICE_SOURCE_REVISION" \
    -t "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" -f "$FINAL_DOCKERFILE"
  )
  case "${ONLINE_OFFICE_FINAL_DOCKER_NO_CACHE:-}" in
    1|true|TRUE|yes|YES) docker_build_args=(--no-cache "${docker_build_args[@]}") ;;
  esac
  DOCKER_BUILDKIT=1 docker build "${docker_build_args[@]}" . || exit 1
  if [ "$FINAL_DOCKERFILE" = Distroless ]; then
    python3 "$SRCDIR/debrand.py" scan-distroless-image \
      "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" \
      "$BUILDDIR/online/engine/static/emscripten/favicon.ico" \
      "$ONLINE_OFFICE_SOURCE_REVISION" || exit 1
    DOCKER_BUILDKIT=1 docker build \
      --build-arg "RUNTIME_IMAGE=$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" \
      -f RuntimeVerifier.Dockerfile . || exit 1
    sh "$SRCDIR/verify-running-image.sh" \
      "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" || exit 1
    sh "$SRCDIR/report-image-size.sh" \
      "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" || exit 1
  else
    python3 "$SRCDIR/debrand.py" scan-image \
      "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" \
      "$BUILDDIR/online/engine/static/emscripten/favicon.ico" \
      "$ONLINE_OFFICE_SOURCE_REVISION" || exit 1
  fi
else
  echo "Skipping docker image build"
fi;
