// --local-root: raytrace.hlsl with the tinted hit group's color coming through its local root
// arguments -- a root constant, a root CBV and a descriptor table, one of each kind a binding table
// record can hold -- so a capture's records carry the captured process's GPU addresses and GPU
// descriptor handles, which the replay has to turn into its own.
#define LOCAL_ROOT 1
#include "raytrace.hlsl"
