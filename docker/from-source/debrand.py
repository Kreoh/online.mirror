"""Apply and verify the Online Office runtime debranding policy."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

PRODUCT_NAME = "Online Office"
FORBIDDEN_MARK = re.compile(r"\bcollabora(?:[_ -]?(?:office|online))?\b", re.IGNORECASE)
URL_WITH_MARK = re.compile(r"https?://[^\s\"'<>]*collabora[^\s\"'<>]*", re.IGNORECASE)
EMAIL_WITH_MARK = re.compile(
    r"[\w.+-]+@(?:[\w-]+\.)*collabora(?:office)?\.[A-Za-z]{2,}", re.IGNORECASE
)

LEGAL_OR_PROVENANCE = re.compile(
    r"copyright|spdx|mozilla public licen[cs]e|\bmpl\b|"
    r"this (?:source code form|file) is (?:subject to|part of)|"
    r"contributor|last-translator|language-team|weblate|author(?:s|ed)?\b",
    re.IGNORECASE,
)

COMPATIBILITY = re.compile(
    r"COM\.COLLABORAOFFICE\.MSVD|"
    r"CollaboraOffice:ThreadedCommentPerson:|"
    r"urn:com:collaboraoffice|xmlns:coext|\bcoext:|"
    r"startsWithIgnoreAsciiCase\(\"Collabora\"\)",
    re.IGNORECASE,
)

TEXT_SUFFIXES = {
    "",
    ".1",
    ".5",
    ".8",
    ".ac",
    ".aff",
    ".am",
    ".at",
    ".c",
    ".cc",
    ".conf",
    ".cpp",
    ".css",
    ".cxx",
    ".desktop",
    ".dic",
    ".h",
    ".hh",
    ".hpp",
    ".hrc",
    ".htm",
    ".html",
    ".hxx",
    ".in",
    ".ini",
    ".js",
    ".json",
    ".m4",
    ".md",
    ".mk",
    ".pl",
    ".pm",
    ".po",
    ".pot",
    ".properties",
    ".proto",
    ".py",
    ".rst",
    ".scss",
    ".service",
    ".sh",
    ".socket",
    ".spec",
    ".svg",
    ".timer",
    ".toml",
    ".ts",
    ".tsx",
    ".txt",
    ".ui",
    ".ulf",
    ".xcu",
    ".xcd",
    ".xcs",
    ".xml",
    ".xsl",
    ".xslt",
    ".yaml",
    ".yml",
}

SOURCE_ROOT_FILES = {
    "Makefile.am",
    "configure.ac",
    "coolkitconfig.xcu.in",
    "coolwsd.service",
    "coolwsd.spec",
    "coolwsd.xml.in",
    "discovery.xml",
}

SOURCE_PREFIXES = (
    "browser/",
    "common/",
    "engine/",
    "etc/",
    "kit/",
    "man/",
    "net/",
    "tools/",
    "wsd/",
)

SOURCE_EXCLUDES = (
    "/external/",
    "/node_modules/",
    "/qa/",
    "/test/",
    "/tests/",
    "/workdir/",
)

ARCHIVE_SUFFIXES = {".ods", ".odp", ".odt"}


def is_preserved_line(line: str) -> bool:
    return bool(LEGAL_OR_PROVENANCE.search(line))


def replace_marks(text: str) -> str:
    """Replace product marks while protecting required compatibility tokens."""
    protected: list[str] = []

    def protect(match: re.Match[str]) -> str:
        protected.append(match.group(0))
        return f"@@ONLINE_OFFICE_COMPAT_{len(protected) - 1}@@"

    text = COMPATIBILITY.sub(protect, text)
    text = EMAIL_WITH_MARK.sub("support@example.invalid", text)
    text = URL_WITH_MARK.sub("about:blank", text)
    replacements = (
        (
            re.compile(
                r"Collabora Online Development Edition(?: \(unbranded\))?",
                re.IGNORECASE,
            ),
            PRODUCT_NAME,
        ),
        (re.compile(r"Collabora Office", re.IGNORECASE), PRODUCT_NAME),
        (re.compile(r"Collabora Online", re.IGNORECASE), PRODUCT_NAME),
        (
            re.compile(r"Collabora Productivity(?: Limited)?", re.IGNORECASE),
            PRODUCT_NAME,
        ),
        (re.compile(r"Collabora_Office", re.IGNORECASE), "Online_Office"),
        (re.compile(r"CollaboraOffice", re.IGNORECASE), "OnlineOffice"),
        (re.compile(r"collaboraoffice/4", re.IGNORECASE), "onlineoffice/4"),
        (
            re.compile(r"collabora-online-server\.local", re.IGNORECASE),
            "online-office-server.local",
        ),
        (
            re.compile(r"server-embedding-collabora-online-iframe", re.IGNORECASE),
            "server-embedding-online-office-iframe",
        ),
        (re.compile(r"\bcollabora-online\b", re.IGNORECASE), "online-office"),
        (re.compile(r"\bCollabora\b", re.IGNORECASE), PRODUCT_NAME),
    )
    for pattern, replacement in replacements:
        text = pattern.sub(replacement, text)
    for index, value in enumerate(protected):
        text = text.replace(f"@@ONLINE_OFFICE_COMPAT_{index}@@", value)
    return text


def rewrite_text(text: str) -> str:
    output = []
    for line in text.splitlines(keepends=True):
        output.append(line if is_preserved_line(line) else replace_marks(line))
    return "".join(output)


def read_text(path: Path) -> tuple[str, str] | None:
    if path.suffix.lower() not in TEXT_SUFFIXES:
        return None
    try:
        raw = path.read_bytes()
    except OSError:
        return None
    if b"\0" in raw:
        return None
    for encoding in ("utf-8", "latin-1"):
        try:
            return raw.decode(encoding), encoding
        except UnicodeDecodeError:
            continue
    return None


def rewrite_file(path: Path) -> bool:
    decoded = read_text(path)
    if decoded is None:
        return False
    original, encoding = decoded
    rewritten = rewrite_text(original)
    if rewritten == original:
        return False
    path.write_bytes(rewritten.encode(encoding))
    return True


def rewrite_archive(path: Path) -> bool:
    changed = False
    output = io.BytesIO()
    try:
        with (
            zipfile.ZipFile(path, "r") as source,
            zipfile.ZipFile(output, "w") as destination,
        ):
            for info in source.infolist():
                payload = source.read(info.filename)
                if info.filename in {"content.xml", "meta.xml", "styles.xml"}:
                    try:
                        original = payload.decode("utf-8")
                    except UnicodeDecodeError:
                        pass
                    else:
                        rewritten = replace_marks(original)
                        payload = rewritten.encode("utf-8")
                        changed = changed or rewritten != original
                destination.writestr(info, payload)
    except zipfile.BadZipFile:
        return False
    if changed:
        path.write_bytes(output.getvalue())
    return changed


def source_paths(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        stdout=subprocess.PIPE,
    )
    paths: list[Path] = []
    for raw_name in result.stdout.split(b"\0"):
        if not raw_name:
            continue
        name = raw_name.decode("utf-8", "surrogateescape")
        padded = f"/{name}/"
        if name not in SOURCE_ROOT_FILES and not name.startswith(SOURCE_PREFIXES):
            continue
        if any(excluded in padded for excluded in SOURCE_EXCLUDES):
            continue
        paths.append(root / name)
    return paths


def apply_source(root: Path) -> None:
    extensions = root / "browser/extensions"
    if extensions.exists():
        for demo_extension in extensions.glob("com.collaboraoffice.demo-*"):
            shutil.rmtree(demo_extension)

    changed = 0
    for path in source_paths(root):
        if path.suffix.lower() in ARCHIVE_SUFFIXES:
            changed += rewrite_archive(path)
        elif path.is_file():
            changed += rewrite_file(path)

    neutral_favicon = root / "engine/static/emscripten/favicon.ico"
    if not neutral_favicon.is_file():
        raise RuntimeError(f"neutral favicon is missing: {neutral_favicon}")
    shutil.copyfile(neutral_favicon, root / "favicon.ico")
    print(f"Debranded {changed} source files and installed the neutral favicon source.")


def legal_path(path: Path) -> bool:
    upper_parts = {part.upper() for part in path.parts}
    name = path.name.upper()
    return bool(
        upper_parts & {"LICENSES", "LEGAL"}
        or name.startswith(
            ("COPYING", "LICENSE", "NOTICE", "THIRDPARTY", "CODA-THIRDPARTY")
        )
        or "COPYRIGHT" in name
    )


def mark_violations(path: Path, text: str) -> list[str]:
    if legal_path(path):
        return []
    violations = []
    for number, line in enumerate(text.splitlines(), 1):
        scan_line = COMPATIBILITY.sub("", line)
        if FORBIDDEN_MARK.search(scan_line) and not is_preserved_line(line):
            violations.append(f"{path}:{number}:{line[:240]}")
    return violations


def scan_files(paths: list[Path], include_binaries: bool = False) -> list[str]:
    violations: list[str] = []
    for path in paths:
        if not path.is_file() or path.is_symlink():
            continue
        if FORBIDDEN_MARK.search(path.name) and not legal_path(path):
            violations.append(f"branded filename: {path}")
        if path.suffix.lower() in ARCHIVE_SUFFIXES:
            try:
                with zipfile.ZipFile(path) as archive:
                    for member in ("content.xml", "meta.xml", "styles.xml"):
                        if member in archive.namelist():
                            text = archive.read(member).decode("utf-8", "replace")
                            violations.extend(mark_violations(path, text))
            except zipfile.BadZipFile:
                pass
            continue
        decoded = read_text(path)
        if decoded is not None:
            violations.extend(mark_violations(path, decoded[0]))
        elif include_binaries and (
            os.access(path, os.X_OK)
            or path.suffix in {".so", ".bin"}
            or ".so." in path.name
        ):
            result = subprocess.run(
                ["strings", "-a", str(path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                check=False,
            )
            violations.extend(mark_violations(path, result.stdout))
    return violations


def fail_on_violations(violations: list[str]) -> None:
    if not violations:
        return
    print("Forbidden product marks found:", file=sys.stderr)
    for violation in violations[:200]:
        print(f"  {violation}", file=sys.stderr)
    if len(violations) > 200:
        print(f"  ... and {len(violations) - 200} more", file=sys.stderr)
    raise SystemExit(1)


def scan_source(root: Path) -> None:
    violations = scan_files(source_paths(root))
    fail_on_violations(violations)
    print("Online Office pre-build debranding scan passed.")


def runtime_paths(rootfs: Path) -> list[Path]:
    roots = (
        rootfs / "etc/apache2",
        rootfs / "etc/coolwsd",
        rootfs / "etc/nginx",
        rootfs / "opt/online-office",
        rootfs / "usr/bin",
        rootfs / "usr/lib",
        rootfs / "usr/libexec",
        rootfs / "usr/share/coolwsd",
        rootfs / "usr/share/doc/coolwsd",
        rootfs / "usr/share/man",
    )
    paths: list[Path] = []
    for directory in roots:
        if directory.exists():
            paths.extend(directory.rglob("*"))
    return paths


def remove_commercial_eula(rootfs: Path) -> None:
    for path in list(rootfs.rglob("*")):
        if path.is_file() and path.name.upper().startswith("EULA"):
            path.unlink()


def apply_rootfs(rootfs: Path, source_root: Path) -> None:
    remove_commercial_eula(rootfs)

    welcome = rootfs / "usr/share/coolwsd/browser/dist/welcome"
    if welcome.exists():
        shutil.rmtree(welcome)

    browser_dist = rootfs / "usr/share/coolwsd/browser/dist"
    if browser_dist.exists():
        for path in sorted(browser_dist.rglob("*"), reverse=True):
            if FORBIDDEN_MARK.search(path.name):
                if path.is_dir():
                    shutil.rmtree(path)
                else:
                    path.unlink()

    for path in runtime_paths(rootfs):
        if path.suffix.lower() in ARCHIVE_SUFFIXES:
            rewrite_archive(path)
        elif path.is_file():
            rewrite_file(path)

    neutral_favicon = source_root / "engine/static/emscripten/favicon.ico"
    favicon_targets = (
        rootfs / "usr/share/coolwsd/favicon.ico",
        rootfs / "usr/share/coolwsd/browser/dist/images/favicon.ico",
    )
    for target in favicon_targets:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(neutral_favicon, target)

    scan_rootfs(rootfs, neutral_favicon)


def find_bootstrap(rootfs: Path) -> Path:
    candidates = tuple((rootfs / "opt/online-office").glob("program/bootstraprc"))
    if not candidates:
        candidates = tuple((rootfs / "opt/online-office").glob("program/bootstrap.ini"))
    if len(candidates) != 1:
        raise RuntimeError("could not find exactly one engine bootstrap configuration")
    return candidates[0]


def assert_runtime_identity(rootfs: Path, neutral_favicon: Path) -> None:
    eulas = [
        path
        for path in rootfs.rglob("*")
        if path.is_file() and path.name.upper().startswith("EULA")
    ]
    if eulas:
        raise RuntimeError(f"commercial EULA remains in rootfs: {eulas[0]}")

    bootstrap = find_bootstrap(rootfs).read_text(encoding="utf-8")
    if not re.search(r"^ProductKey=Online Office [0-9]", bootstrap, re.MULTILINE):
        raise RuntimeError("engine ProductKey is not Online Office with a version")
    if "UserInstallation=$SYSUSERCONFIG/onlineoffice/4" not in bootstrap:
        raise RuntimeError("engine user profile is not the neutral onlineoffice/4 path")

    sboms = list(rootfs.rglob("*-sbom.spdx.json"))
    if len(sboms) != 1 or sboms[0].name != "online-office-sbom.spdx.json":
        raise RuntimeError(f"expected one neutral SBOM filename, found: {sboms}")
    sbom = json.loads(sboms[0].read_text(encoding="utf-8"))
    if sbom.get("name") != PRODUCT_NAME:
        raise RuntimeError("SBOM document name is not Online Office")

    expected_hash = hashlib.sha256(neutral_favicon.read_bytes()).hexdigest()
    favicon = rootfs / "usr/share/coolwsd/browser/dist/images/favicon.ico"
    if (
        not favicon.is_file()
        or hashlib.sha256(favicon.read_bytes()).hexdigest() != expected_hash
    ):
        raise RuntimeError("browser favicon is not the neutral repository asset")

    if (rootfs / "usr/share/coolwsd/browser/dist/welcome").exists():
        raise RuntimeError("branded browser welcome assets are still installed")


def scan_rootfs(rootfs: Path, neutral_favicon: Path) -> None:
    assert_runtime_identity(rootfs, neutral_favicon)
    violations = scan_files(runtime_paths(rootfs), include_binaries=True)
    fail_on_violations(violations)
    print("Online Office post-build rootfs debranding scan passed.")


def docker_json(arguments: list[str]) -> object:
    result = subprocess.run(
        ["docker", *arguments],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    return json.loads(result.stdout)


def scan_image(image: str) -> None:
    inspect = docker_json(["image", "inspect", image])
    serialised = json.dumps(inspect, sort_keys=True)
    violations = mark_violations(Path("OCI image configuration"), serialised)

    history_result = subprocess.run(
        [
            "docker",
            "image",
            "history",
            "--no-trunc",
            "--format",
            "{{.CreatedBy}}",
            image,
        ],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    violations.extend(mark_violations(Path("OCI image history"), history_result.stdout))
    fail_on_violations(violations)

    command = r"""set -eu
test ! -e /opt/online-office/EULA_en-US.rtf
test ! -e /usr/share/coolwsd/browser/dist/welcome
test -e /usr/share/doc/coolwsd/online-office-sbom.spdx.json
test ! -e /usr/share/doc/coolwsd/collabora-online-sbom.spdx.json
grep -Eq '^ProductKey=Online Office [0-9]' /opt/online-office/program/bootstraprc
grep -Fq 'UserInstallation=$SYSUSERCONFIG/onlineoffice/4' /opt/online-office/program/bootstraprc
grep -Fq '"name": "Online Office"' /usr/share/doc/coolwsd/online-office-sbom.spdx.json
"""
    subprocess.run(
        ["docker", "run", "--rm", "--entrypoint", "/bin/sh", image, "-ec", command],
        check=True,
    )
    print("Online Office OCI metadata and runtime identity scan passed.")


def self_test() -> None:
    assert not FORBIDDEN_MARK.search("collaboration collaborator collaborative")
    branded = (
        "Collabora Online; Collabora Office; Collabora_Office; "
        "hello@collaboraoffice.com; https://sdk.collaboraonline.com/x"
    )
    rewritten = rewrite_text(branded)
    assert "Collabora" not in rewritten
    assert PRODUCT_NAME in rewritten
    assert "support@example.invalid" in rewritten
    assert "about:blank" in rewritten
    assert "Online_Office" in rewritten

    embedded = "prefixCollaboraOfficeSuffix PRODUCTNAME.collaboraoffice CollaboraOffice"
    assert rewrite_text(embedded) == (
        "prefixOnlineOfficeSuffix PRODUCTNAME.OnlineOffice OnlineOffice"
    )

    legal = "Copyright the Collabora Online contributors."
    assert rewrite_text(legal) == legal
    compatibility = (
        "COM.COLLABORAOFFICE.MSVD CollaboraOffice:ThreadedCommentPerson: "
        'xmlns:coext="urn:com:collaboraoffice:names:experimental" coext:value'
    )
    assert rewrite_text(compatibility) == compatibility
    assert not mark_violations(Path("compatibility.xml"), compatibility)
    detection_code = 'aGenerator.startsWithIgnoreAsciiCase("Collabora")'
    assert rewrite_text(detection_code) == detection_code
    assert not mark_violations(Path("detection.cxx"), detection_code)

    mixed_compatibility = compatibility + " CollaboraOffice"
    mixed_rewritten = rewrite_text(mixed_compatibility)
    assert mixed_rewritten == compatibility + " OnlineOffice"

    generator_metadata = (
        "<office:meta><meta:generator>"
        "Collabora_Office/26.04.2.0$Linux_X86_64"
        "</meta:generator></office:meta>"
    )
    rewritten_metadata = rewrite_text(generator_metadata)
    assert "Online_Office/26.04.2.0" in rewritten_metadata
    assert not FORBIDDEN_MARK.search(rewritten_metadata)
    assert not mark_violations(Path("meta.xml"), rewritten_metadata)

    with tempfile.TemporaryDirectory() as directory:
        archive_path = Path(directory) / "intro.odp"
        with zipfile.ZipFile(archive_path, "w") as archive:
            archive.writestr(
                "content.xml",
                '<office xmlns:coext="urn:com:collaboraoffice:names:experimental">Collabora Office</office>',
            )
        assert rewrite_archive(archive_path)
        with zipfile.ZipFile(archive_path) as archive:
            content = archive.read("content.xml").decode()
        assert PRODUCT_NAME in content
        assert "urn:com:collaboraoffice" in content
        assert not mark_violations(archive_path, content)

    with tempfile.TemporaryDirectory() as directory:
        source_root = Path(directory)
        (source_root / "browser/html").mkdir(parents=True)
        demo = source_root / "browser/extensions/com.collaboraoffice.demo-test"
        demo.mkdir(parents=True)
        (source_root / "engine/static/emscripten").mkdir(parents=True)
        (source_root / "browser/html/cool.html").write_text(
            "<title>Collabora Online</title>\n", encoding="utf-8"
        )
        (demo / "manifest.json").write_text(
            '{"name": "Collabora Office demo"}\n', encoding="utf-8"
        )
        neutral_icon = b"neutral-icon"
        (source_root / "engine/static/emscripten/favicon.ico").write_bytes(neutral_icon)
        subprocess.run(
            ["git", "init", "--quiet", str(source_root)],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(source_root), "add", "."],
            check=True,
        )
        apply_source(source_root)
        scan_source(source_root)
        assert not demo.exists()
        assert (source_root / "favicon.ico").read_bytes() == neutral_icon

    print("Online Office debranding helper self-test passed.")


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    for command in ("source", "scan-source"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("root", type=Path)

    rootfs_parser = subparsers.add_parser("rootfs")
    rootfs_parser.add_argument("rootfs", type=Path)
    rootfs_parser.add_argument("source_root", type=Path)

    rootfs_scan_parser = subparsers.add_parser("scan-rootfs")
    rootfs_scan_parser.add_argument("rootfs", type=Path)
    rootfs_scan_parser.add_argument("neutral_favicon", type=Path)

    image_parser = subparsers.add_parser("scan-image")
    image_parser.add_argument("image")
    subparsers.add_parser("self-test")

    arguments = parser.parse_args()
    if arguments.command == "source":
        apply_source(arguments.root.resolve())
    elif arguments.command == "scan-source":
        scan_source(arguments.root.resolve())
    elif arguments.command == "rootfs":
        apply_rootfs(arguments.rootfs.resolve(), arguments.source_root.resolve())
    elif arguments.command == "scan-rootfs":
        scan_rootfs(arguments.rootfs.resolve(), arguments.neutral_favicon.resolve())
    elif arguments.command == "scan-image":
        scan_image(arguments.image)
    else:
        self_test()


if __name__ == "__main__":
    main()
