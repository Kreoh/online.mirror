"""Apply and verify the Online Office runtime debranding policy."""

from __future__ import annotations

import argparse
import contextlib
import errno
import hashlib
import io
import json
import os
import re
import shutil
import selectors
import stat
import time
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

PRODUCT_NAME = "Online Office"
LIBRELOGO_EXTENSION_ID = "com.collaboraoffice.librelogo"
FORBIDDEN_MARK = re.compile(r"\bcollabora(?:[_ -]?(?:office|online))?\b", re.IGNORECASE)
URL_WITH_MARK = re.compile(
    r"""https?://[^\s\\\"'<>\(\)\[\]\{\},;]*collabora"""
    r"""[^\s\\\"'<>\(\)\[\]\{\},;]*""",
    re.IGNORECASE,
)
BRANDED_HOST_FRAGMENT = re.compile(
    r"(?:[A-Za-z0-9-]+\.)*collabora(?:online|office)?\.[A-Za-z]{2,}",
    re.IGNORECASE,
)
BRANDED_HOST_PREFIX = re.compile(
    r"(?:[A-Za-z0-9-]+\.)*collabora(?:online|office)?\.(?![A-Za-z]{2,})",
    re.IGNORECASE,
)
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
    rf"(?<![\w.-]){re.escape(LIBRELOGO_EXTENSION_ID)}(?:\.pot)?(?![\w.-])|"
    r"COM\.COLLABORAOFFICE\.MSVD|"
    r"urn:com:collaboraoffice|xmlns:coext|\bcoext:",
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
    # Gettext wraps long URLs across quoted lines, which can leave a marked
    # hostname on a line without its scheme. Neutralise that fragment too.
    text = BRANDED_HOST_FRAGMENT.sub("example.invalid", text)
    text = BRANDED_HOST_PREFIX.sub("example.invalid.", text)
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


def rewrite_file(path: Path, report: dict | None = None) -> bool:
    decoded = read_text(path)
    if decoded is None:
        return False
    original, encoding = decoded
    rewritten = rewrite_text(original)
    if rewritten == original:
        return False
    payload = rewritten.encode(encoding)
    path.write_bytes(payload)
    if report is not None:
        mtime_note_write(report, path, payload)
    return True


def rewrite_archive(path: Path, report: dict | None = None) -> bool:
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
        payload = output.getvalue()
        path.write_bytes(payload)
        if report is not None:
            mtime_note_write(report, path, payload)
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


def apply_source(root: Path, report_path: Path | None = None) -> None:
    report = MtimeWrites()
    paths = source_paths(root)
    removed: list[str] = []
    extensions = root / "browser/extensions"
    if extensions.exists():
        for demo_extension in extensions.glob("com.collaboraoffice.demo-*"):
            removed.extend(str(path.relative_to(root)) for path in paths
                           if demo_extension in path.parents and len(removed) <= MTIME_ENTRY_LIMIT)
            shutil.rmtree(demo_extension)

    changed = 0
    for path in paths:
        if path.suffix.lower() in ARCHIVE_SUFFIXES:
            changed += rewrite_archive(path, report if report_path else None)
        elif path.is_file():
            changed += rewrite_file(path, report if report_path else None)

    neutral_favicon = root / "engine/static/emscripten/favicon.ico"
    if not neutral_favicon.is_file():
        raise RuntimeError(f"neutral favicon is missing: {neutral_favicon}")
    shutil.copyfile(neutral_favicon, root / "favicon.ico")
    if report_path is not None:
        mtime_source_report(root, report_path, report, removed)
    print(f"Debranded {changed} source files and installed the neutral favicon source.")



# These helpers inspect source bytes only. Whole-tree checks run in Jenkins;
# self-tests use tiny repositories and never invoke native preparation.
MTIME_JSON_LIMIT = 4 * 1024 * 1024
MTIME_ENTRY_LIMIT = 4096
MTIME_FILE_LIMIT = 16 * 1024 * 1024
MTIME_CANDIDATE_BYTES = 256 * 1024 * 1024
MTIME_TREE_BYTES = 4 * 1024 * 1024 * 1024
MTIME_TREE_FILE_LIMIT = 128 * 1024 * 1024
MTIME_TREE_ENTRIES = 100000
MTIME_GIT_BYTES = 32 * 1024 * 1024
MTIME_SECONDS = 300


class MtimeCold(Exception):
    """A cache hint cannot safely be used; ordinary preparation may continue."""


class MtimeBudget:
    def __init__(self):
        self.started = time.monotonic()
        self.bytes = 0
        self.candidate_bytes = 0

    def check(self, count=0, candidate=False):
        self.bytes += count
        if candidate:
            self.candidate_bytes += count
        if (self.bytes > MTIME_TREE_BYTES
                or self.candidate_bytes > MTIME_CANDIDATE_BYTES
                or time.monotonic() - self.started > MTIME_SECONDS):
            raise MtimeCold("limit exceeded")


class MtimeWrites(dict):
    def __init__(self):
        super().__init__()
        self.hashed_bytes = 0
        self.cold = False


def mtime_note_write(report, path, payload):
    if report.cold:
        return
    size = len(payload)
    if (size > MTIME_FILE_LIMIT or report.hashed_bytes + size > MTIME_CANDIDATE_BYTES
            or len(report) >= MTIME_ENTRY_LIMIT):
        report.clear()
        report.cold = True
        return
    report.hashed_bytes += size
    report[str(path)] = {"size": size, "sha256": hashlib.sha256(payload).hexdigest()}


def mtime_path(name):
    if (not isinstance(name, str) or not name or name.startswith("/")
            or any(part in {"", ".", ".."} for part in name.split("/"))
            or "\0" in name or len(name.encode("utf-8")) > 1024):
        raise MtimeCold("baseline rejected")
    return name


def mtime_candidate(name):
    mtime_path(name)
    padded = f"/{name}/"
    parts = name.split("/")
    return ((name in SOURCE_ROOT_FILES or name.startswith(SOURCE_PREFIXES))
            and not any(item in padded for item in SOURCE_EXCLUDES)
            and not set(parts) & {"instdir", "autom4te.cache", ".cache", "unpacked", "download"}
            and parts[-1] not in {"Makefile", "config.status", "config.log", "config_host.mk", "config_build.mk"}
            and not (set(parts) & {"config_host", "config_build"} and not name.endswith(".in")))


def mtime_git(root, budget, *args):
    """Bound both streamed output and elapsed time, including silent children."""
    command = ["git", "--literal-pathspecs", "-c", "core.fsmonitor=false", "-C", str(root), *args]
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as process:
        try:
            chunks = []
            count = 0
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while True:
                    budget.check()
                    if not selector.select(timeout=0.1):
                        continue
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    count += len(chunk)
                    if count > MTIME_GIT_BYTES:
                        raise MtimeCold("limit exceeded")
                    chunks.append(chunk)
            status = process.wait(timeout=max(0.01, MTIME_SECONDS - (time.monotonic() - budget.started)))
            if status:
                raise MtimeCold("baseline rejected")
            return b"".join(chunks)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def mtime_binding(root, budget):
    if mtime_git(root, budget, "rev-parse", "--show-object-format").strip() != b"sha1":
        raise MtimeCold("baseline rejected")
    identity = root.stat()
    return {"version": 1, "root": str(root), "device": identity.st_dev, "inode": identity.st_ino,
            "head": mtime_git(root, budget, "rev-parse", "HEAD").decode().strip(),
            "origin": mtime_git(root, budget, "config", "--get", "remote.origin.url").decode().strip(),
            "policy": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


@contextlib.contextmanager
def mtime_parent(root, name):
    parts = mtime_path(name).split("/")
    descriptors = [os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)]
    try:
        for component in parts[:-1]:
            descriptors.append(os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                       dir_fd=descriptors[-1]))
        yield descriptors[-1], parts[-1]
    except OSError as error:
        if error.errno in {errno.ELOOP, errno.ENOTDIR}:
            raise MtimeCold("baseline rejected") from None
        raise
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def mtime_stat(value):
    return {"mode": stat.S_IMODE(value.st_mode), "uid": value.st_uid, "gid": value.st_gid,
            "size": value.st_size, "mtime": value.st_mtime_ns, "ctime": value.st_ctime_ns,
            "device": value.st_dev, "inode": value.st_ino, "links": value.st_nlink}


def mtime_read_fd(descriptor, budget, candidate=False):
    before = os.fstat(descriptor)
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise MtimeCold("baseline rejected")
    limit = MTIME_FILE_LIMIT if candidate else MTIME_TREE_FILE_LIMIT
    if (before.st_size > limit or budget.bytes + before.st_size > MTIME_TREE_BYTES
            or (candidate and budget.candidate_bytes + before.st_size > MTIME_CANDIDATE_BYTES)):
        raise MtimeCold("limit exceeded")
    blob = hashlib.sha1(f"blob {before.st_size}\0".encode())
    digest = hashlib.sha256() if candidate else None
    total = 0
    while True:
        budget.check()
        chunk = os.read(descriptor, 65536)
        if not chunk:
            break
        total += len(chunk)
        budget.check(len(chunk), candidate)
        if total > limit:
            raise MtimeCold("limit exceeded")
        blob.update(chunk)
        if digest is not None:
            digest.update(chunk)
    after = os.fstat(descriptor)
    if mtime_stat(before) != mtime_stat(after) or total != after.st_size:
        raise RuntimeError("Source timestamp check detected a file race.")
    result = mtime_stat(after)
    result["blob"] = blob.hexdigest()
    if digest is not None:
        result["sha256"] = digest.hexdigest()
    return result


def mtime_recheck_path(root, name, info):
    try:
        with mtime_parent(root, name) as (parent, leaf):
            current = os.stat(leaf, dir_fd=parent, follow_symlinks=False)
            if not stat.S_ISREG(current.st_mode) or mtime_stat(current) != {key: info[key] for key in mtime_stat(current)}:
                raise RuntimeError("Source timestamp check detected a path race.")
    except (OSError, MtimeCold):
        raise RuntimeError("Source timestamp check detected a path race.") from None


def mtime_inventory(root, budget, wanted):
    """Compare all actual tracked bytes; never consult Git's cached stat data."""
    raw = mtime_git(root, budget, "ls-tree", "-r", "-l", "-z", "HEAD")
    tracked = {}
    for entry in raw.split(b"\0"):
        if not entry:
            continue
        fields, raw_name = entry.split(b"\t", 1)
        mode, kind, blob, size = fields.split()
        name = mtime_path(raw_name.decode("utf-8"))
        if name in tracked or len(tracked) >= MTIME_TREE_ENTRIES:
            raise MtimeCold("limit exceeded")
        if kind != b"blob" or mode not in {b"100644", b"100755", b"120000"}:
            raise MtimeCold("baseline rejected")
        tracked[name] = (mode.decode(), blob.decode())
    index = {}
    raw_index = mtime_git(root, budget, "ls-files", "-v", "--stage", "-z")
    for entry in raw_index.split(b"\0"):
        if not entry:
            continue
        fields, raw_name = entry.split(b"\t", 1)
        flag, mode, blob, stage = fields.split()
        name = mtime_path(raw_name.decode("utf-8"))
        # Lower-case flags include assume-unchanged; S includes skip-worktree.
        if flag != b"H" or stage != b"0" or name in index or len(index) >= MTIME_TREE_ENTRIES:
            raise MtimeCold("baseline rejected")
        index[name] = (mode.decode(), blob.decode())
    if index != tracked:
        raise MtimeCold("baseline rejected")
    dirty, files = {}, {}
    for name, (mode, blob) in tracked.items():
        budget.check()
        if len(dirty) > MTIME_ENTRY_LIMIT:
            raise MtimeCold("limit exceeded")
        try:
            with mtime_parent(root, name) as (parent, leaf):
                before = os.stat(leaf, dir_fd=parent, follow_symlinks=False)
                if mode == "120000":
                    if not stat.S_ISLNK(before.st_mode):
                        raise MtimeCold("baseline rejected")
                    target = os.fsencode(os.readlink(leaf, dir_fd=parent))
                    budget.check(len(target))
                    current = hashlib.sha1(f"blob {len(target)}\0".encode() + target).hexdigest()
                    if mtime_stat(before) != mtime_stat(os.stat(leaf, dir_fd=parent, follow_symlinks=False)):
                        raise RuntimeError("Source timestamp check detected a link race.")
                    if current != blob:
                        raise MtimeCold("baseline rejected")
                    continue
                if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
                    raise MtimeCold("baseline rejected")
                descriptor = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
                try:
                    info = mtime_read_fd(descriptor, budget, name in wanted)
                    if mtime_stat(before) != {key: info[key] for key in mtime_stat(before)}:
                        raise RuntimeError("Source timestamp check detected an inode race.")
                finally:
                    os.close(descriptor)
                mtime_recheck_path(root, name, info)
                info["git_mode"] = mode
                if info["blob"] != blob or bool(info["mode"] & 0o111) != (mode == "100755"):
                    dirty[name] = info
                if name in wanted:
                    files[name] = info
        except FileNotFoundError:
            dirty[name] = None
    if mtime_git(root, budget, "ls-files", "-v", "--stage", "-z") != raw_index:
        raise RuntimeError("Source timestamp check detected an index race.")
    return dirty, files, tracked


def mtime_json_read(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > MTIME_JSON_LIMIT:
            raise MtimeCold("baseline rejected")
        with os.fdopen(os.dup(descriptor), "rb") as stream:
            payload = stream.read(MTIME_JSON_LIMIT + 1)
        if len(payload) > MTIME_JSON_LIMIT:
            raise MtimeCold("limit exceeded")
        if mtime_stat(info) != mtime_stat(os.fstat(descriptor)) or len(payload) != info.st_size:
            raise MtimeCold("baseline rejected")
        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise MtimeCold("baseline rejected")
                result[key] = value
            return result
        try:
            value = json.loads(payload, object_pairs_hook=unique)
        except RecursionError:
            raise MtimeCold("baseline rejected") from None
        if not isinstance(value, dict):
            raise MtimeCold("baseline rejected")
        return value
    finally:
        os.close(descriptor)


def mtime_json_write(path, value):
    payload = json.dumps(value, sort_keys=True).encode()
    if len(payload) > MTIME_JSON_LIMIT:
        raise MtimeCold("limit exceeded")
    descriptor, temporary = tempfile.mkstemp(prefix=".source-mtimes-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def mtime_report_paths(report):
    writes, removed = report["writes"], report["removed"]
    if not isinstance(writes, dict) or not isinstance(removed, list):
        raise MtimeCold("baseline rejected")
    if len(writes) + len(removed) > MTIME_ENTRY_LIMIT or len(set(removed)) != len(removed):
        raise MtimeCold("limit exceeded")
    for name, digest in writes.items():
        mtime_path(name)
        if (not isinstance(digest, dict) or set(digest) != {"size", "sha256"}
                or type(digest["size"]) is not int or not 0 <= digest["size"] <= MTIME_FILE_LIMIT
                or not isinstance(digest["sha256"], str)
                or not re.fullmatch(r"[0-9a-f]{64}", digest["sha256"])):
            raise MtimeCold("baseline rejected")
    for name in removed:
        mtime_path(name)
        if not name.startswith("browser/extensions/com.collaboraoffice.demo-") or name in writes:
            raise MtimeCold("baseline rejected")
    return writes, removed


def mtime_verify_report(report, dirty, files, tracked):
    writes, removed = mtime_report_paths(report)
    if set(dirty) - set(writes) - set(removed) or set(writes) - set(tracked):
        raise MtimeCold("baseline rejected")
    for name, digest in writes.items():
        if name not in files or any(files[name][key] != value for key, value in digest.items()):
            raise MtimeCold("baseline rejected")
    for name in removed:
        if name not in tracked or name not in dirty or dirty[name] is not None:
            raise MtimeCold("baseline rejected")


def mtime_source_report(root, path, absolute_writes, removed):
    try:
        if absolute_writes.cold:
            raise MtimeCold("limit exceeded")
        budget = MtimeBudget()
        # Include digests collected during transformation in the favicon budget.
        budget.check(absolute_writes.hashed_bytes, True)
        writes = {str(Path(name).relative_to(root)): digest for name, digest in absolute_writes.items()}
        # The favicon is provenance only; it is never an mtime candidate.
        with mtime_parent(root, "favicon.ico") as (parent, leaf):
            descriptor = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=parent)
            try:
                info = mtime_read_fd(descriptor, budget, True)
            finally:
                os.close(descriptor)
        writes["favicon.ico"] = {key: info[key] for key in ("size", "sha256")}
        report = {"binding": mtime_binding(root, budget), "writes": writes, "removed": removed}
        mtime_report_paths(report)
        mtime_json_write(path, report)
    except (MtimeCold, OSError, ValueError, KeyError, TypeError):
        mtime_json_write(path, {"cold": True})
        print("Source timestamps: baseline rejected.")


def mtime_snapshot(root, record_path, snapshot_path, origin):
    snapshot = {"wall": time.time_ns(), "monotonic": time.monotonic_ns(), "candidates": {}}
    try:
        prior = mtime_json_read(record_path)
    except (MtimeCold, OSError, ValueError, KeyError, TypeError):
        prior = None
    # Invalidation is mandatory even for malformed records, before checkout mutates anything.
    record_path.unlink(missing_ok=True)
    try:
        budget = MtimeBudget()
        binding = mtime_binding(root, budget)
        snapshot["binding"] = binding
        if binding["origin"] != origin or prior is None or prior["binding"] != binding:
            raise MtimeCold("baseline rejected")
        writes, _ = mtime_report_paths(prior)
        dirty, files, tracked = mtime_inventory(root, budget, writes)
        mtime_verify_report(prior, dirty, files, tracked)
        if mtime_binding(root, budget) != binding:
            raise RuntimeError("Source timestamp check detected a revision race.")
        if dirty != prior["dirty"] or files != prior["files"]:
            raise MtimeCold("baseline rejected")
        candidates = {name: info for name, info in files.items() if mtime_candidate(name)}
        completed = prior["completed"]
        if type(completed) is not int or completed > snapshot["wall"]:
            raise MtimeCold("clock anomaly")
        if any(info["mtime"] > completed for info in candidates.values()):
            raise MtimeCold("clock anomaly")
        snapshot["candidates"] = candidates
        snapshot["completed"] = completed
        print("Source timestamps: baseline accepted.")
    except (MtimeCold, OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired):
        print("Source timestamps: baseline rejected.")
    mtime_json_write(snapshot_path, snapshot)


def mtime_clock_ok(snapshot):
    wall, monotonic = time.time_ns(), time.monotonic_ns()
    return (type(snapshot.get("wall")) is int and type(snapshot.get("monotonic")) is int
            and wall >= snapshot["wall"] and monotonic >= snapshot["monotonic"]
            and abs((wall - snapshot["wall"]) - (monotonic - snapshot["monotonic"])) <= 1000000000)


def mtime_complete(root, record_path, snapshot_path, report_path):
    restoring = False
    try:
        budget = MtimeBudget()
        try:
            snapshot, report = mtime_json_read(snapshot_path), mtime_json_read(report_path)
        except OSError:
            raise MtimeCold("baseline rejected") from None
        binding = mtime_binding(root, budget)
        if report["binding"] != binding:
            raise MtimeCold("baseline rejected")
        writes, _ = mtime_report_paths(report)
        # No timestamp can change until the ENTIRE tracked inventory is complete.
        dirty, files, tracked = mtime_inventory(root, budget, writes)
        mtime_verify_report(report, dirty, files, tracked)
        old_binding = dict(snapshot.get("binding", {}))
        old_binding["head"] = binding["head"]
        candidates = snapshot["candidates"] if mtime_clock_ok(snapshot) and old_binding == binding else {}
        if not isinstance(candidates, dict) or len(candidates) > MTIME_ENTRY_LIMIT:
            raise MtimeCold("baseline rejected")
        if candidates and (type(snapshot.get("completed")) is not int
                or snapshot["completed"] > snapshot["wall"]
                or any(not isinstance(old, dict) or type(old.get("mtime")) is not int
                       or old["mtime"] > snapshot["completed"] for old in candidates.values())):
            candidates = {}
        if mtime_binding(root, budget) != binding:
            raise MtimeCold("baseline rejected")
        selected = []
        for name, old in candidates.items():
            budget.check()
            mtime_path(name)
            if not isinstance(old, dict):
                raise MtimeCold("baseline rejected")
            current = files.get(name)
            if not mtime_candidate(name) or current is None:
                continue
            if any(old.get(key) != current[key] for key in ("size", "sha256", "git_mode", "mode", "uid", "gid", "links")):
                continue
            if not (type(old.get("mtime")) is int and old["mtime"] <= snapshot["completed"] <= snapshot["wall"]
                    and old["mtime"] < current["mtime"]):
                continue
            selected.append((name, old["mtime"]))
        # Preflight the extra verification budget before any writes.
        remaining = sum(files[name]["size"] for name, _ in selected)
        if budget.bytes + remaining > MTIME_TREE_BYTES or budget.candidate_bytes + remaining > MTIME_CANDIDATE_BYTES:
            raise MtimeCold("limit exceeded")
        prospective = {**report, "dirty": dirty, "files": files, "completed": time.time_ns()}
        if len(json.dumps(prospective, sort_keys=True).encode()) + 128 > MTIME_JSON_LIMIT:
            raise MtimeCold("limit exceeded")
        restored = 0
        for name, previous in selected:
            if not mtime_clock_ok(snapshot):
                raise MtimeCold("clock anomaly")
            with mtime_parent(root, name) as (parent, leaf):
                descriptor = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
                try:
                    current = mtime_read_fd(descriptor, budget, True)
                    current["git_mode"] = files[name]["git_mode"]
                    if current != files[name]:
                        raise RuntimeError("Source timestamp check detected a restore race.")
                    mtime_recheck_path(root, name, current)
                    before = os.fstat(descriptor)
                    restoring = True
                    os.utime(descriptor, ns=(before.st_atime_ns, previous))
                    after = os.fstat(descriptor)
                    unchanged = ("mode", "uid", "gid", "size", "device", "inode", "links")
                    if (after.st_mtime_ns != previous
                            or any(mtime_stat(after)[key] != mtime_stat(before)[key] for key in unchanged)):
                        raise RuntimeError("Source timestamp restoration could not be verified.")
                    current.update(mtime_stat(after))
                    mtime_recheck_path(root, name, current)
                    files[name] = current
                    if name in dirty:
                        dirty[name] = current
                    restored += 1
                finally:
                    os.close(descriptor)
        budget.check()
        if restoring and not mtime_clock_ok(snapshot):
            raise MtimeCold("clock anomaly")
        record = {**report, "dirty": dirty, "files": files, "completed": time.time_ns()}
        mtime_json_write(record_path, record)
        print(f"Source timestamps: restored {restored} files.")
    except OSError:
        raise RuntimeError("Source timestamp completion encountered an I/O failure.") from None
    except (MtimeCold, ValueError, KeyError, TypeError, subprocess.TimeoutExpired):
        if restoring:
            raise RuntimeError("Source timestamp completion failed after restoration.") from None
        print("Source timestamps: baseline rejected.")


def extension_identity_path(path: Path) -> bool:
    """Preserve the built-in extension ID and its matching gettext template name."""
    return (
        path.name == LIBRELOGO_EXTENSION_ID and path.parent.name == "extensions"
    ) or path.parts[-4:] == (
        "extensions",
        LIBRELOGO_EXTENSION_ID,
        "po",
        f"{LIBRELOGO_EXTENSION_ID}.pot",
    )


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
        if (
            FORBIDDEN_MARK.search(path.name)
            and not legal_path(path)
            and not extension_identity_path(path)
        ):
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
    configure = root / "configure.ac"
    if configure.is_file():
        configure_text = configure.read_text(encoding="utf-8")
        if not re.search(
            r"^AC_INIT\(\[coolwsd\], \[[^]]+\], \[about:blank\], "
            r"\[coolwsd\], \[about:blank\]\)$",
            configure_text,
            re.MULTILINE,
        ):
            raise RuntimeError(
                "configure.ac has an invalid neutral AC_INIT declaration"
            )
    violations = scan_files(source_paths(root))
    fail_on_violations(violations)
    print("Online Office pre-build debranding scan passed.")


def validate_po_catalogues(root: Path, rewrite: bool = False) -> None:
    """Parse every browser PO catalogue with the Online build converter."""
    converter = root / "browser/util/po2json.py"
    catalogue_root = root / "browser/po"
    catalogues = sorted(catalogue_root.glob("*.po"))
    if not converter.is_file():
        raise RuntimeError(f"PO converter is missing: {converter}")
    if not catalogues:
        raise RuntimeError(f"no browser PO catalogues found in: {catalogue_root}")

    with tempfile.TemporaryDirectory() as directory:
        temporary_catalogues = []
        for catalogue in catalogues:
            text = catalogue.read_text(encoding="utf-8")
            if rewrite:
                text = rewrite_text(text)
            temporary_catalogue = Path(directory) / catalogue.name
            temporary_catalogue.write_text(text, encoding="utf-8")
            temporary_catalogues.append(temporary_catalogue)
        try:
            subprocess.run(
                [
                    sys.executable,
                    str(converter),
                    "--quiet",
                    *(str(catalogue) for catalogue in temporary_catalogues),
                ],
                check=True,
            )
        except subprocess.CalledProcessError as error:
            mode = "transformed " if rewrite else ""
            raise RuntimeError(
                f"one or more {mode}browser PO catalogues failed to parse"
            ) from error

    mode = "transformed " if rewrite else ""
    print(f"All {len(catalogues)} {mode}browser PO catalogues passed po2json parsing.")


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
            # SBOM identities and references are provenance, not product copy.
            paths.extend(
                path for path in directory.rglob("*")
                if not path.name.endswith("-sbom.spdx.json")
            )
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
            if FORBIDDEN_MARK.search(path.name) and not extension_identity_path(path):
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

    # Change only the document display name; retain package identities and provenance.
    sbom_path = rootfs / "usr/share/coolwsd/online-office-sbom.spdx.json"
    sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
    if sbom.get("@context") != "https://spdx.org/rdf/3.0.1/spdx-context.jsonld":
        raise RuntimeError("expected an SPDX 3.0.1 Online Office SBOM")
    graph = sbom.get("@graph")
    if not isinstance(graph, list) or not all(isinstance(item, dict) for item in graph):
        raise RuntimeError("SBOM graph is absent or malformed")
    documents = [item for item in graph if item.get("type") == "SpdxDocument"]
    if len(documents) != 1:
        raise RuntimeError("SBOM must contain exactly one SPDX document")
    documents[0]["name"] = PRODUCT_NAME
    sbom_path.write_text(json.dumps(sbom, indent=2) + "\n", encoding="utf-8")

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

    sbom_path = rootfs / "usr/share/coolwsd/online-office-sbom.spdx.json"
    sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
    if sbom.get("@context") != "https://spdx.org/rdf/3.0.1/spdx-context.jsonld":
        raise RuntimeError("expected an SPDX 3.0.1 Online Office SBOM")
    graph = sbom.get("@graph")
    if not isinstance(graph, list) or not all(isinstance(item, dict) for item in graph):
        raise RuntimeError("SBOM graph is absent or malformed")
    documents = [item for item in graph if item.get("type") == "SpdxDocument"]
    if len(documents) != 1 or documents[0].get("name") != PRODUCT_NAME:
        raise RuntimeError("SBOM must contain one neutral Online Office document")

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


def assert_distroless_runtime(rootfs: Path) -> None:
    forbidden_paths = (
        "bin/sh",
        "bin/bash",
        "bin/busybox",
        "usr/bin/sh",
        "usr/bin/bash",
        "usr/bin/busybox",
        "usr/bin/apt",
        "usr/bin/apt-get",
        "usr/bin/dpkg",
        "usr/bin/rpm",
        "sbin/apk",
        "usr/bin/python",
        "usr/bin/python3",
        "usr/bin/java",
        "usr/bin/javac",
        "usr/bin/ssh",
        "usr/bin/gcc",
        "usr/bin/g++",
        "usr/bin/make",
        "usr/bin/nano",
        "usr/bin/vi",
        "usr/bin/coolconfig",
        "usr/bin/coolconvert",
        "usr/bin/coolstress",
        "usr/bin/coolwsd-systemplate-setup",
    )
    for relative in forbidden_paths:
        if (rootfs / relative).exists():
            raise RuntimeError(f"forbidden runtime path is present: /{relative}")

    forbidden_names = re.compile(
        r"(?:^python(?:[0-9.]*)?$|pyuno|pythonloader|libpython|libjvm|"
        r"javasettings|javavendors|libjvmaccess|libjvmfwk|sunjavaplugin|javaldx|"
        r"\.py[co]?$|\.jar$|\.class$)",
        re.IGNORECASE,
    )
    for path in rootfs.rglob("*"):
        if forbidden_names.search(path.name):
            raise RuntimeError(f"Python, PyUNO or Java content is present: {path}")

    if not (rootfs / "usr/lib/locale/locale-archive").is_file():
        raise RuntimeError("C.UTF-8 locale archive is absent")
    if not (rootfs / "etc/ssl/certs/ca-certificates.crt").is_file():
        raise RuntimeError("CA trust bundle is absent")
    for font_root in (
        rootfs / "usr/share/fonts",
        rootfs / "opt/cool/systemplate/usr/share/fonts",
    ):
        if font_root.exists() and any(path.is_file() for path in font_root.rglob("*")):
            raise RuntimeError(f"external system font leaked into runtime: {font_root}")


def extract_container_rootfs(container: str, destination: Path) -> None:
    """Stream a container rootfs into a scanner-owned directory."""
    with subprocess.Popen(
        ["docker", "cp", f"{container}:/.", "-"],
        stdout=subprocess.PIPE,
    ) as docker_cp:
        if docker_cp.stdout is None:
            raise RuntimeError("Docker rootfs archive stream is unavailable")
        try:
            extraction = subprocess.run(
                [
                    "tar",
                    "--extract",
                    "--directory",
                    str(destination),
                    "--no-same-owner",
                    "--no-same-permissions",
                ],
                stdin=docker_cp.stdout,
                check=False,
            )
        finally:
            docker_cp.stdout.close()
        docker_cp_returncode = docker_cp.wait()

    if docker_cp_returncode:
        raise subprocess.CalledProcessError(docker_cp_returncode, docker_cp.args)
    extraction.check_returncode()


def scan_image(
    image: str,
    neutral_favicon: Path,
    expected_revision: str,
    *,
    distroless: bool,
) -> None:
    inspect = docker_json(["image", "inspect", image])
    serialised = json.dumps(inspect, sort_keys=True)
    violations = mark_violations(Path("OCI image configuration"), serialised)

    if not isinstance(inspect, list) or len(inspect) != 1:
        raise RuntimeError("Docker returned an unexpected image inspection result")
    config = inspect[0].get("Config", {})
    labels = config.get("Labels", {})
    revision = labels.get("org.opencontainers.image.revision", "")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise RuntimeError("OCI revision label is not a full lower-case Git revision")
    if revision != expected_revision:
        raise RuntimeError(
            "OCI revision label differs from the requested source revision"
        )
    if labels.get("org.opencontainers.image.title") != PRODUCT_NAME:
        raise RuntimeError("OCI title is not Online Office")
    if labels.get("org.opencontainers.image.source") != (
        "https://github.com/Kreoh/online.mirror"
    ):
        raise RuntimeError("OCI source label differs from the admitted repository")
    if config.get("User") != "100":
        raise RuntimeError("OCI runtime user is not UID 100")
    expected_entrypoint = [
        "/usr/bin/coolwsd",
        "--use-env-vars",
        "--o:sys_template_path=/opt/cool/systemplate",
        "--o:child_root_path=/opt/cool/child-roots",
        "--o:file_server_root_path=/usr/share/coolwsd",
        "--o:cache_files.path=/opt/cool/cache",
        "--o:logging.color=false",
        "--o:stop_on_config_change=true",
    ]
    if config.get("Entrypoint") != expected_entrypoint:
        raise RuntimeError("OCI entrypoint differs from the direct coolwsd contract")
    if distroless:
        if labels.get("io.kreoh.online-office.runtime") != "distroless-source":
            raise RuntimeError("OCI runtime label is not the source distroless route")
        healthcheck = config.get("Healthcheck", {}).get("Test")
        if healthcheck != ["CMD", "/usr/bin/coolwsd", "--probe", "--use-env-vars"]:
            raise RuntimeError(
                "OCI healthcheck differs from the shell-free probe contract"
            )

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

    container = subprocess.run(
        ["docker", "create", "--entrypoint", "/usr/bin/coolwsd", image, "--version"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.strip()
    try:
        with tempfile.TemporaryDirectory() as directory:
            rootfs = Path(directory) / "rootfs"
            rootfs.mkdir()
            if distroless:
                extract_container_rootfs(container, rootfs)
            else:
                subprocess.run(
                    ["docker", "cp", f"{container}:/.", str(rootfs)],
                    check=True,
                )
            scan_rootfs(rootfs, neutral_favicon.resolve())
            if distroless:
                assert_distroless_runtime(rootfs)
    finally:
        subprocess.run(
            ["docker", "rm", "-f", container],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    route = "distroless" if distroless else "diagnostic"
    print(f"Online Office {route} OCI metadata and rootfs scan passed.")



def mtime_self_test():
    """Tiny source and Git fixtures only: no native tools or build targets."""
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        root = base / "source"
        root.mkdir()
        def git(*args):
            subprocess.run(["git", "-C", str(root), *args], check=True, timeout=10,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           env={**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
                                "GIT_AUTHOR_NAME": "Fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
                                "GIT_COMMITTER_NAME": "Fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid"})
        git("init", "-q")
        git("remote", "add", "origin", "fixture://source")
        inputs = {
            "common/shared.hpp": 'const char *name = "Collabora Online";\n',
            "common/plain.hpp": "unchanged source\n",
            "engine/config_host/generated.h": "Collabora Online\n",
            "engine/config_host/template.in": "Collabora Online\n",
            "engine/config_build/generated.h": "Collabora Online\n",
            "engine/config_build/template.in": "Collabora Online\n",
            "engine/config_build.mk": "Collabora Online\n",
            "engine/workdir/generated.hpp": "generated output\n",
            "engine/static/emscripten/favicon.ico": "neutral icon",
            "favicon.ico": "old icon",
            "browser/extensions/com.collaboraoffice.demo-fixture/manifest.json": "demo\n",
            "browser/po/en.po": 'msgid "hello"\nmsgstr "hello"\n',
            "browser/util/po2json.py": "# Synthetic parser; no native commands.\nimport sys\nfrom pathlib import Path\nassert all('INVALID' not in Path(p).read_text() for p in sys.argv[2:])\n",
        }
        for name, value in inputs.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value)
        git("add", ".")
        git("commit", "-qm", "fixture")
        original = mtime_binding(root, MtimeBudget())["head"]
        record, snapshot, report = (base / name for name in ("record.json", "snapshot.json", "report.json"))
        target = root / "common/shared.hpp"
        def reset():
            git("update-index", "--no-assume-unchanged", "common/plain.hpp")
            git("update-index", "--no-skip-worktree", "common/plain.hpp")
            git("reset", "--hard", original)
            git("clean", "-fd")
            for path in (record, snapshot, report):
                path.unlink(missing_ok=True)
        def begin():
            mtime_snapshot(root, record, snapshot, "fixture://source")
            assert not record.exists()
        def finish(seed=False):
            apply_source(root, report)
            scan_source(root)
            validate_po_catalogues(root)
            if seed:
                old = time.time_ns() - 20000000000
                for name in mtime_json_read(report)["writes"]:
                    os.utime(root / name, ns=(old, old))
            fresh = target.stat().st_mtime_ns if target.exists() else None
            mtime_complete(root, record, snapshot, report)
            return fresh
        def baseline():
            reset()
            begin()
            finish(True)
            assert record.exists()
            return target.stat().st_mtime_ns
        old = baseline()
        begin()
        assert mtime_json_read(snapshot)["candidates"]
        git("reset", "--hard", original)
        fresh = finish()
        assert fresh > old and target.stat().st_mtime_ns == old
        assert (root / "engine/config_host/generated.h").stat().st_mtime_ns > old
        assert (root / "favicon.ico").stat().st_mtime_ns > old
        assert (root / "engine/config_host/template.in").stat().st_mtime_ns == old
        assert (root / "engine/config_build/generated.h").stat().st_mtime_ns > old
        assert (root / "engine/config_build.mk").stat().st_mtime_ns > old
        assert (root / "engine/config_build/template.in").stat().st_mtime_ns == old
        assert "common/plain.hpp" not in mtime_json_read(snapshot)["candidates"]
        assert not (root / "browser/extensions/com.collaboraoffice.demo-fixture").exists()

        # A different upstream blob is reusable only when its final bytes match.
        for text, reusable in [('const char *name = "Collabora Office";\n', True),
                               ('const char *name = "Collabora Online changed";\n', False)]:
            old = baseline()
            begin()
            git("reset", "--hard", original)
            target.write_text(text)
            git("add", ".")
            git("commit", "-qm", "new source")
            finish()
            assert (target.stat().st_mtime_ns == old) == reusable

        # Unknown edits, staged edits, hidden index flags and metadata tampering
        # must all invalidate the complete baseline, even with a preserved mtime.
        for case in ("dirty", "same-size", "index", "assume", "skip", "mtime", "mode",
                     "policy", "origin", "root", "head", "owner", "truncated", "duplicate", "oversized"):
            old = baseline()
            saved = mtime_json_read(record)
            if case == "dirty":
                (root / "common/plain.hpp").write_text("unexpected\n")
            elif case == "same-size":
                contents = target.read_bytes()
                before = target.stat()
                target.write_bytes(contents.replace(b"name", b"evil"))
                os.utime(target, ns=(before.st_atime_ns, before.st_mtime_ns))
            elif case == "index":
                git("add", "common/shared.hpp")
            elif case in {"assume", "skip"}:
                git("update-index", "--assume-unchanged" if case == "assume" else "--skip-worktree", "common/plain.hpp")
            elif case == "mtime":
                os.utime(target, ns=(old + 1, old + 1))
            elif case == "mode":
                target.chmod(0o755)
            elif case in {"policy", "origin", "root", "head"}:
                saved["binding"][case] = "invalid"
                mtime_json_write(record, saved)
            elif case == "owner":
                saved["files"]["common/shared.hpp"]["uid"] += 1
                mtime_json_write(record, saved)
            elif case == "truncated":
                record.write_text('{"')
            elif case == "duplicate":
                record.write_text('{"binding": {}, "binding": {}}')
            elif case == "oversized":
                record.write_bytes(b" " * (MTIME_JSON_LIMIT + 1))
            begin()
            assert not mtime_json_read(snapshot)["candidates"], case
            if case in {"assume", "skip"}:
                git("update-index", "--no-assume-unchanged", "common/plain.hpp")
                git("update-index", "--no-skip-worktree", "common/plain.hpp")

        for case in ("rename", "delete", "executable", "future", "backwards", "divergence", "precision"):
            old = baseline()
            begin()
            git("reset", "--hard", original)
            if case == "rename":
                git("mv", "common/shared.hpp", "common/renamed.hpp")
                git("commit", "-qm", "rename")
            elif case == "delete":
                git("rm", "common/shared.hpp")
                git("commit", "-qm", "delete")
            elif case == "executable":
                target.chmod(0o755)
                git("add", ".")
                git("commit", "-qm", "mode")
            apply_source(root, report)
            scan_source(root)
            validate_po_catalogues(root)
            state = mtime_json_read(snapshot)
            if case == "future":
                state["candidates"]["common/shared.hpp"]["mtime"] = time.time_ns() + 10000000000
            elif case == "backwards":
                state["wall"] = time.time_ns() + 10000000000
            elif case == "divergence":
                state["wall"] -= 2000000000
            elif case == "precision":
                os.utime(target, ns=(old, old))
            mtime_json_write(snapshot, state)
            fresh = target.stat().st_mtime_ns if target.exists() else None
            mtime_complete(root, record, snapshot, report)
            if target.exists():
                assert target.stat().st_mtime_ns == fresh, case
            if case == "rename":
                assert (root / "common/renamed.hpp").stat().st_mtime_ns != old

        # No symlink or hard link can cause an outside timestamp write.
        for case in ("leaf", "parent", "hardlink"):
            baseline()
            outside = base / "outside"
            outside.write_text("outside content\n")
            before = outside.stat().st_mtime_ns
            if case == "leaf":
                target.unlink()
                target.symlink_to(outside)
            elif case == "parent":
                shutil.rmtree(root / "common")
                (root / "common").symlink_to(base, target_is_directory=True)
            else:
                target.unlink()
                os.link(outside, target)
            begin()
            assert not mtime_json_read(snapshot)["candidates"]
            assert outside.stat().st_mtime_ns == before
            if case == "parent":
                (root / "common").unlink()
            outside.unlink()

        # Limits are injected only into tiny fixtures; no native tree is scanned.
        for limit in ("MTIME_TREE_BYTES", "MTIME_CANDIDATE_BYTES", "MTIME_FILE_LIMIT",
                      "MTIME_TREE_FILE_LIMIT", "MTIME_TREE_ENTRIES", "MTIME_GIT_BYTES", "MTIME_SECONDS", "MTIME_ENTRY_LIMIT"):
            baseline()
            previous = globals()[limit]
            try:
                globals()[limit] = 0
                begin()
                assert not mtime_json_read(snapshot)["candidates"], limit
            finally:
                globals()[limit] = previous

        # Before publication, interruptions leave no record to adopt. A later
        # invocation starts cold regardless of an abandoned transient snapshot.
        for phase in ("snapshot", "checkout", "transform", "scan", "po"):
            baseline()
            begin()
            if phase != "snapshot":
                git("reset", "--hard", original)
            if phase in {"transform", "scan", "po"}:
                apply_source(root, report)
            if phase in {"scan", "po"}:
                scan_source(root)
            if phase == "po":
                validate_po_catalogues(root)
            assert not record.exists()
            begin()
            assert not mtime_json_read(snapshot)["candidates"]

        # A complete inventory failure prevents *all* timestamp writes.
        old = baseline()
        begin()
        git("reset", "--hard", original)
        apply_source(root, report)
        fresh = target.stat().st_mtime_ns
        (root / "common/plain.hpp").write_text("unexpected final edit\n")
        mtime_complete(root, record, snapshot, report)
        assert not record.exists() and target.stat().st_mtime_ns == fresh and fresh != old

        # Simulate interruption immediately after the first mtime write.
        baseline()
        begin()
        git("reset", "--hard", original)
        apply_source(root, report)
        real_utime = os.utime
        def interrupted(*args, **kwargs):
            real_utime(*args, **kwargs)
            raise OSError("injected interruption")
        try:
            os.utime = interrupted
            try:
                mtime_complete(root, record, snapshot, report)
            except RuntimeError:
                pass
            else:
                raise AssertionError("restoration failure was swallowed")
        finally:
            os.utime = real_utime
        assert not record.exists()
        begin()
        assert not mtime_json_read(snapshot)["candidates"]

        for name in ("/absolute", "../outside", "common//file", "common/./file", "x" * 1025):
            try:
                mtime_path(name)
            except MtimeCold:
                pass
            else:
                raise AssertionError("unsafe metadata path was accepted")
        for name in ("engine/workdir/x.hpp", "engine/instdir/x.hpp", "engine/config_host/a.h",
                     "engine/config_host.mk", "engine/config_build.mk", "engine/config_build/a.h",
                     "engine/config.status", "engine/Makefile"):
            assert not mtime_candidate(name)
        assert mtime_candidate("engine/config_host/template.in")
        assert mtime_candidate("engine/config_build/template.in")

        # Report generation must check limits before invoking SHA-256, and
        # ordinary source transformation must finish after the hint goes cold.
        original_file_limit = MTIME_FILE_LIMIT
        original_phase_limit = MTIME_CANDIDATE_BYTES
        real_sha256 = hashlib.sha256
        hashed = []
        def counted_sha256(payload=b""):
            hashed.append(len(payload))
            return real_sha256(payload)
        try:
            hashlib.sha256 = counted_sha256
            globals()["MTIME_FILE_LIMIT"] = 3
            globals()["MTIME_CANDIDATE_BYTES"] = 5
            writes = MtimeWrites()
            mtime_note_write(writes, root / "a", b"1234")
            mtime_note_write(writes, root / "b", b"1")
            assert writes.cold and not writes and hashed == []
            mtime_source_report(root, report, writes, [])
            assert mtime_json_read(report) == {"cold": True} and hashed == []
            writes = MtimeWrites()
            mtime_note_write(writes, root / "a", b"123")
            mtime_note_write(writes, root / "b", b"12")
            assert not writes.cold and writes.hashed_bytes == 5 and hashed == [3, 2]
            mtime_note_write(writes, root / "c", b"1")
            mtime_note_write(writes, root / "d", b"1")
            assert writes.cold and not writes and hashed == [3, 2]
            mtime_source_report(root, report, writes, [])
            assert mtime_json_read(report) == {"cold": True} and hashed == [3, 2]
            # Exhausting the transformation budget also prevents favicon hashing.
            globals()["MTIME_FILE_LIMIT"] = 64
            writes = MtimeWrites()
            mtime_note_write(writes, root / "a", b"123")
            mtime_note_write(writes, root / "b", b"12")
            before = list(hashed)
            mtime_source_report(root, report, writes, [])
            assert mtime_json_read(report) == {"cold": True} and hashed == before
            # A byte limit reached by the first rewrite must not stop later writes.
            git("reset", "--hard", original)
            apply_source(root, report)
            assert target.read_text() == 'const char *name = "Online Office";\n'
            assert (root / "engine/config_host/template.in").read_text() == "Online Office\n"
            assert mtime_json_read(report) == {"cold": True} and hashed == before
        finally:
            hashlib.sha256 = real_sha256
            globals()["MTIME_FILE_LIMIT"] = original_file_limit
            globals()["MTIME_CANDIDATE_BYTES"] = original_phase_limit

        # Unexpected raw newline conversion is not hidden behind Git filters.
        baseline()
        begin()
        git("reset", "--hard", original)
        apply_source(root, report)
        plain = root / "common/plain.hpp"
        plain.write_bytes(plain.read_bytes().replace(b"\n", b"\r\n"))
        mtime_complete(root, record, snapshot, report)
        assert not record.exists()

        # Unsupported Git modes decline the whole inventory.
        reset()
        git("update-index", "--add", "--cacheinfo", f"160000,{original},common/submodule")
        git("commit", "-qm", "unsupported mode")
        try:
            mtime_inventory(root, MtimeBudget(), {})
        except MtimeCold:
            pass
        else:
            raise AssertionError("unsupported tracked mode was accepted")

        # Replace the checked inode while the descriptor is still open.
        baseline()
        begin()
        git("reset", "--hard", original)
        apply_source(root, report)
        real_read = mtime_read_fd
        inode = target.stat().st_ino
        def raced_read(descriptor, budget, candidate=False):
            info = real_read(descriptor, budget, candidate)
            if info["inode"] == inode:
                replacement = root / "replacement"
                replacement.write_bytes(target.read_bytes())
                replacement.replace(target)
            return info
        try:
            globals()["mtime_read_fd"] = raced_read
            try:
                mtime_complete(root, record, snapshot, report)
            except RuntimeError:
                pass
            else:
                raise AssertionError("inode replacement was accepted")
        finally:
            globals()["mtime_read_fd"] = real_read
        assert not record.exists()

        baseline()
        begin()
        git("reset", "--hard", original)
        apply_source(root, report)
        real_write = mtime_json_write
        def failed_publication(path, value):
            if path == record:
                raise OSError("injected publication failure")
            real_write(path, value)
        try:
            globals()["mtime_json_write"] = failed_publication
            try:
                mtime_complete(root, record, snapshot, report)
            except RuntimeError:
                pass
            else:
                raise AssertionError("publication failure was swallowed")
        finally:
            globals()["mtime_json_write"] = real_write
        assert not record.exists()
        begin()
        assert not mtime_json_read(snapshot)["candidates"]
        print("Source timestamp fixtures passed (tiny Git repositories, no native commands).")


def self_test() -> None:
    mtime_self_test()
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

    wrapped_catalogue_url = (
        '"For help see https://"\n'
        '"sdk.collaboraonline.com/docs/installation/Proxy_settings.html"\n'
        '"More details at https://www."\n'
        '"collaboraonline.org/post/build-code/"\n'
        '"A malformed translation has https ://collaboraonline."\n'
        '"org/post/translate/"\n'
    )
    rewritten_catalogue_url = rewrite_text(wrapped_catalogue_url)
    assert "collabora" not in rewritten_catalogue_url.lower()
    assert rewritten_catalogue_url.count("example.invalid") == 3

    wrapped_translated_html = (
        'msgid ""\n'
        '"Read <a href=\\"https://www."\n'
        '"collaboraonline.com/docs/\\">the documentation</a>."\n'
        'msgstr ""\n'
        '"Lesen Sie <a href=\\"https://collaboraoffice.com\\">die "\n'
        '"Dokumentation</a>."\n'
    )
    rewritten_translated_html = rewrite_text(wrapped_translated_html)
    assert "collabora" not in rewritten_translated_html.lower()
    assert 'href=\\"https://www."' in rewritten_translated_html
    assert '"example.invalid/docs/\\">the documentation' in (rewritten_translated_html)
    assert 'href=\\"about:blank\\">die ' in rewritten_translated_html

    autoconf_declaration = (
        "AC_INIT([coolwsd], [26.04.4.0], "
        "[https://github.com/CollaboraOnline/online/issues], [coolwsd], "
        "[https://www.collaboraoffice.org/])\n"
    )
    rewritten_autoconf_declaration = rewrite_text(autoconf_declaration)
    assert rewritten_autoconf_declaration == (
        "AC_INIT([coolwsd], [26.04.4.0], [about:blank], [coolwsd], [about:blank])\n"
    )
    assert (
        rewrite_text(rewritten_autoconf_declaration) == rewritten_autoconf_declaration
    )

    embedded = "prefixCollaboraOfficeSuffix PRODUCTNAME.collaboraoffice CollaboraOffice"
    assert rewrite_text(embedded) == (
        "prefixOnlineOfficeSuffix PRODUCTNAME.OnlineOffice OnlineOffice"
    )

    legal = "Copyright the Collabora Online contributors."
    assert rewrite_text(legal) == legal
    compatibility = (
        "COM.COLLABORAOFFICE.MSVD "
        'xmlns:coext="urn:com:collaboraoffice:names:experimental" coext:value'
    )
    assert rewrite_text(compatibility) == compatibility
    assert not mark_violations(Path("compatibility.xml"), compatibility)

    threaded_comment_namespace = '"CollaboraOffice:ThreadedCommentPerson:"'
    assert rewrite_text(threaded_comment_namespace) == (
        '"OnlineOffice:ThreadedCommentPerson:"'
    )
    generator_detection = 'aGenerator.startsWithIgnoreAsciiCase("Collabora")'
    assert rewrite_text(generator_detection) == (
        'aGenerator.startsWithIgnoreAsciiCase("Online Office")'
    )
    assert mark_violations(Path("threaded-comment.cxx"), threaded_comment_namespace)
    assert mark_violations(Path("generator-detection.cxx"), generator_detection)

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
        (source_root / "browser/po/templates").mkdir(parents=True)
        (source_root / "browser/po/templates/cool-ui.pot").write_text(
            wrapped_catalogue_url,
            encoding="utf-8",
        )
        (source_root / "browser/po/ui-en_GB.po").write_text(
            wrapped_catalogue_url,
            encoding="utf-8",
        )
        (source_root / "configure.ac").write_text(
            autoconf_declaration,
            encoding="utf-8",
        )
        threaded_comment_source = (
            source_root
            / "engine/sc/source/ui/operation/InsertThreadedCommentOperation.cxx"
        )
        threaded_comment_source.parent.mkdir(parents=True)
        threaded_comment_source.write_text(
            'OString aInput = "CollaboraOffice:ThreadedCommentPerson:";\n',
            encoding="utf-8",
        )
        generator_source = (
            source_root / "engine/sc/source/filter/oox/workbookhelper.cxx"
        )
        generator_source.parent.mkdir(parents=True)
        generator_source.write_text(
            'aGenerator.startsWithIgnoreAsciiCase("Collabora");\n',
            encoding="utf-8",
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
        assert "OnlineOffice:ThreadedCommentPerson:" in (
            threaded_comment_source.read_text(encoding="utf-8")
        )
        assert 'startsWithIgnoreAsciiCase("Online Office")' in (
            generator_source.read_text(encoding="utf-8")
        )

        threaded_comment_source.write_text(
            'OString aInput = "CollaboraOffice:ThreadedCommentPerson:";\n',
            encoding="utf-8",
        )
        generator_source.write_text(
            'aGenerator.startsWithIgnoreAsciiCase("Collabora");\n',
            encoding="utf-8",
        )
        scan_errors = io.StringIO()
        with contextlib.redirect_stderr(scan_errors):
            try:
                scan_source(source_root)
            except SystemExit as error:
                assert error.code == 1
            else:
                raise AssertionError("pre-build scan accepted compiled product marks")
        scan_output = scan_errors.getvalue()
        assert "CollaboraOffice:ThreadedCommentPerson:" in scan_output
        assert 'startsWithIgnoreAsciiCase("Collabora")' in scan_output

    with tempfile.TemporaryDirectory() as directory:
        rootfs = Path(directory)
        (rootfs / "usr/lib/locale").mkdir(parents=True)
        (rootfs / "usr/lib/locale/locale-archive").write_bytes(b"locale")
        (rootfs / "etc/ssl/certs").mkdir(parents=True)
        (rootfs / "etc/ssl/certs/ca-certificates.crt").write_bytes(b"ca")
        assert_distroless_runtime(rootfs)
        forbidden_python = rootfs / "opt/online-office/program/libpython3.13.so"
        forbidden_python.parent.mkdir(parents=True)
        forbidden_python.write_bytes(b"python")
        try:
            assert_distroless_runtime(rootfs)
        except RuntimeError as error:
            assert "Python, PyUNO or Java" in str(error)
        else:
            raise AssertionError("Python runtime fixture was accepted")
        forbidden_python.unlink()
        external_font = rootfs / "usr/share/fonts/debian-font.ttf"
        external_font.parent.mkdir(parents=True)
        external_font.write_bytes(b"font")
        try:
            assert_distroless_runtime(rootfs)
        except RuntimeError as error:
            assert "external system font" in str(error)
        else:
            raise AssertionError("external system font fixture was accepted")

    print("Online Office debranding helper self-test passed.")


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    for command in ("source", "scan-source"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("root", type=Path)
        if command == "source":
            subparser.add_argument("--report", type=Path)

    for command in ("mtime-snapshot", "mtime-complete"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("root", type=Path)
        subparser.add_argument("record", type=Path)
        subparser.add_argument("snapshot", type=Path)
        subparser.add_argument("extra")

    po_parser = subparsers.add_parser("validate-po")
    po_parser.add_argument("root", type=Path)
    po_parser.add_argument(
        "--rewrite",
        action="store_true",
        help="validate temporary transformed copies without changing the source tree",
    )

    rootfs_parser = subparsers.add_parser("rootfs")
    rootfs_parser.add_argument("rootfs", type=Path)
    rootfs_parser.add_argument("source_root", type=Path)

    rootfs_scan_parser = subparsers.add_parser("scan-rootfs")
    rootfs_scan_parser.add_argument("rootfs", type=Path)
    rootfs_scan_parser.add_argument("neutral_favicon", type=Path)

    for command in ("scan-image", "scan-distroless-image"):
        image_parser = subparsers.add_parser(command)
        image_parser.add_argument("image")
        image_parser.add_argument("neutral_favicon", type=Path)
        image_parser.add_argument("expected_revision")
    subparsers.add_parser("self-test")

    arguments = parser.parse_args()
    if arguments.command == "source":
        apply_source(arguments.root.resolve(), arguments.report)
    elif arguments.command == "mtime-snapshot":
        try:
            mtime_snapshot(arguments.root.resolve(), arguments.record, arguments.snapshot, arguments.extra)
        except (OSError, RuntimeError, MtimeCold):
            raise SystemExit("Source timestamp preparation failed.") from None
    elif arguments.command == "mtime-complete":
        try:
            mtime_complete(arguments.root.resolve(), arguments.record, arguments.snapshot, Path(arguments.extra))
        except (OSError, RuntimeError, MtimeCold):
            raise SystemExit("Source timestamp completion failed.") from None
    elif arguments.command == "scan-source":
        scan_source(arguments.root.resolve())
    elif arguments.command == "validate-po":
        validate_po_catalogues(arguments.root.resolve(), arguments.rewrite)
    elif arguments.command == "rootfs":
        apply_rootfs(arguments.rootfs.resolve(), arguments.source_root.resolve())
    elif arguments.command == "scan-rootfs":
        scan_rootfs(arguments.rootfs.resolve(), arguments.neutral_favicon.resolve())
    elif arguments.command in {"scan-image", "scan-distroless-image"}:
        scan_image(
            arguments.image,
            arguments.neutral_favicon,
            arguments.expected_revision,
            distroless=arguments.command == "scan-distroless-image",
        )
    else:
        self_test()


if __name__ == "__main__":
    main()
