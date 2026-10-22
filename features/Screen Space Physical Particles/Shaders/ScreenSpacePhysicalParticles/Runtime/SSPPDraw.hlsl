// Bearforj SSPP runtime v2: atlas sprites, procedural quad if the atlas or the sprite id is missing.
#include "Common/ColorSpaces.hlsli"
#include "ScreenSpacePhysicalParticles/Runtime/SSPPCommon.hlsli"

StructuredBuffer<Particle> Particles : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> SpriteAtlas : register(t2);
SamplerState SpriteSamp : register(s0);

struct VSOut
{
	float4 Position : SV_POSITION;
	float2 UV : TEXCOORD0;
	nointerpolation float4 Color : TEXCOORD1;   // rgb premultiplied later, a: opacity
	nointerpolation uint Kind : TEXCOORD2;
	float ViewZ : TEXCOORD3;
	nointerpolation uint Sprite : TEXCOORD4;
};

#if defined(VSHADER)
VSOut main(uint vid : SV_VertexID)
{
	VSOut o = (VSOut)0;
	uint pid = vid / 6;
	uint corner = vid % 6;
	const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
	float2 c = corners[corner];
	o.UV = c;
	Particle p = Particles[pid];
	o.Kind = p.Kind & 0xFFu;
	o.Sprite = (p.Kind >> SSPP_SPRITE_SHIFT) & 0xFFu;
	// 0.6.5.1: no explicit cell -> pick the closest atlas cell from the type id (SSPP_TYPES.md).
	// Cells (1-based): 1 rain 2 snow 3 leaf 4 narrow leaf 5 needle 6 twig 7 spark 8 ember 9 ash
	// 10 ice 11 dust 12 blood 13 water 14 pollen 15 insect 16 fly. Bit 0x80 = tint by particle colour.
	if (o.Sprite == 0u) {
		static const uint kTypeCell[27] = {
			1u, 2u, 3u, 13u, 7u, 11u, 12u, 8u, 10u,             // 0-8 rain snow leaf splash spark dust blood ember ice
			14u | 0x80u, 14u | 0x80u, 15u | 0x80u, 16u, 16u, 15u, // 9-14 pollen firefly butterfly gnat fly lamp
			5u, 6u,                                              // 15-16 needle twig
			14u | 0x80u, 10u | 0x80u, 14u | 0x80u, 9u | 0x80u,  // 17-20 restoration alteration illusion conjuration
			11u | 0x80u, 10u | 0x80u, 13u | 0x80u,              // 21-23 shout ward shard ward ripple
			12u, 13u, 13u                                        // 24-26 blood stain, water stain, drip
		};
		o.Sprite = o.Kind < 27u ? kTypeCell[o.Kind] : 0u;
	}
	if (p.Life <= 0.0 || p.Age >= p.Life || pid >= Ranges.w) {
		o.Position = float4(0, 0, -2, 1);
		return o;
	}
	float3 rel = p.Position - CameraPos.xyz;
	float3 toCam = normalize(-rel);
	float3 right, up;
	float width = p.Size, height = p.Size;
	float4 color = UnpackColor(p.Color);
	// no popping: time fade in/out (leaves fade over 3 s) and distance fade towards the fade distance
	float fadeOut = (o.Kind == SSPP_LEAF || o.Kind == SSPP_NEEDLE || o.Kind == SSPP_TWIG || o.Kind == SSPP_EMBER || o.Kind == SSPP_ICE || o.Kind == SSPP_BLOOD_STAIN || o.Kind == SSPP_WATER_STAIN || o.Kind == SSPP_POLLEN || o.Kind == SSPP_BUTTERFLY) ? 3.0 : 1.0;
	float fade = saturate((p.Life - p.Age) / min(fadeOut, p.Life * 0.3)) * saturate(p.Age * 8.0);
	fade *= saturate((ExtraP2.w - length(rel)) / max(ExtraP2.w * 0.25, 1.0));
	bool settled = (p.Kind & SSPP_FLAG_SETTLED) != 0;
	float3 light = LightColor.rgb;
	uint kind = o.Kind;
	bool flat = kind == SSPP_BLOOD_STAIN || kind == SSPP_WATER_STAIN || kind == SSPP_WARD_RIPPLE;
	if (flat) {
		float s, co;
		sincos(p.Spin, s, co);
		right = float3(co, s, 0);
		up = float3(-s, co, 0);
		if (kind == SSPP_WARD_RIPPLE) {
			float grow = 1.0 + p.Age * 6.0;
			width *= grow;
			height *= grow;
			color.rgb *= light * 1.5;
			color.a *= saturate(1.0 - p.Age / max(p.Life, 0.05));
		} else {
			width *= 1.0;
			height *= 1.0;
			color.rgb = (kind == SSPP_BLOOD_STAIN ? float3(0.55, 0.06, 0.04) : float3(0.62, 0.7, 0.78)) * light;
			color.a *= 0.9;
		}
	} else if ((kind == SSPP_RAIN || kind == SSPP_SPARK || kind == SSPP_SPLASH || kind == SSPP_WATER_DRIP || kind == SSPP_WARD_SHARD) && !settled && dot(p.Velocity, p.Velocity) > 1.0) {
		// velocity-stretched streak
		up = normalize(p.Velocity);
		right = cross(up, toCam);
		right = dot(right, right) > 1e-6 ? normalize(right) : float3(1, 0, 0);
		float speed = length(p.Velocity);
		if (kind == SSPP_RAIN) {
			height = speed * 0.025 * RainP.y;
			color.rgb *= light * RainP.z;
			color.a *= 0.55;
		} else if (kind == SSPP_SPARK) {
			height = max(speed * 0.02, p.Size * 2.0);
			float heat = saturate(1.0 - p.Age / p.Life);
			bool shock = color.b > color.r;  // shock magic: blue-white sparks
			color.rgb = (shock ? lerp(float3(0.3, 0.5, 1.0), float3(0.85, 0.92, 1.0), heat) : lerp(float3(1.0, 0.25, 0.05), float3(1.0, 0.85, 0.5), heat)) * SparkP.z * (0.4 + heat);
			color.a = 0.0;  // additive
		} else if (kind == SSPP_WARD_SHARD) {
			height = max(speed * 0.015, p.Size * 1.6);
			color.rgb *= SparkP.z * 0.35;
			color.a = 0.0;
		} else if (kind == SSPP_WATER_DRIP) {
			height = max(speed * 0.02, p.Size * 1.8);
			color.rgb *= light * SplashP.y;
			color.a *= 0.75;
		} else {
			height = max(speed * 0.015, p.Size);
			color.rgb *= light * SplashP.y;
			color.a *= 0.6;
		}
	} else {
		right = normalize(cross(toCam, float3(0, 0, 1)) + float3(1e-5, 0, 0));
		up = cross(right, toCam);
		if (kind == SSPP_LEAF || kind == SSPP_NEEDLE || kind == SSPP_TWIG) {
			float s, co;
			sincos(p.Spin, s, co);
			float3 r2 = right * co + up * s;
			up = -right * s + up * co;
			right = r2;
			if (kind == SSPP_NEEDLE) {
				height *= 1.7;
				width *= 0.45;
			} else if (kind == SSPP_TWIG) {
				height *= 1.15;
				width *= 0.7;
			} else {
				if (!settled)
					width *= 0.35 + 0.65 * abs(cos(p.Spin * 1.3));  // tumble
				height *= 0.6;
			}
			color.rgb *= light * LeafP.y;
		} else if (kind == SSPP_SNOW) {
			color.rgb *= light * SnowP.y;
			color.a *= 0.9;
		} else if (kind == SSPP_DUST) {
			float grow = 1.0 + p.Age * 1.5;
			width *= grow;
			height *= grow;
			color.rgb *= light * DustP.y;
			color.a *= 0.35 * saturate(1.0 - p.Age / p.Life);
		} else if (kind == SSPP_BLOOD) {
			color.rgb *= light * BloodP.y;
			color.a *= 0.95;
		} else if (kind == SSPP_SPARK || kind == SSPP_EMBER) {
			// resting: cools orange -> dark red -> black over its rest time; flying ember: soft glow
			float cool = settled ? saturate((p.Life - p.Age) / max(p.Spin, 0.1)) : 1.0;
			bool shock = color.b > color.r;
			float3 hot = shock ? float3(0.5, 0.7, 1.0) : float3(1.0, 0.45, 0.1);
			color.rgb = lerp(float3(0.15, 0.02, 0.0), hot, cool * cool) * SparkP.z * (kind == SSPP_EMBER ? 0.35 : 0.5) * (0.15 + cool);
			color.a = 0.0;
		} else if (kind == SSPP_SHOUT) {
			float grow = 1.0 + p.Age * 2.2;
			width *= grow;
			height *= grow;
			color.rgb *= light * 1.4;
			color.a *= 0.35 * saturate(1.0 - p.Age / p.Life);
		} else if (kind == SSPP_FIREFLY || kind == SSPP_RESTORATION || kind == SSPP_ILLUSION || kind == SSPP_WARD_SHARD) {
			color.rgb *= 2.2;
			color.a = 0.0;
		} else if (kind == SSPP_ICE) {
			float s, co;
			sincos(p.Spin, s, co);
			float3 r2 = right * co + up * s;
			up = -right * s + up * co;
			right = r2;
			height *= 1.8;  // shard
			float melt = settled ? saturate((p.Life - p.Age) / max(p.Spin, 0.1)) : 1.0;
			color.rgb = float3(0.75, 0.9, 1.0) * light * 1.6;
			color.a = 0.85 * melt;
		} else {
			color.rgb *= light * SplashP.y;
			color.a *= 0.6;
		}
	}
	// Drawn after the Linear Lighting scene decode: with LL the target is linear (optionally ACEScg),
	// so decode the authored gamma colours exactly like LinearLighting/SceneGammaDecodeCS (DustP.w = LL, BloodP.w = ACEScg).
	if (DustP.w > 0.5) {
		color.rgb = pow(max(color.rgb, 0.0), 1.6);
		if (BloodP.w > 0.5)
			color.rgb = sRGBToAP1(color.rgb);
	}
	color *= fade;
	float3 world = rel + right * c.x * width + up * c.y * height;
	o.Position = mul(ViewProj, float4(world, 1.0));
	o.ViewZ = o.Position.w;
	o.Color = color;
	return o;
}
#endif

#if defined(PSHADER)
float4 main(VSOut i) : SV_Target
{
	float sceneZ = ViewDepthFromDevice(SceneDepth.Load(int3(i.Position.xy, 0)));
	if (sceneZ > 0.0 && i.ViewZ > sceneZ)
		discard;
	float soft = sceneZ > 0.0 ? saturate((sceneZ - i.ViewZ) / max(Flags.z, 0.1)) : 1.0;
	float2 uv = i.UV;
	float shape;
	float3 rgb = i.Color.rgb;
	// ReactPad.x is 1 once atlas.dds loaded. Sprite 0 stays the old procedural shape.
	uint spr = i.Sprite & 0x7Fu;
	bool tint = (i.Sprite & 0x80u) != 0;
	bool textured = ReactPad.x > 0 && spr >= 1 && spr <= 16;
	if (textured) {
		uint cell = spr - 1u;
		float2 uv01 = float2(uv.x, -uv.y) * 0.5 + 0.5;
		float2 atlasUV = (float2(cell & 3u, cell >> 2u) + uv01) * 0.25;
		float4 tex = SpriteAtlas.Sample(SpriteSamp, atlasUV);
		shape = tex.a;
		if (i.Kind == SSPP_WARD_RIPPLE)  // keep the ring, water texture only breaks it up
			shape *= saturate(1.0 - abs(length(uv) - 0.55) * 7.0);
		if (shape < 0.03)
			discard;
		if (i.Color.a <= 0.001) {
			rgb = i.Color.rgb * (tint ? tex.rgb * 1.5 : 1.0);
		} else {
			// Atlas albedo is the picture. Floor the lighting so blood and ash stay readable
			// after the linear-lighting decode, without blowing out rain and snow.
			float3 albedo = tex.rgb;
			if (DustP.w > 0.5) {
				albedo = pow(max(albedo, 0.0), 1.6);
				if (BloodP.w > 0.5)
					albedo = sRGBToAP1(albedo);
			}
			float peak = max(i.Color.r, max(i.Color.g, i.Color.b));
			rgb = albedo * max(peak, 0.65);
			if (tint)  // magic, glow motes: keep the texture detail, take the hue from the emitter colour
				rgb = dot(albedo, float3(0.2126, 0.7152, 0.0722)) * i.Color.rgb / max(peak, 1e-3) * max(peak, 0.65) * 1.4;
		}
	} else if (i.Kind == SSPP_RAIN || i.Kind == SSPP_SPARK || i.Kind == SSPP_SPLASH || i.Kind == SSPP_WATER_DRIP || i.Kind == SSPP_WARD_SHARD)
		shape = saturate(1.0 - uv.x * uv.x) * saturate(1.0 - uv.y * uv.y * uv.y * uv.y);
	else if (i.Kind == SSPP_ICE)
		shape = step(abs(uv.x) + abs(uv.y) * 0.55, 0.9);  // diamond shard
	else if (i.Kind == SSPP_LEAF || i.Kind == SSPP_TWIG)
		shape = step(uv.x * uv.x + uv.y * uv.y * 0.6 + abs(uv.y) * uv.x * uv.x * 0.8, 0.85);
	else if (i.Kind == SSPP_NEEDLE)
		shape = saturate(1.0 - abs(uv.x) * 6.0) * saturate(1.0 - uv.y * uv.y);
	else if (i.Kind == SSPP_WARD_RIPPLE)
		shape = saturate(1.0 - abs(length(uv) - 0.55) * 7.0);
	else if (i.Kind == SSPP_BLOOD_STAIN || i.Kind == SSPP_WATER_STAIN) {
		float r = length(uv);
		float n = 0.5 + 0.5 * sin(uv.x * 9.0) * sin(uv.y * 7.0);
		shape = saturate(1.0 - r / (0.82 + 0.12 * n));
		shape *= shape;
	} else {
		float r = dot(uv, uv);
		shape = saturate(1.0 - r);
		shape *= shape;
	}
	float a = shape * soft;
	// premultiplied: alpha 0 means additive (sparks)
	float opacity = i.Color.a > 0.0 ? i.Color.a : 1.0;
	return float4(rgb * opacity * a, i.Color.a * a);
}
#endif
