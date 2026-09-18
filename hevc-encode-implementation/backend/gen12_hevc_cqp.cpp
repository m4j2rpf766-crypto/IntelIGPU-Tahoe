/*
 * Portions Copyright (c) 2017-2020, Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "gen12_hevc_cqp.h"
#include "official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_vdenc_hwcmd_g12_X.h"
#include <cstring>
#include <utility>

namespace reims {
template<class T> static std::vector<uint32_t> serialize(const T &cmd) {
    static_assert(sizeof(T) % 4 == 0, "DWORD command alignment");
    std::vector<uint32_t> words(sizeof(T)/4);
    std::memcpy(words.data(), &cmd, sizeof(T));
    return words;
}

// Specialized from MhwVdboxVdencInterfaceG12X::AddVdencCmd1/2Cmd at pinned
// fba6a82d, lines 2320-2841. I_TYPE, no ACQP/default-QP-deltas override.
// No Gen11 opaque words are reused. The packed constructors are upstream.
bool build_hevc_idr_cqp(const HevcIdrCqp &p, HevcVdencState &out) {
    // This entry does not implement crop/padding or a distinct coded extent.
    if (!p.width || !p.height || p.width > 8192 || p.height > 8192 ||
        p.width % 8 || p.height % 8 || p.qp < 10 || p.qp > 51 ||
        !p.refs_l0 || p.refs_l0>3 || p.refs_l1!=1 || (p.inter&&!p.low_delay) ||
        (p.target_usage!=1 && p.target_usage!=4 && p.target_usage!=7) ||
        (p.rounding && (p.round_inter<2 || p.round_inter>4 || p.round_intra<8 || p.round_intra>10))) return false;

    mhw_vdbox_vdenc_g12_X::VDENC_CMD1_CMD a;
    a.DW1.Value=0x05030200; a.DW2.Value=0x0b090806;
    a.DW3.Value=0x1c140c04; a.DW4.Value=0x3c342c24; a.DW5.Value=0x5c544c44;
    a.DW6.Value=0x1c140c04; a.DW7.Value=0x3c342c24; a.DW8.Value=0x5c544c44;
    a.DW14.Value=0; a.DW15.Value=0; a.DW16.Value &= 0xffff0000;
    a.DW19.Value=(a.DW19.Value & 0xff0000ff) | 0x140400;
    a.DW20.Value=a.DW21.Value=0x14141414;
    a.DW22.Value=a.DW23.Value=a.DW24.Value=a.DW25.Value=0x10101010;
    a.DW26.Value=a.DW27.Value=a.DW28.Value=a.DW29.Value=0x10101010;
    a.DW10.Value=0x23131f0f;
    a.DW11.Value=(a.DW11.Value & 0xffff0000) | 0x2313;
    a.DW12.Value=0x3e5c445c;
    a.DW13.Value=(a.DW13.Value & 0xff00) | 0x1e040044;
    a.DW16.Value=(a.DW16.Value & 0xffff) | 0x70000;
    a.DW17.Value=0x0d0e1007;
    a.DW18.Value=(a.DW18.Value & 0xffff0000) | 0x1e32;
    a.DW30.Value=(a.DW30.Value & 0xff000000) | 0x101010;
    if(p.quality_improvement && p.qp>=22){
        static const uint32_t q[]={0,0x60000,0xc0000,0x120000,0x190000,0x1f0000,0x250000,0x2c0000,0x320000,0x380000};
        a.DW14.Value=(a.DW14.Value&0xff00ffff) | (p.qp<32?q[p.qp-22]:0x3f0000);
    }

    mhw_vdbox_vdenc_g12_X::VDENC_CMD2_CMD b;
    b.DW2.Value=(b.DW2.Value & 0xdff00000) | 0x2005aff3;
    b.DW3.Value=0xfe02ff01; b.DW4.Value=0xfc04fd03;
    b.DW5.Value=(b.DW5.Value & 0xff7e03ff) | 0x80ac00;
    b.DW7.Value=(b.DW7.Value & 0xffd90ff0) | 0x62003;
    b.DW9.Value=(b.DW9.Value & 0xffff) | 0x43840000;
    b.DW12.Value=0xffffffff; b.DW15.Value=0x4e201f40;
    b.DW16.Value=(b.DW16.Value & 0xf0ff0000) | 0x0f003300;
    b.DW17.Value=(b.DW17.Value & 0xfff00000) | 0x2710;
    b.DW19.Value=(b.DW19.Value & 0x80ffffff) | 0x18000000;
    b.DW21.Value &= 0x0fffffff;
    b.DW22.Value=0x1f001102; b.DW23.Value=0xaaaa1f00;
    b.DW27.Value=(b.DW27.Value & 0xffff0000) | 0x1a1a;
    b.DW1.FrameWidthInPixelsMinusOne=p.width-1;
    b.DW1.FrameHeightInPixelsMinusOne=p.height-1;
    b.DW2.PictureType=0; b.DW2.TemporalMvpEnableFlag=0;
    b.DW2.TransformSkip=p.transform_skip;
    b.DW5.NumRefIdxL0Minus1=0; b.DW5.NumRefIdxL1Minus1=0;
    b.DW5.Value=(b.DW5.Value & 0xff83ffff) | 0x400000;
    b.DW14.Value=(b.DW14.Value & 0xffff) | 0x07d00000;
    b.DW18.Value=(b.DW18.Value & 0xffff) | 0x600000;
    b.DW19.Value=(b.DW19.Value & 0xffff0000) | 0xc0;
    b.DW20.Value &= 0xfffeffff;
    b.DW7.TilingEnable=0; b.DW37.TileReplayEnable=0;
    b.DW3.Value=b.DW4.Value=0;
    if(p.target_usage==1){
        b.DW2.Value=(b.DW2.Value & 0xdffffffc) | 2;
        b.DW7.Value=(b.DW7.Value & 0xfffff6ff) | 0x800;
        b.DW9.Value=(b.DW9.Value & 0x000fffff) | 0x43800000;
        b.DW12.Value=0xffffffff;
        b.DW34.Value=(b.DW34.Value & 0x00ffffff) | 0x21000000;
    } else if(p.target_usage==4){
        b.DW2.Value &= 0xdfffffff;b.DW7.Value &= 0xfffffeff;
        b.DW9.Value=(b.DW9.Value & 0x000fffff) | 0x43800000;
        b.DW12.Value=0xce4014a0;
        b.DW34.Value=(b.DW34.Value & 0x00ffffff) | 0x21000000;
    } else {
        b.DW2.Value=(b.DW2.Value & 0xdfffffff) | 0x20000000;
        b.DW7.Value=(b.DW7.Value & 0xfff7feff) | 0x80100;
        b.DW9.Value=(b.DW9.Value & 0xffff) | 0x22420000;
        b.DW12.Value=0x89800dc0;
        b.DW34.Value=(b.DW34.Value & 0x00ffffff) | 0x20000000;
    }
    b.DW7.Value=(b.DW7.Value & 0xfff7ffff) | 0x80000;
    if(!p.low_delay)b.DW7.Value &= 0xfff7feff;
    b.DW7.VdencStreamInEnable=0; b.DW7.PakOnlyMultiPassEnable=0;
    b.DW8.Value=0x54555555;
    b.DW9.Value=(b.DW9.Value & 0xffff0000) | 0x5555;
    if(p.low_delay){b.DW8.Value=0;b.DW9.Value &= 0xffff0000;}
    b.DW16.MinQp=10; b.DW16.MaxQp=51;
    b.DW17.TemporalMVEnableForIntegerSearch=0;
    static const uint32_t lambda[42]={
        0x30002,0x30002,0x30002,0x30003,0x40004,0x40005,0x50006,
        0x60008,0x6000a,0x7000c,0x8000f,0x90013,0xa0018,0xb001e,
        0xc0026,0xe0030,0x10003d,0x12004d,0x140061,0x16007a,
        0x19009a,0x1c00c2,0x1f00f4,0x230133,0x270183,0x2c01e8,
        0x320266,0x380306,0x3e03cf,0x4604cd,0x4f060c,0x58079f,
        0x63099a,0x6f0c18,0x7d0f3d,0x8c1333,0x9d1831,0xb11e7a,
        0xc62666,0xdf3062,0xfa3cf5,0x1184ccd};
    b.DW26.Value=(b.DW26.Value & 0xfe000000) | lambda[p.qp-10];
    if(p.quality_improvement && p.qp>=22){
        static const uint32_t sad[30]={0xa,0xb,0xd,0xf,0x11,0x14,0x17,0x1a,0x1e,0x22,0x27,0x2d,0x33,0x3b,0x43,0x4d,0x57,0x64,0x72,0x82,0x95,0xa7,0xbb,0xd2,0xec,0x109,0x129,0x14e,0x177,0x1a5};
        b.DW26.SadQpLambda=sad[p.qp-22];

    }
    int picture_qp=p.picture_qp<0?p.qp:p.picture_qp;
    if(p.quality_improvement&&picture_qp>=22&&picture_qp<=51)b.DW6.Value=(b.DW6.Value&0xc00fffff)|0x1fb00000;
    b.DW27.QpPrimeYAc=p.qp;
    b.DW27.Value &= 0xffffff00;
    b.DW35.Value=(b.DW35.Value & 0xfffff0ff) | 0x700;
    if(p.rounding){
        b.DW28.Value=0x07d00fa0;b.DW29.Value=0x02bc0bb8;
        b.DW30.Value=0x032003e8;b.DW31.Value=0x01f4012c;
        static const uint32_t t32[3][3]={{0x88220000,0x99220000,0xaa220000},{0x88330000,0x99330000,0xaa330000},{0x88440000,0x99440000,0xaa440000}};
        static const uint32_t t33[3][3]={{0x22882222,0x22992222,0x22aa2222},{0x33883333,0x33993333,0x33aa3333},{0x44884444,0x44994444,0x44aa4444}};
        static const uint32_t t34[3][3]={{0x228822,0x229922,0x22aa22},{0x338833,0x339933,0x33aa33},{0x448844,0x449944,0x44aa44}};
        b.DW32.Value=t32[p.round_inter-2][p.round_intra-8]|0x190;
        b.DW33.Value=t33[p.round_inter-2][p.round_intra-8];
        b.DW34.Value=(b.DW34.Value&0xff000000)|t34[p.round_inter-2][p.round_intra-8];
    }
    // Invalid reference frame indices for this IDR; unused forward slots 1/2
    // are the upstream 7 sentinel, and Gen12 enables its reference-ID mapping.
    b.DW11.Value=(b.DW11.Value & 0x7f0000ff) | 0x80070700;
    if(p.inter){
        a.DW10.Value=0x23131f0f;a.DW11.Value=0x331b2313;
        a.DW12.Value=0x476e4d6e;a.DW13.Value=0x3604004d;
        a.DW16.Value=(a.DW16.Value&0xffff)|0x04150000;
        a.DW17.Value=0x23231415;a.DW18.Value=(a.DW18.Value&0xffff0000)|0x443f;
        a.DW30.Value=(a.DW30.Value&0xff000000)|0x232323;
        b.DW2.PictureType=3;b.DW2.TemporalMvpEnableFlag=p.temporal_mvp;
        b.DW3.Value=p.poc_l01;b.DW4.Value=p.poc_l2;
        b.DW5.NumRefIdxL0Minus1=p.refs_l0-1;b.DW5.NumRefIdxL1Minus1=p.refs_l1-1;b.DW5.SubPelMode=3;
        if(p.target_usage==1){b.DW2.Value=(b.DW2.Value&0xfffffffc)|3;b.DW7.Value&=0xfffff7ff;}
        if(p.refs_l0>1&&p.target_usage!=7)b.DW7.Value&=0xfff7ffff;
        b.DW17.TemporalMVEnableForIntegerSearch=p.temporal_mvp;
        static const uint32_t inter_lambda[42]={
          0x30003,0x30003,0x30003,0x40003,0x40004,0x50005,0x50007,0x60008,0x6000a,0x7000d,0x80011,0x90015,0xa001a,0xb0021,
          0xd002a,0xe0034,0x100042,0x120053,0x140069,0x170084,0x1a00a6,0x1d00d2,0x210108,0x24014d,0x2901a3,0x2e0210,
          0x34029a,0x3a0347,0x410421,0x490533,0x52068d,0x5c0841,0x670a66,0x740d1a,0x821082,0x9214cd,0xa41a35,0xb82105,
          0xce299a,0xe8346a,0x1044209,0x1245333};
        b.DW26.Value=(b.DW26.Value&0xfe000000)|inter_lambda[p.qp-10];
        b.DW11.Value=0x80000000;
        for(unsigned i=0;i<3;i++){
            if(p.reference_ids[i]>7)return false;
            b.DW11.Value|=uint32_t(i<p.refs_l0?p.reference_ids[i]:7)<<(i*8);
        }
    }
    HevcVdencState result{serialize(a),serialize(b)};
    out=std::move(result);
    return true;
}
}
