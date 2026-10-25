"""Edit RAM in an inspeccy dump without reformatting unchanged banks."""
import hashlib
import json
from pathlib import Path


def digest(path):
    """The SHA-1 a build manifest records for a file."""
    return hashlib.sha1(Path(path).read_bytes()).hexdigest()


class Snapshot:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.data = json.loads(self.path.read_text(encoding='utf-8'))
        if self.data.get('dump_version') != 1:
            raise ValueError('unsupported dump version')
        self.banks = {}
        for key, text in self.data['ram'].items():
            if key.startswith('bank_'):
                bank = int(key[5:])
                data = bytearray(16384)
                if text != f'{bank}:zeros':
                    for line in text.splitlines():
                        header, values = line.split(None, 1)
                        offset = int(header.split(':')[1], 16)
                        chunk = bytes.fromhex(values)
                        if offset < 0 or offset + len(chunk) > len(data):
                            raise ValueError(f'bank {bank}: invalid hexdump range')
                        data[offset:offset+len(chunk)] = chunk
                self.banks[bank] = data
        self._saved_banks = {bank: bytes(data) for bank, data in self.banks.items()}
        # Resolve once against the *input* directory. Saving into another
        # directory, or saving twice, must not reinterpret a relative path.
        refs = list(self.data.get('external_refs', {}).values())
        refs += [self.data.get('nex'), self.data.get('dotcmd')]
        for ref in refs:
            if isinstance(ref, dict) and ref.get('path'):
                ref['path'] = self._absolute(ref['path'])
        for key in ('psg', 'psg2'):
            ref = self.data.get(key)
            if isinstance(ref, dict) and ref.get('out_path'):
                ref['out_path'] = self._absolute(ref['out_path'])

    def _absolute(self, path):
        return str((self.path.parent / path).resolve())

    def validate_build(self, manifest, *, allow_mismatch=None):
        """Verify original image identity and current symbols, never RAM hashes."""
        manifest = Path(manifest).resolve()
        build = json.loads(manifest.read_text(encoding='utf-8'))
        if build.get('version') != 1:
            raise ValueError('unsupported build manifest version')
        image = build['image']
        expected = image['sha1']
        if digest(manifest.parent / image['path']) != expected:
            raise ValueError('program differs from build manifest; rebuild the manifest after successful assembly')
        actual = (self.data.get('nex') or {}).get('sha1')
        if actual != expected and not allow_mismatch:
            raise ValueError('snapshot build is unknown or differs from manifest; create a fresh fixture')
        for symbol in build['symbols']:
            if digest(manifest.parent / symbol['path']) != symbol['sha1']:
                raise ValueError('symbol file differs from build manifest')
        if allow_mismatch:
            self.data['nex']['identity_override'] = allow_mismatch
        return build

    def save(self, path):
        if not self._saved_banks.keys() <= self.banks.keys():
            raise ValueError('removing RAM banks is not supported; clear their bytes instead')
        for bank, data in self.banks.items():
            if not 0 <= bank < 128 or len(data) != 16384:
                raise ValueError(f'invalid 16K RAM bank: {bank}')
            if data != self._saved_banks.get(bank):
                self.data['ram'][f'bank_{bank}'] = '\n'.join(
                    f'{bank}:{i:04X}   ' + data[i:i+32].hex(' ').upper()
                    for i in range(0, len(data), 32)) + '\n'
                self._saved_banks[bank] = bytes(data)
        path = Path(path)
        path.write_text(json.dumps(self.data))
        return path
