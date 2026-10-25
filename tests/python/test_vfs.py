#!/usr/bin/env python3
"""Exercise VFS resolution through internal and escaping directory links."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def link_directory(target: Path, link: Path) -> None:
    if os.name == "nt":
        # Junctions do not require Windows developer mode or administrator rights.
        quote = lambda path: "'" + str(path).replace("'", "''") + "'"
        subprocess.run([
            "powershell", "-NoProfile", "-NonInteractive", "-Command",
            "New-Item -ItemType Junction -Path " + quote(link) + " -Target " + quote(target) + " | Out-Null",
        ], check=True, capture_output=True)
    else:
        link.symlink_to(target, target_is_directory=True)


def main() -> int:
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="inspeccy_vfs_") as directory:
        base = Path(directory)
        root = base / "mount"
        outside = base / "mount-sibling"
        root.mkdir()
        outside.mkdir()
        (root / "inside.txt").write_bytes(b"inside")
        sentinel = outside / "sentinel.txt"
        sentinel.write_bytes(b"must remain intact")
        links = [root / "escape", root / "allowed"]
        try:
            link_directory(outside, links[0])
            link_directory(root, links[1])
            result = subprocess.run([executable, str(root)], capture_output=True, text=True)
            if result.returncode:
                print(f"VFS test exited {result.returncode}: {result.stderr}", file=sys.stderr)
                return 1
            if sentinel.read_bytes() != b"must remain intact" or sorted(p.name for p in outside.iterdir()) != ["sentinel.txt"]:
                raise AssertionError("VFS changed files outside the mount")
        finally:
            # Remove only the links before TemporaryDirectory removes the fixtures.
            for link in links:
                if link.exists():
                    if os.name == "nt":
                        os.rmdir(link)
                    else:
                        link.unlink()
    return 0


if __name__ == "__main__":
    sys.exit(main())
