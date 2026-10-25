"""Local timing evidence; not a pass/fail CI threshold. No ROM/game required."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--repeats', type=int, default=7)
    args = parser.parse_args()
    results = {}
    with tempfile.TemporaryDirectory(prefix='inspeccy-benchmark-') as temp:
        root = Path(temp)
        header = bytearray(512); header[:8] = b'NextV1.2'; header[9] = 1
        header[12:16] = bytes.fromhex('00 BF 00 80'); header[20] = 1
        config = root/'irq.json'; config.write_text('{"version":1,"max_events":0}')
        for name, code in [('branch', 'C30080'), ('memory', '210090347E3C77C30380')]:
            bank = bytearray(16384); bank[:len(bytes.fromhex(code))] = bytes.fromhex(code)
            nex = root/(name+'.nex'); nex.write_bytes(header+bank)
            samples = {k: [] for k in (['baseline'] if args.baseline else [])+['disabled','enabled']}
            for repeat in range(args.repeats+1):
                # Alternate order to reduce warmup/thermal ordering bias.
                for mode in list(samples)[::1 if repeat % 2 else -1]:
                    exe = args.baseline if mode == 'baseline' else args.executable
                    cmd = [str(exe.resolve()), '--nex', str(nex), '--cycles', '250000000', '--no-bmp', '--no-dump', '--no-status']
                    if mode == 'enabled': cmd += ['--irq-diagnostics', str(config), '--irq-diagnostics-out', str(root/'irq-result.json')]
                    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
                    elapsed = float(re.search(r'wall_ms=([0-9.]+)', r.stdout).group(1))
                    if repeat: samples[mode].append(elapsed)
            results[name] = {k: dict(median_ms=round(statistics.median(v), 3), samples_ms=v) for k, v in samples.items()}
        # Compare compact device observation to an otherwise identical final dump.
        plan = root/'plan.json'
        plan.write_text(json.dumps(dict(version=1, steps=[dict(op='read_device',device='palette',layer='layer2',bank=1,start=0,count=256)])))
        times = {'compact': [], 'dump': []}
        for repeat in range(args.repeats+1):
            for mode in list(times)[::1 if repeat % 2 else -1]:
                cmd = [str(args.executable.resolve()), '--nex', str(nex), '--batch', str(plan), '--batch-out', str(root/'result.json'), '--no-status', '--no-bmp']
                cmd += ['--no-dump'] if mode == 'compact' else ['--dump-out', str(root/'state.json')]
                start = time.perf_counter(); subprocess.run(cmd, capture_output=True, check=True)
                if repeat: times[mode].append((time.perf_counter()-start)*1000)
        results['device_roundtrip'] = {k: dict(median_ms=round(statistics.median(v),3)) for k,v in times.items()}
        results['device_roundtrip']['report_bytes'] = (root/'result.json').stat().st_size
        results['device_roundtrip']['dump_bytes'] = (root/'state.json').stat().st_size
    text = json.dumps(results, indent=2)
    if args.out: args.out.write_text(text+'\n')
    print(text)


if __name__ == '__main__':
    main()
