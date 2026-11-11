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
#include <string>
#include <unordered_map>
#include <vector>

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

        // ---- WXLSetup.ini, beside the executable. Repeated here as Log.cpp, Culling.cpp and
        // CastAnim.cpp do: WXLCore keeps no shared helper and this module links only WXLCore.
        // [Outline] keys of WXLSetup.ini, through the module API.
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

        // A model path in the form the prefixes are kept in: lower case, backslashes.
        std::string NormalizeModelPath(const char* path)
        {
            std::string s = path ? path : "";
            for (char& c : s)
            {
                if (c == '/') c = '\\';
                else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return s;
        }

        // "Spells\, World\Generic\" -> { "spells\\", "world\\generic\\" }. Separated by commas or bars.
        std::vector<std::string> ParsePathPrefixes(const char* text)
        {
            std::vector<std::string> out;
            std::string cur;
            auto flush = [&] {
                size_t b = 0, e = cur.size();
                while (b < e && (cur[b] == ' ' || cur[b] == '\t')) ++b;
                while (e > b && (cur[e - 1] == ' ' || cur[e - 1] == '\t')) --e;
                if (e > b) out.push_back(NormalizeModelPath(cur.substr(b, e - b).c_str()));
                cur.clear();
            };
            for (const char* p = text ? text : ""; *p; ++p)
            {
                if (*p == ',' || *p == '|') flush();
                else cur += *p;
            }
            flush();
            return out;
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
                {
                    if (FAILED(state_->Capture()))
                    {
                        state_->Release();
                        state_ = nullptr;
                    }
                }
            }
            ~ScopedDeviceState() { Restore(); }

            // False when the device's state could not be saved (a state block is a driver allocation and can fail
            // when video memory is short, which is just what the first frames after a loading screen look like). The
            // caller must then leave the device alone: with nothing to restore from, every state it changed would stay
            // changed, and the client, which caches its own states and does not re-send what it believes is already
            // set, would keep drawing with them: no depth test, no culling, no shaders.
            bool Valid() const { return state_ != nullptr; }
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
            bool  questTurnIn = false;      // [Outline] QuestTurnIn: also outline whoever waits for a completed quest
            bool  tracked = false;          // [Outline] Tracked: outline what the minimap tracks (behind the quest outlines)
            bool  trackedObjects = true;    //   TrackedObjects: resource nodes, mailboxes
            bool  trackedCreatures = true;  //   TrackedCreatures: the creatures the tracking spells show
            bool  trackedNpcs = true;       //   TrackedNpcs: the NPCs of the tracking menu (vendors, trainers, ...)
            bool  trackedThroughWalls = false;
            float trackedRange = 80.0f;     //   TrackedRange: yards from the player, 0 = every visible one
            bool  hideBehindObjects = false; // [Outline] HideBehindObjects: every outline is depth-tested
            float depthTolerance = 1.0f;    // [Outline] DepthTolerance: scales the bias that lets a unit pass its own depth
            bool  transparentUnits = false; // [Outline] TransparentUnits: also outline see-through batches (ghosts, spirits)
            bool  playersThroughWalls = false; // [Outline] PlayersThroughWalls: a targeted / hovered player is not depth-tested either
            std::vector<std::string> excludePrefixes; // [Outline] ExcludeEffects / ExcludePaths: models never outlined, by path
            float transparentAlphaRef = 0.05f; // [Outline] TransparentAlphaRef: texture alpha below this is not outlined on them
            // Defaults as shipped: red / yellow / green by the unit's attitude toward the player, light blue for quests.
            float hostileColor[4]  = { 1.0f, 0.0f,  0.0f, 1.0f };
            float neutralColor[4]  = { 1.0f, 1.0f,  0.0f, 1.0f };
            float friendlyColor[4] = { 0.0f, 1.0f,  0.0f, 1.0f };
            float questColor[4]    = { 0.0f, 0.78f, 1.0f, 1.0f };
            float turnInColor[4]   = { 1.0f, 0.82f, 0.0f, 1.0f };   // the gold of the "?" over a quest giver
            float trackedObjectColor[4]   = { 1.0f, 0.63f, 0.25f, 1.0f };  // amber: nodes, mailboxes
            float trackedCreatureColor[4] = { 1.0f, 0.25f, 1.0f,  1.0f };  // magenta: tracked creatures
            float trackedNpcColor[4]      = { 1.0f, 1.0f,  1.0f,  1.0f };  // white: tracked NPC services
        };

        explicit Outline(const Options& o)
            : spread_(o.spread), intensity_(o.intensity), debug_(o.debug), mouseTarget_(o.mouseTarget), quest_(o.quest),
              questTurnIn_(o.questTurnIn), questThroughWalls_(o.questThroughWalls), hideBehind_(o.hideBehindObjects),
              tracked_(o.tracked), trackedObjects_(o.trackedObjects), trackedCreatures_(o.trackedCreatures),
              trackedNpcs_(o.trackedNpcs), trackedThroughWalls_(o.trackedThroughWalls), trackedRange_(o.trackedRange),
              transparentUnits_(o.transparentUnits), transparentAlphaRef_(o.transparentAlphaRef),
              playersThroughWalls_(o.playersThroughWalls),
              excludePrefixes_(o.excludePrefixes)
        {
            depthTol_ = o.depthTolerance;
            std::memcpy(trackedObjectColor_, o.trackedObjectColor, sizeof(trackedObjectColor_));
            std::memcpy(trackedCreatureColor_, o.trackedCreatureColor, sizeof(trackedCreatureColor_));
            std::memcpy(trackedNpcColor_, o.trackedNpcColor, sizeof(trackedNpcColor_));
            std::memcpy(questColor_, o.questColor, sizeof(questColor_));
            std::memcpy(turnInColor_, o.turnInColor, sizeof(turnInColor_));
            std::memcpy(hostileColor_, o.hostileColor, sizeof(hostileColor_));
            std::memcpy(neutralColor_, o.neutralColor, sizeof(neutralColor_));
            std::memcpy(friendlyColor_, o.friendlyColor, sizeof(friendlyColor_));
            on<&Outline::OnEndScene>(ev::Event::OnEndScene);
            on<&Outline::OnM2Batch>(ev::Event::OnM2BatchDraw);
            on<&Outline::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

    private:
        static constexpr int kMaxQuestTargets = 32;
        static constexpr int kMaxTrackedTargets = 48;
        static constexpr int kMaxTargets = 2 + 2 * kMaxQuestTargets + kMaxTrackedTargets;   // mouseover + target, objectives, turn-ins, tracked
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
        bool OutlinePass(IDirect3DDevice9* dev);
        bool ClearMask(IDirect3DDevice9* dev);

        // --- helpers ---
        int  FindTarget(const void* model) const;
        bool IsExcludedModel(const void* instance);
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
        IDirect3DStateBlock9*  stampSave_ = nullptr; // records exactly the states StampSilhouette changes (the viewport too)
        IDirect3DStateBlock9*  viewportSave_ = nullptr; // records the viewport alone: the pure device cannot be asked for it
        bool                stamped_  = false;     // a silhouette reached the mask this frame
        bool                maskDirty_ = false;    // the mask holds silhouettes that have not been cleared yet
        bool                loggedSkip_ = false;   // a pass skipped for want of a saved device state, said once
        float               spread_;
        float               intensity_;
        bool                debug_;                // draw the raw mask in the top-left corner
        bool                mouseTarget_;
        bool                quest_;
        bool                questTurnIn_;
        bool                questThroughWalls_;
        bool                hideBehind_;
        bool                tracked_;
        bool                trackedObjects_, trackedCreatures_, trackedNpcs_;
        bool                trackedThroughWalls_;
        float               trackedRange_;
        bool                transparentUnits_;
        float               transparentAlphaRef_;
        bool                playersThroughWalls_;
        std::vector<std::string> excludePrefixes_;   // model path prefixes that are never outlined (spell and aura visuals)
        std::unordered_map<const void*, bool> excludeCache_; // loaded model file -> excluded; cleared every frame
        float               trackedObjectColor_[4]{}, trackedCreatureColor_[4]{}, trackedNpcColor_[4]{};
        Tgt                 trackedTargets_[kMaxTrackedTargets]{};   // what the minimap tracks: the last in the order
        int                 trackedCount_ = 0;
        bool                loggedNoDepth_ = false;
        bool                loggedDepthInfo_ = false;
        float               depthTol_ = 1.0f;
        float               alphaRef_ = 0.5f;      // the engine's cutout reference for the batch being stamped
        float               questColor_[4]{};
        float               turnInColor_[4]{};
        float               hostileColor_[4]{};
        float               neutralColor_[4]{};
        float               friendlyColor_[4]{};
        Tgt                 questTargets_[kMaxQuestTargets]{}; // refreshed every kQuestRefreshMs, appended every frame
        int                 questCount_ = 0;
        Tgt                 turnInTargets_[kMaxQuestTargets]{}; // the same, for the NPCs that take a completed quest
        int                 turnInCount_ = 0;
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
        SafeRelease(viewportSave_);
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
            maskDirty_ = !ClearMask(dev);
        EnsureTarget(dev, blurA_, kBlurDivisor);
        EnsureTarget(dev, blurB_, kBlurDivisor);

        // The client creates a pure device, which cannot be asked for its state. The states a stamp changes are
        // recorded once here; Capture() before and Apply() after put back exactly those, whatever they were.
        if (!stampSave_ && fillPS_)
        {
            const float zero[8] = {};
            // Only the kinds of state a recording sees are recorded, and the viewport is one of them: recorded with any
            // value, Capture() later reads the device's real one and Apply() puts it back. That is the only way to
            // save and restore it here, because a pure device refuses GetViewport.
            const D3DVIEWPORT9 anyViewport = { 0, 0, 1, 1, 0.0f, 1.0f };
            if (!viewportSave_ && SUCCEEDED(dev->BeginStateBlock()))
            {
                dev->SetViewport(&anyViewport);
                if (FAILED(dev->EndStateBlock(&viewportSave_))) viewportSave_ = nullptr;
            }
            if (SUCCEEDED(dev->BeginStateBlock()))
            {
                dev->SetViewport(&anyViewport);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, 0);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                dev->SetRenderState(D3DRS_ZENABLE, 0);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, 0);
                dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                dev->SetRenderState(D3DRS_STENCILENABLE, 0);
                dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
                dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
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

    // True when the model being drawn is one that is never outlined: by default the models under Spells\, which is where
    // the spell, aura and state visuals live. Those are separate model instances attached to the unit, so FindTarget
    // finds the unit through their parent link and, without this, they would be outlined as part of it. Only the model
    // actually being drawn is looked at, so a weapon that carries an effect is outlined and its effect is not. The
    // answer is kept per loaded model file for the frame (a path lookup is a call into the client); a model whose path
    // cannot be read yet is outlined, and asked again with the next batch.
    bool Outline::IsExcludedModel(const void* instance)
    {
        if (excludePrefixes_.empty() || !instance) return false;
        const WxlApi* api = wxl::module::Api();
        if (!api || !api->modelInfo || !api->sharedPath) return false;
        WxlModelInfo info = {};
        info.structSize = sizeof(info);
        if (api->modelInfo(const_cast<void*>(instance), &info) != 0 || !info.shared) return false;

        const auto cached = excludeCache_.find(info.shared);
        if (cached != excludeCache_.end()) return cached->second;

        char path[260] = {};
        if (api->sharedPath(info.shared, path, sizeof(path)) == 0 || !path[0]) return false;
        const std::string norm = NormalizeModelPath(path);
        bool excluded = false;
        for (const std::string& prefix : excludePrefixes_)
            if (norm.compare(0, prefix.size(), prefix) == 0) { excluded = true; break; }

        if (excludeCache_.size() > 512) excludeCache_.clear();
        excludeCache_[info.shared] = excluded;
        return excluded;
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
                // Players hide behind walls (unless PlayersThroughWalls); NPCs only with HideBehindObjects
                targets_[count_].depthTest = hideBehind_ || (units[i].isPlayer != 0 && !playersThroughWalls_);
                std::memcpy(targets_[count_].color, color, sizeof(targets_[count_].color));
                ++count_;
            }
        }
        if (quest_ || questTurnIn_ || tracked_) AppendQuestTargets();
    }

    // Rebuilds the quest target list from the client (the quest log, the quest cache and the visible objects). Run on
    // a timer, not per frame: the log changes on kills and loot, and nothing here has to be frame-exact.
    void Outline::RefreshQuestTargets()
    {
        const void* models[kMaxQuestTargets] = {};

        // Pointers only: they are compared against the model being drawn, never dereferenced, so one that outlives
        // its object by a refresh interval is harmless.
        questCount_ = 0;
        if (quest_)
        {
            const uint32_t n = wxl::module::Api()->outlineQuestTargets(models, kMaxQuestTargets);
            for (uint32_t i = 0; i < n && questCount_ < kMaxQuestTargets; ++i)
            {
                Tgt& t = questTargets_[questCount_++];
                t.model = models[i];
                std::memcpy(t.color, questColor_, sizeof(t.color));
                t.depthTest = hideBehind_ || !questThroughWalls_;
            }
        }

        // Who waits for a completed quest (the "?" over an NPC or object), in its own color.
        turnInCount_ = 0;
        if (questTurnIn_)
        {
            const uint32_t n = wxl::module::Api()->outlineQuestTurnIns(models, kMaxQuestTargets);
            for (uint32_t i = 0; i < n && turnInCount_ < kMaxQuestTargets; ++i)
            {
                Tgt& t = turnInTargets_[turnInCount_++];
                t.model = models[i];
                std::memcpy(t.color, turnInColor_, sizeof(t.color));
                t.depthTest = hideBehind_ || !questThroughWalls_;
            }
        }

        // What the minimap tracks (nodes, tracked creatures, the tracking menu's NPCs), by the client's own rules.
        trackedCount_ = 0;
        if (tracked_)
        {
            WxlOutlineTracked found[kMaxTrackedTargets] = {};
            const uint32_t n = wxl::module::Api()->outlineTrackedTargets(found, kMaxTrackedTargets, trackedRange_);
            for (uint32_t i = 0; i < n && trackedCount_ < kMaxTrackedTargets; ++i)
            {
                const float* color = nullptr;
                switch (found[i].kind)
                {
                case WXL_TRACKED_OBJECT:   if (trackedObjects_)   color = trackedObjectColor_;   break;
                case WXL_TRACKED_CREATURE: if (trackedCreatures_) color = trackedCreatureColor_; break;
                case WXL_TRACKED_NPC:      if (trackedNpcs_)      color = trackedNpcColor_;      break;
                default: break;
                }
                if (!color) continue;
                Tgt& t = trackedTargets_[trackedCount_++];
                t.model = found[i].model;
                std::memcpy(t.color, color, sizeof(t.color));
                t.depthTest = hideBehind_ || !trackedThroughWalls_;
            }
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

        // Turn-in NPCs last: the mouseover / target color and the objective color win over theirs.
        for (int q = 0; q < turnInCount_ && count_ < kMaxTargets; ++q)
        {
            bool dup = false;
            for (int i = 0; i < count_; ++i)
                if (targets_[i].model == turnInTargets_[q].model) { dup = true; break; }
            if (!dup) targets_[count_++] = turnInTargets_[q];
        }

        // Tracked things come last of all: every other outline wins over them, and they are the first dropped
        // when the list is full.
        for (int q = 0; q < trackedCount_ && count_ < kMaxTargets; ++q)
        {
            bool dup = false;
            for (int i = 0; i < count_; ++i)
                if (targets_[i].model == trackedTargets_[q].model) { dup = true; break; }
            if (!dup) targets_[count_++] = trackedTargets_[q];
        }
    }

    // Draw the model again into the mask render target.
    void Outline::StampSilhouette(IDirect3DDevice9* dev, const ev::M2BatchDrawArgs& a, int idx)
    {
        // No full state block: this runs once per model batch. Everything the stamp changes besides render targets
        // is in stampSave_; the targets, depth surface and viewport are saved by hand.
        IDirect3DSurface9* oldRT = nullptr; dev->GetRenderTarget(0, &oldRT);
        IDirect3DSurface9* oldDS = nullptr; dev->GetDepthStencilSurface(&oldDS);
        D3DVIEWPORT9 oldVP{};
        const bool haveVP = SUCCEEDED(dev->GetViewport(&oldVP)) && oldVP.Width > 0 && oldVP.Height > 0;   // a pure device says no

        // Nothing below may run unless the way back is in hand: without the saved states the stamp's own changes
        // (no Z write, a depth bias, the fill shader) would be left on the client's device.
        if (!oldRT || FAILED(stampSave_->Capture()))   // everything it records, the engine's viewport included
        {
            SafeRelease(oldRT);
            SafeRelease(oldDS);
            return;
        }
        if (viewportSave_) viewportSave_->Capture();

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

        // The first change to the device. If it is refused, nothing has been touched and nothing needs undoing; going on
        // would draw the silhouette into the scene itself.
        if (FAILED(dev->SetRenderTarget(0, mask_.surface)))
        {
            SafeRelease(oldRT);
            SafeRelease(oldDS);
            return;
        }

        // Setting a render target resets the viewport to the whole target with a depth range of 0 .. 1. The world is
        // not drawn with that range: it uses 0 .. 0.94 (CWorld::s_unk_ADEEE4), the rest of the depth range being kept
        // for the horizon and the sky. The scene's depth buffer therefore holds depth x 0.94, and a silhouette drawn
        // with 0 .. 1 comes out about 6 percent deeper than the scene on every pixel, so it fails the depth test
        // everywhere, even on the unit's own pixels. Put the engine's own viewport back before drawing.
        if (haveVP) dev->SetViewport(&oldVP);
        else if (viewportSave_) viewportSave_->Apply();

        if (useDepth)
        {
            // The silhouette is the unit's own triangles drawn again after the engine drew them, so on its own
            // pixels the depth is (nearly) equal to what the scene wrote, and it passes only if "equal" counts and
            // the two draws agree to the last bit. Make that robust: follow the engine's depth convention (a
            // reversed depth buffer would flip the test), accept equal, pull the redraw a hair toward the camera
            // (a wall in front is still far nearer than that), and switch the engine's stencil test off, which
            // could otherwise reject the second draw of a surface it has already marked.
            DWORD engineZFunc = D3DCMP_LESSEQUAL;
            dev->GetRenderState(D3DRS_ZFUNC, &engineZFunc);
            const bool reversed = engineZFunc == D3DCMP_GREATER || engineZFunc == D3DCMP_GREATEREQUAL;
            const float sign = reversed ? 1.0f : -1.0f;
            const float bias = sign * 1.0e-5f * depthTol_, slope = sign * 1.0f * depthTol_;
            DWORD biasBits = 0, slopeBits = 0;
            std::memcpy(&biasBits, &bias, sizeof(biasBits));
            std::memcpy(&slopeBits, &slope, sizeof(slopeBits));

            dev->SetDepthStencilSurface(oldDS);
            dev->SetRenderState(D3DRS_ZENABLE, 1);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, 0);
            dev->SetRenderState(D3DRS_ZFUNC, reversed ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRS_STENCILENABLE, 0);
            dev->SetRenderState(D3DRS_DEPTHBIAS, biasBits);
            dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, slopeBits);

            if (!loggedDepthInfo_)
            {
                loggedDepthInfo_ = true;
                D3DSURFACE_DESC d{};
                oldDS->GetDesc(&d);
                wxl::LogCat("OUTLINE", "depth test on: scene depth %ux%u format %u, mask %ux%u, engine ZFUNC %lu (%s), bias %.2g slope %.2g",
                            d.Width, d.Height, static_cast<unsigned>(d.Format), mask_.width, mask_.height,
                            static_cast<unsigned long>(engineZFunc), reversed ? "reversed" : "standard", bias, slope);
                if (haveVP)
                    wxl::LogCat("OUTLINE", "depth test: the engine's viewport %lux%lu, depth range %.3f .. %.3f, put back for the stamp",
                                static_cast<unsigned long>(oldVP.Width), static_cast<unsigned long>(oldVP.Height), oldVP.MinZ, oldVP.MaxZ);
                else
                    wxl::LogCat("OUTLINE", "depth test: the device cannot report its viewport; it is saved and put back with a state block%s",
                                viewportSave_ ? "" : " - NOT AVAILABLE, the stamp uses the default viewport");
            }
        }
        else
        {
            dev->SetDepthStencilSurface(nullptr);
            dev->SetRenderState(D3DRS_ZENABLE, 0);
        }
        stamped_ = true;
        maskDirty_ = true;
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
        if (haveVP) dev->SetViewport(&oldVP);       // switching the target reset it again
        stampSave_->Apply();                        // also puts the viewport back when it could not be read
        SafeRelease(oldRT);
        SafeRelease(oldDS);
    }

    // mask -> blurA (horizontal) -> blurB (vertical) -> frame. A blur reaches further than a neighbour test, so the
    // falloff is a gradient rather than a one-texel step and the width is a radius rather than a tap offset.
    //
    // The state block does not capture render targets, so those are the one thing saved by hand.
    bool Outline::OutlinePass(IDirect3DDevice9* dev)
    {
        ScopedDeviceState state(dev);
        if (!state.Valid())
        {
            // Nothing to restore from, so nothing may be changed: see ScopedDeviceState::Valid.
            if (!loggedSkip_)
            {
                loggedSkip_ = true;
                wxl::LogCat("OUTLINE", "outline pass skipped: the device state could not be saved (CreateStateBlock failed); the frame is left untouched");
            }
            return false;
        }
        IDirect3DSurface9* oldRT = nullptr; dev->GetRenderTarget(0, &oldRT);
        IDirect3DSurface9* oldDS = nullptr; dev->GetDepthStencilSurface(&oldDS);
        if (!oldRT) { SafeRelease(oldDS); return false; }
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
        bool targetsOk = true;   // a refused SetRenderTarget would send the blur into the scene itself
        for (int i = 0; i < iterations && targetsOk; ++i)
        {
            const float horizontal[4] = { 1.0f / blurA_.width, 0.0f, step, 0.0f };
            if (FAILED(dev->SetRenderTarget(0, blurA_.surface))) { targetsOk = false; break; }
            dev->SetTexture(0, source);
            dev->SetPixelShaderConstantF(0, horizontal, 1);
            DrawFullscreenQuad(dev, blurA_.width, blurA_.height);

            const float vertical[4] = { 0.0f, 1.0f / blurB_.height, step, 0.0f };
            if (FAILED(dev->SetRenderTarget(0, blurB_.surface))) { targetsOk = false; break; }
            dev->SetTexture(0, blurA_.texture);
            dev->SetPixelShaderConstantF(0, vertical, 1);
            DrawFullscreenQuad(dev, blurB_.width, blurB_.height);

            source = blurB_.texture; // the next iteration blurs the blurred result
        }

        const float apply[4] = { intensity_, 0.0f, 0.0f, 0.0f };
        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        if (targetsOk)
        {
            dev->SetTexture(0, blurB_.texture);
            dev->SetTexture(1, mask_.texture);
            dev->SetPixelShader(applyPS_);
            dev->SetPixelShaderConstantF(0, apply, 1);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            DrawFullscreenQuad(dev, frame.Width, frame.Height);
        }

        if (targetsOk && debug_ && debugPS_)
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
        return targetsOk;
    }

    // Stages 0 and 1 are dropped first: the blur and the composite sampled the mask, and a surface still bound as a
    // texture cannot be made a render target -- the clear would be dropped and the silhouettes would survive into
    // the next frame.
    //
    // True when the mask was wiped. False leaves it dirty, and the next EndScene tries again.
    bool Outline::ClearMask(IDirect3DDevice9* dev)
    {
        ScopedDeviceState state(dev);
        if (!state.Valid())
            return false;   // see ScopedDeviceState::Valid: no saved state, no changes
        IDirect3DSurface9* oldRT = nullptr; dev->GetRenderTarget(0, &oldRT);
        IDirect3DSurface9* oldDS = nullptr; dev->GetDepthStencilSurface(&oldDS);

        dev->SetTexture(0, nullptr);
        dev->SetTexture(1, nullptr);
        if (!oldRT || FAILED(dev->SetRenderTarget(0, mask_.surface)))
        {
            SafeRelease(oldRT);
            SafeRelease(oldDS);
            return false;
        }
        dev->SetDepthStencilSurface(nullptr);

        // A clear is bounded by the scissor rect the same way a draw is, so an inherited one would leave part of the
        // mask untouched.
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, 0);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        const bool cleared = SUCCEEDED(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0));

        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        SafeRelease(oldRT);
        SafeRelease(oldDS);
        if (cleared)
            maskDirty_ = false;
        return cleared;
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
        if (IsExcludedModel(a.model)) return;   // a spell or aura visual attached to the unit
        // 1 = a solid batch, 2 = a see-through one (alpha-blended, additive, or no depth write: ghosts and spirits).
        // Those are stamped only when [Outline] TransparentUnits=1, and with their own alpha reference: the engine's
        // cutout value means nothing for a blended batch.
        const int batchState = wxl::module::Api()->outlineBatchState(&alphaRef_);
        if (batchState == 0) return;
        if (batchState == 2)
        {
            if (!transparentUnits_) return;
            alphaRef_ = transparentAlphaRef_;
        }
        if (!DrawingIntoFrame(dev, mask_.width, mask_.height)) return;
        StampSilhouette(dev, a, idx);
    }

    void Outline::OnEndScene(const ev::EndSceneArgs& a)
    {
        const bool stamped = stamped_;
        stamped_ = false;
        excludeCache_.clear();   // a freed model file's address can come back as another one

        auto* dev = static_cast<IDirect3DDevice9*>(a.device);
        if (!dev) return;

        // EnsureResources first and unconditionally: it creates the shaders and targets, and nothing can ever be
        // stamped until it has.
        if (!EnsureResources(dev)) return;

        // The clear is paired with the stamping rather than run unconditionally: the mask starts empty and is wiped
        // at the end of every frame that wrote into it, so a frame that stamps nothing finds it already empty.
        if (stamped)
            OutlinePass(dev);
        // The mask is wiped after every frame that wrote into it, and again next frame if that wipe could not be done:
        // silhouettes left in it would pile up.
        if (stamped || maskDirty_)
            ClearMask(dev);
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
        o.questTurnIn = IniInt("Outline", "QuestTurnIn", 0) != 0;
        if (o.questTurnIn && !WXL_API_HAS(wxl::module::Api(), outlineQuestTurnIns))
        {
            wxl::LogCat("OUTLINE", "[Outline] QuestTurnIn=1, but this client does not offer the turn-in list: ignored");
            o.questTurnIn = false;
        }
        o.tracked          = IniInt("Outline", "Tracked", 0) != 0;
        o.trackedObjects   = IniInt("Outline", "TrackedObjects", 1) != 0;
        o.trackedCreatures = IniInt("Outline", "TrackedCreatures", 1) != 0;
        o.trackedNpcs      = IniInt("Outline", "TrackedNpcs", 1) != 0;
        if (o.tracked && !WXL_API_HAS(wxl::module::Api(), outlineTrackedTargets))
        {
            wxl::LogCat("OUTLINE", "[Outline] Tracked=1, but this client does not offer the tracking list: ignored");
            o.tracked = false;
        }
        if (o.tracked && !o.trackedObjects && !o.trackedCreatures && !o.trackedNpcs) o.tracked = false;
        if (!o.mouseTarget && !o.quest && !o.questTurnIn && !o.tracked)
        {
            wxl::LogCat("OUTLINE", "unit outline off ([Outline] Enabled, Quest, QuestTurnIn and Tracked are 0 or absent)");
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
        IniColor("QuestTurnInColor", o.turnInColor, o.turnInColor);
        IniColor("TrackedObjectColor",   o.trackedObjectColor,   o.trackedObjectColor);
        IniColor("TrackedCreatureColor", o.trackedCreatureColor, o.trackedCreatureColor);
        IniColor("TrackedNpcColor",      o.trackedNpcColor,      o.trackedNpcColor);
        o.trackedThroughWalls = IniInt("Outline", "TrackedThroughWalls", o.questThroughWalls ? 1 : 0) != 0;
        o.depthTolerance = IniFloat("Outline", "DepthTolerance", 1.0f);
        if (!(o.depthTolerance >= 0.0f) || o.depthTolerance > 100.0f) o.depthTolerance = 1.0f;
        o.transparentUnits = IniInt("Outline", "TransparentUnits", 0) != 0;
        o.playersThroughWalls = IniInt("Outline", "PlayersThroughWalls", 0) != 0;
        if (IniInt("Outline", "ExcludeEffects", 1) != 0)
        {
            char paths[256] = {};
            if (!IniText("Outline", "ExcludePaths", paths, sizeof(paths))) std::strcpy(paths, "Spells\\");
            o.excludePrefixes = ParsePathPrefixes(paths);
        }
        o.transparentAlphaRef = IniFloat("Outline", "TransparentAlphaRef", 0.05f);
        if (!(o.transparentAlphaRef >= 0.0f) || o.transparentAlphaRef > 1.0f) o.transparentAlphaRef = 0.05f;
        o.trackedRange = IniFloat("Outline", "TrackedRange", 80.0f);
        if (!(o.trackedRange >= 0.0f)) o.trackedRange = 80.0f;

        static Outline s_outline(o);
        wxl::LogCat("OUTLINE", "unit outline active: mouseover/target %s, quest objectives %s, quest turn-ins %s, tracked %s, transparent units %s (spread %.2f, intensity %.2f)",
                    o.mouseTarget ? "on" : "off", o.quest ? "on" : "off", o.questTurnIn ? "on" : "off",
                    o.tracked ? "on" : "off", o.transparentUnits ? "on" : "off", o.spread, o.intensity);
        return true;
    }
}
