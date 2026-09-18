#include "video_batch_rewrite.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstring>
#include <cassert>
#include <algorithm>

static std::vector<uint32_t> read(const char *p){
    std::ifstream f(p,std::ios::binary);
    std::vector<char>b((std::istreambuf_iterator<char>(f)),{});
    assert(!b.empty() && b.size()%4==0);
    std::vector<uint32_t>w(b.size()/4);std::memcpy(w.data(),b.data(),b.size());return w;
}
int main(int argc,char **argv){
    assert(argc==3);
    reims::VideoBatch original{read(argv[1]),read(argv[2]),5064/4},out;
    assert(reims::rewrite_video_batch(original,{},out));
    assert(original.commands==out.commands && original.sideband==out.sideband);
    // Expand PIPE_MODE_SELECT 6->7 DWORDs. All 57 relocations after it must
    // move by one DWORD without any changes to their handles/addends/access.
    reims::PacketReplacement p{0x2c/4,6,{0x73800005,0,0,0,0,0,0},{}};
    assert(reims::rewrite_video_batch(original,{p},out));
    assert(out.used_dwords==original.used_dwords+1);
    unsigned checked=0;
    for(size_t r=48/4;r<1188/4;r+=5){
        uint32_t before=(original.sideband[r]>>5)&0xfffff;
        uint32_t after=(out.sideband[r]>>5)&0xfffff;
        assert(after==before+(before>=0x44/4));
        assert((out.sideband[r]&~(0xfffffu<<5))==(original.sideband[r]&~(0xfffffu<<5)));
        for(unsigned j=1;j<5;j++)assert(out.sideband[r+j]==original.sideband[r+j]);
        checked++;
    }
    assert(out.sideband[4132/4+1]==out.used_dwords);
    // Replacing an address-bearing packet without an explicit field mapping
    // is rejected; never assume that a generation change preserves offsets.
    reims::PacketReplacement address{0x68/4,121,{}, {}};
    address.words.assign(original.commands.begin()+address.old_dword,
                         original.commands.begin()+address.old_dword+address.old_length);
    auto prior=out;
    assert(!reims::rewrite_video_batch(original,{address},out));
    assert(out.commands==prior.commands && out.sideband==prior.sideband);
    for(size_t r=48/4;r<1188/4;r+=5){
        uint32_t target=(original.sideband[r]>>5)&0xfffff;
        auto pair=std::make_pair(target-address.old_dword,target-address.old_dword);
        if(target>=address.old_dword && target<address.old_dword+address.old_length &&
           std::find(address.address_dwords.begin(),address.address_dwords.end(),pair)==address.address_dwords.end())
            address.address_dwords.push_back(pair);
    }
    assert(reims::rewrite_video_batch(original,{address},out));
    assert(out.commands==original.commands && out.sideband==original.sideband);
    auto bad=original;bad.sideband[48/4]=(bad.sideband[48/4]&~(0xfffffu<<5))|(0xfffffu<<5);
    assert(!reims::rewrite_video_batch(bad,{},out));
    bad=original;bad.sideband[4132/4+1]--;
    assert(!reims::rewrite_video_batch(bad,{},out));
    p.old_dword++;assert(!reims::rewrite_video_batch(original,{p},out));
    p.old_dword--;p.words.resize(original.commands.size()+1);
    assert(!reims::rewrite_video_batch(original,{p},out));
    printf("PASS identity, expansion and %u preserved relocations; malformed inputs rejected\n",checked);
}
