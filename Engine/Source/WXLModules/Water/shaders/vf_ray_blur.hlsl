float4 cRadialBlur : register(c9);
float4 cRayTarget : register(c10);

sampler2D sSource : register(s0);

static const int kTaps = 8;
static const float kDecayPerTap = 0.9;

float2 SunUv()
{
    return cRadialBlur.xy;
}

float StepFractionTowardSun()
{
    return cRadialBlur.z;
}

float TapWeightNormalisation()
{
    return cRadialBlur.w;
}

float2 TargetTexelSize()
{
    return cRayTarget.zw;
}

float4 main(float2 texel : VPOS) : COLOR0
{
    float2 uv = (texel + 0.5) * TargetTexelSize();
    float2 tapStep = (SunUv() - uv) * StepFractionTowardSun();
    float3 sum = 0;
    float tapWeight = 1;
    [unroll] for (int k = 0; k < kTaps; k++)
    {
        sum += tex2Dlod(sSource, float4(uv + tapStep * k, 0, 0)).rgb * tapWeight;
        tapWeight *= kDecayPerTap;
    }
    return float4(sum * TapWeightNormalisation(), 1);
}
