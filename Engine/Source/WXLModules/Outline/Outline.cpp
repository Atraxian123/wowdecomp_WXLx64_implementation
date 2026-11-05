// WXL unit outline, as the module wxl_outline.dll. See README.md for the ini keys. Ported from the standalone
// wxl-unit-outline extension: the SDK's gx facade is replaced by the D3D9 device the events carry, and everything the
// outline needs from the client (which units to outline, what the quests need, the batch state) comes through the
// module API's outline calls (Engine/Source/WXLApi/OutlineHost.cpp).
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

#include "Hlsl.hpp"
#include "QuestMatch.h"   // ParseHexColor (the shared, engine-free header in Engine/Source/WXLOutline)
#include "wxl_module.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cmath>
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
        // [Outline] keys of WXLExtendedEquipment.ini, through the module API.
        int IniInt(const char* section, const char* key, int def)
        {
            const WxlApi* api = wxl::module::Api();
            return api && api->iniGetInt ? api->iniGetInt(section, key, def) : def;
        }

        bool IniText(const char* section, const char* key, char* out, size_t cap)
        {
            if (out && cap) out[0] = '\0';
            const WxlApi* api = wxl::module::Api();
            return api && api->iniGetString && api->iniGetString(section, key, out, cap) != 0 && out[0];
        }

        float IniFloat(const char* section, const char* key, float def)
        {
            char out[64] = {};
            if (!IniText(section, key, out, sizeof(out))) return def;
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

        // [Outline] <Key>=RRGGBB, or `def` when the key is absent or not six hex digits. Absent is silent, a bad value is
        // said once in the log.
        void IniColor(const char* key, const float* def, float* out)
        {
            out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; out[3] = 1.0f;

            char text[32] = {};
            if (!IniText("Outline", key, text, sizeof(text))) return;

            float rgb[3];
            if (ParseHexColor(text, rgb)) { out[0] = rgb[0]; out[1] = rgb[1]; out[2] = rgb[2]; }
            else wxl::LogCat("OUTLINE", "[Outline] %s \"%s\" is not RRGGBB hex, using the default", key, text);
        }
    }

    class Outline final : public ev::EventScript
    {
    public:
        struct Options
        {
            float spread = 1.5f, intensity = 2.0f;
            bool  debug = false;
            bool  mouseTarget = true;       // [Outline] Enabled: the reaction-colored mouseover / target outline
            bool  quest = false;            // [Outline] Quest: the always-on quest objective outline
            bool  questThroughWalls = false;
            bool  hideBehindObjects = false; // [Outline] HideBehindObjects: every outline is depth-tested
            // Defaults as shipped: red / yellow / green by the unit's attitude toward the player, light blue for quests.
            float hostileColor[4]  = { 1.0f, 0.0f,  0.0f, 1.0f };
            float neutralColor[4]  = { 1.0f, 1.0f,  0.0f, 1.0f };
            float friendlyColor[4] = { 0.0f, 1.0f,  0.0f, 1.0f };
            float questColor[4]    = { 0.0f, 0.78f, 1.0f, 1.0f };
        };

        explicit Outline(const Options& o)
            : spread_(o.spread), intensity_(o.intensity), debug_(o.debug), mouseTarget_(o.mouseTarget), quest_(o.quest),
              questThroughWalls_(o.questThroughWalls), hideBehind_(o.hideBehindObjects)
        {
            std::memcpy(questColor_, o.questColor, sizeof(questColor_));
            std::memcpy(hostileColor_, o.hostileColor, sizeof(hostileColor_));
            std::memcpy(neutralColor_, o.neutralColor, sizeof(neutralColor_));
            std::memcpy(friendlyColor_, o.friendlyColor, sizeof(friendlyColor_));
            on<&Outline::OnEndScene>(ev::Event::OnEndScene);
            on<&Outline::OnM2Batch>(ev::Event::OnM2BatchDraw);
            on<&Outline::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

    private:
        static constexpr int kMaxQuestTargets = 32;
        static constexpr int kMaxTargets = 2 + kMaxQuestTargets;
        static constexpr unsigned long long kQuestRefreshMs = 250;
        // depthTest: the scene hides the outline where it hides the unit. Otherwise the outline shows through.
        struct Tgt { const void* model; float color[4]; bool depthTest; };

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
        int  FindTarget(const void* model) const;
        void RefreshQuestTargets();
        void AppendQuestTargets();
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
        bool                mouseTarget_;
        bool                quest_;
        bool                questThroughWalls_;
        bool                hideBehind_;
        bool                loggedNoDepth_ = false;
        float               alphaRef_ = 0.5f;      // the engine's cutout reference for the batch being stamped
        float               questColor_[4]{};
        float               hostileColor_[4]{};
        float               neutralColor_[4]{};
        float               friendlyColor_[4]{};
        Tgt                 questTargets_[kMaxQuestTargets]{}; // refreshed every kQuestRefreshMs, appended every frame
        int                 questCount_ = 0;
        unsigned long long  lastQuestRefresh_ = 0;
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
            const float zero[8] = {};
            if (SUCCEEDED(dev->BeginStateBlock()))
            {
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, 0);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                dev->SetRenderState(D3DRS_ZENABLE, 0);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, 0);
                dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                dev->SetPixelShader(fillPS_);
                dev->SetPixelShaderConstantF(0, zero, 2); // c0 = silhouette color, c1 = alpha reference
                if (FAILED(dev->EndStateBlock(&stampSave_))) stampSave_ = nullptr;
            }
        }

        return fillPS_ && blurPS_ && applyPS_ && stampSave_
            && mask_.surface && blurA_.surface && blurB_.surface;
    }

    // The model, or any model it is attached to, being one of the picked units' models: weapons, shoulders and
    // mounts are separate instances parented to the unit.
    int Outline::FindTarget(const void* model) const
    {
        const WxlApi* api = wxl::module::Api();
        for (int hop = 0; model && hop < 8; ++hop)
        {
            for (int i = 0; i < count_; ++i)
                if (targets_[i].model == model) return i;
            WxlModelInfo info = {};
            info.structSize = sizeof(info);
            model = (api->modelInfo(const_cast<void*>(model), &info) == 0) ? info.parent : nullptr;
        }
        return -1;
    }

    void Outline::RebuildTargets()
    {
        count_ = 0;
        if (mouseTarget_)
        {
            // The mouseover, then the target; the client picks them and the unit's attitude toward the player.
            WxlOutlineUnit units[2] = {};
            const uint32_t n = wxl::module::Api()->outlineUnits(units, 2);
            for (uint32_t i = 0; i < n && count_ < kMaxTargets; ++i)
            {
                const float* color = units[i].reaction == 0 ? hostileColor_ : units[i].reaction == 1 ? neutralColor_ : friendlyColor_;
                targets_[count_].model     = units[i].model;
                targets_[count_].depthTest = hideBehind_ || units[i].isPlayer != 0; // players always hide behind walls; NPCs only with HideBehindObjects
                std::memcpy(targets_[count_].color, color, sizeof(targets_[count_].color));
                ++count_;
            }
        }
        if (quest_) AppendQuestTargets();
    }

    // Rebuilds the quest target list from the client (the quest log, the quest cache and the visible objects). Run on
    // a timer, not per frame: the log changes on kills and loot, and nothing here has to be frame-exact.
    void Outline::RefreshQuestTargets()
    {
        const void* models[kMaxQuestTargets] = {};
        const uint32_t n = wxl::module::Api()->outlineQuestTargets(models, kMaxQuestTargets);

        // Pointers only: they are compared against the model being drawn, never dereferenced, so one that outlives
        // its object by a refresh interval is harmless.
        questCount_ = 0;
        for (uint32_t i = 0; i < n && questCount_ < kMaxQuestTargets; ++i)
        {
            Tgt& t = questTargets_[questCount_++];
            t.model = models[i];
            std::memcpy(t.color, questColor_, sizeof(t.color));
            t.depthTest = hideBehind_ || !questThroughWalls_;
        }
    }

    void Outline::AppendQuestTargets()
    {
        const unsigned long long now = GetTickCount64();
        if (now - lastQuestRefresh_ >= kQuestRefreshMs)
        {
            lastQuestRefresh_ = now;
            RefreshQuestTargets();
        }

        for (int q = 0; q < questCount_ && count_ < kMaxTargets; ++q)
        {
            bool dup = false;
            for (int i = 0; i < count_; ++i)
                if (targets_[i].model == questTargets_[q].model) { dup = true; break; } // the mouseover / target color wins
            if (!dup) targets_[count_++] = questTargets_[q];
        }
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
        if (targets_[idx].depthTest && oldDS)
        {
            D3DSURFACE_DESC d{};
            useDepth = SUCCEEDED(oldDS->GetDesc(&d)) && d.MultiSampleType == D3DMULTISAMPLE_NONE
                    && d.Width >= mask_.width && d.Height >= mask_.height;
        }

        if (targets_[idx].depthTest && !useDepth && !loggedNoDepth_)
        {
            // Asked to hide behind objects, but the scene's depth surface cannot be paired with the mask (multisampled,
            // or smaller): the outline falls back to showing through. Said once.
            loggedNoDepth_ = true;
            wxl::LogCat("OUTLINE", "depth testing unavailable (scene depth surface is multisampled or smaller than the mask): outlines show through objects");
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
        const float alphaRef[4] = { alphaRef_, 0.0f, 0.0f, 0.0f };
        dev->SetPixelShaderConstantF(1, alphaRef, 1);
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

        // One 5-tap pass spreads its taps 1.4 * step apart; past a step of about 1.5 pixels they skip over small or thin
        // silhouettes and leave separate ghost copies instead of a smooth falloff. A wide halo is therefore built from
        // repeated passes at a fixed small step: the variances add, so n passes of spread / sqrt(n) are as wide as one
        // pass of `spread`, and the step never exceeds kMaxPassSpread. Up to the limit this is the single pass it was.
        constexpr float kMaxPassSpread = 1.5f;
        constexpr int   kMaxIterations = 8;
        int iterations = 1;
        while (iterations < kMaxIterations && spread_ > kMaxPassSpread * std::sqrt(static_cast<float>(iterations)))
            ++iterations;
        const float passSpread = spread_ / std::sqrt(static_cast<float>(iterations));

        // The step is in blur-target texels, spread in frame pixels, hence the divisor.
        const float step = passSpread / float(kBlurDivisor);
        IDirect3DTexture9* source = mask_.texture;
        for (int i = 0; i < iterations; ++i)
        {
            const float horizontal[4] = { 1.0f / blurA_.width, 0.0f, step, 0.0f };
            dev->SetRenderTarget(0, blurA_.surface);
            dev->SetTexture(0, source);
            dev->SetPixelShaderConstantF(0, horizontal, 1);
            DrawFullscreenQuad(dev, blurA_.width, blurA_.height);

            const float vertical[4] = { 0.0f, 1.0f / blurB_.height, step, 0.0f };
            dev->SetRenderTarget(0, blurB_.surface);
            dev->SetTexture(0, blurA_.texture);
            dev->SetPixelShaderConstantF(0, vertical, 1);
            DrawFullscreenQuad(dev, blurB_.width, blurB_.height);

            source = blurB_.texture; // the next iteration blurs the blurred result
        }

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
        const int idx = FindTarget(a.model);
        if (idx < 0) return;
        if (!wxl::module::Api()->outlineBatchState(&alphaRef_)) return;
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

    // Reads the ini and, when [Outline] Enabled=1 or Quest=1, brings the outline up. False when it stays off.
    bool Start()
    {
        Outline::Options o;
        o.mouseTarget = IniInt("Outline", "Enabled", 0) != 0;
        o.quest       = IniInt("Outline", "Quest", 0) != 0;
        if (!o.mouseTarget && !o.quest)
        {
            wxl::LogCat("OUTLINE", "unit outline off ([Outline] Enabled and Quest are 0 or absent)");
            return false;
        }

        o.spread    = IniFloat("Outline", "Spread", 1.5f);
        o.intensity = IniFloat("Outline", "Intensity", 2.0f);
        if (!(o.spread > 0.0f))    o.spread = 1.5f;
        if (!(o.intensity > 0.0f)) o.intensity = 2.0f;
        o.debug = IniInt("Outline", "Debug", 0) != 0;
        o.questThroughWalls = IniInt("Outline", "QuestThroughWalls", 0) != 0;
        o.hideBehindObjects = IniInt("Outline", "HideBehindObjects", 0) != 0;

        IniColor("HostileColor",  o.hostileColor,  o.hostileColor);
        IniColor("NeutralColor",  o.neutralColor,  o.neutralColor);
        IniColor("FriendlyColor", o.friendlyColor, o.friendlyColor);
        IniColor("QuestColor",    o.questColor,    o.questColor);

        static Outline s_outline(o);
        wxl::LogCat("OUTLINE", "unit outline active: mouseover/target %s, quest objectives %s (spread %.2f, intensity %.2f)",
                    o.mouseTarget ? "on" : "off", o.quest ? "on" : "off", o.spread, o.intensity);
        return true;
    }
}
