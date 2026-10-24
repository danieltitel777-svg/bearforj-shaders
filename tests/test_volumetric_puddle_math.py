"""Water-line math for volumetric puddles.

Keep the fractions in sync with EvaluateVolumetricPuddle in
features/Wetness Effects/Shaders/WetnessEffects/WetnessEffects.hlsli.
Height is (parallax sample - 0.5) * HeightScale: negative is a depression,
positive is a stone. This does not compile HLSL and does not run the game.
"""

FULL_LINE = 0.2
SHORE_FRACTION = 0.08


def saturate(value):
    return max(0.0, min(1.0, value))


def smoothstep(edge0, edge1, value):
    span = edge1 - edge0
    t = saturate((value - edge0) / span) if span != 0.0 else 0.0
    return t * t * (3.0 - 2.0 * t)


def water_line(fill, depth):
    filled = saturate(fill)
    return (-depth) * (1.0 - filled) + (depth * FULL_LINE) * filled


def coverage(height, fill, depth):
    line = water_line(fill, depth)
    shore = depth * SHORE_FRACTION
    return 1.0 - smoothstep(line - shore, line, height)


def test_full_pool_covers_ground_and_leaves_stones():
    depth = 0.5
    assert coverage(0.0, 1.0, depth) == 1.0
    assert coverage(-0.2, 1.0, depth) == 1.0
    assert coverage(0.3, 1.0, depth) == 0.0


def test_drying_shrinks_toward_the_deepest_point():
    depth = 0.5
    limits = [water_line(fill, depth) for fill in (1.0, 0.75, 0.5, 0.25, 0.0)]
    assert limits == sorted(limits, reverse=True)
    assert limits[0] > limits[-1]

    depression = -0.4
    flat = 0.0
    assert coverage(depression, 0.5, depth) == 1.0
    assert coverage(flat, 0.5, depth) < 1.0
    assert coverage(flat, 0.1, depth) == 0.0
    assert coverage(depression, 0.1, depth) < coverage(depression, 0.5, depth)


def test_empty_pool_only_reaches_the_bottom_of_the_range():
    depth = 0.5
    assert water_line(0.0, depth) == -depth
    assert coverage(0.0, 0.0, depth) == 0.0
    assert coverage(-depth, 0.0, depth) < 1.0
    assert coverage(-depth - 0.05, 0.0, depth) == 1.0
