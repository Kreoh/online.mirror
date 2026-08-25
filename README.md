# Online Office

Online Office is a source-built, browser-based office suite for collaborative editing. The
Debian OCI image in this branch builds the document engine and server from source, then publishes
the image as `ghcr.io/kreoh/online-office`.

The source is derived from the upstream Collabora Online and Collabora Office projects. Their
copyright notices, contributor attribution, compatibility identifiers and applicable licences are
retained. The distributed product identity is Online Office.

## Components

- `engine/`: document rendering engine
- `wsd/`: WebSocket and HTTP server
- `kit/`: jailed document process
- `browser/`: browser client
- `common/`: shared server code
- `test/`: C++ tests

## Building the Debian OCI image

The Jenkins pipeline uses `docker/from-source/Builder.Dockerfile` and
`docker/from-source/build.sh`. It builds the engine with the `OnlineLinux-LOKit` configuration,
builds the server against that engine, assembles a Debian runtime root filesystem and applies the
debranding verification gates before publishing.

For a local source build, provide a full Git revision and use the same builder image and environment
variables as the Jenkinsfile. The build is deliberately pinned to an exact source revision.

## Development build

For development outside the OCI pipeline:

```sh
cd engine
./autogen.sh --with-distro=OnlineLinux-LOKit
make
cd ..
./autogen.sh --enable-developer
make
```

Run `make run` from the top-level build tree to start `coolwsd`.

## Integration

Online Office implements the WOPI discovery and browser integration surfaces under the existing
technical `/cool` routes. The `coolwsd` executable and protocol names remain unchanged for
compatibility.

## Licences and attribution

The primary licence is the Mozilla Public License 2.0. Some components use other open-source
licences. See [COPYING](COPYING), [browser/LICENSE](browser/LICENSE),
[THIRDPARTYLICENSES](THIRDPARTYLICENSES) and the source headers for details.

Upstream provenance is recorded in Git history and the OCI
`org.opencontainers.image.source` label. Factual upstream names in legal notices, copyright headers
and compatibility markers are preserved.
