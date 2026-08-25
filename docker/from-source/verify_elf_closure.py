"""Assemble and verify the loader-reachable ELF dependency closure."""

from __future__ import annotations

import argparse
import functools
import os
import re
import shutil
import stat
import subprocess
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

SYSTEMPLATE = PurePosixPath("/opt/cool/systemplate")
ORIGIN = re.compile(r"\$\{ORIGIN\}|\$ORIGIN")
TARGET_GLIBC = re.compile(
    r"^(?:ld(?:64)?(?:-linux[^.]*)?|libc|libm|libmvec|libdl|libpthread|librt|"
    r"libresolv|libutil|libnsl|libnss_[a-z0-9_]+|libanl|libBrokenLocale|"
    r"libthread_db)\.so"
)


class ClosureError(RuntimeError):
    pass


@dataclass(frozen=True)
class ElfInfo:
    machine: str
    elf_class: str
    interpreter: str | None
    needed: tuple[str, ...]
    rpath: tuple[str, ...]
    runpath: tuple[str, ...]
    required_versions: dict[str, frozenset[str]]
    provided_versions: frozenset[str]


def run_readelf(path: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["readelf", *arguments, str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode != 0:
        raise ClosureError(f"readelf failed for {path}")
    return result.stdout


@functools.cache
def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as stream:
            return stream.read(4) == b"\x7fELF"
    except OSError:
        return False


def version_information(path: Path) -> tuple[dict[str, frozenset[str]], frozenset[str]]:
    output = run_readelf(path, "--version-info")
    required: dict[str, set[str]] = {}
    provided: set[str] = set()
    section: str | None = None
    current_file: str | None = None
    for line in output.splitlines():
        if line.startswith("Version needs section"):
            section = "needs"
            current_file = None
            continue
        if line.startswith("Version definition section"):
            section = "definitions"
            current_file = None
            continue
        if line and not line[0].isspace():
            section = None
            current_file = None
            continue
        if section == "needs":
            file_match = re.search(r"\bFile: (\S+)", line)
            if file_match:
                current_file = file_match.group(1)
                required.setdefault(current_file, set())
                continue
            name_match = re.search(r"\bName: (\S+)", line)
            if current_file and name_match:
                required[current_file].add(name_match.group(1))
        elif section == "definitions":
            name_match = re.search(r"\bName: (\S+)", line)
            if name_match:
                provided.add(name_match.group(1))
    return (
        {name: frozenset(versions) for name, versions in required.items()},
        frozenset(provided),
    )


@functools.cache
def elf_info(path: Path) -> ElfInfo:
    header = run_readelf(path, "-h")
    machine_match = re.search(r"^\s*Machine:\s*(.+)$", header, re.MULTILINE)
    class_match = re.search(r"^\s*Class:\s*(\S+)$", header, re.MULTILINE)
    if not machine_match or not class_match:
        raise ClosureError(f"cannot determine ELF architecture: {path}")

    program_headers = run_readelf(path, "-l")
    interpreter_match = re.search(
        r"Requesting program interpreter: ([^]]+)", program_headers
    )
    dynamic = run_readelf(path, "-d")
    needed = tuple(re.findall(r"\(NEEDED\).*Shared library: \[([^]]+)\]", dynamic))

    def paths(tag: str) -> tuple[str, ...]:
        match = re.search(rf"\({tag}\).*Library (?:r|run)path: \[([^]]*)\]", dynamic)
        return tuple(match.group(1).split(":")) if match and match.group(1) else ()

    required, provided = version_information(path)
    return ElfInfo(
        machine=machine_match.group(1).strip(),
        elf_class=class_match.group(1),
        interpreter=interpreter_match.group(1) if interpreter_match else None,
        needed=needed,
        rpath=paths("RPATH"),
        runpath=paths("RUNPATH"),
        required_versions=required,
        provided_versions=provided,
    )


def lexical_exists(root: Path, relative: PurePosixPath) -> bool:
    return (root / str(relative).lstrip("/")).exists() or (
        root / str(relative).lstrip("/")
    ).is_symlink()


def resolve_union(
    payload_root: Path, target_root: Path, relative: PurePosixPath
) -> tuple[Path, PurePosixPath] | None:
    if not relative.is_absolute():
        raise ClosureError(f"ELF lookup path is not absolute: {relative}")
    pending = relative
    seen: set[PurePosixPath] = set()
    for _ in range(80):
        if pending in seen:
            raise ClosureError(f"ELF symlink loop: {relative}")
        seen.add(pending)
        components = pending.parts[1:]
        current = PurePosixPath("/")
        restarted = False
        for index, component in enumerate(components):
            candidate = current / component
            selected_root: Path | None = None
            for root in (payload_root, target_root):
                if lexical_exists(root, candidate):
                    selected_root = root
                    break
            if selected_root is None:
                return None
            host_path = selected_root / str(candidate).lstrip("/")
            if not host_path.is_symlink():
                current = candidate
                continue
            link = os.readlink(host_path)
            remainder = components[index + 1 :]
            if link.startswith("/"):
                redirected = PurePosixPath(link)
            else:
                redirected = PurePosixPath(
                    os.path.normpath(str(candidate.parent / link))
                )
            pending = redirected.joinpath(*remainder)
            restarted = True
            break
        if restarted:
            continue
        for root in (payload_root, target_root):
            host_path = root / str(current).lstrip("/")
            if host_path.is_file():
                return host_path, current
        return None
    raise ClosureError(f"ELF symlink depth exceeded: {relative}")


def default_directories(machine: str, elf_class: str) -> tuple[PurePosixPath, ...]:
    if "X86-64" in machine:
        triples = ("x86_64-linux-gnu",)
    elif "AArch64" in machine:
        triples = ("aarch64-linux-gnu",)
    elif "80386" in machine:
        triples = ("i386-linux-gnu",)
    else:
        triples = ()
    directories = [
        PurePosixPath(f"/{prefix}/{triple}")
        for triple in triples
        for prefix in ("lib", "usr/lib")
    ]
    if elf_class == "ELF64":
        directories.extend((PurePosixPath("/lib64"), PurePosixPath("/usr/lib64")))
    directories.extend((PurePosixPath("/lib"), PurePosixPath("/usr/lib")))
    return tuple(directories)


def expand_search_path(value: str, requester: PurePosixPath) -> PurePosixPath:
    expanded = ORIGIN.sub(str(requester.parent), value)
    if not expanded.startswith("/"):
        raise ClosureError(f"relative ELF search path in {requester}: {value}")
    normalised = PurePosixPath(os.path.normpath(expanded))
    if ".." in normalised.parts:
        raise ClosureError(f"escaping ELF search path in {requester}: {value}")
    return normalised


def search_directories(
    info: ElfInfo, requester: PurePosixPath
) -> tuple[PurePosixPath, ...]:
    tagged = info.runpath if info.runpath else info.rpath
    expanded = tuple(expand_search_path(value, requester) for value in tagged)
    return (*expanded, *default_directories(info.machine, info.elf_class))


def compatible(
    requester: ElfInfo, needed: str, provider_path: Path
) -> tuple[bool, str]:
    provider = elf_info(provider_path)
    if (provider.machine, provider.elf_class) != (
        requester.machine,
        requester.elf_class,
    ):
        return False, f"architecture {provider.machine} {provider.elf_class}"
    required = requester.required_versions.get(needed, frozenset())
    missing = sorted(required - provider.provided_versions)
    if missing:
        return False, f"missing symbol versions {', '.join(missing)}"
    return True, ""


def find_dependency(
    payload_root: Path,
    target_root: Path,
    requester_path: PurePosixPath,
    requester: ElfInfo,
    needed: str,
) -> tuple[Path, PurePosixPath] | None:
    if "/" in needed:
        candidate = PurePosixPath(needed)
        if not candidate.is_absolute():
            candidate = requester_path.parent / candidate
        candidates = (candidate,)
    else:
        candidates = tuple(
            directory / needed
            for directory in search_directories(requester, requester_path)
        )
    for candidate in candidates:
        resolved = resolve_union(payload_root, target_root, candidate)
        if resolved is None:
            continue
        provider, _ = resolved
        if not is_elf(provider):
            raise ClosureError(
                f"incompatible {needed} for {requester_path}: {candidate}: not ELF"
            )
        matches, reason = compatible(requester, needed, provider)
        if matches:
            return provider, candidate
        raise ClosureError(
            f"incompatible {needed} for {requester_path}: {candidate}: {reason}"
        )
    return None


def runtime_elfs(root: Path) -> list[tuple[Path, PurePosixPath]]:
    result: list[tuple[Path, PurePosixPath]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.is_symlink() or not is_elf(path):
            continue
        relative = PurePosixPath("/") / path.relative_to(root).as_posix()
        if relative == SYSTEMPLATE or SYSTEMPLATE in relative.parents:
            continue
        result.append((path, relative))
    return result


def check_interpreter(
    payload_root: Path, target_root: Path, requester: PurePosixPath, info: ElfInfo
) -> None:
    if not info.interpreter:
        return
    interpreter = PurePosixPath(info.interpreter)
    if not interpreter.is_absolute():
        raise ClosureError(f"non-absolute PT_INTERP in {requester}: {interpreter}")
    resolved = resolve_union(payload_root, target_root, interpreter)
    if resolved is None:
        raise ClosureError(f"missing PT_INTERP {interpreter} for {requester}")
    provider = elf_info(resolved[0])
    if (provider.machine, provider.elf_class) != (info.machine, info.elf_class):
        raise ClosureError(f"incompatible PT_INTERP {interpreter} for {requester}")


def registry_records(
    root: Path, registry: Path
) -> list[tuple[Path, PurePosixPath, ElfInfo, str]]:
    if not registry.is_file():
        raise ClosureError(f"dynamic dependency registry is absent: {registry}")
    records: list[tuple[Path, PurePosixPath, ElfInfo, str]] = []
    for number, raw in enumerate(registry.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        requester_path = PurePosixPath(fields[0]) if fields else PurePosixPath(".")
        if (
            len(fields) != 2
            or not requester_path.is_absolute()
            or ".." in requester_path.parts
            or not fields[1]
            or fields[1] in {".", ".."}
            or "/" in fields[1]
        ):
            raise ClosureError(f"invalid registry record at line {number}: {raw}")
        requester = root / str(requester_path).lstrip("/")
        if not requester.is_file() or not is_elf(requester):
            raise ClosureError(f"registered ELF is absent: {fields[0]}")
        records.append((requester, requester_path, elf_info(requester), fields[1]))
    return records


def verify_registry(root: Path, target: Path, registry: Path) -> None:
    for _, requester_path, requester_info, library in registry_records(root, registry):
        if (
            find_dependency(
                root,
                target,
                requester_path,
                requester_info,
                library,
            )
            is None
        ):
            raise ClosureError(
                f"missing registered loader requirement {library} for {requester_path}"
            )


def verify(root: Path, target: Path, registry: Path) -> None:
    failures: list[str] = []
    for host_path, requester_path in runtime_elfs(root):
        try:
            info = elf_info(host_path)
            check_interpreter(root, target, requester_path, info)
            for needed in info.needed:
                if find_dependency(root, target, requester_path, info, needed) is None:
                    raise ClosureError(f"missing {needed} for {requester_path}")
        except ClosureError as error:
            failures.append(str(error))
    try:
        verify_registry(root, target, registry)
    except ClosureError as error:
        failures.append(str(error))
    if failures:
        raise ClosureError("\n".join(failures))


def assemble(root: Path, target: Path, source: Path, registry: Path) -> None:
    for _ in range(64):
        copied = False
        for host_path, requester_path in runtime_elfs(root):
            info = elf_info(host_path)
            check_interpreter(root, target, requester_path, info)
            for needed in info.needed:
                try:
                    present = find_dependency(
                        root, target, requester_path, info, needed
                    )
                except ClosureError:
                    present = None
                if present is not None:
                    continue
                if TARGET_GLIBC.match(needed):
                    raise ClosureError(
                        f"target base does not provide a compatible {needed} "
                        f"for {requester_path}"
                    )
                source_dependency = find_dependency(
                    source, source, requester_path, info, needed
                )
                if source_dependency is None:
                    raise ClosureError(f"cannot source {needed} for {requester_path}")
                source_path, lexical_path = source_dependency
                destination = root / str(lexical_path).lstrip("/")
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source_path, destination)
                destination.chmod(stat.S_IMODE(source_path.stat().st_mode))
                is_elf.cache_clear()
                elf_info.cache_clear()
                print(f"added {lexical_path} for {requester_path}")
                copied = True
        for _, requester_path, requester_info, library in registry_records(
            root, registry
        ):
            try:
                present = find_dependency(
                    root,
                    target,
                    requester_path,
                    requester_info,
                    library,
                )
            except ClosureError:
                present = None
            if present is not None:
                continue
            if TARGET_GLIBC.match(library):
                raise ClosureError(
                    f"target base does not provide a compatible registered "
                    f"{library} for {requester_path}"
                )
            source_dependency = find_dependency(
                source,
                source,
                requester_path,
                requester_info,
                library,
            )
            if source_dependency is None:
                raise ClosureError(
                    f"cannot source registered loader requirement {library} "
                    f"for {requester_path}"
                )
            source_path, lexical_path = source_dependency
            destination = root / str(lexical_path).lstrip("/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_path, destination)
            destination.chmod(stat.S_IMODE(source_path.stat().st_mode))
            is_elf.cache_clear()
            elf_info.cache_clear()
            print(f"added registered {lexical_path} for {requester_path}")
            copied = True
        if not copied:
            verify(root, target, registry)
            return
    raise ClosureError("ELF closure did not converge")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("verify", "assemble"))
    parser.add_argument("root", type=Path)
    parser.add_argument("target", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("source", nargs="?", type=Path)
    arguments = parser.parse_args()
    try:
        if arguments.mode == "verify":
            verify(
                arguments.root.resolve(),
                arguments.target.resolve(),
                arguments.registry.resolve(),
            )
        else:
            if arguments.source is None:
                parser.error("assemble mode requires SOURCE")
            assemble(
                arguments.root.resolve(),
                arguments.target.resolve(),
                arguments.source.resolve(),
                arguments.registry.resolve(),
            )
    except ClosureError as error:
        raise SystemExit(f"verify-elf-closure: {error}") from error
    print("ELF loader reachability, architecture and symbol versions passed.")


if __name__ == "__main__":
    main()
