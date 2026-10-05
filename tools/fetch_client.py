"""Fetch the public desktop distribution without executing the client."""
import argparse
import hashlib
import json
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

URL = "https://dl.pokemmo.com/download/PokeMMO-Client.zip"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=URL)
    parser.add_argument("--output", type=Path, default=Path("private/PokeMMO-Client.zip"))
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".part")
    digest = hashlib.sha256()
    request = urllib.request.Request(args.url, headers={"User-Agent": "PokeMMO-NX/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response, temporary.open("wb") as out:
            resolved_url = response.url
            while block := response.read(1024 * 1024):
                out.write(block)
                digest.update(block)
        with zipfile.ZipFile(temporary) as archive:
            if not any(info.filename.endswith("PokeMMO.exe") for info in archive.infolist()):
                raise ValueError("Archive does not contain PokeMMO.exe")
        temporary.replace(args.output)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        temporary.unlink(missing_ok=True)
        parser.exit(1, f"Download failed: {error}\n")
    provenance = {"requested_url": args.url, "resolved_url": resolved_url,
                  "sha256": digest.hexdigest(), "bytes": args.output.stat().st_size}
    args.output.with_suffix(".provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(provenance, indent=2))

if __name__ == "__main__":
    main()
