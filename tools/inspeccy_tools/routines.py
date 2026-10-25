"""Routine-level tests: call one routine by name inside a stopped emulator and
read the result back by name.

A snapshot plus the program's SLD gives symbols; a build manifest binds the
snapshot to the image those symbols came from. `invoke` sets registers, maps a
page into slot 7 if asked, calls the routine and parks, returning the dump.
Nothing here knows any particular program: the page map for unbanked
addresses, the stack pointer and the parking address are the caller's.

    d = SymbolSnapshot(state, sld=..., manifest=..., slot_pages=[...])
    d.set('Particles', bytes(2048))
    e = invoke(emu, d, folder, 'UpdateParticle', registers={'AF': 100 << 8, 'HL': 50, 'BC': (1 << 8) | 192},
               slot7_page=d.symbols['PARTICLE_PAGE'][1], snapshot_class=type(d))
    assert e.read('Particles', 8)[2] == 100

"""
from pathlib import Path

from .batch import run_batch
from .snapshot import Snapshot


def read_sld_symbols(sld):
    """{name: (page, value)} for every label and EQU the SLD records."""
    symbols = {}
    for line in Path(sld).read_text().splitlines():
        fields = line.split('|')
        if len(fields) == 8 and fields[6] in ('F', 'D'):
            symbols[fields[7]] = (int(fields[4]), int(fields[5]))
    return symbols


class SymbolSnapshot(Snapshot):
    """A dump that knows the program's symbols.

    slot_pages: the 8K page the assembler had in each of the eight slots, for
    symbols the SLD marks unbanked (page -1); 255 for ROM.
    """

    def __init__(self, path, *, sld, manifest, slot_pages):
        super().__init__(path)
        self.manifest = Path(manifest)
        self.validate_build(self.manifest)
        self.symbols = read_sld_symbols(sld)
        self.slot_pages = list(slot_pages)

    def loc(self, name, offset=0):
        page, address = self.symbols[name]
        if page < 0:
            page = self.slot_pages[address // 8192]
        return page // 2, (page % 2) * 8192 + (address % 8192) + offset

    def read(self, name, count=1, offset=0):
        bank, address = self.loc(name, offset)
        return bytes(self.banks[bank][address:address + count])

    def val(self, name, count=1, offset=0):
        return int.from_bytes(self.read(name, count, offset), 'little')

    def set(self, name, value, count=1, offset=0):
        bank, address = self.loc(name, offset)
        value = value.to_bytes(count, 'little') if isinstance(value, int) else value
        self.banks[bank][address:address + len(value)] = value


def call_steps(routine, *, registers=None, times=1, slot7_page=None, sp=0xbe00,
               return_pc=None, cycles=5_000_000):
    """The batch steps that call `routine` `times` times from a quiet CPU."""
    steps = [dict(op='set', registers={'SP': sp, 'IFF1': 0, 'IFF2': 0,
                                       'EI_PENDING': 0, 'HALTED': 0, **(registers or {})})]
    if slot7_page is not None:
        steps.append(dict(op='nextreg', index=0x57, value=slot7_page))
    if return_pc is None:
        return_pc = 0x5e00 + 8 + 3 * times
    steps.append(dict(op='repeat', count=times,
                      steps=[dict(op='call', symbol=routine, return_pc=return_pc, cycles=cycles)]))
    return steps


def batch(executable, snapshot, folder, steps, *, extra=(), snapshot_class=None, **kw):
    """Run a plan of steps from `snapshot` in one emulator process; return the
    resulting dump as `snapshot_class` (default: the snapshot's own class,
    constructed with the same sld/manifest/slot_pages)."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    seed = snapshot.save(folder / 'seed.json')
    _, state = run_batch(executable, folder, steps, resume=seed, dump=True,
                         extra=['--vfs', str(folder), '--build-manifest', str(snapshot.manifest), *extra])
    cls = snapshot_class or type(snapshot)
    if cls is SymbolSnapshot:
        return cls(state, sld=kw.get('sld'), manifest=snapshot.manifest, slot_pages=snapshot.slot_pages)
    return cls(state, **kw)


def invoke(executable, snapshot, folder, routine, *, registers=None, times=1, slot7_page=None,
           sp=0xbe00, return_pc=None, cycles=5_000_000, extra=(), snapshot_class=None, **kw):
    """Call one routine by name and return the dump afterwards."""
    steps = call_steps(routine, registers=registers, times=times, slot7_page=slot7_page,
                       sp=sp, return_pc=return_pc, cycles=cycles)
    return batch(executable, snapshot, folder, steps, extra=extra, snapshot_class=snapshot_class, **kw)
