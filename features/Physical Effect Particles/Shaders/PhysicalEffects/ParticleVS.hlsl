#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "PhysicalEffects/Common.hlsli"

StructuredBuffer<PhysFx::Particle> Particles : register(t0);

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	nointerpolation float4 Color : COLOR0;
	nointerpolation uint Kind : TEXCOORD1;
	float ViewDepth : TEXCOORD2;
	nointerpolation float Softness : TEXCOORD3;
};

VS_OUTPUT main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
	VS_OUTPUT o = (VS_OUTPUT)0;
	o.Position = float4(2.0, 2.0, 2.0, 1.0);

	PhysFx::Particle p = Particles[instanceId];
	if (p.Lifetime <= 0.0)
		return o;

	float life = saturate(p.Age / p.Lifetime);
	float fade = (1.0 - life) * (1.0 - life);
	float2 corner = float2(vertexId & 1u, vertexId >> 1u) * 2.0 - 1.0;
	float3 center = p.Position - PhysFx::CameraPosAdjust.xyz;
	float3 viewDir = normalize(center);
	float3 right = normalize(mul(FrameBuffer::CameraViewInverse[0], float4(1.0, 0.0, 0.0, 0.0)).xyz);
	float3 up = normalize(mul(FrameBuffer::CameraViewInverse[0], float4(0.0, 1.0, 0.0, 0.0)).xyz);

	float size = p.Size * lerp(1.0, 0.45, life);
	float3 offset = (right * corner.x + up * corner.y) * size;
	if (p.Kind == PhysFx::KindSpark || p.Kind == PhysFx::KindShock) {
		float3 vel = p.Velocity;
		float3 axis = vel - dot(vel, viewDir) * viewDir;
		float axisLen = length(axis);
		if (axisLen > 1.0) {
			axis /= axisLen;
			float3 side = normalize(cross(axis, viewDir));
			float stretch = 1.0 + min(axisLen * (p.Kind == PhysFx::KindShock ? 0.02 : 0.008), 6.0);
			offset = side * corner.x * size + axis * corner.y * size * stretch;
		}
	} else {
		float ang = p.Age * (p.Kind == PhysFx::KindFrost ? 1.5 : 4.0);
		float s, c;
		sincos(ang, s, c);
		float2 rot = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
		offset = (right * rot.x + up * rot.y) * size;
	}

	float3 world = center + offset;
	o.Position = mul(FrameBuffer::CameraViewProj[0], float4(world, 1.0));
	o.ViewDepth = o.Position.w;
	o.TexCoord = corner;
	o.Kind = p.Kind;
	o.Softness = max(size * 1.5, 2.0);
	float bright = p.Brightness * fade * smoothstep(6.0, 40.0, o.Position.w);
	o.Color = float4(p.Color * bright, bright);
	return o;
}
