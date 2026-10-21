#!/usr/bin/env bash
# Extracted shell and fake Docker commands only; no native builds, JVM or containers.
set -euo pipefail
ulimit -v 262144
ulimit -t 20
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
timeout --signal=TERM --kill-after=1s 30s python3 - "$repo_root" <<'PY'
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
pipeline = (root / "Jenkinsfile").read_text()
build = (root / "docker/from-source/build.sh").read_text()
assert "booleanParam(name: 'NATIVE_VALIDATION_ONLY', defaultValue: false," in pipeline
matches = list(re.finditer(r"stage\('([^']+)'\)", pipeline))
stages = {
    match[1]: pipeline[match.end():matches[i + 1].start() if i + 1 < len(matches) else pipeline.index('\n    post {', match.end())]
    for i, match in enumerate(matches)
}
assert set(stages) == {'Set Build Variables', 'Checkout Source', 'Validate Source',
                       'Build Image', 'Export Java Client', 'Push Image'}

# Evaluate only the small Boolean routing expressions, without a Groovy JVM.
def condition(expression, values):
    expression = expression.replace('env.SHOULD_BUILD.toBoolean()', 'should')
    for source, name in [('env.BRANCH_NAME', 'branch'), ('env.SOURCE_BRANCH', 'source'),
                         ('params.BUILD_MODE', 'mode'), ('params.NATIVE_VALIDATION_ONLY', 'native'),
                         ('params.EXPORT_QUEUE_VALIDATION', 'queue')]:
        expression = expression.replace(source, name)
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', ' not ', expression)
    return eval(expression.strip(), {'__builtins__': {}}, values)

setup = stages['Set Build Variables']
conflicts = re.findall(r"if \((.*?)\) \{\s*error\(", setup)
assert len(conflicts) == 2
should_expression = re.search(r'env.SHOULD_BUILD = \((.*?)\)', setup)[1]
guards = {name: re.search(r'expression \{\s*(.*?)\s*\}', block, re.S)[1]
          for name, block in stages.items() if name != 'Set Build Variables'}
success_guard = re.search(r'if \((.*?)\) \{', pipeline[pipeline.index('\n    post {'):])[1]
for mode, native, queue, branch in itertools.product(
        ['image', 'client-only', 'invalid'], [False, True], [False, True], ['main', 'other']):
    values = dict(mode=mode, native=native, queue=queue, branch=branch, source='main')
    rejected = any(condition(item, values) for item in conflicts)
    assert rejected == ((native and mode != 'image') or (queue and not native))
    if rejected:
        continue
    values['should'] = condition(should_expression, values)
    assert values['should'] == (branch == 'main' and mode in ['image', 'client-only'])
    expected = {
        'Checkout Source': values['should'],
        'Validate Source': values['should'] and mode == 'image',
        'Build Image': values['should'] and mode == 'image',
        'Export Java Client': values['should'] and not native,
        'Push Image': values['should'] and mode == 'image' and not native,
    }
    for name, expression in guards.items():
        assert condition(expression, values) == expected[name], (name, values)
    assert condition(success_guard, values) == expected['Push Image']

assignments = re.findall(
    r"env\.(ONLINE_OFFICE_RUN_AGENT_CONTROL|ONLINE_OFFICE_RUN_JSUNO|NO_DOCKER_IMAGE) = params.NATIVE_VALIDATION_ONLY \? '([^']*)' : '([^']*)'",
    setup)
parameter_exports = re.findall(
    r"env\.(NATIVE_VALIDATION_ONLY|EXPORT_QUEUE_VALIDATION) = params\.\1 \? '([^']*)' : '([^']*)'",
    setup)
assert dict((name, (yes, no)) for name, yes, no in parameter_exports) == {
    'NATIVE_VALIDATION_ONLY': ('true', 'false'),
    'EXPORT_QUEUE_VALIDATION': ('true', 'false')}
assert len(assignments) == 3
assert dict((name, (yes, no)) for name, yes, no in assignments) == {
    'ONLINE_OFFICE_RUN_AGENT_CONTROL': ('1', '0'),
    'ONLINE_OFFICE_RUN_JSUNO': ('1', '0'), 'NO_DOCKER_IMAGE': ('1', '')}

stage_shell = re.search(r"sh '''(.*?)'''", stages['Build Image'], re.S)[1]
push_shell = re.search(r"sh '''(.*?)'''", stages['Push Image'], re.S)[1]
defaults = build[:build.index('if [[ "${ONLINE_OFFICE_RUN_AGENT_CONTROL:-}"')]
assert not re.search(r'StaticLibrary_|ExternalProject_', build)
assert build.count('\nconfigure_release || exit 1\n') == 1
release_steps = ['( cd online/engine && ./autogen.sh', '( cd online/engine && make $ENGINE_BUILD_TARGET )',
                 'cp -a online/engine/instdir', 'prepare_systemplate || exit 1',
                 '\nconfigure_release || exit 1', '( cd online && make -j $(nproc))',
                 '( cd online && DESTDIR="$INSTDIR" make install )']
positions = [build.index(step) for step in release_steps]
assert positions == sorted(positions)
for flag in ['ONLINE_OFFICE_RUN_JSUNO', 'ONLINE_OFFICE_RUN_AGENT_CONTROL']:
    assert f'if [[ "${{{flag}:-}}" == "1" ]]; then' in build
assert 'if [ -z "$NO_DOCKER_IMAGE" ]; then' in build

with tempfile.TemporaryDirectory(prefix='pipeline-modes-') as directory:
    fixture = Path(directory)
    bin_dir = fixture / 'bin'
    bin_dir.mkdir()
    # Every Docker command is intercepted, including build and run. None launches a container.
    docker = bin_dir / 'docker'
    docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['FAKE_DOCKER_LOG'], 'a') as log:
    log.write(json.dumps(args) + '\\n')
if args[-1:] == ['docker/from-source/build.sh'] and os.environ.get('FAIL_NATIVE') == '1':
    sys.exit(23)
if args[:2] == ['image', 'save']:
    pathlib.Path(args[args.index('-o') + 1]).write_bytes(b'fixture')
if args[:2] == ['image', 'inspect']:
    if '--format' not in args:
        print('[]')
    elif 'org.opencontainers.image.revision' in args[args.index('--format') + 1]:
        print(os.environ['ONLINE_OFFICE_SOURCE_REVISION'])
    elif 'io.kreoh.online-office.runtime' in args[args.index('--format') + 1]:
        print('distroless-source')
    else:
        print('sha256:' + 'a' * 64)
''')
    docker.chmod(0o755)
    stat = bin_dir / 'stat'
    stat.write_text('#!/bin/sh\nprintf "1000\\n"\n')
    stat.chmod(0o755)
    shell_path = fixture / 'stage.sh'
    shell_path.write_text(stage_shell)
    subprocess.run(['bash', '-n', str(shell_path)], check=True)
    for native, queue, failure in [(None, None, False), (False, False, False), (True, False, False),
                                   (True, True, False), (True, False, True)]:
        log = fixture / 'docker.log'
        log.write_text('')
        environment = dict(os.environ, PATH=f'{bin_dir}:{os.environ["PATH"]}',
                           FAKE_DOCKER_LOG=str(log), IMAGE_BASE_NAME='fixture/image',
                           IMAGE_TAG='fixture', ONLINE_OFFICE_SOURCE_REVISION='b' * 40,
                           BUILD_NUMBER='1', SOURCE_BRANCH='main',
                           ONLINE_OFFICE_SOURCE_CACHE_ROOT=str(fixture / 'cache'),
                           FAIL_NATIVE=str(int(failure)))
        # New parameters may be absent from the inherited Jenkins environment on their first run.
        # Apply the explicit setup exports, including the null parameter default, before the shell.
        parameter_values = {'NATIVE_VALIDATION_ONLY': native, 'EXPORT_QUEUE_VALIDATION': queue}
        for name, yes, no in parameter_exports:
            environment.pop(name, None)
            environment[name] = yes if parameter_values[name] else no
        # Jenkins may omit empty assignments entirely from the shell environment.
        for name, yes, no in assignments:
            environment.pop(name, None)
            value = yes if native else no
            if value:
                environment[name] = value
        assert ('NO_DOCKER_IMAGE' in environment) == bool(native)
        result = subprocess.run(['bash', str(shell_path)], cwd=fixture, env=environment,
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == (23 if failure else 0), result.stderr
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        builds = [call for call in calls if call[-1:] == ['docker/from-source/build.sh']]
        assert len(builds) == 1
        call = builds[0]
        for name, yes, no in assignments:
            assert f'{name}={yes if native else no}' in call
        cache = fixture / 'cache' / ('main-native-validation' if native else 'main')
        assert f'{cache}:/workspace-cache' in call
        assert all((cache / name).is_dir() for name in ['build', 'ccache', 'home'])
        assert 'ONLINE_OFFICE_SOURCE_CACHE_DIR=/workspace-cache/build' in call
        assert 'CCACHE_DIR=/workspace-cache/ccache' in call
        assert 'HOME=/workspace-cache/home' in call
        final_inspects = [call for call in calls if call[:2] == ['image', 'inspect']
                          and call[-1] == 'fixture/image:fixture']
        assert len(final_inspects) == (0 if native else 2)
        assert not any(call[0] in ['push', 'login', 'tag'] for call in calls)

    # The publication shell rejects validation mode even if a stage guard regresses.
    log.write_text('')
    result = subprocess.run(['bash', '-c', push_shell], cwd=fixture, env=environment,
                            capture_output=True, timeout=5)
    assert result.returncode != 0 and log.read_text() == ''
    for supplied in [None, '1']:
        environment = dict(os.environ)
        for flag in ['ONLINE_OFFICE_RUN_AGENT_CONTROL', 'ONLINE_OFFICE_RUN_JSUNO']:
            environment.pop(flag, None)
            if supplied:
                environment[flag] = supplied
        result = subprocess.run(['bash', '-c', defaults + '\nprintf "%s %s" "$ONLINE_OFFICE_RUN_AGENT_CONTROL" "$ONLINE_OFFICE_RUN_JSUNO"'],
                                env=environment, capture_output=True, text=True, check=True, timeout=5)
        assert result.stdout == ('0 0' if supplied is None else '1 1')

print('Pipeline mode checks passed (routing, isolated caches, mocked shell and publication guards; no native compilation).')
PY
