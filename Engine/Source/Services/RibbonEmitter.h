#ifndef ENGINE_SOURCE_SERVICES_RIBBONEMITTER_H
#define ENGINE_SOURCE_SERVICES_RIBBONEMITTER_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/CGxVertexPCT.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CAaRect.h"
#include "Engine/Source/Tempest/CImVector.h"

class C44Matrix;
class CGxBatch;
class CGxTex;
class CTexture;

class CRibbonMat {
    public:
    /* 0000 */ uint32_t m_lit : 1;
    uint32_t m_fogged : 1;
    uint32_t m_depthTest : 1;
    uint32_t m_depthWrite : 1;
    uint32_t m_cull : 1;
    /* 0004 */ uint32_t m_blendMode;

    CRibbonMat();
};

// WXL: values of CRibbonEmitter::m_foldLayers
enum {
    kRibbonLayersStock = 0,    // one pass per layer, each with its own material's blend mode (the stock draw)
    kRibbonLayersMultiply = 1, // a ribbon of 2-4 layers is ONE pass: the layers multiply into one image
    kRibbonLayersAdd = 2,      // a ribbon of 2+ layers: every layer is drawn additively (blend mode "add"), so the layers sum
    kRibbonLayersMultiplyPlain = 3, // like Multiply, but the alpha is the plain product (never scaled) and the colour gain comes
                                    // from [Ribbons] MultiTextureGain: the opt-in MultiTextureBlend=Multiply
};

class CRibbonEmitter {
    public:
    /* 0000 */ int32_t m_refCount;
    /* 0004 */ TSGrowableArray<float> m_edgeAges;
    /* 0014 */ uint32_t m_writePos;
    /* 0018 */ uint32_t m_readPos;
    /* 001C */ float m_edgeFraction;
    /* 0020 */ C3Vector m_prevPos;
    /* 002C */ C3Vector m_localPos;
    /* 0038 */ TSGrowableArray<CGxVertexPCT> m_vertices;
    /* 0048 */ TSGrowableArray<uint16_t> m_indices;
    /* 0058 */ float m_invEdgeLifetime;
    /* 005C */ float m_texCellU;
    /* 0060 */ float m_texCellV;
    /* 0064 */ float m_invTexCellU;
    /* 0068 */ float m_invTexCellV;
    /* 006C */ CAaRect m_texSlotCoords;
    /* 007C */ C3Vector m_prevUp;
    /* 0088 */ C3Vector m_currUp;
    /* 0094 */ C3Vector m_prevDir;
    /* 00A0 */ C3Vector m_currDir;
    /* 00AC */ C3Vector m_startTangent;
    /* 00B8 */ C3Vector m_endTangent;
    /* 00C4 */ C3Vector m_startBelow;
    /* 00D0 */ C3Vector m_endBelow;
    /* 00DC */ C3Vector m_startAbove;
    /* 00E8 */ C3Vector m_endAbove;
    /* 00F4 */ CAaBox m_worldBounds;
    /* 010C */ float m_edgesPerSec;
    /* 0110 */ float m_edgeLifetime;
    /* 0114 */ TSGrowableArray<CRibbonMat> m_materials;
    /* 0124 */ TSGrowableArray<CTexture*> m_textures;
    /* 0134 */ TSGrowableArray<uint32_t> m_textureTypes;
    /* 0144 */ CImVector m_color;
    /* 0148 */ CAaRect m_texCoords;
    /* 0158 */ uint32_t m_rows;
    /* 015C */ uint32_t m_cols;
    /* 0160 */ uint32_t m_flags;
    /* 0164 */ C3Vector m_currPos;
    /* 0170 */ uint32_t m_texSlot;
    /* 0174 */ float m_above;
    /* 0178 */ float m_below;
    /* 017C */ float m_gravity;
    /* WXL */ uint32_t m_foldLayers; // see kRibbonLayers*: how a ribbon of several texture layers is drawn (0 = stock, one pass per layer)

    CRibbonEmitter();
    ~CRibbonEmitter();
    void ChangeFrameOfReference(const C44Matrix* matrix);
    void CloseTextureHandles();
    void ConvertTexSlotToTexCoords();
    void DecRef();
    uint32_t GetNumBatches();
    uint32_t GetNumPrimitives();
    void InitInterpDeltas();
    void Initialize(float edgesPerSecond, float edgeLifetime, const CImVector& color, const TSGrowableArray<CTexture*>& textures, const TSGrowableArray<CRibbonMat>& materials, const TSGrowableArray<uint32_t>& textureTypes, const CAaRect& texCoords, uint32_t rows, uint32_t cols);
    void InterpEdge(float age, float t, uint32_t advance);
    int32_t IsDead();
    int32_t AddsLayers(); // WXL: true when this ribbon has 2+ texture layers and they blend additively
    int32_t Render(const C44Matrix* frameOfReference);
    void RenderFolded(const CGxBatch& batch, CGxTex* const* layerTex, uint32_t layers, bool plainAlpha); // WXL
    int32_t ReplaceTexture(uint32_t type, CTexture* texture);
    void SetAbove(float above);
    void SetAlpha(float alpha);
    void SetBelow(float below);
    void SetColor(float r, float g, float b);
    void SetDataEnabled(int32_t enabled);
    void SetGravity(float gravity);
    void SetPos(const C44Matrix& transform, const C3Vector& position, const C44Matrix* frameOfReference);
    void SetTexSlot(uint32_t slot);
    void SetUserEnabled(uint32_t enabled);
    void Update(float deltaTime, int32_t suppressNewEdges);
};

#endif
