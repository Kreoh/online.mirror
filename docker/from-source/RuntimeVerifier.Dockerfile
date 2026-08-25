# syntax=docker/dockerfile:1.7
ARG RUNTIME_IMAGE
ARG VERIFIER_IMAGE="debian:stable@sha256:a317324860a60f88f98be05d1cab92f2262ef03884d1a6d7894894732ac9eb42"

FROM ${RUNTIME_IMAGE} AS runtime

FROM ${VERIFIER_IMAGE}
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get -y install --no-install-recommends binutils libcap2-bin python3 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=runtime / /merged-rootfs
COPY --chmod=755 /verify-source-rootfs.sh /usr/local/bin/verify-source-rootfs
COPY --chmod=755 /verify-elf-closure.sh /usr/local/bin/verify-elf-closure.sh
COPY /verify_elf_closure.py /usr/local/bin/verify_elf_closure.py
COPY /runtime-dynamic-dependencies.conf /usr/local/share/online-office/runtime-dynamic-dependencies.conf
RUN /usr/local/bin/verify-source-rootfs --merged /merged-rootfs
