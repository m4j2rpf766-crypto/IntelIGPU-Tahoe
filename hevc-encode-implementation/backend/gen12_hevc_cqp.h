#pragma once
#include <cstdint>
#include <vector>

namespace reims {
// Main/NV12, single pipe, one IDR or low-delay inter slice; no tiles/ROI/IBC.
// Packet builders only: native session rate control supplies the per-frame QP.
struct HevcIdrCqp {
    uint32_t width, height;
    uint8_t qp;
    bool transform_skip;
    uint8_t target_usage=1;
    bool low_delay=false;
    bool quality_improvement=false;
    bool rounding=false;
    uint8_t round_inter=4,round_intra=10;
    bool inter=false, temporal_mvp=false;
    uint8_t refs_l0=1, refs_l1=1, reference_ids[3]={0,7,7};
    uint32_t poc_l01=0, poc_l2=0;
    int picture_qp=-1;
};
struct HevcVdencState {
    std::vector<uint32_t> cmd1, cmd2;
};
// On invalid input, output is unchanged. No resources or GPU work are created.
bool build_hevc_idr_cqp(const HevcIdrCqp &, HevcVdencState &);
}
