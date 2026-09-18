#!/usr/bin/env python3
"""Explicit session-only activation. Never installed as a launch service."""
import argparse, datetime, hashlib, json, os, pathlib, plistlib, subprocess, time
from runtime_video import ensure_video, verify_bundle
BASE=pathlib.Path(__file__).resolve().parent
CONTROL=BASE/'manual-gate/control'
NATIVE=pathlib.Path('/Library/Extensions/ReimsTGLBoot.kext')
LINK=pathlib.Path('/Library/Extensions/ReimsADLDesktopLink.kext')
def run(args,timeout=10):
    try:
        return subprocess.check_output([str(x) for x in args],stderr=subprocess.STDOUT,timeout=timeout)
    except subprocess.CalledProcessError as error:
        print(error.output.decode(errors="replace"),flush=True)
        raise
def registry():
    return plistlib.loads(run(['ioreg','-r','-n','GFX0','-l','-a']))[0]
def objects(root,class_name):
    result=[]
    if root.get('IOObjectClass')==class_name:result.append(root)
    for child in root.get('IORegistryEntryChildren',[]):result.extend(objects(child,class_name))
    return result
def action(name,timeout=10):
    print(run([CONTROL,name],timeout).decode(),flush=True)
def preflight(phase):
    assert os.geteuid()==0,'Run as administrator'
    assert run(['sw_vers','-buildVersion']).strip()==b'25G83'
    receipt=json.loads((BASE/'deferred-runtime.json').read_text())
    assert hashlib.sha256((NATIVE/'Contents/MacOS/AppleIntelTGLGraphics').read_bytes()).hexdigest()==receipt['candidate_sha256']
    assert hashlib.sha256((LINK/'Contents/MacOS/ReimsADLDesktopLink').read_bytes()).hexdigest()=='239dd2df4867059451b1707ea1fbdf1bd60f69776f04855dbdd154c12c35006a'
    loaded=run(['kmutil','showloaded']).decode().upper()
    uuid=receipt['candidate_uuid'].upper()
    assert uuid in loaded.replace('-',''),'Approved deferred runtime is not loaded; do not use the old runtime'
    gate_receipt=json.loads((BASE/'manual-gate-current.json').read_text())
    assert gate_receipt['uuid'].upper().replace('-','') in loaded.replace('-',''),'Corrected manual controller is not loaded'
    gate_binary=pathlib.Path('/Library/Extensions/ReimsADLManualActivation.kext/Contents/MacOS/ReimsADLManualActivation')
    assert hashlib.sha256(gate_binary.read_bytes()).hexdigest()==gate_receipt['sha256'],'Manual controller on disk changed'
    pci=registry()
    assert pci['device-id']==bytes.fromhex('ffff0000'),'Startup isolation changed'
    # Check the media package before any display mutation. Loading/matching
    # it occurs only after the accelerator is explicitly published.
    verify_bundle(run)
    if phase=='prepare' and '9F0C79A353D830FABD5E94A604A04F3F' not in loaded.replace('-',''):
        assert not objects(pci,'IntelAccelerator'),'Runtime already exists; inspect instead of preparing twice'
        run(['codesign','--verify','--deep','--strict',LINK])
        print(run(['kmutil','load','-p',LINK,'--load-style','start-only'],30).decode())
        loaded=run(['kmutil','showloaded']).decode().upper()
    assert '9F0C79A353D830FABD5E94A604A04F3F' in loaded.replace('-',''),'Reset recovery DesktopLink 0.6.23 is not loaded'
    return pci
def hidden_ready(pci):
    accelerators=objects(pci,'IntelAccelerator')
    return len(accelerators)==1 and not (accelerators[0]['IOServiceState']&3) and pci['IOServiceBusyState']==0
parser=argparse.ArgumentParser()
parser.add_argument('phase',choices=['prepare','commit','video'])
args=parser.parse_args()
pci=preflight(args.phase)
logdir=BASE/('session-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+args.phase)
logdir.mkdir()
(logdir/'before.plist').write_bytes(plistlib.dumps(pci))
if args.phase=='prepare':
    assert not objects(pci,'IntelAccelerator'),'A runtime already exists; inspect it instead of starting twice'
    # Preflight loaded only the approved link code, without PCI matching.
    if not objects(pci,'ReimsADLManualMatchProbe'):
        action('SelfTest')
    deadline=time.monotonic()+5
    while not objects(registry(),'ReimsADLManualMatchProbe'):
        assert time.monotonic()<deadline,'Read-only matching self-test did not appear'
        time.sleep(.25)
    action('PrepareRuntime',50)
    deadline=time.monotonic()+45
    while True:
        pci=registry()
        a=objects(pci,'IntelAccelerator')
        assert not any(x['IOServiceState']&2 for x in a),'Premature publication; stop and inspect'
        if hidden_ready(pci):break
        assert time.monotonic()<deadline,'Hidden initialization did not finish; no display commit attempted'
        time.sleep(1)
    assert objects(pci,'IONDRVFramebuffer'),'Firmware display disappeared before commit'
    action('PrepareDisplay')
    pci=registry()
    assert hidden_ready(pci),'Accelerator must remain unpublished after preparation'
    link=objects(pci,'ReimsADLDesktopLink')
    assert len(link)==1 and link[0].get('RCSADLPWorkaroundLive') is True
    assert link[0].get('RCSResetPSMIRegistered') is True,'Native reset restore table was not prepared'
    assert objects(pci,'IONDRVFramebuffer') and not objects(pci,'ReimsIntelADLFramebuffer')
    print('Prepared but NOT published. Existing desktop remains on firmware framebuffer.')
elif args.phase=='commit':
    assert hidden_ready(pci),'Hidden preparation is required before display commit'
    link=objects(pci,'ReimsADLDesktopLink')
    assert len(link)==1 and link[0].get('RCSADLPWorkaroundLive') is True
    assert link[0].get('RCSResetPSMIRegistered') is True,'Native reset restore table was not prepared'
    assert objects(pci,'IONDRVFramebuffer'),'Unexpected existing display route'
    action('CommitDisplay',15)
    deadline=time.monotonic()+10
    while True:
        pci=registry();a=objects(pci,'IntelAccelerator')
        if len(a)==1 and a[0]['IOServiceState']&2:break
        assert time.monotonic()<deadline,'Commit returned but accelerator publication not observed'
        time.sleep(.5)
    assert len(objects(pci,'ReimsIntelADLFramebuffer'))==1
    print('Display route published. This is not a pass: verify login, WindowServer stability, and real completed GPU flips.')
if args.phase in ('commit','video'):
    try:
        pci=ensure_video(run,registry,objects,logdir)
    except Exception:
        (logdir/'video-failed.plist').write_bytes(plistlib.dumps(registry()))
        print('Display activation is not repeated or rolled back here. Video runtime is NOT ready; inspect/complete macOS approval, then use session.py video.',flush=True)
        raise
(logdir/'after.plist').write_bytes(plistlib.dumps(pci))
print('Evidence:',logdir)
