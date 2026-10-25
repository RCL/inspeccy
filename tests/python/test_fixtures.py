"""Check the public binary inventory and the portable fixture generators."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

TESTS = Path(__file__).resolve().parents[1]


def main():
    for generator in ("data/sna/src/generate.py", "data/dotcmd/src/generate.py",
                      "data/nex/src/generate_loading_screen.py"):
        subprocess.run([sys.executable, str(TESTS / generator), "--check"], check=True)
    manifest = json.loads((TESTS / "data/manifest.json").read_text(encoding="utf-8"))
    listed = set()
    for entry in manifest["files"]:
        relative = entry["path"]
        path = TESTS / relative
        assert path.resolve().is_relative_to(TESTS), relative
        assert relative not in listed, relative
        listed.add(relative)
        data = path.read_bytes()
        assert len(data) == entry["bytes"], relative
        assert hashlib.sha256(data).hexdigest() == entry["sha256"], relative
        assert entry["provenance"], relative
    # New binary fixtures require an inventory entry and a provenance decision.
    for folder in (TESTS / "data", TESTS / "refs"):
        for path in folder.rglob("*"):
            if not path.is_file() or "__pycache__" in path.parts:
                continue
            try:
                data = path.read_bytes()
                data.decode("utf-8")
                binary = b"\0" in data
            except UnicodeDecodeError:
                binary = True
            if binary:
                assert path.relative_to(TESTS).as_posix() in listed, path
    print(f"Verified {len(listed)} inventoried binary fixtures")


if __name__ == "__main__":
    main()
