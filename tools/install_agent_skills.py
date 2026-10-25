"""Install the shared Inspeccy Skills into Codex and/or Claude Code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

RECORD = '.inspeccy-skills-install.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def install(package, destinations, update=False):
    """Preflight every destination before copying; never delete or follow symlinks."""
    source = package.resolve() / 'skills'
    skills = sorted(p for p in source.iterdir() if p.is_dir() and (p/'SKILL.md').is_file())
    if not skills:
        raise ValueError(f'no skills in {source}')
    jobs = []
    for destination in destinations:
        destination = destination.expanduser().resolve()
        for skill in skills:
            target = destination/skill.name
            if target.is_symlink():
                raise ValueError(f'refusing symlinked skill destination: {target}')
            files = {}
            for path in sorted(skill.rglob('*')):
                if path.is_symlink():
                    raise ValueError(f'source skill contains a symlink: {path}')
                if path.is_file():
                    files[path.relative_to(skill).as_posix()] = path.read_bytes()
            previous = {}
            record = target/RECORD
            if record.exists():
                data = json.loads(record.read_text(encoding='utf-8'))
                if data.get('package') != 'inspeccy-skills' or data.get('version') != 1:
                    raise ValueError(f'unrecognised installation record: {record}')
                previous = data['files']
            for relative, content in files.items():
                path = target/relative
                # Check every path component: nested links/junctions cannot
                # redirect an update outside this skill directory.
                if not path.resolve().is_relative_to(target.resolve()):
                    raise ValueError(f'destination escapes skill directory: {path}')
                if path.is_symlink() or any(p.is_symlink() for p in path.parents if p != destination):
                    raise ValueError(f'refusing a symlink in destination: {path}')
                if path.exists() and path.read_bytes() != content:
                    if not update or relative not in previous:
                        raise ValueError(f'existing different file preserved: {path}; use --update for an owned installation')
                    if digest(path.read_bytes()) != previous[relative]:
                        raise ValueError(f'locally edited skill preserved: {path}')
            if record.is_symlink() or not record.resolve().is_relative_to(target.resolve()):
                raise ValueError(f'invalid installation record path: {record}')
            jobs.append((target, files))
    for target, files in jobs:
        for relative, content in files.items():
            path = target/relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_bytes() != content:
                path.write_bytes(content)
        (target/RECORD).write_text(json.dumps(dict(version=1, package='inspeccy-skills',
            files={name: digest(content) for name, content in files.items()}), indent=2)+'\n', encoding='utf-8')
    return [str(target) for target, _ in jobs]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', choices=('codex', 'claude', 'both'), default='both')
    parser.add_argument('--destination', type=Path, help='explicit skill directory, for one client only')
    parser.add_argument('--package', type=Path, default=Path(__file__).resolve().parents[1]/'plugins/inspeccy-skills')
    parser.add_argument('--update', action='store_true')
    args = parser.parse_args()
    if args.destination and args.client == 'both':
        parser.error('--destination requires one --client')
    home = Path.home()
    roots = dict(codex=home/'.agents/skills', claude=home/'.claude/skills')
    destinations = [args.destination] if args.destination else [roots[c] for c in ('codex', 'claude') if args.client in (c, 'both')]
    try:
        paths = install(args.package, destinations, args.update)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps(dict(installed=paths, next_step='Start a new client session to discover the skills.'), indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
