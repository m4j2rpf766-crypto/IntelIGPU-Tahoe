#include "video_batch_rewrite.h"
#include <algorithm>
#include <limits>

namespace reims {
bool rewrite_video_batch(const VideoBatch &in, const std::vector<PacketReplacement> &replacements, VideoBatch &out) {
    const uint32_t missing=std::numeric_limits<uint32_t>::max();
    if (!in.used_dwords || in.used_dwords>in.commands.size() ||
        in.commands[in.used_dwords-1]!=0x05000000 || in.sideband.size()<6 ||
        std::any_of(in.commands.begin()+in.used_dwords,in.commands.end(),[](uint32_t w){return w!=0;})) return false;
    std::vector<bool> boundary(in.used_dwords,false);
    for(uint32_t i=0;i<in.used_dwords;) {
        boundary[i]=true;
        const uint32_t h=in.commands[i],type=h>>29,op=(h>>23)&63;
        uint32_t n=0;
        if(type==0) {
            if(op==0 || op==10)n=1;
            else if(op==32 || op==36 || op==38 || op==46)n=(h&63)+2;
            else return false;
            if(op==10 && i+1!=in.used_dwords)return false;
        } else if((h&0xffff0000)==0x68000000)n=1;
        else if(type==3)n=(h&4095)+2;
        else return false;
        if(n>in.used_dwords-i)return false;
        i+=n;
    }
    std::vector<uint32_t> map(in.used_dwords,missing), rebuilt;
    uint32_t pos=0;
    for(const auto &r:replacements) {
        if(r.old_dword<pos || !r.old_length || r.old_dword>=in.used_dwords || !boundary[r.old_dword] ||
           r.old_length>in.used_dwords-r.old_dword || r.words.empty())return false;
        // Replacements must begin with a media packet, and must account for
        // its entire encoded length. MI/fence commands cannot be replaced.
        uint32_t old_header=in.commands[r.old_dword],new_header=r.words[0];
        if(old_header>>29!=3 || new_header>>29!=3 ||
           (old_header&0xffff0000)!=(new_header&0xffff0000) ||
           (old_header&4095)+2!=r.old_length || (new_header&4095)+2!=r.words.size())return false;
        while(pos<r.old_dword){map[pos]=rebuilt.size();rebuilt.push_back(in.commands[pos++]);}
        if(!r.before.empty()){
            // Audited Gen12 no-address prolog only. No arbitrary MI writes,
            // synchronization replacement or address-bearing insertions.
            const std::vector<uint32_t> init={0x1d000000,0x03000300,0x708b0000,1,0x738a0001,1,0,0x68000100};
            const std::vector<uint32_t> implicit_flush={0x738a0001,0,4};
            const std::vector<uint32_t> reference_surface={0x73810003,0x50000000,0x20000000,0,0};
            if(r.before!=init && !(old_header==0x77800000&&r.before==implicit_flush) &&
               !(old_header==0x73820077&&r.before==reference_surface))return false;
            rebuilt.insert(rebuilt.end(),r.before.begin(),r.before.end());
        }
        for(auto pair:r.address_dwords){
            if(pair.first==0 || pair.second==0 || pair.first>=r.old_length || pair.second>=r.words.size() ||
               map[pos+pair.first]!=missing)return false;
            map[pos+pair.first]=rebuilt.size()+pair.second;
        }
        rebuilt.insert(rebuilt.end(),r.words.begin(),r.words.end());
        pos+=r.old_length;
        if(rebuilt.size()>in.commands.size())return false;
    }
    while(pos<in.used_dwords){map[pos]=rebuilt.size();rebuilt.push_back(in.commands[pos++]);}
    if(rebuilt.size()>in.commands.size())return false;
    VideoBatch result;
    result.used_dwords=rebuilt.size();
    rebuilt.resize(in.commands.size(),0);
    result.commands=std::move(rebuilt);
    result.sideband=in.sideband;
    auto &s=result.sideband;
    bool used_token=false,terminal=false;
    for(size_t t=4;t<s.size();) {
        const uint32_t token=s[t],n=token>>16,kind=token&65535;
        if(token==0x200 && t+2==s.size() && s[t+1]==in.commands.size()) {terminal=true;break;}
        if(n<2 || n>s.size()-t)return false;
        if(kind==0x8200) {
            // Currently audited producer uses a single command buffer base.
            if(n<3 || s[t+1]!=0 || s[t+2]!=0)return false;
            for(size_t r=t+3;r<t+n;r+=5) {
                if(std::all_of(s.begin()+r,s.begin()+t+n,[](uint32_t w){return w==0;}))break;
                if(t+n-r<5 || !(s[r]&1))return false;
                uint32_t target=(s[r]>>5)&0xfffff;
                if(target>=map.size() || map[target]==missing || map[target]>0xfffff)return false;
                s[r]=(s[r]&~(0xfffffu<<5)) | (map[target]<<5);
            }
        } else if(kind==0x8000) {
            if(n!=3 || used_token || s[t+1]!=in.used_dwords)return false;
            s[t+1]=result.used_dwords;used_token=true;
        } else if(!((kind==0x100 && n==2) || (kind==0 && n==3))) {
            return false;
        }
        t+=n;
    }
    if(!terminal || !used_token)return false;
    out=std::move(result);
    return true;
}
}
