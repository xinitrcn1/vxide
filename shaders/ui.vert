#version 450
// One instance = one axis-aligned quad. Six vertices per instance.
layout(location = 0) in ivec4 in_rect; // x, y, w, h in pixels
layout(location = 1) in uvec2 in_uv;   // atlas texel origin; u = 0xFFFF solid, 0xFFFE squiggle
layout(location = 2) in vec4 in_color; // straight alpha

layout(push_constant) uniform PC { vec2 inv_half_size; vec4 bg; } pc;

layout(location = 0) flat out uvec2 v_uv;
layout(location = 1) flat out vec4 v_color;
layout(location = 2) noperspective out vec2 v_local;

void main()
{
	const vec2 corner[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1),
	                               vec2(1, 0), vec2(1, 1), vec2(0, 1));
	vec2 c = corner[gl_VertexIndex];
	vec2 size = vec2(in_rect.zw);
	vec2 p = vec2(in_rect.xy) + c * size;
	gl_Position = vec4(p * pc.inv_half_size - 1.0, 0.0, 1.0);
	v_uv = in_uv;
	v_color = in_color.wzyx; // u32 0xRRGGBBAA, little-endian bytes A B G R
	v_local = c * size;
}
