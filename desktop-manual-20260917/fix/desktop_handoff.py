"""Bounded desktop handoff; never restart twice in the same boot."""
import json
import os
import pathlib
import signal
import subprocess
import time

MARKER = pathlib.Path('/private/var/run/reims-igpu-desktop-handoff.json')


def save_marker(record):
    temporary = MARKER.with_suffix('.tmp')
    with temporary.open('w') as stream:
        json.dump(record, stream)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, MARKER)


def same_process(left, right):
    return (left.get('pid'), left.get('identity')) == (right.get('pid'), right.get('identity'))


def force_stuck_exit(run, observe, record, logdir):
    """One bounded escalation for the original, still unbound desktop only."""
    current = observe()
    if (not same_process(current, record['before']) or current['power'] != 0
            or current['completed'] not in (None, 0)):
        raise RuntimeError('Desktop changed before forced exit; no signal sent')
    # Capture the exit stall before removing its process. Capture failure is evidence too.
    try:
        output = run(['/usr/bin/sample', str(current['pid']), '3', '1'], timeout=10)
        (logdir / 'windowserver-before-force.sample.txt').write_bytes(output)
    except (OSError, subprocess.SubprocessError) as error:
        (logdir / 'windowserver-sample-error.txt').write_text(str(error))
    latest = observe()
    if latest != current:
        raise RuntimeError('Desktop changed during exit sampling; no signal sent')
    record['phase'] = 'force-intent'
    record['force_evidence'] = str(logdir)
    save_marker(record)
    os.kill(current['pid'], signal.SIGKILL)


def snapshot(run, registry, objects):
    frames = objects(registry(), 'ReimsIntelADLFramebuffer')
    if len(frames) != 1:
        raise RuntimeError('Expected exactly one native framebuffer')
    frame = frames[0]
    pids = run(['pgrep', '-x', 'WindowServer']).decode().split()
    if len(pids) != 1:
        raise RuntimeError('Expected exactly one WindowServer')
    pid = int(pids[0])
    return dict(pid=pid, identity=run(['ps', '-p', str(pid), '-o', 'lstart=']).decode().strip(),
                power=frame.get('IOPowerManagement', {}).get('CurrentPowerState'),
                submitted=frame.get('ReimsFlipSubmitted'),
                completed=frame.get('ReimsFlipCompleted'), pending=frame.get('ReimsFlipPending'))


def progressing(before, after):
    return (before['pid'] == after['pid'] and before['identity'] == after['identity']
            and before['power'] == after['power'] == 2
            and all(type(s.get(k)) is int for s in (before, after) for k in ('submitted', 'completed'))
            and after['completed'] > before['completed']
            and after['submitted'] >= after['completed']
            and before['submitted'] >= before['completed']
            and after['pending'] in (False, True, 0, 1))


def ensure_desktop(run, registry, objects, logdir):
    def observe():
        value = snapshot(run, registry, objects)
        with (logdir / 'desktop-observations.jsonl').open('a') as stream:
            stream.write(json.dumps(dict(at=time.time(), **value)) + '\n')
        return value

    first = observe()
    time.sleep(3)
    second = observe()
    if progressing(first, second):
        return second
    # An active but stalled display is a diagnostic incident, not a restart trigger.
    if second['power'] != 0 or second['completed'] not in (None, 0):
        raise RuntimeError('Desktop is not progressing; preserve evidence, no automatic restart')
    if (first['pid'], first['identity']) != (second['pid'], second['identity']):
        raise RuntimeError('WindowServer changed during observation; no restart')
    boot = run(['sysctl', '-n', 'kern.boottime']).decode().strip()
    record = json.loads(MARKER.read_text()) if MARKER.exists() else {}
    resume = record.get('boot') == boot
    if resume:
        # Upgrade the previous implementation's proven TERM timeout without clearing its guard.
        failure_path = pathlib.Path(record.get('evidence', '')) / 'failure.json'
        legacy_timeout = False
        if 'phase' not in record and failure_path.is_file():
            failure = json.loads(failure_path.read_text())
            legacy_timeout = failure.get('error') == 'Desktop handoff did not produce completed flips within 120 seconds; no retry'
        if (not legacy_timeout or not same_process(second, record.get('before', {}))):
            raise RuntimeError('Desktop handoff already attempted this boot; no repeated logout')
    current = observe()
    if current != second:
        raise RuntimeError('Desktop state changed before handoff; no restart')
    # Persist intent before signaling: interrupted attempts cannot cause repeated logout.
    if not resume:
        record = dict(boot=boot, before=current, evidence=str(logdir), phase='term-intent')
        save_marker(record)
        os.kill(current['pid'], signal.SIGTERM)
    forced = resume
    if resume:
        force_stuck_exit(run, observe, record, logdir)
    force_at = time.monotonic() + 15
    deadline = time.monotonic() + 120
    previous = None
    while time.monotonic() < deadline:
        time.sleep(3)
        try:
            value = observe()
        except (RuntimeError, OSError, subprocess.SubprocessError) as error:
            (logdir / 'desktop-observation-error.txt').write_text(str(error))
            continue
        if value['pid'] != current['pid'] and previous and progressing(previous, value):
            record['phase'] = 'verified'
            save_marker(record)
            return value
        if not forced and time.monotonic() >= force_at and same_process(value, current):
            force_stuck_exit(run, observe, record, logdir)
            forced = True
        previous = value
    raise RuntimeError('Desktop handoff did not produce completed flips within 120 seconds; no retry')
