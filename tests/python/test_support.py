"""Shared test executable and optional ROM discovery."""
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]


def binary_candidates():
    override = os.environ.get('INSPECCY')
    if override:
        return (Path(override).resolve(),)
    names = ('inspeccy.exe', 'inspeccy')
    folders = ('build/inspeccy/Release', 'build/inspeccy', 'build/Release', 'build', 'bin')
    found = tuple(ROOT / folder / name for folder in folders for name in names)
    on_path = shutil.which('inspeccy')
    return found + ((Path(on_path),) if on_path else ())


def locate_test_binary():
    for candidate in binary_candidates():
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError('Build inspeccy or set INSPECCY to its executable path')


def test_rom(name):
    """No ROMs are bundled; callers decide whether their test can run without one."""
    directory = os.environ.get('INSPECCY_ROM_DIR')
    return (Path(directory).resolve() if directory else ROOT / 'roms') / name
