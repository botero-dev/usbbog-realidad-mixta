shader_type canvas_item;

// Colour grade for the camera feed only (the TextureRect that shows the video).
//
// The OV3660 on this board captures a low-contrast, slightly hazy picture: the
// sensor's black level sits around 56/255 and the highlights clip, so the frame
// reads as washed out and the colours look pastel. The sensor's own
// brightness/contrast/saturation knobs do not fix that -- contrast there is
// effectively an exposure gain, which lifts the blacks instead of deepening
// them (measured against the live board).
//
// So the correction lives here, in display space, where it costs no bandwidth
// and is tunable without a reflash. Every step is done on sRGB values, the same
// space the sensor writes, because that is where the lift and the clipping are.

// Amount of the lifted black pedestal to remove, 0.0 = untouched.
uniform float black_point : hint_range(0.0, 0.4) = 0.15;
// Midtone curve, applied after the black point. 1.0 = linear.
uniform float grade_gamma : hint_range(0.5, 2.0) = 1.02;
// Contrast around the per-pixel mean. 1.0 = untouched.
uniform float contrast : hint_range(0.5, 3.0) = 1.20;
// Chroma gain around luminance. 1.0 = untouched.
uniform float saturation : hint_range(0.0, 3.0) = 1.40;
// Final gain, to recover overall brightness lost to the black point.
uniform float gain : hint_range(0.25, 4.0) = 1.12;

void fragment() {
	vec4 tex = texture(TEXTURE, UV);

	vec3 c = clamp((tex.rgb - vec3(black_point)) / max(1.0 - black_point, 0.001), 0.0, 1.0);
	c = pow(c, vec3(grade_gamma));

	float mean = (c.r + c.g + c.b) / 3.0;
	c = clamp(vec3(mean) + (c - vec3(mean)) * contrast, 0.0, 1.0);

	float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
	c = clamp(vec3(luma) + (c - vec3(luma)) * saturation, 0.0, 1.0);

	c = clamp(c * gain, 0.0, 1.0);

	// COLOR is assigned wholesale, so the node's modulate is not applied. That
	// is deliberate: this material exists for camera pixels and the TextureRect
	// keeps a white modulate.
	COLOR = vec4(c, tex.a);
}
