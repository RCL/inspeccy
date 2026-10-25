"""Small public adapter; the C++ CLI owns validation and execution."""
import json
import subprocess
from pathlib import Path


class BatchError(RuntimeError):
    """A batch that did not finish.

    The message is a summary, because it ends up in a caller's traceback and
    the full report is a failure's whole machine state -- registers, mapping,
    disassembly around the PC and the top of the stack, sometimes tens of
    kilobytes of it. That belongs in a file, not in a traceback. The report
    is still on disk at .path, and parsed in .report for a caller that wants
    to assert on it.
    """

    def __init__(self, report, stderr, path, steps=None):
        self.report = report
        self.path = Path(path)
        super().__init__(self._summary(report, stderr, path, steps))

    @staticmethod
    def _summary(report, stderr, path, steps):
        failure = report.get('failure') or {}
        where = failure.get('path') or f"step {report.get('failed_step')}"
        named = failure.get('name') or failure.get('symbol')
        head = f"{where}{f' ({named})' if named else ''}: " if failure or report.get('failed_step') is not None else ''
        lines = [head + (report.get('error') or 'batch failed')]

        done, total = report.get('completed_steps'), steps if steps is None else len(steps)
        if done is not None:
            lines.append(f"  {done} of {total} steps done" if total else f"  {done} steps done")

        cpu = failure.get('cpu') or {}
        if 'PC' in cpu:
            at = f"  PC ${cpu['PC']:04X}"
            labels = ((failure.get('state') or {}).get('pc') or {}).get('labels') or []
            if labels:
                first = labels[0]
                at += f" = {first.get('name')}+{first.get('offset', 0)}"
            if 'SP' in cpu:
                at += f", SP ${cpu['SP']:04X}"
            lines.append(at)

        for want, got in (('expected', 'actual'), ('expected_hex', 'actual_hex')):
            if want in failure:
                lines.append(f"  expected {failure[want]}, got {failure.get(got)}")

        tail = (stderr or '').strip().splitlines()[-3:]
        lines += ['  ' + line for line in tail]
        lines.append(f"  full report: {path}")
        return '\n'.join(lines)


def run_batch(executable, folder, steps, *, resume=None, nex=None, dump=False, extra=()):
    if (resume is None) == (nex is None):
        raise ValueError('provide exactly one of resume and nex')
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    plan, report, state = (folder/name for name in ('batch.json', 'result.json', 'state.json'))
    plan.write_text(json.dumps({'version': 1, 'steps': steps}))
    # A failed invocation must not expose yesterday's successful report.
    if report.exists():
        report.unlink()
    args = [str(executable), '--resume' if resume is not None else '--nex', str(resume or nex),
            '--batch', str(plan), '--batch-out', str(report), '--no-status', '--no-bmp']
    args += ['--dump-out', str(state)] if dump else ['--no-dump']
    result = subprocess.run(args + list(map(str, extra)), capture_output=True, text=True)
    output = json.loads(report.read_text()) if report.exists() else {'ok': False, 'error': 'no batch report'}
    if result.returncode or not output.get('ok'):
        raise BatchError(output, result.stdout + result.stderr, report, steps)
    return output, state if dump else None
