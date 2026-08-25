#!/bin/sh
set -eu

test "$#" -eq 1 || {
    echo 'verify-running-image: usage: IMAGE' >&2
    exit 1
}

image=$1
container="online-office-runtime-verify-$$"
cleanup()
{
    docker rm -f "$container" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

docker create \
    --name "$container" \
    --cap-drop ALL \
    --cap-add CHOWN \
    --cap-add FOWNER \
    --cap-add MKNOD \
    --cap-add SYS_ADMIN \
    --cap-add SYS_CHROOT \
    "$image" >/dev/null
docker start "$container" >/dev/null

attempt=0
while test "$attempt" -lt 48; do
    state=$(docker inspect --format \
        '{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{else}}missing{{end}}' \
        "$container")
    case "$state" in
        running\|healthy) break ;;
        exited\|*|dead\|*)
            docker logs "$container" >&2 || true
            echo "verify-running-image: coolwsd stopped before becoming healthy: $state" >&2
            exit 1
            ;;
    esac
    attempt=$((attempt + 1))
    sleep 5
done

test "$state" = 'running|healthy' || {
    docker logs "$container" >&2 || true
    echo "verify-running-image: healthcheck did not pass: $state" >&2
    exit 1
}
docker exec "$container" /usr/bin/coolwsd --probe --use-env-vars
echo 'Merged distroless image launched and reached healthy status.'
