This directory contains the host-driven from-source Docker build. `build.sh` requires `COLLABORA_SOURCE_REVISION` to contain the full lowercase revision to build, accepts only the Kreoh repository and `kreoh-co-26.04.3.1-agent` branch, then packages the result using the immutable Debian or Ubuntu base selected from the supported host. Its sole accepted output tag is derived from the first twelve characters of that revision.

For the local incremental Docker route, pass the same full revision with `--build-arg COLLABORA_SOURCE_REVISION="$(git rev-parse HEAD)"`. The build verifies that value against `git rev-parse HEAD`, the branch and the origin inside the supplied `collabora_source` context, then records it in `org.opencontainers.image.revision`.

Alpine, Arch Linux, Fedora and openSUSE routes are disabled until each route has immutable base inputs and equivalent source and revision enforcement.
