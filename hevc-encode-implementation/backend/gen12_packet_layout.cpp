#include "gen12_packet_layout.h"
#include "official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_hcp_hwcmd_g12_X.h"
#include "official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_vdenc_hwcmd_g12_X.h"
#include <cstring>
#include "command-catalog/gen12_rdoq_tables.h"
namespace reims {
using H=mhw_vdbox_hcp_g12_X;
using V=mhw_vdbox_vdenc_g12_X;
template<class T> static std::vector<uint32_t> pack(const T &p){std::vector<uint32_t>v(sizeof(T)/4);memcpy(v.data(),&p,sizeof(T));return v;}
template<class T> static T prefix(const std::vector<uint32_t>&old){
    T t; // Upstream defaults for newly introduced fields, never unspecified padding.
    memcpy((char*)&t+4,old.data()+1,(old.size()-1)*4);return t;
}
bool convert_hevc_idr_packet(const std::vector<uint32_t>&old,const HevcIdrCqp&p,std::vector<uint32_t>&out){
    if(old.empty() || (old[0]&4095)+2!=old.size() || p.width>4096)return false;
    HevcVdencState states;
    if(!build_hevc_idr_cqp(p,states))return false;
    switch(old[0]){
    case 0x77800000:{
        if(old.size()!=2 || (old[1]!=0x2001a&&old[1]!=0x10011))return false;
        out=old;
        // Gen12 slice drain flushes both VDEnc and HCP before MI_FLUSH_DW.
        // Retain every original wait bit; do not bypass completion.
        if(old[1]==0x2001a)out[1]|=0x10000;
        return true;
    }
    case 0x708a001c:out=std::move(states.cmd1);return true;
    case 0x70890023:out=std::move(states.cmd2);return true;
    case 0x73800004:{
        auto c=prefix<H::HCP_PIPE_MODE_SELECT_CMD>(old);
        if(!c.DW1.CodecSelect || !c.DW1.VdencMode || c.DW1.CodecStandardSelect || c.DW1.MultiEngineMode || c.DW1.PipeWorkingMode)return false;
        out=pack(c);return true;
    }
    case 0x73810002:{out=pack(prefix<H::HCP_SURFACE_STATE_CMD>(old));return true;}
    case 0x70800000:{
        auto c=prefix<V::VDENC_PIPE_MODE_SELECT_CMD>(old);
        if(c.DW1.StandardSelect || c.DW1.BitDepth || c.DW1.ScalabilityMode || c.DW1.VdencStreamInEnable)return false;
        // Gen12 CQP/no lookahead does not request VDEnc BRC frame statistics.
        c.DW1.FrameStatisticsStreamOutEnable=0;
        c.DW2.HmeRegionPreFetchenable=0;
        c.DW3.PreFetchoffsetforsource=4;c.DW3.Numverticalreqminus1Src=1;
        out=pack(c);return true;
    }
    case 0x7084003c:{
        // DW1..61 preserve the same Gen12 resource slots. The Gen11 third
        // downscaled reference becomes reserved; IDR must not bind it.
        if(old[7]||old[8]||old[9])return false;
        out=pack(prefix<V::VDENC_PIPE_BUF_ADDR_STATE_CMD>(old));return true;
    }
    case 0x73820077:{
        auto c=prefix<H::HCP_PIPE_BUF_ADDR_STATE_CMD>(old);
        // Main8/420/CTU64 up to 4K: official G12 RowStoreCacheAddrHEVC[1].
        // Gen11 may disable its smaller row stores above 2048 pixels.
        // Gen12's verified 4K Main8 row-store layout remains enabled here;
        // prepare removes the obsolete Gen11 off-chip relocations first.
        c.DeblockingFilterLineBuffer.DW0_1.Value[0]=c.DeblockingFilterLineBuffer.DW0_1.Value[1]=0;
        c.MetadataLineBuffer.DW0_1.Value[0]=c.MetadataLineBuffer.DW0_1.Value[1]=0;c.SaoLineBuffer.DW0_1.Value[0]=c.SaoLineBuffer.DW0_1.Value[1]=0;
        c.DeblockingFilterLineBuffer.DW0_1.Graphicsaddress476=256;
        c.MetadataLineBuffer.DW0_1.Graphicsaddress476=0;
        c.SaoLineBuffer.DW0_1.Graphicsaddress476=1280;
        auto words=pack(c);for(unsigned d:{6u,15u,24u})words[d]=0x1000;
        out=std::move(words);return true;
    }
    case 0x73900024:{
        auto c=prefix<H::HCP_PIC_STATE_CMD>(old);
        if(c.DW2.Mincusize || c.DW2.CtbsizeLcusize!=3 || c.DW2.ChromaSubsampling!=1 ||
           c.DW4.TilesEnabledFlag || c.DW4.WeightedPredFlag || c.DW4.WeightedBipredFlag ||
           c.DW5.BitDepthLumaMinus8 || c.DW5.BitDepthChromaMinus8 || !c.DW4.CuQpDeltaEnabledFlag)return false;
        if((c.DW1.Framewidthinmincbminus1+1)*8!=p.width || (c.DW1.Frameheightinmincbminus1+1)*8!=p.height)return false;
        if(old[34]||old[35]||old[36])return false;
        c.DW19.TemporalMvPredDisable=!p.temporal_mvp;
        c.DW19.Nalunittypeflag=0; // Gen12 enables this only for dynamic slicing.
        c.DW19.PartialFrameUpdateMode=0;
        c.DW37.Rdoqintratuthreshold=0; // Gen12 builder leaves this at default.
        out=pack(c);return true;
    }
    case 0x73880080:{
        H::HEVC_VP9_RDOQ_STATE_CMD c;
        H::HEVC_VP9_RDOQ_LAMBDA_FIELDS_CMD *groups[]={c.Intralumalambda,c.Intrachromalambda,c.Interlumalambda,c.Interchromalambda};
        for(unsigned g=0;g<4;g++)for(unsigned i=0;i<32;i++){
            groups[g][i].DW0.Lambdavalue0=reims_rdoq[p.inter][g][i*2];
            groups[g][i].DW0.Lambdavalue1=reims_rdoq[p.inter][g][i*2+1];
        }
        out=pack(c);return true;
    }
    case 0x73940009:{
        auto c=prefix<H::HCP_SLICE_STATE_CMD>(old);
        if(c.DW3.SliceType!=(p.inter?0:2) || !c.DW3.Lastsliceofpic || c.DW3.Sliceqp!=p.qp)return false;
        out=pack(c);return true;
    }
    case 0x70880003:{
        V::VDENC_WEIGHTSOFFSETS_STATE_CMD c;
        // G11 HEVC forward/backward fields lived in DW3/4, not AVC DW1/2.
        c.DW1.Value=old[3];c.DW2.Value=old[4];out=pack(c);return true;
    }
    case 0x70870008:{
        auto c=prefix<V::VDENC_WALKER_STATE_CMD>(old);
        if(old[1]||old[3]||old[4]||old[6]||old[7]||old[8]||old[9])return false;
        const uint32_t wc=(p.width+63)/64,hc=(p.height+63)/64;
        c.DW2.NextsliceMbLcuStartXPosition=wc;c.DW2.NextsliceMbStartYPosition=hc;
        c.DW5.TileWidth=p.width-1;c.DW5.TileHeight=p.height-1;
        out=pack(c);return true;
    }
    default:return false;
    }
}
}
