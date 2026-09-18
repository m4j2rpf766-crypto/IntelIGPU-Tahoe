#!/usr/bin/env python3
from pathlib import Path
import subprocess
r=Path(__file__).resolve().parent;b=r.parent;out=r/'tests';out.mkdir(exist_ok=True)
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror']
sources=['gen12_scratch.c','capture_no_submit.c','gen12_hevc_cqp.cpp','video_batch_rewrite.cpp','gen12_packet_layout.cpp','prepare_gen12_probe.cpp']
sources+=['official-reference/media_driver/agnostic/gen12/hw/vdbox/'+n for n in ['mhw_vdbox_vdenc_hwcmd_g12_X.cpp','mhw_vdbox_hcp_hwcmd_g12_X.cpp']]
objs=[]
for name in sources:
 src=b/name;o=out/(src.name+'.o');compiler=['clang'] if src.suffix=='.c' else ['clang++','-std=c++17']
 subprocess.run([*compiler,*flags,'-DREIMS_PLUGIN_MODE','-I',str(b/'command-catalog'),'-c',str(src),'-o',str(o)],check=True);objs.append(str(o))
subprocess.run(['clang++','-std=c++17',*flags,'-DREIMS_RECOVERY_TEST',str(r/'test_recovery.cpp'),str(b/'hevc_recovery.cpp'),*objs,'-framework','CoreFoundation','-framework','IOKit','-framework','IOSurface','-o',str(out/'test_recovery')],check=True)
result=subprocess.run([str(out/'test_recovery')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=20)
(out/'result.txt').write_text(result.stdout);print(result.stdout);result.check_returncode()
