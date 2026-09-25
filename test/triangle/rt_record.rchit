#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference : require

// --shader-record: rt.rchit, tinted by a buffer only the hit group's shader record names, by its
// device address -- bound in no descriptor set, so a replay has it only if the capture read it back
// and wrote the replay's own address into the record.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Tint {
    vec4 color;
};
layout(shaderRecordEXT, std430) buffer Record {
    Tint tint;
};

layout(location = 0) rayPayloadInEXT vec3 color;
hitAttributeEXT vec2 barycentrics;

void main() {
    color = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y) * tint.color.rgb;
}
