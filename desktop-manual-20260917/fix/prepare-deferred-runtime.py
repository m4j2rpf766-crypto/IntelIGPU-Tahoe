"""Prepare only: defer TGL service publication until explicit display commit."""
from pathlib import Path
import hashlib, json, plistlib, shutil, struct, subprocess, argparse
base=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('source',type=Path,help='Locally obtained compatible TGL runtime bundle (not distributed)')
source=parser.parse_args().source.resolve()
target=base/'ReimsTGLManualRuntime.kext'
b=(source/'Contents/MacOS/AppleIntelTGLGraphics').read_bytes()
expected='ae99582bd5a945494ee684d339ac1abd0526828bcd3ea981239c0fd38f794d47'
assert hashlib.sha256(b).hexdigest()==expected
# Validate vtable relocation, not merely the call's apparent offset.
cursor=32
for _ in range(struct.unpack_from('<I',b,16)[0]):
    command,size=struct.unpack_from('<II',b,cursor)
    if command==2:sym=struct.unpack_from('<IIII',b,cursor+8)
    if command==0x1b:uuid_offset=cursor+8
    if command==11:dyn=struct.unpack_from('<18I',b,cursor+8)
    cursor+=size
names=[]; values={}
for i in range(sym[1]):
    offset,kind,section,desc,value=struct.unpack_from('<IBBHQ',b,sym[0]+16*i)
    name=b[sym[2]+offset:b.index(b'\0',sym[2]+offset)].decode()
    names.append(name);values[name]=value
relocs={}
for i in range(dyn[15]):
    address,info=struct.unpack_from('<II',b,dyn[14]+8*i)
    if info>>27&1:relocs[address]=names[info&0xffffff]
assert values['__ZN16IntelAccelerator5startEP9IOService']==0x23ecc
assert relocs[values['__ZTV16IntelAccelerator']+16+0x5b0]=='__ZN9IOService15registerServiceEj'
site=0x246b5
assert b[site-12:site+6]==bytes.fromhex('498b45004531ff4c89ef31f6ff90b0050000')
assert not any(site<=a<site+6 for a in relocs)
patched=bytearray(b);patched[site:site+6]=b'\x90'*6
assert b[:site]==patched[:site] and b[site+6:]==patched[site+6:]
assert patched[site:site+6]==bytes([0x90])*6
deferred_uuid=hashlib.sha256(patched).digest()[:16]
patched[uuid_offset:uuid_offset+16]=deferred_uuid
(base/'manual-gate/deferred_uuid.hpp').write_text('static const unsigned char deferredUUID[]={'+','.join(hex(x) for x in deferred_uuid)+'};\n')
if target.exists(): raise SystemExit('Candidate already exists; do not overwrite a reviewed release.')
shutil.copytree(source,target)
(target/'Contents/MacOS/AppleIntelTGLGraphics').write_bytes(patched)
p=target/'Contents/Info.plist';info=plistlib.loads(p.read_bytes())
# No GPU personalities in the installed candidate. Only the explicit controller
# may add a current-session personality. Physical identity remains FFFF at boot.
info['IOKitPersonalities']={}
info['CFBundleVersion']=info['CFBundleShortVersionString']='16.0.3'
info['ReimsManualPublicationDeferred']=True
p.write_bytes(plistlib.dumps(info))
subprocess.run(['codesign','--force','--sign','-',str(target)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(target)],check=True)
record={'source_sha256':expected,'candidate_sha256':hashlib.sha256((target/'Contents/MacOS/AppleIntelTGLGraphics').read_bytes()).hexdigest(),'deferred_call_file_offset':hex(site),'deferred_call':'IOService::registerService(unsigned int)','functional_text_change_bytes':6,'candidate_uuid':deferred_uuid.hex(),'automatic_personalities':0,'installed':False,'hardware_tested':False}
(base/'deferred-runtime.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
