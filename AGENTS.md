# Native builds

Collabora Online and its bundled LibreOffice core must be built only in Jenkins,
using the existing Jenkins pipeline. Do not compile them locally, build local
builder images, run local native configuration, or run local test targets that
compile native code. Do not fall back to a local build when Jenkins is unavailable.

Local read-only source inspection and lightweight checks that do not compile are
allowed. Use existing authorisation for Jenkins build work without requesting it
again. Build authorisation does not implicitly authorise publication, deployment,
or pushing source changes.
