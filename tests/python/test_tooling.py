"""Shared tools: no external programs, ROM or assembler required."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from inspeccy_tools import Snapshot, run_batch, BatchError, digest


def main():
    exe = Path(sys.argv[1]).resolve()

    def asm(*command, expect=0):
        """One static-analysis command, as parsed JSON."""
        r = subprocess.run([str(exe), *map(str, command), '--json'],
                           capture_output=True, text=True)
        assert r.returncode == expect, (r.returncode, r.stdout, r.stderr)
        return json.loads(r.stdout) if r.returncode != 2 else None
    with tempfile.TemporaryDirectory(prefix='inspeccy-tools-') as tmp:
        root = Path(tmp)
        # An 8K-page routine increments a resident counter and returns.
        header = bytearray(512)
        header[:8] = b'NextV1.2'
        header[9] = 1
        header[12:16] = bytes.fromhex('00 BF 00 80')
        header[20] = 1
        bank = bytearray(16384)
        bank[:5] = bytes.fromhex('21 00 90 34 C9')
        nex = root/'fixture.nex'; nex.write_bytes(header+bank)
        sld = root/'fixture.sld'
        sld.write_text('fixture.asm|1||0|-1|-1|Z|pages.size:8192\n'
                       'fixture.asm|1||0|4|32768|F|Increment\n'
                       'fixture.asm|1||0|4|36864|F|Counter\n'
                       'fixture.asm|1||0|4|36864|F|Alias\n'
                       'fixture.asm|1||0|-1|42|D|Answer\n')
        build = root/'build.json'
        subprocess.check_call([str(exe),'--write-manifest',str(build),
                               '--image',str(nex),'--sld',str(sld)],stdout=subprocess.DEVNULL)
        def go(name, steps, resume=None, **kwargs):
            return run_batch(exe,root/name,steps, resume=resume, nex=None if resume else nex,
                             dump=True, extra=['--build-manifest',build, *kwargs.get('extra',[])])
        setup = [dict(op='set',registers={'SP':0xbf00,'IFF1':0,'IFF2':0,'HALTED':0,'EI_PENDING':0}),
                 dict(op='write_value',symbol='Counter',type='u32le',value={'symbol':'Answer'})]
        r, dump = go('named',setup+[
            dict(op='call',symbol='Increment',return_pc=0x8fff,cycles=1000),
            dict(op='expect_value',symbol='Alias',type='u32le',value=43),
            dict(op='read_value',name='count',symbol='Counter',type='u32le')])
        assert r['observations'][0]['value'] == 43
        saved = Snapshot(dump)
        original_text = dict(saved.data['ram'])
        assert saved.data['nex']['sha1'] == digest(nex)
        assert len(saved.banks[0]) == 16384 and not any(saved.banks[0])
        saved.banks[2][0x1000] = 99
        moved = root/'moved'; moved.mkdir()
        edited = saved.save(moved/'edited.json')
        reread = Snapshot(edited)
        assert reread.banks == saved.banks and reread.data['nex']['path'] == str(nex)
        assert all(v == saved.data['ram'][k] for k,v in original_text.items() if k != 'bank_2')
        saved.banks[2] = bytearray(16384)
        saved.save(edited)
        assert not any(Snapshot(edited).banks[2])
        saved.save(moved/'again.json')
        assert edited.read_bytes() == (moved/'again.json').read_bytes()
        try:
            go('failure',[dict(op='repeat',count=2,steps=[
                dict(op='expect_value',name='counter assertion',symbol='Counter',type='u32le',value=100)])],resume=dump)
            assert False
        except BatchError as e:
            f=e.report['failure']
            assert f['name']=='counter assertion' and 'repeat[0]' in f['path']
            assert f['expected_hex']=='64000000' and f['actual_hex']=='2B000000'
            assert len(f['mmu'])==8 and f['symbol']=='Counter'
        try:
            go('mapping',[dict(op='nextreg',index=0x54,value=6),
                          dict(op='call',symbol='Increment',return_pc=0x8fff,cycles=1000)])
            assert False
        except BatchError as e: assert 'not mapped' in str(e)
        # A relocated resume preserves the original NEX identity. Rebuilding
        # that NEX is rejected; an explicit override retains old provenance.
        old_hash=digest(nex)
        nex.write_bytes(nex.read_bytes()+b'new build')
        try:
            go('stale',[dict(op='read_regs')],resume=dump)
            assert False
        except BatchError as e: assert 'differs' in str(e)
        _, patched = go('patch',[dict(op='read_regs')],resume=dump,extra=['--allow-build-mismatch','test patch'])
        assert Snapshot(patched).data['nex']['sha1']==old_hash
        assert Snapshot(patched).data['nex']['identity_override']=='test patch'
        sld.write_text(sld.read_text()+'\n')
        try:
            go('stale_symbols',[dict(op='read_regs')],resume=dump,extra=['--allow-build-mismatch','test patch'])
            assert False
        except BatchError as e: assert 'symbol file differs' in str(e)

        # Mapping checks cover macro-expanded code, control flow and direct data.
        source=root/'gate.asm'
        source.write_text('Gate:\nnextreg $57, 30\ncall Target\nnextreg $57, 31\nret\n'
                          'Target:\nld a,(Data)\nret\nData: db 0\n')
        records=['gate.asm|1||0|-1|-1|Z|pages.size:8192']
        for line,page,address,name in [(1,4,32768,'Gate'),(6,30,57344,'Target'),(9,31,57350,'Data')]:
            records.append(f'gate.asm|{line}||0|{page}|{address}|F|{name}')
        for line,page,address in [(2,4,32768),(3,4,32772),(4,4,32775),(5,4,32779),(7,30,57344),(8,30,57347)]:
            records.append(f'gate.asm|{line}||0|{page}|{address}|T|')
        sld.write_text('\n'.join(records))
        cfg=root/'assembly.json'
        def banks(config,expect=0):
            cfg.write_text(json.dumps(dict(version=1,**config)))
            return asm('--asm-banks','--sld',sld,'--source-root',root,'--asm-config',cfg,expect=expect)
        config=dict(fixed={'4':4},routines={
            'Gate':dict(end=32780,requires={'7':31},returns={'7':31}),
            'Target':dict(external=True,requires={'7':30},returns={'7':30})})
        assert not banks(config)['errors']
        config['routines']['Gate']['returns']['7']=30
        assert any('return' in e['message'] for e in banks(config,expect=1)['errors'])
        config['routines']['Target']=dict(end=57348,requires={'7':30},returns={'7':30})
        assert any('data Data' in e['message'] for e in banks(config,expect=1)['errors'])
        assert asm('--asm-query','Target','--sld',sld,'--source-root',root)['callers'][0]['target']=='Target'
        cfg.write_text(json.dumps(dict(version=1,regions=[
            dict(name='gate',page=4,start='Gate',end=32780,limit=40960)])))
        assert asm('--asm-layout','--sld',sld,'--source-root',root,'--asm-config',cfg)[0]['free']==8180
        # An unreadable symbol or a bad configuration is a usage error, not a pass.
        assert asm('--asm-query','Nowhere','--sld',sld,'--source-root',root,expect=2) is None
        # 16K SLD geometry is converted before physical lookup.
        sld.write_text('x|1||0|-1|-1|Z|pages.size:16384\nx|1||0|3|57344|F|High\n')
        assert asm('--asm-query','High','--sld',sld,'--source-root',root)['page']==7
    print('PASS: shared snapshots, symbols, typed batches, identity, diagnostics, layout and bank contracts')


if __name__=='__main__': main()
