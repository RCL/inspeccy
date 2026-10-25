"""Public inspection/device/IRQ interfaces using self-contained guest programs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    exe = str(Path(sys.argv[1]).resolve())
    caps = json.loads(subprocess.check_output([exe, '--capabilities'], text=True))
    assert caps['inspection_version'] == 1 and 'expect_device' in caps['batch_operations']
    with tempfile.TemporaryDirectory(prefix='inspeccy-inspection-') as temp:
        root = Path(temp)
        header = bytearray(512)
        header[:8] = b'NextV1.2'; header[9] = 1
        header[12:16] = bytes.fromhex('00 BF 00 80'); header[20] = 1
        bank = bytearray(16384); bank[:3] = bytes.fromhex('C3 00 80')
        nex = root/'test.nex'; nex.write_bytes(header + bank)
        sld = root/'test.sld'
        sld.write_text('x|1||0|-1|-1|Z|pages.size:8192\n'
                       'x|1||0|4|32768|F|Loop\n'
                       'x|1||0|4|32768|F|Alias\n'
                       'x|1||0|4|36864|F|Guard\n'
                       'x|1||0|-1|36864|D|GuardEqu\n')
        build = root/'build.json'
        subprocess.check_call([exe, '--write-manifest', str(build),
                               '--image', str(nex), '--sld', str(sld)],
                              stdout=subprocess.DEVNULL)

        def go(name, steps, code=0, extra=(), irq=None, machine='next60', bound=True):
            folder = root/name; folder.mkdir()
            plan = folder/'plan.json'; plan.write_text(json.dumps(dict(version=1, steps=steps)))
            command = [exe, '--machine', machine, '--nex', str(nex), '--batch', str(plan),
                       '--batch-out', str(folder/'result.json'), '--dump-out', str(folder/'state.json'),
                       '--status-out', str(folder/'status.json'), '--inspect-out', str(folder/'inspect.json'),
                       '--coverage-out', str(folder/'coverage.bin'), '--no-bmp']
            command += ['--build-manifest', str(build)] if bound else ['--sld', str(sld)]
            if irq is not None:
                config = folder/'irq.json'; config.write_text(json.dumps(irq))
                command += ['--irq-diagnostics', str(config), '--irq-diagnostics-out', str(folder/'irq-result.json')]
            result = subprocess.run(command + list(extra), capture_output=True, text=True)
            if result.returncode != code and (folder/'result.json').exists():
                print(json.loads((folder/'result.json').read_text()).get('failure', {}).get('device_mismatch'))
            assert result.returncode == code, (name, result.returncode, result.stdout, result.stderr)
            def read(file):
                p = folder/file
                return json.loads(p.read_text()) if p.exists() else None
            return dict(report=read('result.json'), dump=read('state.json'), state=read('inspect.json'),
                        status=read('status.json'), irq=read('irq-result.json'), folder=folder)

        def setregs(**r): return dict(op='set', registers=r)
        def nr(i, v): return dict(op='nextreg', index=i, value=v)
        def write(page, offset, data): return dict(op='write', page=page, offset=offset, hex=data)
        def state(r): return r['state']['state']
        def obs(r): return r['report']['observations']

        r = go('state', [setregs(PC=0xe000, SP=0xdfff), nr(0x57, 4), nr(0x56, 30),
                        write(30, 8191, '34'), dict(op='read_state', stack_words=1, pc_bytes=2)])
        s = obs(r)[0]['state']
        assert s['pc']['mapping']['page'] == 4 and s['pc']['ambiguous']
        assert {a['name'] for a in s['pc']['labels']} == {'Loop', 'Alias'}
        assert s['pc']['labels'][0]['assembled_address'] == 0x8000
        assert s['stack'][0]['bytes'] == [0x34, 0xc3]
        assert s['stack'][0]['location']['mapping']['page'] == 30
        assert s['stack'][0]['high_byte_mapping']['page'] == 4
        assert not s['instructions'], 'a truncated JP must not be disassembled'
        assert s['identity']['symbols'] == 'manifest_bound'
        r = go('unbound', [dict(op='read_state')], bound=False)
        assert state(r)['identity']['symbols'] == 'unverified'
        r = go('rom', [nr(0x50, 255), setregs(PC=0)])
        assert state(r)['pc']['mapping']['kind'] == 'rom' and not state(r)['pc']['labels']
        r = go('open_bus', [nr(0x57, 230), setregs(PC=0xe000)], extra=['--ram-mb', '1'])
        assert state(r)['pc']['mapping']['kind'] == 'open_bus' and state(r)['pc_bytes'][0] == 255
        r = go('high_page', [nr(0x57, 230), setregs(PC=0xe000)])
        assert state(r)['pc']['mapping']['page'] == 230
        r = go('wrap', [nr(0x50, 4), setregs(PC=0xffff, SP=0xffff), dict(op='read_state', stack_words=1, pc_bytes=4)])
        assert obs(r)[0]['state']['stack_wraps'] and obs(r)[0]['state']['pc_wraps']
        assert obs(r)[0]['state']['stack'][0]['high_byte_mapping']['page'] == 4

        # Hardware writes, not host device patches, exercise the real upload path.
        guest = bytearray()
        def nextreg(i, v): guest.extend([0xed, 0x91, i, v])
        def out(port, value): guest.extend([0x01, port & 255, port >> 8, 0x3e, value, 0xed, 0x79])
        reads = []
        for p in range(8):
            nextreg(0x43, p << 4); nextreg(0x40, 255)
            nextreg(0x44, 30 + p); nextreg(0x44, 1)
            layer = ['ula', 'layer2', 'sprites', 'tilemap'][p % 4]
            reads += [dict(op='expect_device', device='palette', layer=layer, bank=1+p//4, start=255, values=[61 + p*2]),
                      dict(op='expect_device', device='palette', layer=layer, bank=1+p//4, start=255, encoding='raw', hex=f'{30+p:02x}01')]
        nextreg(0x09, 0x10)
        out(0x303b, 127)
        for b in (15, 20, 0, 0xe0, 1): out(0x57, b)
        out(0x303b, 63)
        for b in range(256): out(0x5b, b)
        nextreg(0x08, 2)  # Next's separate TurboSound hardware gate
        for chip, selector in [(0, 0xff), (1, 0xfe)]:
            out(0xfffd, selector); out(0xfffd, 7); out(0xbffd, 40+chip)
        guest.append(0xc9)
        setup = [setregs(SP=0xbf00, IFF1=0, IFF2=0, HALTED=0, EI_PENDING=0),
                 write(4, 0, guest.hex()), dict(op='call', pc=0x8000, return_pc=0x8fff, cycles=100000)]
        reads += [dict(op='expect_device', device='sprite_attrs', start=127, hex='0F1400E001'),
                  dict(op='expect_device', device='sprite_pattern', slot=63, offset=254, hex='FEFF'),
                  dict(op='expect_device', device='ay', chip=0, start=7, values=[40]),
                  dict(op='expect_device', device='ay', chip=1, start=7, values=[41]),
                  dict(op='read_device', device='ay', chip=1, start=7, count=1),
                  dict(op='read_device', device='sprite_attrs', start=127, count=1),
                  dict(op='read_device', device='sprite_pattern', slot=63, offset=254, length=2),
                  dict(op='read_device', device='palette', layer='tilemap', bank=2, start=255, count=1),
                  dict(op='read_state')]
        plain = go('device_plain', setup, extra=['--ts'])
        checked = go('device_checked', setup + reads, extra=['--ts'])
        for section in ('cpu', 'ram', 'next', 'sprites', 'ay', 'scheduler', 'stats'):
            assert plain['dump'].get(section) == checked['dump'].get(section), section
        assert (plain['folder']/'coverage.bin').read_bytes() == (checked['folder']/'coverage.bin').read_bytes()
        assert obs(checked)[0]['device_state']['values'] == [41]
        assert obs(checked)[1]['device_state']['hex'] == '0F1400E001'
        assert obs(checked)[2]['device_state']['hex'] == 'FEFF'
        assert obs(checked)[3]['device_state']['values'] == [75]
        r = go('mismatch', setup + [dict(op='expect_device', device='ay', chip=1, start=7, values=[42])], 9, extra=['--ts'])
        f = r['report']['failure']; assert f['device_mismatch']['actual'] == 41 and 'state' in f
        r = go('disabled_ay', [dict(op='read_device', device='ay', chip=0, start=0, count=1)], extra=['--no-ay'])
        assert not obs(r)[0]['device_state']['enabled']

        bad = [dict(op='read_state', stack_words=65), dict(op='read_state', pc_bytes=65),
               dict(op='read_device', device='ay', chip=1, start=0, count=1),
               dict(op='read_device', device='ay', chip=0, start=15, count=2),
               dict(op='read_device', device='palette', layer='bad', bank=1, start=0, count=1),
               dict(op='read_device', device='palette', layer='ula', bank=0, start=0, count=1),
               dict(op='read_device', device='sprite_attrs', start=128, count=1),
               dict(op='read_device', device='sprite_pattern', slot=64, offset=0, length=1),
               dict(op='expect_device', device='sprite_attrs', start=0, hex='00'),
               dict(op='expect_device', device='ay', chip=0, start=0, values=[256]),
               dict(op='expect_device', device='palette', layer='ula', bank=1, start=0, values=[512]),
               dict(op='read_device', device='ay', chip=0, start=0, count=1, typo=0),
               dict(op='repeat', count=10000, steps=[dict(op='read_state')])]
        for i, step in enumerate(bad):
            r = go('bad'+str(i), [setregs(AF=1234), step], code=4)
            assert r['report']['completed_steps'] == 0 and state(r)['cpu']['AF'] != 1234
        # Output collision is checked before writing anything.
        before = nex.read_bytes()
        go('collision', [dict(op='read_state')], 4, extra=['--inspect-out', str(nex)])
        assert nex.read_bytes() == before

        loop = [setregs(IFF1=0, IFF2=0, PC=0x8000, HALTED=0, EI_PENDING=0)]
        timing = ['--frame-int', '1000', '--int-window', '32']
        cfg = dict(version=1, sources=['frame'], max_events=2)
        r = go('missed', loop + [dict(op='run', cycles=5010)], irq=cfg, extra=timing)
        summary = r['irq']['summary']
        assert summary['frame_pulse_expired'] == r['status']['interrupts']['missed'] >= 4
        assert summary['retained_events'] == 2 and summary['omitted_events'] >= 2
        e = r['irq']['events'][0]
        assert e['pc']['mapping']['page'] == 4 and e['pc']['labels'] and e['iff1'] == 0
        assert e['deadline_cycle'] == e['scheduled_cycle'] + 32 <= e['cycles'] and e['batch_step'] == 1
        r2 = go('split', loop + [dict(op='run', cycles=2500), dict(op='run', cycles=2510)], irq=cfg, extra=timing)
        assert r2['irq']['summary'] == summary
        r0 = go('no_diagnostics', loop + [dict(op='run', cycles=5010)], extra=timing)
        for section in ('cpu', 'ram', 'next', 'scheduler', 'stats'):
            assert r0['dump'].get(section) == r['dump'].get(section), section
        assert r['report']['irq_diagnostics'] == summary
        assert r['status']['interrupts']['diagnostics']['summary'] == summary

        guard = dict(name='busy', source='frame', symbol='Guard', type='u8', idle_value=0)
        cfg = dict(version=1, sources=['frame'], guards=[guard])
        accept = [nr(0x50, 0), write(0, 0x38, 'FBED4D'), write(4, 0x1000, '01'),
                  setregs(IFF1=1, IFF2=1, IM=1, SP=0xbf00, PC=0x8000, HALTED=0, EI_PENDING=0), dict(op='run', cycles=5010)]
        r = go('busy', accept, irq=cfg, extra=timing)
        assert r['irq']['summary']['guard_busy_at_accept'] == r['status']['interrupts']['accepted'] >= 4
        e = r['irq']['events'][0]
        assert e['pc']['address'] == 0x8000 and e['sp']['address'] == 0xbf00 and e['iff1'] == 1 and e['guard_value'] == 1
        idle = accept.copy(); idle[2] = write(4, 0x1000, '00')
        r = go('idle_guard', idle, irq=cfg, extra=timing)
        assert r['irq']['summary']['guard_busy_at_accept'] == 0
        masked = go('masked', loop + [nr(0x22, 4), dict(op='run', cycles=5010)], irq=cfg, extra=timing)
        assert masked['irq']['summary']['frame_pulse_expired'] == 0
        latched = go('latched', loop + [nr(0xc0, 1), dict(op='run', cycles=5010)], irq=cfg, extra=timing)
        assert latched['irq']['summary']['frame_pulse_expired'] == 0
        for name, guard_bad in [('equ', dict(guard, symbol='GuardEqu')),
                                ('cross', dict(name='x', source='frame', page=4, offset=8191, type='u16le', idle_value=0))]:
            r = go(name, loop, code=4, irq=dict(version=1, guards=[guard_bad]))
            assert r['report'] is None
        for name, bad_config in [('version', dict(version=2)), ('limit', dict(version=1, max_events=4097)),
                                 ('sources', dict(version=1, sources=['frame', 'frame']))]:
            go('bad_irq_'+name, loop, code=4, irq=bad_config)
        line_cfg = dict(version=1, sources=['line'])
        r = go('line_miss', loop + [nr(0x23, 10), nr(0x22, 2), dict(op='run', cycles=1000000)], irq=line_cfg)
        assert r['irq']['summary']['line_pulse_expired'] == r['status']['interrupts']['line_missed'] > 0
        periods = []
        for machine in ('next50', 'next60'):
            r = go(machine, loop + [dict(op='run', cycles=2000000)], irq=dict(version=1, sources=['frame']), machine=machine)
            periods.append(r['irq']['events'][1]['scheduled_cycle'] - r['irq']['events'][0]['scheduled_cycle'])
            assert r['irq']['events'][1]['emulated_ns'] > r['irq']['events'][0]['emulated_ns']
        assert periods[0] > periods[1]
        # NR07 rescaling must produce monotonic real timestamps over a turbo flip.
        r = go('turbo', loop + [dict(op='run', cycles=600000), nr(7, 0), dict(op='run', cycles=150000)],
               irq=dict(version=1, sources=['frame']))
        times = [e['emulated_ns'] for e in r['irq']['events']]
        assert len(times) >= 2 and all(b > a for a, b in zip(times, times[1:]))
    print('PASS: stopped state, physical labels, device I/O, observation purity, validation and IRQ attribution')


if __name__ == '__main__':
    main()
