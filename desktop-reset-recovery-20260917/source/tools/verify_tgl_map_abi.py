#!/usr/bin/env python3
"""Check native map vptr slots against the actual thin x86_64 TGL Mach-O."""
import argparse
import re
import struct
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('image',type=Path)
p.add_argument('--helper',type=Path,default=Path(__file__).resolve().parents[1]/'desktop-link/native_mapping.hpp')
a=p.parse_args();b=a.image.read_bytes()
assert struct.unpack_from('<II',b)==(0xfeedfacf,0x1000007),'expected thin x86_64 Mach-O'
pos=32;segments=[];symtab=None
for _ in range(struct.unpack_from('<I',b,16)[0]):
 cmd,size=struct.unpack_from('<II',b,pos)
 if cmd==0x19:
  vm,vs,off,fs=struct.unpack_from('<QQQQ',b,pos+24);segments.append((vm,vs,off,fs))
 elif cmd==2:symtab=struct.unpack_from('<IIII',b,pos+8)
 assert size>=8;pos+=size
assert symtab,'symbol table required for private ABI verification'
so,n,st,ss=symtab;symbols={}
for i in range(n):
 name,t,section,desc,value=struct.unpack_from('<IBBHQ',b,so+16*i)
 if name and t&0xe:
  end=b.index(b'\0',st+name,st+ss);symbols[b[st+name:end].decode()]=value
def u64(vm):
 for start,_,off,fs in segments:
  if start<=vm and vm+8<=start+fs:return struct.unpack_from('<Q',b,off+vm-start)[0]
 raise AssertionError('vtable outside file-backed segments')
v=symbols['__ZTV16IGAccelMemoryMap']+16 # C++ object vptr address point
assert u64(v+0x168)==symbols['__ZNK16IGAccelMemoryMap9getLengthEv']
assert u64(v+0x178)==symbols['__ZN16IGAccelMemoryMap23releaseFromGPUPageTableEv']
calls=re.findall(r'ReimsIOAccelMapValue\([^,]+,\s*(0x[0-9a-fA-F]+)\)',a.helper.read_text())
assert calls and all(int(slot,16) in (0x128,0x168) for slot in calls),'unexpected native-map virtual call'
print('PASS: actual vptr+168=getLength; +178=releases PTEs and is forbidden in inspector')

rv=symbols['__ZTV15IGAccelResource']+16
assert u64(rv+0x178)==symbols['__ZN15IGAccelResource8completeEv']
pv=symbols['__ZTV18IGAccelDisplayPipe']+16
assert u64(pv+0x8a8)==symbols['__ZN18IGAccelDisplayPipe18performTransactionEP30IOAccelDisplayPipeTransaction2']
assert u64(pv+0x8b8)==symbols['__ZN18IGAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2']
assert u64(pv+0x8b0)==symbols['__ZN18IGAccelDisplayPipe21isTransactionCompleteEP30IOAccelDisplayPipeTransaction2']
print('PASS: resource complete +178; pipe +8a8 perform, +8b8 submit, +8b0 completion query')

# DesktopLink shares native wake references and must observe that requester's
# ACK, not the legacy direct request bit. Pin both the caller and MMIO writes.
for offset, expected in (
 (0x28dab, 'b901000000'),
 (0x2d5ce, 'ba01000000d3e2'),
 (0x2d5f9, '8b4dd0898878a20000'),
 (0x2d69b, '8b4dd0898888a10000'),
):
 raw=bytes.fromhex(expected)
 assert b[offset:offset+len(raw)]==raw, 'native forcewake ABI changed'
source=(a.helper.parent/'ReimsADLDesktopLink.cpp').read_text()
assert 'constexpr uint32_t kForceWakeBit = 1U << 1U;' in source
print('PASS: native refcounted forcewake uses RENDER/GT requester bit 1')
