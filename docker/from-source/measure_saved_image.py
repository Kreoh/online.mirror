"""Measure deterministic compressed bytes in a Docker save archive."""

from __future__ import annotations

import argparse
import gzip
import io
import json
import tarfile
from pathlib import Path


class Counter(io.RawIOBase):
    def __init__(self) -> None:
        self.count = 0

    def writable(self) -> bool:
        return True

    def write(self, data: bytes | bytearray) -> int:
        self.count += len(data)
        return len(data)


def measure(archive_path: Path) -> tuple[int, int]:
    with tarfile.open(archive_path, "r") as archive:
        manifest_file = archive.extractfile("manifest.json")
        if manifest_file is None:
            raise RuntimeError("docker-save manifest is absent")
        manifest = json.load(manifest_file)
        if not isinstance(manifest, list) or len(manifest) != 1:
            raise RuntimeError("expected one saved image manifest")
        record = manifest[0]
        layers = record.get("Layers")
        config_name = record.get("Config")
        if (
            not isinstance(layers, list)
            or not layers
            or not all(isinstance(layer, str) for layer in layers)
            or not isinstance(config_name, str)
        ):
            raise RuntimeError("malformed saved image manifest")
        config_bytes = archive.getmember(config_name).size
        compressed_bytes = config_bytes
        uncompressed_bytes = config_bytes
        for layer_name in layers:
            layer_member = archive.getmember(layer_name)
            source = archive.extractfile(layer_name)
            if source is None:
                raise RuntimeError(f"saved layer is absent: {layer_name}")
            header = source.read(2)
            source.seek(0)
            if header == b"\x1f\x8b":
                compressed_bytes += layer_member.size
                with gzip.GzipFile(fileobj=source, mode="rb") as uncompressed:
                    while block := uncompressed.read(1024 * 1024):
                        uncompressed_bytes += len(block)
            else:
                uncompressed_bytes += layer_member.size
                counter = Counter()
                with gzip.GzipFile(
                    fileobj=counter,
                    mode="wb",
                    compresslevel=9,
                    mtime=0,
                    filename="",
                ) as compressed:
                    while block := source.read(1024 * 1024):
                        compressed.write(block)
                compressed_bytes += counter.count
    return uncompressed_bytes, compressed_bytes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--uncompressed",
        action="store_true",
        help="report uncompressed saved-layer and config bytes",
    )
    parser.add_argument("archive", type=Path)
    arguments = parser.parse_args()
    try:
        uncompressed_bytes, compressed_bytes = measure(arguments.archive)
        print(uncompressed_bytes if arguments.uncompressed else compressed_bytes)
    except (OSError, tarfile.TarError, json.JSONDecodeError, RuntimeError) as error:
        raise SystemExit(f"measure-saved-image: {error}") from error


if __name__ == "__main__":
    main()
