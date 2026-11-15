#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas; // R8 coverage
layout(set = 0, binding = 1) uniform sampler2D bgimg; // background image, RGBA
// bg = image origin on screen (px) and 1 / its size on screen
layout(push_constant) uniform PC { vec2 inv_half_size; vec4 bg; } pc;

layout(location = 0) flat in uvec2 v_uv;
layout(location = 1) flat in vec4 v_color;
layout(location = 2) noperspective in vec2 v_local;

layout(location = 0) out vec4 o_color;

void main()
{
	float cov;
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
