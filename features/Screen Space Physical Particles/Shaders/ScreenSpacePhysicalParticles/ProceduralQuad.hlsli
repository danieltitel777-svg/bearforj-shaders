#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_PROCEDURAL_QUAD_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_PROCEDURAL_QUAD_INCLUDED

// Maps four procedural triangle-strip vertices to a centered unit quad.

// Summary: Returns the signed corner for a procedural quad vertex.
// Args:
//   vertex: Triangle-strip vertex index in the range [0, 3].
// Returns: Corner coordinate in the square [-1, 1].
float2 GetProceduralQuadCorner(uint vertex)
{
	if (vertex == 0u)
		return float2(-1.0f, -1.0f);
	if (vertex == 1u)
		return float2(-1.0f, 1.0f);
	if (vertex == 2u)
		return float2(1.0f, -1.0f);
	return float2(1.0f, 1.0f);
}

#endif
