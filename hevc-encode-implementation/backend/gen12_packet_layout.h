#pragma once
#include "gen12_hevc_cqp.h"
#include <vector>
namespace reims {
// Convert one of the audited Main8/CTU64 IDR media packets. This supplies
// command state only; the caller must rewrite relocations, replace undersized
// scratch, add platform prolog, and validate completion before submission.
bool convert_hevc_idr_packet(const std::vector<uint32_t>&,const HevcIdrCqp&,std::vector<uint32_t>&);
}
