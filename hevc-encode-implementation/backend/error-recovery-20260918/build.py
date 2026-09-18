#!/usr/bin/env python3
"""Build the candidate from source without modifying the installed bundle."""
from pathlib import Path
import hashlib, json, subprocess

r=Path(__file__).resolve().parent
b=r.parent
out=r/'build'
out.mkdir(exist_ok=True)
sources=['gen12_scratch.c','gen12_hevc_cqp.cpp','video_batch_rewrite.cpp',
         'gen12_packet_layout.cpp','prepare_gen12_probe.cpp','hevc_recovery.cpp',
         'capture_no_submit.c','service-route-20260917/service_bridge.c']
sources += ['official-reference/media_driver/agnostic/gen12/hw/vdbox/'+x
            for x in ['mhw_vdbox_vdenc_hwcmd_g12_X.cpp','mhw_vdbox_hcp_hwcmd_g12_X.cpp']]
objects=[]
for name in sources:
    src=b/name
    obj=out/(src.name+'.o')
    compiler=['clang'] if src.suffix=='.c' else ['clang++','-std=c++17']
    subprocess.run(compiler+['-Wall','-Wextra','-Werror','-DREIMS_PLUGIN_MODE',
        '-I',str(b/'command-catalog'),'-c',str(src),'-o',str(obj)],check=True)
    objects.append(obj)
target=out/'libReimsHEVCService.dylib'
subprocess.run(['clang++','-dynamiclib','-undefined','dynamic_lookup',
    '-framework','CoreFoundation','-framework','IOKit','-framework','IOSurface',
    *map(str,objects),'-o',str(target)],check=True)
subprocess.run(['codesign','--force','--sign','-',str(target)],check=True)
subprocess.run(['codesign','--verify','--strict',str(target)],check=True)
receipt={'sources':{name:hashlib.sha256((b/name).read_bytes()).hexdigest() for name in sources},
         'library_sha256':hashlib.sha256(target.read_bytes()).hexdigest()}
(out/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt,indent=2))
