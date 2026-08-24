# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

FROM debian:stable@sha256:a317324860a60f88f98be05d1cab92f2262ef03884d1a6d7894894732ac9eb42

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get -y install --no-install-recommends \
        acl \
        ant \
        ant-optional \
        autoconf \
        automake \
        bison \
        brotli \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        curl \
        default-jdk \
        docker-cli \
        doxygen \
        flex \
        fontconfig \
        gperf \
        graphviz \
        git \
        junit4 \
        libarchive-tools \
        libexpat1-dev \
        libcap-dev \
        libcups2-dev \
        libfontconfig1-dev \
        libgstreamer-plugins-base1.0-dev \
        libgstreamer1.0-dev \
        libgtk-3-dev \
        libkf5config-dev \
        libkf5coreaddons-dev \
        libkf5i18n-dev \
        libkf5kio-dev \
        libkf5windowsystem-dev \
        libkrb5-dev \
        libcppunit-dev \
        libpam0g-dev \
        libpcre2-dev \
        libpng-dev \
        libssl-dev \
        libtool \
        libunwind-dev \
        libxrandr-dev \
        libxml2-utils \
        libxslt1-dev \
        libzstd-dev \
        meson \
        nasm \
        ninja-build \
        nodejs \
        npm \
        pkg-config \
        python3 \
        python3-dev \
        python3-lxml \
        python3-polib \
        qtbase5-dev \
        rsync \
        unzip \
        uuid-runtime \
        wget \
        xsltproc \
        zip \
        zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
