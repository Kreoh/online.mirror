pipeline {
    agent {
        node {
            label 'online-office-builder'
            customWorkspace "workspace/${env.JOB_NAME}"
        }
    }
    parameters {
        choice(name: 'BUILD_MODE', choices: ['image', 'client-only'], description: 'Build and publish an image by default; select client-only to export from an existing image.')
        string(name: 'CLIENT_SOURCE_REVISION', defaultValue: '262f997a4b1fccadbe45664a8f21889af3632447', description: 'Full target source revision, independent of the pipeline revision.')
        string(name: 'CLIENT_IMAGE', defaultValue: 'ghcr.io/kreoh/online-office@sha256:bffd112aa342cfcfdeac6bab9884d787f77786cbf17b2bd230a0bb92a6c9aee6', description: 'Exact existing image for client-only export; must already be present on the Jenkins agent.')
        booleanParam(name: 'NATIVE_VALIDATION_ONLY', defaultValue: false, description: 'Run native tests in an isolated persistent cache without building or publishing a runtime image. Requires image mode.')
        booleanParam(name: 'EXPORT_QUEUE_VALIDATION', defaultValue: false, description: 'Archive the exact builder and debug unit runtime for isolated queue validation. Does not authorise publication.')
    }
    environment {
        IMAGE_BASE_NAME = 'ghcr.io/kreoh/online-office'
        SOURCE_BRANCH = 'main'
    }
    options {
        skipDefaultCheckout(true)
        disableConcurrentBuilds()
        timeout(time: 12, unit: 'HOURS')
        lock(resource: "online-office-build-${env.BRANCH_NAME}", inversePrecedence: true)
    }
    stages {
        stage('Set Build Variables') {
            steps {
                script {
                    if (params.NATIVE_VALIDATION_ONLY && params.BUILD_MODE != 'image') {
                        error('NATIVE_VALIDATION_ONLY requires BUILD_MODE=image')
                    }
                    if (params.EXPORT_QUEUE_VALIDATION && !params.NATIVE_VALIDATION_ONLY) {
                        error('EXPORT_QUEUE_VALIDATION requires NATIVE_VALIDATION_ONLY')
                    }
                    // Export explicit defaults when a newly added parameter has no build environment entry.
                    env.NATIVE_VALIDATION_ONLY = params.NATIVE_VALIDATION_ONLY ? 'true' : 'false'
                    env.EXPORT_QUEUE_VALIDATION = params.EXPORT_QUEUE_VALIDATION ? 'true' : 'false'
                    env.ONLINE_OFFICE_RUN_AGENT_CONTROL = params.NATIVE_VALIDATION_ONLY ? '1' : '0'
                    env.ONLINE_OFFICE_RUN_JSUNO = params.NATIVE_VALIDATION_ONLY ? '1' : '0'
                    env.NO_DOCKER_IMAGE = params.NATIVE_VALIDATION_ONLY ? '1' : ''
                    env.SHOULD_BUILD = (env.BRANCH_NAME == env.SOURCE_BRANCH && params.BUILD_MODE in ['client-only', 'image'])
                    env.BUILD_MODE = params.BUILD_MODE ?: ''
                }
            }
        }
        stage('Checkout Source') {
            when {
                expression {
                    env.SHOULD_BUILD.toBoolean()
                }
            }
            steps {
                script {
                    // Use the revision from which Jenkins loaded this pipeline.
                    def scmVars = checkout(scm)
                    env.ONLINE_OFFICE_SOURCE_REVISION =
                        scmVars.GIT_COMMIT ?: sh(script: 'git rev-parse HEAD', returnStdout: true).trim()
                    env.PIPELINE_REVISION = env.ONLINE_OFFICE_SOURCE_REVISION
                    def sourceVersion = sh(
                        script: '''awk -F'[][]' '$2 == "coolwsd" { print $4; exit }' configure.ac''',
                        returnStdout: true
                    ).trim()
                    if (!sourceVersion) {
                        error('Could not read the source version from configure.ac')
                    }
                    env.SOURCE_VERSION = "${sourceVersion}-agent-save"
                    env.IMAGE_TAG = "${env.SOURCE_VERSION}-${env.ONLINE_OFFICE_SOURCE_REVISION.take(12)}"
                    echo "Checked out ${env.ONLINE_OFFICE_SOURCE_REVISION}; image tag is ${env.IMAGE_TAG}"
                }
            }
        }
        stage('Validate Source') {
            when {
                expression {
                    env.SHOULD_BUILD.toBoolean() && params.BUILD_MODE == 'image'
                }
            }
            steps {
                sh '''
                    set -eu
                    case "$ONLINE_OFFICE_SOURCE_REVISION" in
                        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
                        *) echo "ONLINE_OFFICE_SOURCE_REVISION must be a full lower-case Git revision." >&2; exit 1 ;;
                    esac
                    test "$(git rev-parse HEAD)" = "$ONLINE_OFFICE_SOURCE_REVISION"
                    test "$(grep -Fxc -- '--without-branding' engine/distro-configs/OnlineLinuxCommon.conf)" -eq 1
                    ! grep -Eq -- '--with-branding|--with-vendor' \
                        engine/distro-configs/OnlineLinuxCommon.conf \
                        engine/distro-configs/OnlineLinux-LOKit.conf
                    grep -Fq 'INCLUDE:OnlineLinuxCommon' engine/distro-configs/OnlineLinux-LOKit.conf
                    grep -Fqx -- '--disable-python' engine/distro-configs/OnlineLinux-LOKit.conf
                    grep -Fqx -- '--without-java' engine/distro-configs/OnlineLinux-LOKit.conf
                    ! grep -Eq -- '--enable-python|--with-python' \
                        engine/distro-configs/OnlineLinuxCommon.conf \
                        engine/distro-configs/OnlineLinux-LOKit.conf
                    grep -Fq -- '--with-distro=OnlineLinux-LOKit' docker/from-source/build.sh
                    grep -Fq -- '--with-lo-builddir="$BUILDDIR"/online/engine' docker/from-source/build.sh
                    grep -Fq -- '--with-lo-path=/opt/online-office' docker/from-source/build.sh
                    grep -Fq -- '--without-app-branding' docker/from-source/build.sh
                    grep -Fq -- "--with-app-name='Online Office'" docker/from-source/build.sh
                    grep -Fq -- '--with-app-package-name=org.onlineoffice.app' docker/from-source/build.sh
                    grep -Fq -- '--without-vendor' docker/from-source/build.sh
                    grep -Fq -- '--with-info-url=about:blank' docker/from-source/build.sh
                    test ! -e browser/images/collabora-office-white.svg
                    grep -Fq 'background-image: none;' browser/css/backstage.css
                    bash docker/from-source/test-debranding.sh
                '''
            }
        }
        stage('Build Image') {
            when {
                expression {
                    env.SHOULD_BUILD.toBoolean() && params.BUILD_MODE == 'image'
                }
            }
            steps {
                sh '''
                    set -eu
                    export DOCKER_BUILDKIT=1
                    docker buildx version >/dev/null
                    builder_tag="$IMAGE_BASE_NAME:source-builder"
                    docker build \
                        -t "$builder_tag" \
                        -f docker/from-source/Builder.Dockerfile \
                        docker/from-source
                    builder_id="$(docker image inspect --format '{{.Id}}' "$builder_tag")"
                    queue_report="$PWD/native-retirement-reports/$BUILD_NUMBER"
                    if [ "$EXPORT_QUEUE_VALIDATION" = true ]; then
                        mkdir -p "$queue_report"
                        printf '%s\\n' "$builder_id" > "$queue_report/builder-image.id"
                        docker image inspect "$builder_id" > "$queue_report/builder-image.json"
                        (ulimit -f 20971520; docker image save -o "$queue_report/builder-image.tar" "$builder_id")
                        gzip -1 "$queue_report/builder-image.tar"
                        test -s "$queue_report/builder-image.tar.gz"
                        (cd "$queue_report" && sha256sum builder-image.tar.gz > builder-image.sha256)
                    fi
                    docker run --rm \
                        -v "$PWD:/workspace:ro" \
                        -w /workspace \
                        "$builder_tag" \
                        bash docker/from-source/test-distroless.sh
                    docker build --no-cache \
                        --build-arg "BUILDER_IMAGE=$builder_tag" \
                        -f docker/from-source/CapabilityProbe.Dockerfile \
                        docker/from-source
                    uid="$(id -u)"
                    gid="$(id -g)"
                    docker_socket=/var/run/docker.sock
                    docker_gid="$(stat -c '%g' "$docker_socket")"
                    workspace_parent="$(dirname "$PWD")"
                    cache_root="${ONLINE_OFFICE_SOURCE_CACHE_ROOT:-$workspace_parent/.online-office-source-cache}"
                    cache_branch="$(printf '%s' "$SOURCE_BRANCH" | tr -c 'A-Za-z0-9_.-' '_')"
                    if [ "$NATIVE_VALIDATION_ONLY" = true ]; then
                        cache_branch="$cache_branch-native-validation"
                    fi
                    source_cache_host="$cache_root/$cache_branch"
                    mkdir -p "$source_cache_host"/build "$source_cache_host"/ccache "$source_cache_host"/home
                    docker run --rm \
                        --user "$uid:$gid" \
                        --group-add "$docker_gid" \
                        -v "$PWD:/workspace" \
                        -v "$source_cache_host:/workspace-cache" \
                        -v "$docker_socket:$docker_socket" \
                        -w /workspace \
                        -e DOCKER_HUB_REPO="$IMAGE_BASE_NAME" \
                        -e DOCKER_HUB_TAG="$IMAGE_TAG" \
                        -e ONLINE_OFFICE_SOURCE_REVISION="$ONLINE_OFFICE_SOURCE_REVISION" \
                        -e ONLINE_OFFICE_SOURCE_CACHE_DIR=/workspace-cache/build \
                        -e CCACHE_DIR=/workspace-cache/ccache \
                        -e HOME=/workspace-cache/home \
                        "$builder_tag" \
                        bash docker/from-source/check-builder.sh
                    docker run --rm \
                        --security-opt "seccomp=$PWD/docker/cool-seccomp-profile.json" \
                        --user "$uid:$gid" \
                        --group-add "$docker_gid" \
                        -v "$PWD:/workspace" \
                        -v "$source_cache_host:/workspace-cache" \
                        -v "$docker_socket:$docker_socket" \
                        -w /workspace \
                        -e DOCKER_HUB_REPO="$IMAGE_BASE_NAME" \
                        -e DOCKER_HUB_TAG="$IMAGE_TAG" \
                        -e ONLINE_OFFICE_SOURCE_REVISION="$ONLINE_OFFICE_SOURCE_REVISION" \
                        -e ONLINE_OFFICE_SOURCE_BRANCH="$SOURCE_BRANCH" \
                        -e ONLINE_OFFICE_FINAL_DOCKERFILE=Distroless \
                        -e ONLINE_OFFICE_RUN_AGENT_CONTROL="$ONLINE_OFFICE_RUN_AGENT_CONTROL" \
                        -e ONLINE_OFFICE_RUN_JSUNO="$ONLINE_OFFICE_RUN_JSUNO" \
                        -e NO_DOCKER_IMAGE="${NO_DOCKER_IMAGE:-}" \
                        -e ONLINE_OFFICE_EXPORT_QUEUE_VALIDATION="$EXPORT_QUEUE_VALIDATION" \
                        -e ONLINE_OFFICE_AGENT_CONTROL_REPORT_DIR="/workspace/native-retirement-reports/$BUILD_NUMBER" \
                        -e ONLINE_OFFICE_SOURCE_CACHE_DIR=/workspace-cache/build \
                        -e CCACHE_DIR=/workspace-cache/ccache \
                        -e HOME=/workspace-cache/home \
                        "$builder_tag" \
                        docker/from-source/build.sh
                    if [ "$NATIVE_VALIDATION_ONLY" = true ]; then
                        exit 0
                    fi
                    revision=$(
                        docker image inspect \
                            --format '{{ index .Config.Labels "org.opencontainers.image.revision" }}' \
                            "$IMAGE_BASE_NAME:$IMAGE_TAG"
                    )
                    test "$revision" = "$ONLINE_OFFICE_SOURCE_REVISION"
                    runtime_route=$(
                        docker image inspect \
                            --format '{{ index .Config.Labels "io.kreoh.online-office.runtime" }}' \
                            "$IMAGE_BASE_NAME:$IMAGE_TAG"
                    )
                    test "$runtime_route" = distroless-source
                '''
            }
            post {
                always {
                    archiveArtifacts artifacts: "native-retirement-reports/${env.BUILD_NUMBER}/**",
                                     allowEmptyArchive: true, fingerprint: true
                }
            }
        }
        stage('Export Java Client') {
            when { expression { env.SHOULD_BUILD.toBoolean() && !params.NATIVE_VALIDATION_ONLY } }
            steps {
                script {
                    env.CLIENT_SOURCE_REVISION = params.BUILD_MODE == 'image' ? env.ONLINE_OFFICE_SOURCE_REVISION : params.CLIENT_SOURCE_REVISION
                    env.CLIENT_IMAGE = params.BUILD_MODE == 'image' ? "${env.IMAGE_BASE_NAME}:${env.IMAGE_TAG}" : params.CLIENT_IMAGE
                }
                sh 'bash docker/from-source/export-java-client.sh'
                archiveArtifacts artifacts: "uno-client-export-${env.BUILD_NUMBER}/uno-client.tar*", fingerprint: true
            }
        }
        stage('Push Image') {
            when {
                expression {
                    env.SHOULD_BUILD.toBoolean() && params.BUILD_MODE == 'image' && !params.NATIVE_VALIDATION_ONLY
                }
            }
            steps {
                withCredentials([string(credentialsId: 'ghcr-packages-pat', variable: 'GITHUB_ACCESS_TOKEN')]) {
                    sh '''
                        set -eu
                        test "$NATIVE_VALIDATION_ONLY" != true
                        trap 'docker logout ghcr.io >/dev/null 2>&1 || true' EXIT
                        echo "$GITHUB_ACCESS_TOKEN" | docker login ghcr.io -u x-access-token --password-stdin
                        docker push "$IMAGE_BASE_NAME:$IMAGE_TAG"
                        docker tag "$IMAGE_BASE_NAME:$IMAGE_TAG" "$IMAGE_BASE_NAME:$SOURCE_BRANCH-latest"
                        docker push "$IMAGE_BASE_NAME:$SOURCE_BRANCH-latest"
                        docker image inspect --format '{{json .RepoDigests}}' "$IMAGE_BASE_NAME:$IMAGE_TAG" > "uno-client-export-$BUILD_NUMBER/published-image-digests.json"
                        cp "uno-client-export-$BUILD_NUMBER/uno-client.tar.sha256" "uno-client-export-$BUILD_NUMBER/published-client.sha256"
                    '''
                }
                archiveArtifacts artifacts: "uno-client-export-${env.BUILD_NUMBER}/published-*", fingerprint: true
            }
        }
    }
    post {
        success {
            script {
                if (env.SHOULD_BUILD.toBoolean() && params.BUILD_MODE == 'image' && !params.NATIVE_VALIDATION_ONLY) {
                    echo "Published ${env.IMAGE_BASE_NAME}:${env.IMAGE_TAG}"
                } else {
                    echo "Skipping image publication for branch ${env.BRANCH_NAME}."
                }
            }
        }
    }
}
