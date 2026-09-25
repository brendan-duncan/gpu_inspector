// --bindless: cube.hlsl's pixel shader, also reading a texture straight out of the descriptor heap
// (shader model 6.6) at the slot the root constants name, which no root table covers.
#define BINDLESS 1
#include "cube.hlsl"
