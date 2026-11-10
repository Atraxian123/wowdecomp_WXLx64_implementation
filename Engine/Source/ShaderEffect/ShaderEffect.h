#ifndef ENGINE_SOURCE_SHADEREFFECT_SHADEREFFECT_H
#define ENGINE_SOURCE_SHADEREFFECT_SHADEREFFECT_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C4Vector.h"

class C44Matrix;
class CGxShader;
class CImVector;
class CM2Light;
class CM2Lighting;

struct LocalLights {
    /* 0000 */ C4Vector m_color[4];
    /* 0040 */ C4Vector m_position[4];
    /* 0080 */ float m_constantAttenuation[4];
    /* 0090 */ float m_linearAttenuation[4];
    /* 00A0 */ float m_quadraticAttenuation[4];

    LocalLights();
};

class CShaderEffect : public TSHashObject<CShaderEffect, HASHKEY_STRI> {
    public:
    static CShaderEffect* s_curEffect;
    static int32_t s_enableShaders;
    static C4Vector s_fogColorAlphaRef;
    static float s_fogMul;
    static int32_t s_fogActive; // WXL: 1 when the last SetFogEnabled turned fog on (the shaders read it from a constant, so nothing else says)
    static C4Vector s_fogParams;
    static int32_t s_lightEnabled;
    static uint32_t s_localLightCount;
    static LocalLights s_localLights;
    static uint32_t s_shadowValue;
    static C3Vector s_sunAmbient;
    static C3Vector s_sunDiffuse;
    static C3Vector s_sunDir;
    static C3Vector s_sunSpec;
    static int32_t s_useAlphaRef;
    static uint32_t s_usePcfFiltering;

    static void ComputeLocalLights(LocalLights* localLights, uint32_t lightCount, CM2Light** lights, const C3Vector* origin);
    static void InitShaderSystem(int32_t enableShaders, int32_t usePcfFiltering);
    static uint32_t SelectShadowShader();
    static void SetAlphaRef(float alphaRef);
    static void SetAlphaRefDefault();
    static void SetDefaultShaders(uint32_t lightCount);
    static void SetDiffuse(const C4Vector& color);
    static void SetEmissive(const C4Vector& color);
    static void SetFogEnabled(int32_t enabled);
    static void SetFogParams(float start, float end, float density, const CImVector& color);
    static void SetGlobalLighting(CM2Light* light);
    static void SetLightingEnabled(int32_t enabled);
    static void SetLocalLighting(CM2Lighting* lighting, int32_t enabled, const C3Vector* origin);
    static void SetShaders(uint32_t vertexPermute, uint32_t pixelPermute);
    static void SetTexMtx(const C44Matrix& matrix, uint32_t stage);
    static void SetTexMtx_Identity(uint32_t stage);
    static void SetTexMtx_SphereMap(uint32_t stage);
    static void SetTexMtx_TexGen(EGxTexGen texGen, const C44Matrix& matrix, uint32_t stage);
    static void SetUsePcfFiltering(uint32_t usePcfFiltering);
    static void UpdateProjMatrix();
    static void UpdateWorldViewMatrix();

    /* 0018 */ uint32_t m_fixedFuncPassCount;
    /* 001C */ EGxTexOp m_colorOps[2];
    /* 0024 */ EGxTexOp m_alphaOps[2];
    /* 002C */ CGxShader* m_vertexShaders[90];
    /* 0194 */ CGxShader* m_pixelShaders[16];

    CShaderEffect();
    ~CShaderEffect();
    void InitEffect(const char* vertexShader, const char* pixelShader);
    void InitFixedFuncPass(const EGxTexOp* colorOps, const EGxTexOp* alphaOps, uint32_t count);
    void SetCurrent();
};

#endif
