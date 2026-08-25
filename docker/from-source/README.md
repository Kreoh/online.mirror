This directory contains the source-built Online Office OCI pipeline. `build.sh`
builds the engine from the monorepo's `engine/` directory, builds `coolwsd`,
applies the debranding policy and packages the result with the Debian runtime
Dockerfile used by Jenkins.

`debrand.py` performs the deterministic source transformation before the
expensive compilation, sanitises the staged runtime and verifies the browser
bundles, translations, configuration, documentation, binaries, engine identity,
SBOM and neutral favicon.

Other distribution Dockerfiles remain available for upstream development, but
they are outside the Jenkins image route.
