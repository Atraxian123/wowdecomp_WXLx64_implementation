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
#include "Engine/Source/WXLOutline/QuestMatch.h"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/WXL/EventScript.h"
#include "Engine/Source/WXL/Log.h"
#include "WoW/Source/DB/DBClient/CreatureStats_C.h"
#include "WoW/Source/DB/DBClient/DBCacheInstances.h"
#include "WoW/Source/Object/GameObjectStats.h"
#include "WoW/Source/Object/ObjectClient/GameObject_C.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/QuestCache.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"

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

        // [Outline] <Key>=RRGGBB, or `def` when the key is absent or not six hex digits. Absent is silent, a bad value is
        // said once in the log.
        void IniColor(const char* key, const float* def, float* out)
        {
            out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; out[3] = 1.0f;

            char path[MAX_PATH] = {};
            char text[32] = {};
            if (!IniPath(path, sizeof(path))) return;
            GetPrivateProfileStringA("Outline", key, "", text, sizeof(text), path);
            if (!text[0]) return;

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
        struct Tgt { const CM2Model* model; float color[4]; bool depthTest; };

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
        static float EngineAlphaRef();
        int  FindTarget(const CM2Model* model) const;
        void AddTarget(WOWGUID guid, CGUnit_C* player);
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
        targets_[count_].depthTest = hideBehind_ || (type & TYPEMASK_PLAYER) != 0; // players always hide behind walls; NPCs only with HideBehindObjects
        // hated / hostile, then unfriendly / neutral, then friendly and up
        const float* color = reaction < UNIT_REACTION_UNFRIENDLY ? hostileColor_
                           : reaction < UNIT_REACTION_FRIENDLY   ? neutralColor_
                                                                  : friendlyColor_;
        std::memcpy(targets_[count_].color, color, sizeof(targets_[count_].color));
        ++count_;
    }

    void Outline::RebuildTargets()
    {
        count_ = 0;
        if (mouseTarget_)
        {
            CGUnit_C* player = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), static_cast<TypeMask>(TYPEMASK_PLAYER), __FILE__, __LINE__));
            AddTarget(CGGameUI::m_currentObjectTrack, player); // mouseover
            AddTarget(CGGameUI::m_lockedTarget,       player);
        }
        if (quest_) AppendQuestTargets();
    }

    namespace
    {
        // What the open quests still want, from the quest log (progress) and the quest cache (requirements).
        // A quest whose record is not cached yet is requested and skipped until it arrives.
        // Asking for a record with a callback is what makes the cache query the server for one it does not have; the
        // answer lands in the cache, and the next refresh finds it, so the callback has nothing left to do.
        void QuestRecordArrived(int32_t, const WOWGUID&, void*, bool) {}

        void CollectQuestNeeds(CGPlayer_C* player, QuestNeeds& needs)
        {
            const WOWGUID none = { 0 };
            for (uint32_t slot = 0; slot < 25; ++slot)
            {
                const PlayerQuestLogEntry& e = player->m_player->m_playerQuestLog[slot];
                if (!e.m_questId || (e.m_state & 0x3)) continue; // empty slot, complete (1) or failed (2)

                const QuestCache* quest = g_questDBCache.GetRecord(static_cast<int32_t>(e.m_questId), none, QuestRecordArrived, nullptr, false);
                if (!quest) continue;

                for (uint32_t i = 0; i < 4; ++i) // kill / interact objectives: creature entry, or game object entry with the sign bit set
                    if (quest->m_objectiveId[i] && e.m_counts[i] < quest->m_objectiveRequiredCount[i])
                        needs.AddObjective(quest->m_objectiveId[i]);

                for (uint32_t i = 0; i < 6; ++i) // items to collect
                {
                    const uint32_t item = quest->m_collectItemId[i];
                    if (item && player->m_bag.GetItemTypeCount(static_cast<int32_t>(item), 8) < static_cast<int32_t>(quest->m_collectItemCount[i]))
                        needs.AddItem(item);
                }
            }
        }

        struct QuestScan
        {
            const QuestNeeds* needs;
            const CM2Model*   models[32];
            int               count;
        };

        // Visible objects, one at a time. Returns non-zero to keep going.
        int32_t QuestScanCallback(WOWGUID guid, void* param)
        {
            auto* scan = static_cast<QuestScan*>(param);
            if (scan->count >= static_cast<int>(sizeof(scan->models) / sizeof(scan->models[0]))) return 0;

            CGObject_C* obj = ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
            if (!obj || !obj->m_obj) return 1;

            const uint32_t type = static_cast<uint32_t>(obj->m_obj->m_type);
            const uint32_t entry = obj->m_obj->m_entryID;
            bool match = false;

            if (type & TYPEMASK_PLAYER)
            {
                return 1;
            }
            else if (type & TYPEMASK_UNIT)
            {
                CGUnit_C* unit = static_cast<CGUnit_C*>(obj);
                const CreatureStats_C* stats = unit->m_creatureCacheEntry;

                if (!unit->IsDeadOrGhost())
                {
                    match = scan->needs->NeedsCreature(entry, stats ? stats->m_killCredit : nullptr, 2)
                         || (stats && scan->needs->NeedsItemFrom(stats->m_questItem, 6));
                }
                else if ((unit->m_unit->m_unitDynamicFlags & 0x1) && stats) // a corpse still holding loot
                {
                    match = scan->needs->NeedsItemFrom(stats->m_questItem, 6);
                }
            }
            else if (type & TYPEMASK_GAMEOBJECT)
            {
                CGGameObject_C* go = static_cast<CGGameObject_C*>(obj);
                const GameObjectStats* stats = go->m_stats;
                const uint16_t dyn = go->m_gameObject ? go->m_gameObject->m_dynamicFlags : 0;

                // Besides the quest data, the server's own "sparkle" (0x8, unless the object cannot be used right now,
                // 0x4) is its statement that this object matters to this player's quests; the client draws the sparkle
                // from the same flag.
                match = scan->needs->NeedsGameObject(entry)
                     || (stats && scan->needs->NeedsItemFrom(stats->m_questItems, 6))
                     || ((dyn & 0x8) && !(dyn & 0x4));
            }

            if (match)
            {
                const CM2Model* model = obj->GetObjectModel();
                if (model) scan->models[scan->count++] = model;
            }
            return 1;
        }
    }

    // Rebuilds the quest target list from the quest log and the visible objects. Run on a timer, not per frame: the
    // log changes on kills and loot, and nothing here has to be frame-exact.
    void Outline::RefreshQuestTargets()
    {
        questCount_ = 0;

        CGPlayer_C* player = static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), static_cast<TypeMask>(TYPEMASK_PLAYER), __FILE__, __LINE__));
        if (!player || !player->m_player) return;

        QuestNeeds needs;
        CollectQuestNeeds(player, needs);
        if (needs.Empty()) return;

        QuestScan scan{ &needs, {}, 0 };
        ClntObjMgrEnumVisibleObjects(QuestScanCallback, &scan);

        // Pointers only: they are compared against the model being drawn, never dereferenced, so one that outlives
        // its object by a refresh interval is harmless.
        for (int i = 0; i < scan.count; ++i)
        {
            Tgt& t = questTargets_[questCount_++];
            t.model = scan.models[i];
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

    // The alpha reference the engine cut the batch it just drew with. It lives in one of two places, by device
    // capability: the alpha-test render state, or a pixel shader constant the engine's own shaders read.
    float Outline::EngineAlphaRef()
    {
        if (CShaderEffect::s_useAlphaRef && g_theGxDevicePtr)
            return static_cast<float>(g_theGxDevicePtr->m_hwRenderStates[GxRs_AlphaRef].m_data.i[0]) / 255.0f;
        return CShaderEffect::s_fogColorAlphaRef.w;
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
        const float alphaRef[4] = { EngineAlphaRef(), 0.0f, 0.0f, 0.0f };
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
        Outline::Options o;
        o.mouseTarget = IniInt("Outline", "Enabled", 0) != 0;
        o.quest       = IniInt("Outline", "Quest", 0) != 0;
        if (!o.mouseTarget && !o.quest)
        {
            wxl::LogCat("OUTLINE", "unit outline off ([Outline] Enabled and Quest are 0 or absent)");
            return;
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
    }
}
