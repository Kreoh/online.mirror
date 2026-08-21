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
        COMMIT_SHA_12 = "${env.GIT_COMMIT}".take(12)
        IMAGE_TAG = "${SOURCE_VERSION}-${COMMIT_SHA_12}"
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
                checkout([
                    $class: 'GitSCM',
                    branches: [[name: "*/${env.SOURCE_BRANCH}"]],
                    doGenerateSubmoduleConfigurations: false,
                    extensions: [
                        [$class: 'CloneOption', shallow: true, depth: 1, noTags: true, timeout: 120],
                        [$class: 'CheckoutOption', timeout: 120],
                        [$class: 'CleanBeforeCheckout']
                    ],
                    userRemoteConfigs: [[
                        credentialsId: '61c9e22d-1680-43b6-9452-6ef9c2f2a59c',
                        url: 'https://github.com/Kreoh/online.mirror.git',
                        refspec: "+refs/heads/${env.SOURCE_BRANCH}:refs/remotes/origin/${env.SOURCE_BRANCH}"
                    ]]
                ])
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
                    case "$GIT_COMMIT" in
                        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
                        *) echo "GIT_COMMIT must be a full lower-case Git revision." >&2; exit 1 ;;
                    esac
                    test "$(git rev-parse HEAD)" = "$GIT_COMMIT"
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
                    DOCKER_HUB_REPO="$IMAGE_BASE_NAME" \
                    DOCKER_HUB_TAG="$IMAGE_TAG" \
                    COLLABORA_SOURCE_REVISION="$GIT_COMMIT" \
                    COLLABORA_SOURCE_BUILD_HOST_OS=Debian \
                        docker/from-source/build.sh
                    revision=$(
                        docker image inspect \
                            --format '{{ index .Config.Labels "org.opencontainers.image.revision" }}' \
                            "$IMAGE_BASE_NAME:$IMAGE_TAG"
                    )
                    test "$revision" = "$GIT_COMMIT"
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
