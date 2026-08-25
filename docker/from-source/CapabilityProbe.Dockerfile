# syntax=docker/dockerfile:1.7
ARG BUILDER_IMAGE

FROM ${BUILDER_IMAGE} AS capability-source
RUN cp /bin/true /capability-source && \
    setcap cap_chown=ep /capability-source

FROM ${BUILDER_IMAGE}
COPY --from=capability-source /capability-source /capability-copy
RUN test "$(getcap -n /capability-copy | sed 's/^[^ ]* *//')" = 'cap_chown=ep'
