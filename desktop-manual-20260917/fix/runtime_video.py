"""Required video-discovery stage of the explicitly activated GPU runtime."""
import datetime,hashlib,json,pathlib,plistlib,time
BASE=pathlib.Path(__file__).resolve().parent
BUNDLE=BASE.parents[1]/'video-decode-candidate/manual-runtime-discovery-20260917/build/ReimsVideoDiscovery.kext'
RECEIPT_PATH=BASE.parents[1]/'video-decode-candidate/manual-runtime-discovery-20260917/video-discovery-current.json'
RECEIPT=json.loads(RECEIPT_PATH.read_text())
UUID=RECEIPT['uuid'].upper().replace('-','')
IDENTIFIER='lab.reims.ReimsVideoDiscovery'
HASHES=RECEIPT['hashes']
def verify_bundle(run):
    for name,expected in HASHES.items():
        assert hashlib.sha256((BUNDLE/name).read_bytes()).hexdigest()==expected,'VideoDiscovery package changed: '+name
    run(['codesign','--verify','--deep','--strict',BUNDLE])
    info=plistlib.loads((BUNDLE/'Contents/Info.plist').read_bytes())
    assert info['CFBundleIdentifier']==IDENTIFIER and info['CFBundleVersion']==RECEIPT['version']
    personalities=info['IOKitPersonalities'];assert len(personalities)==1
    return next(iter(personalities.values()))['VideoProperties']
def published(pci,objects,properties):
    accelerators=objects(pci,'IntelAccelerator')
    if len(accelerators)!=1 or not accelerators[0].get('IOServiceState',0)&2:return False
    accelerator=accelerators[0]
    publishers=objects(accelerator,'ReimsVideoDiscovery')
    return (len(publishers)==1 and publishers[0].get('CFBundleIdentifier')==IDENTIFIER
        and publishers[0].get('PhysicalIdentityVerified') is True and publishers[0].get('Published') is True
        and all(accelerator.get(k)==v for k,v in properties.items()))
def check_loaded(loaded):
    lines=[line for line in loaded.splitlines() if IDENTIFIER in line]
    if lines:assert len(lines)==1 and UUID in lines[0].upper().replace('-',''),'Unexpected loaded VideoDiscovery version; do not replace a running publisher'
    return bool(lines)
def ensure_video(run,registry,objects,logdir):
    properties=verify_bundle(run)
    pci=registry();accelerators=objects(pci,'IntelAccelerator')
    assert len(accelerators)==1 and accelerators[0].get('IOServiceState',0)&2,'Video discovery requires an already-published accelerator; no display activation was attempted'
    loaded=check_loaded(run(['kmutil','showloaded']).decode())
    needed=not loaded or not published(pci,objects,properties)
    if needed:
        output=run(['kmutil','load','-p',BUNDLE],30)
        (logdir/'video-load.txt').write_bytes(output)
        assert check_loaded(run(['kmutil','showloaded']).decode()),'VideoDiscovery did not load; complete normal macOS approval, then run session.py video'
    deadline=time.monotonic()+5
    while not published(pci,objects,properties):
        assert time.monotonic()<deadline,'VideoDiscovery loaded but did not publish expected properties; inspect evidence, do not repeat display commit'
        time.sleep(.25);pci=registry()
    record=dict(verified_at=datetime.datetime.now().astimezone().isoformat(),bundle=str(BUNDLE),version=RECEIPT['version'],uuid=UUID,hashes=HASHES,load_requested=needed,published=True,properties=properties)
    (logdir/'video-runtime.json').write_text(json.dumps(record,indent=2)+'\n')
    print('Video runtime ready: approved VideoDiscovery 0.1.3 and all expected accelerator video properties verified. Encoding/pixels still require validation.',flush=True)
    return pci
