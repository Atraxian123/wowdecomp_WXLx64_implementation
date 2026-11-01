// WXL unit outline. See Outline.h. Ported from the standalone wxl-unit-outline extension: the SDK's gx facade and
// object accessors are replaced by the D3D9 device the events carry and by the decompiled engine's own calls.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "Engine/Source/WXLOutline/Outline.h"

#include "Engine/Source/WXLOutline/Hlsl.hpp"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/WXL/EventScript.h"
#include "Engine/Source/WXL/Log.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>

namespace wxl::outline
{
    namespace ev = wxl::events;

    namespace
    {
        constexpr D3DFORMAT kMaskFormat = D3DFMT_A8R8G8B8;

        // Blur resolution relative to the frame. Half costs a quarter of the fetches, and the upscale it implies is
        // only visible with point filtering; OutlinePass states linear, so the halo, low-frequency by construction,
        // survives it.
        constexpr unsigned kBlurDivisor = 2;

        // ---- WXLExtendedEquipment.ini, beside the executable. Repeated here as Log.cpp, Culling.cpp and
        // CastAnim.cpp do: WXLCore keeps no shared helper and this module links only WXLCore.
        bool IniPath(char* path, size_t cap)
        {
            const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(cap));
            if (n == 0 || n >= cap) return false;
            char* slash = std::strrchr(path, '\\');
            const char* name = "WXLExtendedEquipment.ini";
            if (!slash || static_cast<size_t>(slash - path) + 1 + std::strlen(name) >= cap) return false;
            std::strcpy(slash + 1, name);
            return true;
        }

        int IniInt(const char* section, const char* key, int def)
        {
            char path[MAX_PATH] = {};
            if (!IniPath(path, sizeof(path))) return def;
            return static_cast<int>(GetPrivateProfileIntA(section, key, def, path));
        }

        float IniFloat(const char* section, const char* key, float def)
        {
            char path[MAX_PATH] = {};
            if (!IniPath(path, sizeof(path))) return def;
            char out[64] = {};
            GetPrivateProfileStringA(section, key, "", out, sizeof(out), path);
            if (!out[0]) return def;
            char* end = nullptr;
            const double v = std::strtod(out, &end);
            return (end == out) ? def : static_cast<float>(v);
        }

        template <class T> void SafeRelease(T*& p)
        {
            if (p) { p->Release(); p = nullptr; }
        }

        // ---- D3D9 helpers (the standalone module got these from the SDK's gx facade) ----

        struct Target
        {
            IDirect3DTexture9* texture = nullptr;
            IDirect3DSurface9* surface = nullptr;
            UINT width = 0, height = 0;

            void Release() { SafeRelease(surface); SafeRelease(texture); width = height = 0; }
        };

        // Size of the surface currently bound as render target 0: the frame the halo is composited into.
        bool CurrentFrameSize(IDirect3DDevice9* dev, UINT& w, UINT& h)
        {
            IDirect3DSurface9* rt = nullptr;
            if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;
            D3DSURFACE_DESC d{};
            const bool ok = SUCCEEDED(rt->GetDesc(&d));
            rt->Release();
            w = d.Width; h = d.Height;
            return ok && w && h;
        }

        bool EnsureTarget(IDirect3DDevice9* dev, Target& t, unsigned divisor)
        {
            if (t.surface) return true;
            UINT fw = 0, fh = 0;
            if (!CurrentFrameSize(dev, fw, fh)) return false;
            t.width  = (fw + divisor - 1) / divisor;
            t.height = (fh + divisor - 1) / divisor;
            if (FAILED(dev->CreateTexture(t.width, t.height, 1, D3DUSAGE_RENDERTARGET, kMaskFormat, D3DPOOL_DEFAULT, &t.texture, nullptr)) || !t.texture)
            {
                t.Release();
                return false;
            }
            if (FAILED(t.texture->GetSurfaceLevel(0, &t.surface)) || !t.surface)
            {
                t.Release();
                return false;
            }
            return true;
        }

        // d3dcompiler is loaded on demand so the client has no import on it: a machine without it just gets no outline.
        IDirect3DPixelShader9* CompilePixelShader(IDirect3DDevice9* dev, const char* hlsl, const char* profile)
        {
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static CompileFn compile = [] {
                HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
                if (!m) m = LoadLibraryA("d3dcompiler_43.dll");
                return m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
            }();
            if (!compile) return nullptr;

            ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
            const HRESULT hr = compile(hlsl, std::strlen(hlsl), "outline", nullptr, nullptr, "main", profile, 0, 0, &code, &err);
            if (FAILED(hr) || !code)
            {
                wxl::LogCat("OUTLINE", "shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "no message");
                SafeRelease(err);
                SafeRelease(code);
                return nullptr;
            }
            SafeRelease(err);

            IDirect3DPixelShader9* ps = nullptr;
            dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
            code->Release();
            return ps;
        }

        // Full-surface quad in pre-transformed coordinates (the -0.5 is D3D9's texel-centre offset), fixed-function
        // vertex path. A state block restores the stream and format afterwards.
        void DrawQuad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1)
        {
            struct V { float x, y, z, rhw, u, v; };
            const V q[4] = {
                { x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
                { x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
                { x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
                { x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
            };
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        }

        // Full-surface quad (the -0.5 in DrawQuad is D3D9's texel-centre offset), fixed-function vertex path. A state
        // block restores the stream and format afterwards.
        void DrawFullscreenQuad(IDirect3DDevice9* dev, UINT w, UINT h)
        {
            DrawQuad(dev, 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        }

        class ScopedDeviceState final
        {
        public:
            explicit ScopedDeviceState(IDirect3DDevice9* dev)
            {
                if (dev && SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &state_)) && state_)
                    state_->Capture();
            }
            ~ScopedDeviceState() { Restore(); }
            void Restore()
            {
                if (!state_) return;
                state_->Apply();
                state_->Release();
                state_ = nullptr;
            }

        private:
            IDirect3DStateBlock9* state_ = nullptr;
        };

        // True when render target 0 is the same size as the mask, i.e. the main frame. A batch drawn into anything
        // else (a model frame's offscreen target, a shadow or reflection map) is not part of the picture the mask
        // describes, and stamping it would put a silhouette in the wrong place.
        bool DrawingIntoFrame(IDirect3DDevice9* dev, UINT w, UINT h)
        {
            UINT cw = 0, ch = 0;
            return CurrentFrameSize(dev, cw, ch) && cw == w && ch == h;
        }

        void ColorForReaction(int reaction, float* c)
        {
            if (reaction < UNIT_REACTION_UNFRIENDLY)  { c[0] = 1.0f; c[1] = 0.0f; c[2] = 0.0f; } // hostile
            else if (reaction < UNIT_REACTION_FRIENDLY) { c[0] = 1.0f; c[1] = 1.0f; c[2] = 0.0f; } // neutral
            else                                        { c[0] = 0.0f; c[1] = 1.0f; c[2] = 0.0f; } // friendly
            c[3] = 1.0f;
        }
    }

    class Outline final : public ev::EventScript
    {
    public:
        Outline(float spread, float intensity, bool debug) : spread_(spread), intensity_(intensity), debug_(debug)
        {
            on<&Outline::OnEndScene>(ev::Event::OnEndScene);
            on<&Outline::OnM2Batch>(ev::Event::OnM2BatchDraw);
            on<&Outline::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

    private:
        static constexpr int kMaxTargets = 2;
        struct Tgt { const CM2Model* model; float color[4]; bool isPlayer; };

        // --- event handlers ---
        void OnEndScene(const ev::EndSceneArgs& a);
        void OnM2Batch(const ev::M2BatchDrawArgs& a);
        void OnDeviceLost(const ev::DeviceResetArgs&);

        // --- steps ---
        bool EnsureResources(IDirect3DDevice9* dev);
        void Discard();
        void RebuildTargets();
        void StampSilhouette(IDirect3DDevice9* dev, const ev::M2BatchDrawArgs& a, int idx);
        void OutlinePass(IDirect3DDevice9* dev);
        void ClearMask(IDirect3DDevice9* dev);

        // --- helpers ---
        static bool ShouldStampBatch();
        int  FindTarget(const CM2Model* model) const;
        void AddTarget(WOWGUID guid, CGUnit_C* player);
        void ReleaseTargets();

        Tgt                 targets_[kMaxTargets]{};
        int                 count_    = 0;
        IDirect3DDevice9*   device_   = nullptr;   // the device the resources below belong to (identity only)
        Target              mask_{}, blurA_{}, blurB_{};
        IDirect3DPixelShader9* fillPS_  = nullptr; // stamps a batch into the mask, following its cutout
        IDirect3DPixelShader9* blurPS_  = nullptr; // one Gaussian, run once per axis
        IDirect3DPixelShader9* applyPS_ = nullptr; // blurred field minus silhouette, into the frame
        IDirect3DStateBlock9*  stampSave_ = nullptr; // records exactly the states StampSilhouette changes
        bool                stamped_  = false;     // a silhouette reached the mask this frame
        float               spread_;
        float               intensity_;
        bool                debug_;                // draw the raw mask in the top-left corner
        IDirect3DPixelShader9* debugPS_ = nullptr;
    };

    // Drops everything tied to a device. The objects hold the device alive until released, so this is also what
    // actually frees the old one.
    void Outline::Discard()
    {
        SafeRelease(fillPS_);
        SafeRelease(blurPS_);
        SafeRelease(applyPS_);
        SafeRelease(debugPS_);
        SafeRelease(stampSave_);
        ReleaseTargets();
        stamped_ = false;
        count_   = 0;
    }

    void Outline::ReleaseTargets()
    {
        mask_.Release();
        blurA_.Release();
        blurB_.Release();
    }

    bool Outline::EnsureResources(IDirect3DDevice9* dev)
    {
        // A resolution change can destroy the device and create a new one rather than reset it, and nothing
        // announces that. Shaders and targets belong to the device that made them, so the pointer changing is the
        // signal to rebuild.
        if (dev != device_)
        {
            // The old device's objects are released through their own references, never through the new device.
            Discard();
            device_ = dev;
        }

        if (!fillPS_)  fillPS_  = CompilePixelShader(dev, kFillHLSL,  "ps_2_0");
        if (!blurPS_)  blurPS_  = CompilePixelShader(dev, kBlurHLSL,  "ps_2_0");
        if (!applyPS_) applyPS_ = CompilePixelShader(dev, kApplyHLSL, "ps_2_0");
        if (debug_ && !debugPS_) debugPS_ = CompilePixelShader(dev, kDebugHLSL, "ps_2_0");

        // A freshly created target holds whatever was in that memory, and the mask is otherwise only wiped after a
        // frame that stamped into it, so its first clear belongs here.
        if (!mask_.surface && EnsureTarget(dev, mask_, 1))
            ClearMask(dev);
        EnsureTarget(dev, blurA_, kBlurDivisor);
        EnsureTarget(dev, blurB_, kBlurDivisor);

        // The client creates a pure device, which cannot be asked for its state. The states a stamp changes are
        // recorded once here; Capture() before and Apply() after put back exactly those, whatever they were.
        if (!stampSave_ && fillPS_)
        {
            const float zero[4] = {};
            if (SUCCEEDED(dev->BeginStateBlock()))
            {
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, 0);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                dev->SetRenderState(D3DRS_ZENABLE, 0);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, 0);
                dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                dev->SetPixelShader(fillPS_);
                dev->SetPixelShaderConstantF(0, zero, 1);
                if (FAILED(dev->EndStateBlock(&stampSave_))) stampSave_ = nullptr;
            }
        }

        return fillPS_ && blurPS_ && applyPS_ && stampSave_
            && mask_.surface && blurA_.surface && blurB_.surface;
    }

    // The model, or any model it is attached to, being one of the picked units' models: weapons, shoulders and
    // mounts are separate instances parented to the unit.
    int Outline::FindTarget(const CM2Model* model) const
    {
        for (int hop = 0; model && hop < 8; ++hop, model = model->m_attachParent)
            for (int i = 0; i < count_; ++i)
                if (targets_[i].model == model) return i;
        return -1;
    }

    void Outline::AddTarget(WOWGUID guid, CGUnit_C* player)
    {
        if (!guid || count_ >= kMaxTargets) return;

        CGObject_C* obj = ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
        if (!obj || !obj->m_obj) return;

        const uint32_t type = static_cast<uint32_t>(obj->m_obj->m_type);
        if (!(type & (TYPEMASK_UNIT | TYPEMASK_PLAYER))) return; // units and players only: not objects, items, corpses

        const CM2Model* model = obj->GetObjectModel();
        if (!model) return;

        for (int i = 0; i < count_; ++i)
            if (targets_[i].model == model) return; // dedup: target and mouseover on the same unit

        // The unit's own attitude toward the player, the same direction the client's highlight sound uses.
        const int reaction = player ? static_cast<int>(static_cast<CGUnit_C*>(obj)->UnitReaction(player)) : static_cast<int>(UNIT_REACTION_HONORED);
        targets_[count_].model    = model;
        targets_[count_].isPlayer = (type & TYPEMASK_PLAYER) != 0;
        ColorForReaction(reaction, targets_[count_].color);
        ++count_;
    }

    void Outline::RebuildTargets()
    {
        count_ = 0;
        CGUnit_C* player = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), static_cast<TypeMask>(TYPEMASK_PLAYER), __FILE__, __LINE__));
        AddTarget(CGGameUI::m_currentObjectTrack, player); // mouseover
        AddTarget(CGGameUI::m_lockedTarget,       player);
    }

    // Attached particles, glows and billboards draw through the same model context as the mesh. The silhouette is
    // the geometry that claims to be solid: a blended batch is not one, and neither is a batch that declines to
    // write depth, since an effect must not occlude what is behind it. Letting either in puts a broad card in the
    // mask, which the blur turns into a blob sitting next to the unit.
    //
    // Read from the engine's own state shadow: the device is a pure one and cannot be queried, and Draw() has just
    // synced the shadow to the hardware. Opaque and alpha-key batches are the solid ones (alpha key is a cutout,
    // not blending).
    bool Outline::ShouldStampBatch()
    {
        if (!g_theGxDevicePtr) return false;
        const int32_t blend = g_theGxDevicePtr->m_hwRenderStates[GxRs_BlendingMode].m_data.i[0];
        const int32_t zwrite = g_theGxDevicePtr->m_hwRenderStates[GxRs_DepthWrite].m_data.i[0];
        return blend <= GxBlend_AlphaKey && zwrite != 0;
    }

    // Draw the model again into the mask render target.
    void Outline::StampSilhouette(IDirect3DDevice9* dev, const ev::M2BatchDrawArgs& a, int idx)
    {
        // No full state block: this runs once per model batch. Everything the stamp changes besides render targets
        // is in stampSave_; the targets, depth surface and viewport are saved by hand.
        IDirect3DSurface9* oldRT = nullptr; dev->GetRenderTarget(0, &oldRT);
        IDirect3DSurface9* oldDS = nullptr; dev->GetDepthStencilSurface(&oldDS);
        D3DVIEWPORT9 oldVP{}; dev->GetViewport(&oldVP);
        stampSave_->Capture();

        // Players depth-test against the scene so walls occlude the outline, which needs the scene's depth surface
        // to be compatible with the mask target (no multisampling, at least as large). Otherwise, like NPCs, the
        // outline is see-through.
        bool useDepth = false;
        if (targets_[idx].isPlayer && oldDS)
        {
            D3DSURFACE_DESC d{};
            useDepth = SUCCEEDED(oldDS->GetDesc(&d)) && d.MultiSampleType == D3DMULTISAMPLE_NONE
                    && d.Width >= mask_.width && d.Height >= mask_.height;
        }

        dev->SetRenderTarget(0, mask_.surface);
        if (useDepth)
        {
            dev->SetDepthStencilSurface(oldDS);
            dev->SetRenderState(D3DRS_ZENABLE, 1);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, 0);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        }
        else
        {
            dev->SetDepthStencilSurface(nullptr);
            dev->SetRenderState(D3DRS_ZENABLE, 0);
        }
        stamped_ = true;
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, 0);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetPixelShader(fillPS_);
        dev->SetPixelShaderConstantF(0, targets_[idx].color, 1);
        dev->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(a.primType), a.baseVertex, a.minIndex, a.numVerts, a.startIndex, a.primCount);

        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        dev->SetViewport(&oldVP);
        stampSave_->Apply();
        SafeRelease(oldRT);
        SafeRelease(oldDS);
    }

    // mask -> blurA (horizontal) -> blurB (vertical) -> frame. A blur reaches further than a neighbour test, so the
    // falloff is a gradient rather than a one-texel step and the width is a radius rather than a tap offset.
    //
    // The state block does not capture render targets, so those are the one thing saved by hand.
    void Outline::OutlinePass(IDirect3DDevice9* dev)
    {
        ScopedDeviceState state(dev);
        IDirect3DSurface9* oldRT = nullptr; dev->GetRenderTarget(0, &oldRT);
        IDirect3DSurface9* oldDS = nullptr; dev->GetDepthStencilSurface(&oldDS);
        if (!oldRT) { SafeRelease(oldDS); return; }
        D3DSURFACE_DESC frame{};
        oldRT->GetDesc(&frame);

        dev->SetRenderState(D3DRS_ZENABLE, 0);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);

        // The blur passes have to overwrite their target completely. States inherited from the frame can reject a
        // pixel instead, and any pixel not written keeps what the previous frame left there -- which reads as old
        // halos frozen in screen space.
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, 0);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, 0);
        dev->SetRenderState(D3DRS_STENCILENABLE, 0);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, 0);

        // Sampling is inherited too. Point filtering would quantise the kernel back to texel centres and defeat the
        // fractional offsets, and wrap addressing lets the taps at the frame's edge pull in the opposite side.
        for (DWORD stage = 0; stage < 2; ++stage)
        {
            dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, 0);
        }

        dev->SetVertexShader(nullptr);
        dev->SetDepthStencilSurface(nullptr);
        dev->SetPixelShader(blurPS_);

        // The step is in blur-target texels, spread_ in frame pixels, hence the divisor.
        const float step = spread_ / float(kBlurDivisor);
        const float horizontal[4] = { 1.0f / blurA_.width, 0.0f, step, 0.0f };
        dev->SetRenderTarget(0, blurA_.surface);
        dev->SetTexture(0, mask_.texture);
        dev->SetPixelShaderConstantF(0, horizontal, 1);
        DrawFullscreenQuad(dev, blurA_.width, blurA_.height);

        const float vertical[4] = { 0.0f, 1.0f / blurB_.height, step, 0.0f };
        dev->SetRenderTarget(0, blurB_.surface);
        dev->SetTexture(0, blurA_.texture);
        dev->SetPixelShaderConstantF(0, vertical, 1);
        DrawFullscreenQuad(dev, blurB_.width, blurB_.height);

        const float apply[4] = { intensity_, 0.0f, 0.0f, 0.0f };
        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        dev->SetTexture(0, blurB_.texture);
        dev->SetTexture(1, mask_.texture);
        dev->SetPixelShader(applyPS_);
        dev->SetPixelShaderConstantF(0, apply, 1);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        DrawFullscreenQuad(dev, frame.Width, frame.Height);

        if (debug_ && debugPS_)
        {
            // The mask as stamped, a quarter of the frame wide, in the top-left corner.
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
            dev->SetTexture(0, mask_.texture);
            dev->SetPixelShader(debugPS_);
            DrawQuad(dev, 0.0f, 0.0f, frame.Width * 0.25f, frame.Height * 0.25f);
        }

        dev->SetPixelShader(nullptr);
        state.Restore();

        // After the restore, not before: the block captured the bindings from before this pass, so unbinding first
        // would just be undone. The mask becomes a render target again next frame, and a surface bound as both
        // target and texture source is invalid -- which shows up as the mask keeping its contents and silhouettes
        // piling up.
        dev->SetTexture(0, nullptr);
        dev->SetTexture(1, nullptr);

        SafeRelease(oldRT);
        SafeRelease(oldDS);
    }

    // Stages 0 and 1 are dropped first: the blur and the composite sampled the mask, and a surface still bound as a
    // texture cannot be made a render target -- the clear would be dropped and the silhouettes would survive into
    // the next frame.
    void Outline::ClearMask(IDirect3DDevice9* dev)
    {
        ScopedDeviceState state(dev);
        IDirect3DSurface9* oldRT = nullptr; dev->GetRenderTarget(0, &oldRT);
        IDirect3DSurface9* oldDS = nullptr; dev->GetDepthStencilSurface(&oldDS);

        dev->SetTexture(0, nullptr);
        dev->SetTexture(1, nullptr);
        dev->SetRenderTarget(0, mask_.surface);
        dev->SetDepthStencilSurface(nullptr);

        // A clear is bounded by the scissor rect the same way a draw is, so an inherited one would leave part of the
        // mask untouched.
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, 0);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);

        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        SafeRelease(oldRT);
        SafeRelease(oldDS);
    }

    void Outline::OnM2Batch(const ev::M2BatchDrawArgs& a)
    {
        // Not merely "are the resources there" but "are they this device's": a batch can arrive on a freshly created
        // device before the EndScene that rebuilds them.
        auto* dev = static_cast<IDirect3DDevice9*>(a.device);
        if (count_ == 0 || dev != device_) return;
        if (!fillPS_ || !mask_.surface || !stampSave_) return;
        const int idx = FindTarget(static_cast<const CM2Model*>(a.model));
        if (idx < 0) return;
        if (!ShouldStampBatch()) return;
        if (!DrawingIntoFrame(dev, mask_.width, mask_.height)) return;
        StampSilhouette(dev, a, idx);
    }

    void Outline::OnEndScene(const ev::EndSceneArgs& a)
    {
        const bool stamped = stamped_;
        stamped_ = false;

        auto* dev = static_cast<IDirect3DDevice9*>(a.device);
        if (!dev) return;

        // EnsureResources first and unconditionally: it creates the shaders and targets, and nothing can ever be
        // stamped until it has.
        if (!EnsureResources(dev)) return;

        // The clear is paired with the stamping rather than run unconditionally: the mask starts empty and is wiped
        // at the end of every frame that wrote into it, so a frame that stamps nothing finds it already empty.
        if (stamped)
        {
            OutlinePass(dev);
            ClearMask(dev);
        }
        RebuildTargets();
    }

    // The targets are D3DPOOL_DEFAULT resources, so they have to go before the engine resets the device. They are
    // recreated by EnsureResources on the next frame; the pixel shaders and the recorded state block are not pool
    // resources and survive the reset untouched.
    void Outline::OnDeviceLost(const ev::DeviceResetArgs&)
    {
        ReleaseTargets();
        stamped_ = false;
        count_   = 0;
    }

    void Init()
    {
        if (IniInt("Outline", "Enabled", 0) == 0)
        {
            wxl::LogCat("OUTLINE", "unit outline off ([Outline] Enabled is 0 or absent)");
            return;
        }

        float spread = IniFloat("Outline", "Spread", 1.5f);
        float intensity = IniFloat("Outline", "Intensity", 2.0f);
        if (!(spread > 0.0f))    spread = 1.5f;
        if (!(intensity > 0.0f)) intensity = 2.0f;

        const bool debug = IniInt("Outline", "Debug", 0) != 0;
        static Outline s_outline(spread, intensity, debug);
        wxl::LogCat("OUTLINE", "unit outline active on mouseover and target (spread %.2f, intensity %.2f)", spread, intensity);
    }
}
