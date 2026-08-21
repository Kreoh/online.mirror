pipeline {
    agent {
        node {
            label 'docker'
            customWorkspace "workspace/${env.JOB_NAME}"
        }
    }
    environment {
        IMAGE_BASE_NAME = 'ghcr.io/kreoh/collabora-online'
        SOURCE_BRANCH = 'kreoh-co-26.04.3.1-agent'
        SOURCE_VERSION = '26.04.3.1-agent-save'
    }
    options {
        skipDefaultCheckout(true)
        disableConcurrentBuilds()
        timeout(time: 12, unit: 'HOURS')
        lock(resource: "collabora-online-build-${env.BRANCH_NAME}", inversePrecedence: true)
    }
    stages {
        stage('Set Build Variables') {
            steps {
                script {
                    env.SHOULD_BUILD = (env.BRANCH_NAME == env.SOURCE_BRANCH)
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
                    def scmVars = checkout([
                        $class: 'GitSCM',
                        branches: [[name: "*/${env.SOURCE_BRANCH}"]],
                        doGenerateSubmoduleConfigurations: false,
                        extensions: [
                            [$class: 'CloneOption', shallow: true, depth: 1, noTags: true, honorRefspec: true, timeout: 120],
                            [$class: 'CheckoutOption', timeout: 120],
                            [$class: 'CleanBeforeCheckout']
                        ],
                        userRemoteConfigs: [[
                            credentialsId: '61c9e22d-1680-43b6-9452-6ef9c2f2a59c',
                            url: 'https://github.com/Kreoh/online.mirror.git',
                            refspec: "+refs/heads/${env.SOURCE_BRANCH}:refs/remotes/origin/${env.SOURCE_BRANCH}"
                        ]]
                    ])
                    env.COLLABORA_SOURCE_REVISION =
                        scmVars.GIT_COMMIT ?: sh(script: 'git rev-parse HEAD', returnStdout: true).trim()
                    env.IMAGE_TAG = "${env.SOURCE_VERSION}-${env.COLLABORA_SOURCE_REVISION.take(12)}"
                    echo "Checked out ${env.COLLABORA_SOURCE_REVISION}; image tag is ${env.IMAGE_TAG}"
                }
            }
        }
        stage('Validate Source') {
            when {
                expression {
                    env.SHOULD_BUILD.toBoolean()
                }
            }
            steps {
                sh '''
                    set -eu
                    case "$COLLABORA_SOURCE_REVISION" in
                        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
                        *) echo "COLLABORA_SOURCE_REVISION must be a full lower-case Git revision." >&2; exit 1 ;;
                    esac
                    test "$(git rev-parse HEAD)" = "$COLLABORA_SOURCE_REVISION"
                '''
            }
        }
        stage('Build Image') {
            when {
                expression {
                    env.SHOULD_BUILD.toBoolean()
                }
            }
            steps {
                sh '''
                    set -eu
                    builder_tag="$IMAGE_BASE_NAME:source-builder"
                    docker build \
                        -t "$builder_tag" \
                        -f docker/from-source/Builder.Dockerfile \
                        docker/from-source
                    uid="$(id -u)"
                    gid="$(id -g)"
                    docker_socket=/var/run/docker.sock
                    docker_gid="$(stat -c '%g' "$docker_socket")"
                    mkdir -p .jenkins-home
                    docker run --rm \
                        --user "$uid:$gid" \
                        --group-add "$docker_gid" \
                        -v "$PWD:/workspace" \
                        -v "$docker_socket:$docker_socket" \
                        -w /workspace \
                        -e DOCKER_HUB_REPO="$IMAGE_BASE_NAME" \
                        -e DOCKER_HUB_TAG="$IMAGE_TAG" \
                        -e COLLABORA_SOURCE_REVISION="$COLLABORA_SOURCE_REVISION" \
                        -e COLLABORA_SOURCE_BUILD_HOST_OS=Debian \
                        -e HOME=/workspace/.jenkins-home \
                        "$builder_tag" \
                        bash docker/from-source/check-builder.sh
                    docker run --rm \
                        --user "$uid:$gid" \
                        --group-add "$docker_gid" \
                        -v "$PWD:/workspace" \
                        -v "$docker_socket:$docker_socket" \
                        -w /workspace \
                        -e DOCKER_HUB_REPO="$IMAGE_BASE_NAME" \
                        -e DOCKER_HUB_TAG="$IMAGE_TAG" \
                        -e COLLABORA_SOURCE_REVISION="$COLLABORA_SOURCE_REVISION" \
                        -e COLLABORA_SOURCE_BUILD_HOST_OS=Debian \
                        -e HOME=/workspace/.jenkins-home \
                        "$builder_tag" \
                        docker/from-source/build.sh
                    revision=$(
                        docker image inspect \
                            --format '{{ index .Config.Labels "org.opencontainers.image.revision" }}' \
                            "$IMAGE_BASE_NAME:$IMAGE_TAG"
                    )
                    test "$revision" = "$COLLABORA_SOURCE_REVISION"
                '''
            }
        }
        stage('Push Image') {
            when {
                expression {
                    env.SHOULD_BUILD.toBoolean()
                }
            }
            steps {
                withCredentials([string(credentialsId: 'ghcr-packages-pat', variable: 'GITHUB_ACCESS_TOKEN')]) {
                    sh '''
                        set -eu
                        trap 'docker logout ghcr.io >/dev/null 2>&1 || true' EXIT
                        echo "$GITHUB_ACCESS_TOKEN" | docker login ghcr.io -u x-access-token --password-stdin
                        docker push "$IMAGE_BASE_NAME:$IMAGE_TAG"
                        docker tag "$IMAGE_BASE_NAME:$IMAGE_TAG" "$IMAGE_BASE_NAME:$SOURCE_BRANCH-latest"
                        docker push "$IMAGE_BASE_NAME:$SOURCE_BRANCH-latest"
                    '''
                }
            }
        }
    }
    post {
        success {
            script {
                if (env.SHOULD_BUILD.toBoolean()) {
                    echo "Published ${env.IMAGE_BASE_NAME}:${env.IMAGE_TAG}"
                } else {
                    echo "Skipping image publication for branch ${env.BRANCH_NAME}."
                }
            }
        }
    }
}
