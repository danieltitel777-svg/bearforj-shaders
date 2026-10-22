// Bearforj SSPP runtime v2: one procedural draw for all profiles (6 vertices per particle).
#include "Common/ColorSpaces.hlsli"
#include "ScreenSpacePhysicalParticles/Runtime/SSPPCommon.hlsli"

StructuredBuffer<Particle> Particles : register(t0);
Texture2D<float> SceneDepth : register(t1);

struct VSOut
{
	float4 Position : SV_POSITION;
	float2 UV : TEXCOORD0;
	nointerpolation float4 Color : TEXCOORD1;   // rgb premultiplied later, a: opacity
	nointerpolation uint Kind : TEXCOORD2;
	float ViewZ : TEXCOORD3;
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
	float fadeOut = (o.Kind == SSPP_LEAF || o.Kind == SSPP_EMBER || o.Kind == SSPP_ICE) ? 3.0 : 1.0;
	float fade = saturate((p.Life - p.Age) / min(fadeOut, p.Life * 0.3)) * saturate(p.Age * 8.0);
	fade *= saturate((ExtraP2.w - length(rel)) / max(ExtraP2.w * 0.25, 1.0));
	bool settled = (p.Kind & SSPP_FLAG_SETTLED) != 0;
	float3 light = LightColor.rgb;
	uint kind = o.Kind;
	if ((kind == SSPP_RAIN || kind == SSPP_SPARK || kind == SSPP_SPLASH) && !settled && dot(p.Velocity, p.Velocity) > 1.0) {
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
		} else {
			height = max(speed * 0.015, p.Size);
			color.rgb *= light * SplashP.y;
			color.a *= 0.6;
		}
	} else {
		right = normalize(cross(toCam, float3(0, 0, 1)) + float3(1e-5, 0, 0));
		up = cross(right, toCam);
		if (kind == SSPP_LEAF) {
			float s, co;
			sincos(p.Spin, s, co);
			float3 r2 = right * co + up * s;
			up = -right * s + up * co;
			right = r2;
			if (!settled)
				width *= 0.35 + 0.65 * abs(cos(p.Spin * 1.3));  // tumble
			height *= 0.6;
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
	if (i.Kind == SSPP_RAIN || i.Kind == SSPP_SPARK || i.Kind == SSPP_SPLASH)
		shape = saturate(1.0 - uv.x * uv.x) * saturate(1.0 - uv.y * uv.y * uv.y * uv.y);
	else if (i.Kind == SSPP_ICE)
		shape = step(abs(uv.x) + abs(uv.y) * 0.55, 0.9);  // diamond shard
	else if (i.Kind == SSPP_LEAF)
		shape = step(uv.x * uv.x + uv.y * uv.y * 0.6 + abs(uv.y) * uv.x * uv.x * 0.8, 0.85);
	else {
		float r = dot(uv, uv);
		shape = saturate(1.0 - r);
		shape *= shape;
	}
	float a = shape * soft;
	// premultiplied: alpha 0 means additive (sparks)
	float opacity = i.Color.a > 0.0 ? i.Color.a : 1.0;
	return float4(i.Color.rgb * opacity * a, i.Color.a * a);
}
#endif
