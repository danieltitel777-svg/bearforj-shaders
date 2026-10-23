#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "PhysicalEffects/Common.hlsli"

Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);

struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	nointerpolation float4 Color : COLOR0;
	nointerpolation uint Kind : TEXCOORD1;
	float ViewDepth : TEXCOORD2;
	nointerpolation float Softness : TEXCOORD3;
};

float4 main(PS_INPUT input) : SV_Target
{
	float2 uv = input.TexCoord;
	float r2 = dot(uv, uv);
	if (r2 >= 1.0)
		discard;

	float shape = saturate(1.0 - r2);
	shape *= shape;
	if (input.Kind == PhysFx::KindFrost)
		shape = saturate(0.85 - r2) * (0.55 + 0.45 * shape);
	else if (input.Kind == PhysFx::KindShock)
		shape = saturate(1.0 - abs(uv.x)) * saturate(1.0 - abs(uv.y) * 0.35);

	float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	float soft = saturate((sceneDepth - input.ViewDepth) / max(input.Softness, 0.5));
	float coverage = saturate(shape * soft);
	if (coverage <= 0.002 || input.Color.a <= 0.0)
		discard;
	return float4(input.Color.rgb * coverage, coverage);
}
