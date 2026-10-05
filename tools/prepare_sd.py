"""Builds the SD card layout (artifacts/sdmc) and the release archive, which holds the whole layout.

    python tools/prepare_sd.py --client private/PokeMMO-Client.zip   # PokeMMO-NX-<client revision>.zip: application, client, runtime libraries
    python tools/prepare_sd.py                                       # PokeMMO-<version>.zip: application and runtime libraries only
    python tools/prepare_sd.py --client ... --forwarder <file.nsp>   # also puts the home menu launcher in the archive

The official client comes from tools/fetch_client.py (private/PokeMMO-Client.zip); only its Linux ARM64 part is kept. The C++ runtime
libraries come from tools/fetch_runtime_libs.py. The forwarder is an NSP made with an online NSP forwarder generator (NRO path
/switch/PokeMMO/PokeMMO.nro); it is signed with the generating person's keys, so it is not part of the repository.
"""
import argparse
import hashlib
import re
import shutil
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOLDER = "switch/PokeMMO"

def version():
    makefile = (ROOT / "native/Makefile").read_text(encoding="utf-8")
    return re.search(r"^APP_VERSION\s*:=\s*(\S+)", makefile, re.M).group(1)

NOTICE = """libstdc++.so.6 and libgcc_s.so.1: GCC 12.2.0 runtime libraries for arm64, taken unmodified from Debian 12 ("bookworm"),
packages libstdc++6 and libgcc-s1 12.2.0-14+deb12u1 (source package gcc-12: https://deb.debian.org/debian/pool/main/g/gcc-12/).
License: GNU General Public License version 3 with the GCC Runtime Library Exception
(https://www.gnu.org/licenses/gpl-3.0.txt, https://www.gnu.org/licenses/gcc-exception-3.1.html).
"""

def check_arm64_shared_object(data):
    """The client must be a little-endian AArch64 ELF64 (ET_DYN: the native image is position independent)."""
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("The client is not a little-endian ELF64 file")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    if machine != 183 or elf_type != 3:
        raise ValueError("The client is not an AArch64 ET_DYN image")

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--client", type=Path, help="official client archive (private/PokeMMO-Client.zip)")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/sdmc")
    parser.add_argument("--nro", type=Path, default=ROOT / "native/PokeMMO.nro")
    parser.add_argument("--forwarder", type=Path, help="home menu launcher (NSP forwarder) to include as PokeMMO-forwarder.nsp")
    args = parser.parse_args()
    if not args.nro.is_file():
        parser.error("Build the NRO first with tools/build.ps1")
    if args.forwarder and not args.forwarder.is_file():
        parser.error(f"Forwarder not found: {args.forwarder}")
    number = version()
    root = args.output / FOLDER
    root.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.nro, root / "PokeMMO.nro")
    for name in ("LICENSE", "THIRD-PARTY-NOTICES.md"):
        shutil.copyfile(ROOT / name, root / name)
    if args.forwarder:
        shutil.copyfile(args.forwarder, root / "PokeMMO-forwarder.nsp")

    if args.client:
        # The client install folder, exactly as the game expects it, inside the adapters' virtual root (/game).
        game = root / "isolate-root/game"
        with zipfile.ZipFile(args.client) as archive:
            binary = archive.read("bin/linux/arm64/PokeMMO")
            check_arm64_shared_object(binary)
            for entry in archive.infolist():
                name = entry.filename
                if name.endswith(".exe") or name.endswith(".sh") or (name.startswith("bin/") and not name.startswith("bin/linux/arm64/")):
                    continue
                target = game / name
                if entry.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.read(entry))
            print("Client ARM64 sha256:", hashlib.sha256(binary).hexdigest())
        (game / "roms").mkdir(exist_ok=True)  # where the player puts the ROMs
    # The C++ runtime the client's native libraries need (fetched from Debian by tools/fetch_runtime_libs.py).
    libraries = sorted((ROOT / "private/runtime-libs").glob("*.so*"))
    if libraries:
        lib = root / "isolate-root/lib"
        lib.mkdir(parents=True, exist_ok=True)
        for library in libraries:
            shutil.copyfile(library, lib / library.name)
            print("Runtime library:", library.name, hashlib.sha256(library.read_bytes()).hexdigest())
        (lib / "NOTICE.txt").write_bytes(NOTICE.encode("utf-8"))  # LF line endings on every platform
    else:
        print("Warning: no runtime libraries in private/runtime-libs (python tools/fetch_runtime_libs.py): the release archive will not have them.")

    # The release archive: the whole layout, named after the client revision when it holds the client.
    if args.client:
        name = f"PokeMMO-NX-{(root / 'isolate-root/game/revision.txt').read_text(encoding='utf-8').strip()}.zip"
    else:
        name = f"PokeMMO-{number}.zip"
    release = args.output.parent / name
    with zipfile.ZipFile(release, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(root.rglob("*")):
            archive.write(path, path.relative_to(args.output).as_posix())
    print("Release archive:", release, release.stat().st_size, "bytes")
    print("SD card layout:", args.output.resolve(), "- copy its contents to the SD card root.")

if __name__ == "__main__":
    main()
