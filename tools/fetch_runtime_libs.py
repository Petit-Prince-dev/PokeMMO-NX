"""Fetch the C++ runtime (libstdc++.so.6, libgcc_s.so.1) the client's native libraries need, from Debian 12 (aarch64, glibc).

The official client archive does not contain them (a normal Linux system provides them). They are downloaded from the
Debian archive, extracted to private/runtime-libs and never redistributed with the project. The SHA-256 of each .deb is
recorded in tools/runtime-libs.json on first download and verified afterwards.
"""
import hashlib
import io
import json
import lzma
import struct
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = "https://deb.debian.org/debian/pool/main/g/gcc-12/"
PACKAGES = {
    "libstdc++6_12.2.0-14+deb12u1_arm64.deb": ("usr/lib/aarch64-linux-gnu/libstdc++.so.6.0.30", "libstdc++.so.6"),
    "libgcc-s1_12.2.0-14+deb12u1_arm64.deb": ("lib/aarch64-linux-gnu/libgcc_s.so.1", "libgcc_s.so.1"),
}
RECORD = ROOT / "tools/runtime-libs.json"

def ar_members(data):
    assert data[:8] == b"!<arch>\n"
    position = 8
    while position < len(data):
        name, _, _, _, _, size, end = struct.unpack("16s12s6s6s8s10s2s", data[position:position + 60])
        size = int(size)
        yield name.decode().strip().rstrip("/"), data[position + 60:position + 60 + size]
        position += 60 + size + (size & 1)

def extract(deb, member):
    for name, payload in ar_members(deb):
        if name.startswith("data.tar"):
            raw = lzma.decompress(payload) if name.endswith(".xz") else payload
            with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
                for entry in archive:
                    if entry.name.lstrip("./") == member:
                        return archive.extractfile(entry).read()
    raise SystemExit("member %s not found" % member)

def main():
    out = ROOT / "private/runtime-libs"
    out.mkdir(parents=True, exist_ok=True)
    record = json.loads(RECORD.read_text(encoding="utf-8")) if RECORD.exists() else {}
    for package, (member, name) in PACKAGES.items():
        with urllib.request.urlopen(BASE + package, timeout=120) as response:
            deb = response.read()
        digest = hashlib.sha256(deb).hexdigest()
        if package in record:
            assert record[package]["sha256"] == digest, "unexpected content for " + package
        library = extract(deb, member)
        (out / name).write_bytes(library)
        record[package] = dict(sha256=digest, bytes=len(deb), url=BASE + package, extracted=name, extracted_sha256=hashlib.sha256(library).hexdigest(), extracted_bytes=len(library))
        print(name, len(library), "bytes from", package)
    RECORD.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__":
    main()
