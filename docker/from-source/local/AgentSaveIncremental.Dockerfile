# syntax=docker/dockerfile:1.7
ARG BUILDER_IMAGE=kreoh-collabora-builder@sha256:6a45bccbfefe9d8bc148a6dd46fbbe629bf7b0359c8e8cd8854fbd7adb26003a
ARG RUNTIME_IMAGE=chatui-collabora@sha256:aedff845dc2b5b11e5e999aa616488739178a340be2290435f1ad8aeddb73121
ARG KREOH_SOURCE_REVISION=9b56f5583ab8df2202fb0b8471dcbf622d7825f8

FROM ${BUILDER_IMAGE} AS clean-test-config

USER builder
RUN cd /build/builddir/online && \
    ./autogen.sh && \
    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --with-lokit-path=/build/builddir/online/engine/include \
        --with-lo-path=/build/builddir/online/engine/instdir \
        --enable-experimental --enable-debug && \
    make -j "$(nproc)" coolwsd

FROM clean-test-config AS bgsave-test-clean

USER builder
RUN make -C /build/builddir/online/test -j "$(nproc)" unit-save-torture.la && \
    mkdir -p /tmp/cool-bgsave-child-roots /tmp/cool-bgsave-cache && \
    cd /build/builddir/online && \
    ./coolwsd \
        --o:sys_template_path=/build/builddir/online/engine/instdir \
        --o:child_root_path=/tmp/cool-bgsave-child-roots \
        --o:cache_files.path=/tmp/cool-bgsave-cache \
        --o:security.capabilities=false \
        --o:mount_jail_tree=false \
        --o:storage.filesystem[@allow]=true \
        --o:logging.level=warning \
        --o:logging.protocol=false \
        --o:logging.file[@enable]=false \
        --o:logging_ui_cmd.file[@enable]=false \
        --o:ssl.key_file_path=/build/builddir/online/etc/key.pem \
        --o:ssl.cert_file_path=/build/builddir/online/etc/cert.pem \
        --o:ssl.ca_file_path=/build/builddir/online/etc/ca-chain.cert.pem \
        --o:admin_console.username=admin \
        --o:admin_console.password=admin \
        --o:storage.ssl.enable=false \
        --unattended \
        --unitlib=/build/builddir/online/test/.libs/unit-save-torture.so

FROM ${BUILDER_IMAGE} AS incremental-builder
ARG KREOH_SOURCE_REVISION

USER root
COPY --from=collabora_source --chown=builder .git/refs/heads/kreoh-co-26.04.3.1-agent /tmp/kreoh-source-head
RUN test "$(cat /tmp/kreoh-source-head)" = "$KREOH_SOURCE_REVISION"
COPY --from=collabora_source --chown=builder common/Authorization.hpp /build/builddir/online/common/Authorization.hpp
COPY --from=collabora_source --chown=builder kit/ChildSession.cpp /build/builddir/online/kit/ChildSession.cpp
COPY --from=collabora_source --chown=builder kit/ChildSession.hpp /build/builddir/online/kit/ChildSession.hpp
COPY --from=collabora_source --chown=builder kit/Kit.cpp /build/builddir/online/kit/Kit.cpp
COPY --from=collabora_source --chown=builder kit/Kit.hpp /build/builddir/online/kit/Kit.hpp
COPY --from=collabora_source --chown=builder kit/KitWebSocket.cpp /build/builddir/online/kit/KitWebSocket.cpp
COPY --from=collabora_source --chown=builder wsd/ClientRequestDispatcher.cpp /build/builddir/online/wsd/ClientRequestDispatcher.cpp
COPY --from=collabora_source --chown=builder wsd/ClientRequestDispatcher.hpp /build/builddir/online/wsd/ClientRequestDispatcher.hpp
COPY --from=collabora_source --chown=builder wsd/ClientSession.cpp /build/builddir/online/wsd/ClientSession.cpp
COPY --from=collabora_source --chown=builder wsd/ClientSession.hpp /build/builddir/online/wsd/ClientSession.hpp
COPY --from=collabora_source --chown=builder wsd/DocumentBroker.cpp /build/builddir/online/wsd/DocumentBroker.cpp
COPY --from=collabora_source --chown=builder wsd/DocumentBroker.hpp /build/builddir/online/wsd/DocumentBroker.hpp
COPY --from=collabora_source --chown=builder wsd/wopi/WopiStorage.cpp /build/builddir/online/wsd/wopi/WopiStorage.cpp
COPY --from=collabora_source --chown=builder wsd/wopi/WopiStorage.hpp /build/builddir/online/wsd/wopi/WopiStorage.hpp

USER builder
RUN make -C /build/builddir/online -j "$(nproc)" && \
    make -C /build/builddir/online DESTDIR=/build/instdir install

FROM incremental-builder AS agent-save-test-config

USER root
COPY --from=collabora_source --chown=builder test/Makefile.am /build/builddir/online/test/Makefile.am

USER builder
RUN cd /build/builddir/online && \
    ./autogen.sh && \
    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --with-lokit-path=/build/builddir/online/engine/include \
        --with-lo-path=/build/builddir/online/engine/instdir \
        --enable-experimental --enable-debug && \
    make -j "$(nproc)" coolwsd

FROM agent-save-test-config AS agent-save-test

USER root
COPY --from=collabora_source --chown=builder test/UnitAgentSave.cpp /build/builddir/online/test/UnitAgentSave.cpp

USER builder
RUN make -C /build/builddir/online/test -j "$(nproc)" unit-agent-save.la && \
    mkdir -p /tmp/cool-agent-save-child-roots /tmp/cool-agent-save-cache && \
    cd /build/builddir/online && \
    ./coolwsd \
        --o:sys_template_path=/build/builddir/online/engine/instdir \
        --o:child_root_path=/tmp/cool-agent-save-child-roots \
        --o:cache_files.path=/tmp/cool-agent-save-cache \
        --o:security.capabilities=false \
        --o:mount_jail_tree=false \
        --o:storage.filesystem[@allow]=true \
        --o:logging.level=warning \
        --o:logging.protocol=false \
        --o:logging.file[@enable]=false \
        --o:logging_ui_cmd.file[@enable]=false \
        --o:ssl.key_file_path=/build/builddir/online/etc/key.pem \
        --o:ssl.cert_file_path=/build/builddir/online/etc/cert.pem \
        --o:ssl.ca_file_path=/build/builddir/online/etc/ca-chain.cert.pem \
        --o:admin_console.username=admin \
        --o:admin_console.password=admin \
        --o:storage.ssl.enable=false \
        --o:experimental_features=true \
        --unattended \
        --unitlib=/build/builddir/online/test/.libs/unit-agent-save.so

FROM agent-save-test-config AS bgsave-test-patched

USER builder
RUN make -C /build/builddir/online/test -j "$(nproc)" unit-save-torture.la && \
    mkdir -p /tmp/cool-bgsave-child-roots /tmp/cool-bgsave-cache && \
    cd /build/builddir/online && \
    ./coolwsd \
        --o:sys_template_path=/build/builddir/online/engine/instdir \
        --o:child_root_path=/tmp/cool-bgsave-child-roots \
        --o:cache_files.path=/tmp/cool-bgsave-cache \
        --o:security.capabilities=false \
        --o:mount_jail_tree=false \
        --o:storage.filesystem[@allow]=true \
        --o:logging.level=warning \
        --o:logging.protocol=false \
        --o:logging.file[@enable]=false \
        --o:logging_ui_cmd.file[@enable]=false \
        --o:ssl.key_file_path=/build/builddir/online/etc/key.pem \
        --o:ssl.cert_file_path=/build/builddir/online/etc/cert.pem \
        --o:ssl.ca_file_path=/build/builddir/online/etc/ca-chain.cert.pem \
        --o:admin_console.username=admin \
        --o:admin_console.password=admin \
        --o:storage.ssl.enable=false \
        --unattended \
        --unitlib=/build/builddir/online/test/.libs/unit-save-torture.so

FROM ${RUNTIME_IMAGE}
ARG KREOH_SOURCE_REVISION

USER root
COPY --from=incremental-builder /build/instdir/ /
RUN setcap cap_fowner,cap_chown,cap_sys_chroot=ep /usr/bin/coolforkit-caps && \
    setcap cap_sys_admin=ep /usr/bin/coolmount

LABEL org.opencontainers.image.source="https://github.com/Kreoh/online.mirror" \
      org.opencontainers.image.revision="${KREOH_SOURCE_REVISION}" \
      org.opencontainers.image.version="26.04.3.1-agent-save-local"

USER 1001
