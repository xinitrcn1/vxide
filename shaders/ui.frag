#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas; // R8 coverage
layout(set = 0, binding = 1) uniform sampler2D bgimg; // background image, RGBA
layout(set = 0, binding = 2) uniform sampler2D pics;  // picture atlas, RGBA (plots, animations)
// per picture placement: (screen x, screen y, texels per pixel, 0), (u0, v0, u1, v1)
layout(std140, set = 0, binding = 3) uniform Pics { vec4 place[512]; } P;
// bg = image origin on screen (px) and 1 / its size on screen; pic_base =
// this frame's first placement
layout(push_constant) uniform PC { vec2 inv_half_size; vec4 bg; uint pic_base; } pc;

layout(location = 0) flat in uvec2 v_uv;
layout(location = 1) flat in vec4 v_color;
layout(location = 2) noperspective in vec2 v_local;

layout(location = 0) out vec4 o_color;

void main()
{
	float cov;
	if (v_uv.x >= 0xC000u && v_uv.x < 0xFFFDu) {
		// a picture, sampled by pixel position (clipping needs no texture coordinates)
		uint k = (pc.pic_base + (v_uv.x - 0xC000u)) * 2u;
		vec4 a = P.place[k], b = P.place[k + 1u];
		vec2 t = b.xy + (gl_FragCoord.xy - a.xy) * a.z;
		vec2 lo = b.xy + 0.5, hi = b.zw - 0.5;
		vec2 inv = 1.0 / vec2(textureSize(pics, 0));
		vec4 c;
		if (a.z > 1.25) {
			// shrunk: four bilinear taps over the pixel's footprint
			float d = 0.25 * a.z;
			c = 0.25 * (texture(pics, clamp(t + vec2(-d, -d), lo, hi) * inv) +
			            texture(pics, clamp(t + vec2(d, -d), lo, hi) * inv) +
			            texture(pics, clamp(t + vec2(-d, d), lo, hi) * inv) +
			            texture(pics, clamp(t + vec2(d, d), lo, hi) * inv));
		} else {
			c = texture(pics, clamp(t, lo, hi) * inv);
		}
		o_color = vec4(c.rgb, c.a * v_color.a);
		return;
	}
	if (v_uv.x == 0xFFFDu) {
		vec4 c = texture(bgimg, (gl_FragCoord.xy - pc.bg.xy) * pc.bg.zw);
		o_color = vec4(c.rgb, c.a * v_color.a);
		return;
	}
	if (v_uv.x == 0xFFFFu) {
		cov = 1.0;
	} else if (v_uv.x == 0xFFFEu) {
		// Diagnostic squiggle: triangle wave, period 6 px, amplitude 2 px.
		float t = mod(v_local.x, 6.0) / 3.0;
		float y = 0.5 + 2.0 * (t < 1.0 ? t : 2.0 - t);
		cov = clamp(1.25 - abs(v_local.y - y), 0.0, 1.0);
	} else {
		ivec2 t = ivec2(v_uv) + ivec2(v_local);
		cov = texelFetch(atlas, t, 0).r;
	}
	o_color = vec4(v_color.rgb, v_color.a * cov);
}
