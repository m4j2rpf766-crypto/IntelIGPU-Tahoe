#include "gen12_packet_layout.h"
#include "video_batch_rewrite.h"
#include "gen12_scratch.h"
#include "hevc_recovery.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include "official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_vdenc_hwcmd_g12_X.h"

extern "C" int IOAccelVideoContextSubmitDataBuffers(void*,unsigned,unsigned*,unsigned*);
extern "C" int IOAccelVideoContextFinishFenceEvent(void*,unsigned);
extern "C" void reims_dump_surfaces(const char*);

namespace {
uint32_t length(uint32_t h){
    if(h>>29==0){uint32_t op=(h>>23)&63;return op==0||op==10?1:(h&63)+2;}
    return (h&0xffff0000)==0x68000000?1:(h&4095)+2;
}
void save(const char*dir,const char*name,const std::vector<uint32_t>&v){
    if(!dir)return;
    std::string path=std::string(dir)+"/"+name;
    FILE*f=fopen(path.c_str(),"wb");if(f){fwrite(v.data(),4,v.size(),f);fclose(f);}
}
}
// Offline preparation in the live resource namespace. The candidate handles
// are released before return and MUST NOT be replayed from the saved files.
// This entry has no video submit symbol and never writes the original batch.
static int prepare(void*shared,const void*cb,size_t cn,const void*sb,size_t sn,const char*dir,
                   void*io,unsigned flags,unsigned*out,unsigned*error){
    if(!shared||!cb||!sb||cn%4||sn%4||cn>1048576||sn>65536)return 0;
    reims::VideoBatch in;
    in.commands.resize(cn/4);in.sideband.resize(sn/4);
    memcpy(in.commands.data(),cb,cn);memcpy(in.sideband.data(),sb,sn);
    for(size_t t=4;t<in.sideband.size();){
        uint32_t n=in.sideband[t]>>16;
        if(in.sideband[t]==0x200)break;
        if(n<2||n>in.sideband.size()-t)return 0;
        if(in.sideband[t]==0x38000)in.used_dwords=in.sideband[t+1];
        t+=n;
    }
    reims::VideoBatch check;
    if(!reims::rewrite_video_batch(in,{},check))return 0;
    // First-frame conformance fixture. The packet converter verifies extent,
    // Main8/CTU64, IDR slice type, QP, no tiles and no reference use.
    reims::HevcIdrCqp params{1280,720,30,false};
    params.target_usage=4;params.low_delay=true;params.quality_improvement=true;params.rounding=true;
    unsigned cmd2_count=0;
    for(uint32_t i=0;i<in.used_dwords;i+=length(in.commands[i])){
        if(in.commands[i]==0x70890023){
            mhw_vdbox_vdenc_g12_X::VDENC_CMD2_CMD old;
            memcpy(&old,in.commands.data()+i,37*4);
            if(old.DW2.PictureType!=0&&old.DW2.PictureType!=3)return 0;
            params.width=old.DW1.FrameWidthInPixelsMinusOne+1;params.height=old.DW1.FrameHeightInPixelsMinusOne+1;
            params.qp=old.DW27.QpPrimeYAc;params.transform_skip=old.DW2.TransformSkip;
            params.inter=old.DW2.PictureType==3;params.temporal_mvp=old.DW2.TemporalMvpEnableFlag;
            params.refs_l0=old.DW5.NumRefIdxL0Minus1+1;params.refs_l1=old.DW5.NumRefIdxL1Minus1+1;
            if(!params.inter)params.refs_l0=params.refs_l1=1;
            params.poc_l01=old.DW3.Value;params.poc_l2=old.DW4.Value;cmd2_count++;
        }
    }
    if(cmd2_count!=1)return 0;
    for(uint32_t i=0;i<in.used_dwords;i+=length(in.commands[i]))if(in.commands[i]==0x73900024)params.picture_qp=(in.commands[i+19]>>8)&63;
    if(params.inter){
        unsigned refs=0;
        for(uint32_t i=0;i<in.used_dwords;i+=length(in.commands[i]))if(in.commands[i]==0x73920010&&!(in.commands[i+1]&1)){
            for(unsigned k=0;k<3;k++)params.reference_ids[k]=(in.commands[i+2+k]>>8)&7;
            refs++;
        }
        if(refs!=1)return 0;
    }
    fprintf(stderr,"HEVC_GEN12_FRAME width=%u height=%u qp=%u inter=%u refs=%u\n",params.width,params.height,params.qp,params.inter,params.refs_l0);
    std::vector<reims::PacketReplacement> replacements;
    for(uint32_t i=0;i<in.used_dwords;){
        uint32_t n=length(in.commands[i]);
        std::vector<uint32_t> old(in.commands.begin()+i,in.commands.begin()+i+n),converted;
        uint32_t op=in.commands[i]&0xffff0000;
        bool required=op==0x73800000||op==0x73810000||op==0x73820000||op==0x70800000||
            op==0x70840000||op==0x708a0000||op==0x73900000||op==0x70890000||op==0x73880000||
            op==0x73940000||op==0x70880000||op==0x70870000||op==0x77800000;
        if(required){
            if(!reims::convert_hevc_idr_packet(old,params,converted)){fprintf(stderr,"HEVC_GEN12_REJECT packet=%x offset=%u\n",old[0],i);return 0;}
            reims::PacketReplacement r{i,n,std::move(converted),{}, {}};
            if(op==0x73800000)r.before={0x1d000000,0x03000300,0x708b0000,1,0x738a0001,1,0,0x68000100};
            if(op==0x77800000&&old[1]==0x10011)r.before={0x738a0001,0,4};
            if(op==0x73820000)r.before={0x73810003,0x50000000,0x20000000,0,0};
            if(op==0x73810000)r.address_dwords={{1,1},{2,2}};
            if(op==0x73820000)for(unsigned d:{1,7,10,16,19,25,28,31,37,39,41,43,45,47,49,51,54,57,60,63,66,68,70,72,74,76,78,80,95,98,101})r.address_dwords.push_back({d,d});
            if(op==0x70840000)for(unsigned d:{1,4,10,13,16,19,22,25,28,34,37,40,43,46,49,52})r.address_dwords.push_back({d,d});
            replacements.push_back(std::move(r));
        }
        i+=n;
    }
    // G12 on-chip row stores replace optional Gen11 off-chip bindings.
    uint32_t original_pipe=0;
    for(const auto&r:replacements)if(in.commands[r.old_dword]==0x73820077)original_pipe=r.old_dword;
    if(!original_pipe)return 0;
    for(size_t t=4;t<in.sideband.size();){
        uint32_t n=in.sideband[t]>>16;if(in.sideband[t]==0x200)break;
        if((in.sideband[t]&65535)==0x8200){
            size_t dst=t+3;
            for(size_t src=t+3;src+5<=t+n&&in.sideband[src];src+=5){
                uint32_t target=(in.sideband[src]>>5)&0xfffff;
                bool row=target==original_pipe+4||target==original_pipe+13||target==original_pipe+22;
                if(row){if((in.sideband[src]&0x8000001f)!=0x11)return 0;continue;}
                for(unsigned k=0;k<5;k++)in.sideband[dst+k]=in.sideband[src+k];dst+=5;
            }
            std::fill(in.sideband.begin()+dst,in.sideband.begin()+t+n,0);
        }
        t+=n;
    }
    reims::VideoBatch prepared;
    if(!reims::rewrite_video_batch(in,replacements,prepared)){fprintf(stderr,"HEVC_GEN12_REJECT rewrite\n");return 0;}
    // Gen12's reference readers require both U and V row offsets even for
    // interleaved NV12. Apple's Gen11 producer patches U only. Clone the
    // existing typed row-offset relocation so the kernel derives V from the
    // same live plane metadata, rather than guessing pitch/alignment on CPU.
    std::vector<uint32_t> uv_targets;uint32_t recon_surface=0,ref_surface=0;
    for(uint32_t i=0;i<prepared.used_dwords;i+=length(prepared.commands[i])){
        if(prepared.commands[i]==0x73810003){
            if((prepared.commands[i+1]>>28)==0)recon_surface=i;
            if((prepared.commands[i+1]>>28)==5)ref_surface=i;
        }
        if(prepared.commands[i]==0x70820004)uv_targets.push_back(i+4);
        if(prepared.commands[i]==0x70830008){uv_targets.push_back(i+4);uv_targets.push_back(i+8);}
    }
    unsigned uv_added=0;
    for(size_t t=4;t<prepared.sideband.size();){
        uint32_t n=prepared.sideband[t]>>16;if(prepared.sideband[t]==0x200)break;
        if((prepared.sideband[t]&65535)==0x8200){
            std::vector<uint32_t> added;size_t end=t+3;
            for(;end+5<=t+n&&prepared.sideband[end];end+=5){
                uint32_t target=(prepared.sideband[end]>>5)&0xfffff;
                if(std::find(uv_targets.begin(),uv_targets.end(),target)!=uv_targets.end()){
                    if((prepared.sideband[end]&0x8000001f)!=0x80000015)return 0;
                    added.insert(added.end(),prepared.sideband.begin()+end,prepared.sideband.begin()+end+5);
                    added[added.size()-5]=(added[added.size()-5]&~(0xfffffu<<5))|((target+1)<<5);uv_added++;
                }
                if(recon_surface&&ref_surface&&(target==recon_surface+1||target==recon_surface+2)){
                    uint32_t offset=target-recon_surface;
                    added.insert(added.end(),prepared.sideband.begin()+end,prepared.sideband.begin()+end+5);
                    added[added.size()-5]=(added[added.size()-5]&~(0xfffffu<<5))|((ref_surface+offset)<<5);
                    if(offset==1)added[added.size()-3]=(added[added.size()-3]&0x0fffffff)|0x50000000;
                    uv_added++;
                }
            }
            if(end+added.size()>=t+n)return 0;
            std::copy(added.begin(),added.end(),prepared.sideband.begin()+end);
        }
        t+=n;
    }
    if(uv_added!=5)return 0;
    uint32_t pipe=0,count=0;
    for(uint32_t i=0;i<prepared.used_dwords;i+=length(prepared.commands[i]))
        if(prepared.commands[i]==0x73820077){pipe=i;count++;}
    if(count!=1)return 0;
    const size_t wc=((params.width+63)/64)*4,hc=((params.height+63)/64)*4;
    // Current ADL-P/approved-driver cache partition is validated up to 2K.
    // Above that use the official off-chip line-buffer sizes, preserving
    // correct reconstructed neighbours without relying on the larger SRAM layout.
    const bool offchip=params.width>2048;
    const unsigned required_scratch=offchip?11:8;
    const size_t sizes[]={16*64*wc,18*64*hc,4*64*wc,4*64*hc,10*64*wc,18*64*hc,((params.width+31)/32)*128,((params.width+63)/64)*((params.height+63)/64)*4,8*64*wc,2*64*wc,5*64*wc};
    const uint32_t dwords[]={7,10,16,19,25,28};
    ReimsScratch scratch[11]={};
    unsigned created=0,rebound=0;
    for(unsigned i=0;i<required_scratch;i++){
        if(!reims_scratch_create(shared,sizes[i],&scratch[i]))break;
        created++;
    }
    if(created==required_scratch){
        for(size_t t=4;t<prepared.sideband.size();){
            uint32_t n=prepared.sideband[t]>>16;
            if(prepared.sideband[t]==0x200)break;
            if((prepared.sideband[t]&65535)==0x8200){
                for(size_t r=t+3;r+5<=t+n;r+=5){
                    if(!(prepared.sideband[r]&1))break;
                    uint32_t target=(prepared.sideband[r]>>5)&0xfffff;
                    for(unsigned k=0;k<6;k++)if(target==pipe+dwords[k]){
                        if(prepared.sideband[r]&0x80000000)continue;
                        prepared.sideband[r+1]=scratch[k].handle;rebound++;
                    }
                }
            }
            t+=n;
        }
        uint32_t vdenc=0,vdenc_count=0;
        for(uint32_t i=0;i<prepared.used_dwords;i+=length(prepared.commands[i]))
            if(prepared.commands[i]==0x70840045){vdenc=i;vdenc_count++;}
        if(vdenc_count==1 && rebound==6){
            // G12 AllocateEncResources and SetVdencPipeBufAddrStateParams
            // unconditionally bind these two new resources at DW62 and DW65.
            prepared.commands[vdenc+64]=0x12;
            for(size_t t=4;t<prepared.sideband.size();){
                uint32_t n=prepared.sideband[t]>>16;if(prepared.sideband[t]==0x200)break;
                if((prepared.sideband[t]&65535)==0x8200){
                    for(size_t q=t+3;q+10<t+n;q+=5)if(!prepared.sideband[q]){
                        prepared.sideband[q]=((vdenc+62)<<5)|0x11;prepared.sideband[q+1]=scratch[6].handle;
                        prepared.sideband[q+5]=((vdenc+65)<<5)|0x11;prepared.sideband[q+6]=scratch[7].handle;
                        rebound+=2;break;
                    }
                }
                t+=n;
            }
        }
        if(offchip&&rebound==8){
            for(size_t t=4;t<prepared.sideband.size();){
                uint32_t n=prepared.sideband[t]>>16;if(prepared.sideband[t]==0x200)break;
                if((prepared.sideband[t]&65535)==0x8200){
                    size_t q=t+3;while(q+5<=t+n&&prepared.sideband[q])q+=5;
                    if(q+15>=t+n){for(unsigned k=0;k<created;k++)reims_scratch_destroy(&scratch[k]);return 0;}
                    unsigned k=8;
                    for(unsigned slot:{4u,13u,22u}){
                        prepared.commands[pipe+slot]=prepared.commands[pipe+slot+1]=0;prepared.commands[pipe+slot+2]=0x12;
                        prepared.sideband[q]=((pipe+slot)<<5)|0x11;prepared.sideband[q+1]=scratch[k++].handle;q+=5;rebound++;
                    }
                }
                t+=n;
            }
        }
        if(rebound==required_scratch){
            save(dir,"gen12-candidate-commands.bin",prepared.commands);
            save(dir,"gen12-candidate-sideband.bin",prepared.sideband);
        }
    }
    if(io && created==required_scratch && rebound==required_scratch){
        unsigned event=0,event_count=0;
        for(size_t t=4;t<prepared.sideband.size();){
            if(prepared.sideband[t]==0x200)break;
            if(prepared.sideband[t]==0x38000){event=prepared.sideband[t+2];event_count++;}
            t+=prepared.sideband[t]>>16;
        }
        // ICL 0x21582 writes the allocated fence slot into 0x38000 DW2;
        // its caller passes that same slot to FinishFenceEvent (0x21486).
        // The separate submit output is NOT the fence slot.
        if(event_count==1 && out && error && prepared.commands.size()*4==cn && prepared.sideband.size()*4==sn){
            memcpy(const_cast<void*>(cb),prepared.commands.data(),cn);
            memcpy(const_cast<void*>(sb),prepared.sideband.data(),sn);
            fprintf(stderr,"HEVC_GEN12_SUBMIT event=%u used_dwords=%u scratch_retained=%u\n",event,prepared.used_dwords,created);
            int completed=reims_execute_owned_batch(shared,io,flags,event,out,error,
                scratch,created,prepared.sideband.data(),prepared.sideband.size());
            if(completed==2&&getenv("REIMS_HEVC_DUMP_SURFACES")){
                std::vector<uint32_t> patched(cn/4);memcpy(patched.data(),cb,cn);char name[80];snprintf(name,sizeof(name),"post-submit-%u.bin",event);save(dir,name,patched);
            }
            // The owner either completed and released its resources, or kept
            // them pending a real context drain. Unadopted scratch is still ours.
            for(unsigned i=0;i<created;i++)reims_scratch_destroy(&scratch[i]);
            if(completed==2){reims_dump_surfaces(dir);fprintf(stderr,"HEVC_GEN12_COMPLETED event=%u\n",event);}
            return completed;
        }
    }
    for(unsigned i=0;i<created;i++)reims_scratch_destroy(&scratch[i]);
    fprintf(stderr,"HEVC_GEN12_PREPARED replacements=%zu old_dwords=%u new_dwords=%u scratch_created=%u rebound=%u gpu_submitted=0 handles_released=1\n",replacements.size(),in.used_dwords,prepared.used_dwords,created,rebound);
    return created==required_scratch&&rebound==required_scratch;
}

extern "C" int reims_prepare_gen12_probe(void*s,const void*c,size_t cn,const void*b,size_t bn,const char*d){
    return prepare(s,c,cn,b,bn,d,nullptr,0,nullptr,nullptr);
}
extern "C" int reims_submit_gen12_probe(void*s,const void*c,size_t cn,const void*b,size_t bn,const char*d,
                                       void*io,unsigned flags,unsigned*out,unsigned*error){
    return prepare(s,c,cn,b,bn,d,io,flags,out,error);
}
