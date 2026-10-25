#ifndef __RAIN_COLOR_COMPAT_HLSLI__
#define __RAIN_COLOR_COMPAT_HLSLI__

// Rain Rendering was written against the Open Shaders color API. These map its calls onto this tree's
// Color/ColorManagement functions: irradiance is already linear with linear lighting, gamma 2.2 otherwise.
#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/TransferFunctions.hlsli"

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
namespace Color
{
	float3 IrradianceToLinear(float3 color)
	{
#	if defined(ENABLE_LL)
		return color;
#	else
		return TransferFunctions::Gamma22ToLinear(color);
#	endif
	}

	float3 IrradianceToGamma(float3 color)
	{
#	if defined(ENABLE_LL)
		return color;
#	else
		return TransferFunctions::LinearToGamma22(color);
#	endif
	}

	float3 PointLight(float3 color, bool isLinear, uint lightFlags)
	{
		return PointLight(color);
	}
}
#endif

#endif
