This directory contains the source-built Online Office OCI pipeline. `build.sh`
builds the engine from the monorepo's `engine/` directory, builds `coolwsd`,
applies the debranding policy and packages the exact staged installation with
the pinned `Distroless` Dockerfile used by Jenkins. Python, PyUNO and Java are
disabled in the engine and rejected by the runtime checks.

The distroless assembler follows the current upstream hardened image design,
adapted for a source `instdir` rather than vendor Debian packages. It creates
the systemplate in a Debian assembler and recursively resolves dependencies as
the dynamic loader would, including PT_INTERP, architecture, RPATH, RUNPATH,
FHS symlinks and symbol versions. PAM remains supported and `libpam.so.0` is an
explicit closure contract. The assembler uses the final target base's glibc,
NSS, resolver and CA objects inside the jail, generates C.UTF-8, and verifies
file capabilities across a BuildKit stage copy. The final stage has one
source-rootfs `COPY` instruction. It has no shell, package manager, Python,
PyUNO, Java, compiler, editor, SSH client, setup utility, conversion and stress
helpers, service files or external system font packs. The source-built engine
supplies the document fonts, including Noto CJK. Debian's `fontconfig-config`
transitively installs a system font in the assembler; the systemplate keeps the
required fontconfig configuration and deletes every Debian font and generated
cache before final verification.

Modern BuildKit is mandatory because the final image relies on preserved
`security.capability` xattrs. Jenkins runs a small capability-copy probe before
the long source compilation. After the build, a verifier image copies and
checks the actual merged runtime rootfs, including capabilities, ownership,
permissions, ELF closure, legal files, branding and forbidden content. Jenkins
then starts the final image with its minimal capability bounding set and waits
for the built-in healthcheck before publication. The published image keeps UID
100. Application, engine, browser, systemplate and legal trees remain
root-owned and read-only; only child roots, cache, the log file, `/tmp`, and the
arbitrary-UID passwd mapping are writable.

`debrand.py` performs the deterministic source transformation before the
expensive compilation, sanitises the staged runtime, and verifies browser
bundles, translations, configuration, documentation, binaries, engine
identity, the SPDX SBOM and neutral favicon. Image inspection copies the
filesystem from a stopped container, so it does not require a shell in the
final image.

`Debian` remains available only as an explicit diagnostic fallback:

```sh
ONLINE_OFFICE_FINAL_DOCKERFILE=Debian docker/from-source/build.sh
```

The diagnostic route receives the common revision, neutral identity, legal,
SBOM and debranding scans. Distroless-only shell, package-manager, healthcheck
and content assertions are not applied to it. Jenkins always sets
`ONLINE_OFFICE_FINAL_DOCKERFILE=Distroless` and also requires the final
`distroless-source` runtime label before publication. Other distribution
Dockerfiles remain available for upstream development and are outside the
publication route.

After each build, `report-image-size.sh` reports Docker's daemon size and the
complete layer history. It separately measures uncompressed saved-layer and
configuration bytes, plus the stored gzip-compressed layer and configuration
bytes. It supports both OCI compressed blobs and older plain `layer.tar`
archives. No size ceiling is enabled until representative source builds exist.
Operators may set `ONLINE_OFFICE_MAX_COMPRESSED_BYTES` once they have chosen a
measured publication ceiling. Based on the present official image and the
local real-payload packaging test, the first amd64 distroless build is expected
to be roughly 450 MB to 550 MB compressed and 1.2 GB to 1.4 GB uncompressed.
This is an estimate, not a release gate.
