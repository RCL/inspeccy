"""Public JSON batch CLI: banked calls, patches, assertions, resume and errors."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    exe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='inspeccy-batch-') as temp:
        root = Path(temp)
        header = bytearray(512)
        header[:8] = b'NextV1.2'
        header[9] = 1
        header[12:16] = bytes.fromhex('00 BF 00 80')
        header[18+2] = 1
        bank = bytearray(16384)
        bank[:3] = bytes.fromhex('C3 00 80')
        nex = root/'test.nex'
        nex.write_bytes(header+bank)
        def go(name,steps,code=0,extra=(),resume=None):
            plan = root/(name+'.json')
            plan.write_text(json.dumps(dict(version=1,steps=steps)))
            report = root/(name+'.result.json')
            dump = root/(name+'.dump.json')
            cmd = [exe,'--batch',str(plan),'--batch-out',str(report),'--dump-out',str(dump),
                   '--no-bmp','--no-status','--out',str(root/name)]
            cmd += ['--resume',str(resume)] if resume else ['--machine','next60','--nex',str(nex)]
            r = subprocess.run(cmd+list(extra),capture_output=True,text=True)
            assert r.returncode==code,(name,r.returncode,r.stdout,r.stderr)
            result=json.loads(report.read_text()) if report.exists() else None
            if result: assert result['ok']==(code==0)
            return result,dump
        setup = [dict(op='set',registers={'IFF1':0,'IFF2':0,'EI_PENDING':0,'SP':'$BF00'}),
                 dict(op='write',page=30,offset=0,hex='21 00 90 34 C9'),
                 dict(op='nextreg',index='$57',value=30)]
        call = dict(op='call',pc='$E000',return_pc='$8FFE',cycles=1000)
        result,dump = go('sixty',setup+[dict(op='repeat',count=60,steps=[call]),
            dict(op='expect_memory',page=4,offset='$1000',hex='3C'),
            dict(op='expect_regs',registers={'HL':'$9000','SP':'$BF00','PC':'$8FFE'}),
            dict(op='read',name='counter',page=4,offset='$1000',length=1),dict(op='read_regs')])
        assert result['completed_steps']==67 and result['observations'][0]['hex']=='3C'
        assert result['cycles']>0
        # Resume the final dump and call again without re-injecting code or mapping.
        result,_=go('resume',[call,dict(op='expect_memory',page=4,offset='$1000',hex='3D')],resume=dump)
        # Same CPU address in another physical page must not stop too soon.
        result,_=go('banked_break',[dict(op='write',page=30,offset=0,hex='ED 91 57 1F'),
            dict(op='write',page=31,offset=4,hex='C3 00 E0'),
            dict(op='nextreg',index='$57',value=30),dict(op='set',registers={'PC':'$E000','IFF1':0,'IFF2':0}),
            dict(op='run',cycles=1000,until_pc='$E000',until_page=31),dict(op='read_regs')])
        assert result['cycles']>0
        go('run_budget',[dict(op='run',cycles=100),dict(op='expect_regs',registers={'PC':'$8000'})])
        result,_=go('assertion',setup+[call,dict(op='expect_memory',page=4,offset='$1000',hex='02'),call],9)
        assert result['completed_steps']==4 and result['failed_step']==4
        result,_=go('timeout',[dict(op='run',cycles=100,until_pc='$8005')],9)
        assert result['completed_steps']==0
        # Invalid later steps are rejected before earlier patches execute.
        invalid=[dict(op='write',page=256,offset=0,hex='FF'),dict(op='write',page=1,offset=8191,hex='FFFF'),
                 dict(op='write',page=1,offset=0,hex='F'),dict(op='set',registers={'IFF1':2}),
                 dict(op='set',registers={'AF':65536}),dict(op='set',registers={'TYPO':1}),
                 dict(op='repeat',count=100001,steps=[call]),dict(op='run',cycles=0),
                 dict(op='run',cycles=1,until_page=1),dict(op='unknown'),dict(op='read_regs',typo=1)]
        for i,step in enumerate(invalid):
            r,p=go('invalid'+str(i),[dict(op='set',registers={'AF':1234}),step],4)
            assert r['completed_steps']==0
            assert json.loads(p.read_text())['cpu']['AF']!='$04D2'
        go('conflict',[call],4,extra=['--cycles','100'])
        # Report destination must not overwrite the input plan or loaded image.
        go('collision',[dict(op='read_regs')],4,extra=['--batch-out',str(nex)])
        assert nex.read_bytes()==header+bank
        print('PASS: batch calls/repeat, Next bank breakpoint, resume, compact reads, bounded failures, validation and output collision protection')


if __name__=='__main__':
    main()
