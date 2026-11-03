#include "vf_common.hlsli"

sampler2D sDepth : register(s0);
sampler2D sFog : register(s1);

static const float kGridSide = 5;
static const float kGridCellFraction = 0.2;
static const float kGridCellCentreFraction = 0.1;
static const float kClassWorld = 0;
static const float kClassBeyondFarClip = 1;
static const float kClassSky = 2;

float2 GridPoint(float probeIndex)
{
    return float2(fmod(probeIndex, kGridSide), floor(probeIndex / kGridSide));
}

float DepthClass(float depth)
{
    return IsSky(depth) ? kClassSky : (BeyondFarClip(depth) ? kClassBeyondFarClip : kClassWorld);
}

float4 main(float2 probeTexel : VPOS) : COLOR0
{
    float2 gridPoint = GridPoint(floor(probeTexel.x));
    float2 cellCentre = ViewportOrigin() + (gridPoint * kGridCellFraction + kGridCellCentreFraction) * ViewportSize();
    float2 pixel = floor(cellCentre) + 0.5;
    float depth = SampleDepth(sDepth, pixel);
    float4 fog = tex2Dlod(sFog, float4(LowResTexelToUv(FullPixelToLowResTexel(pixel)), 0, 0));
    return float4(depth, LinearDepth(depth), fog.a, DepthClass(depth));
}
