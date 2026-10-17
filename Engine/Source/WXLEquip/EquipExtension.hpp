// wxl-equip-extension: equipment slot extension features for WarcraftXL.
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

#pragma once
#include "Engine/Source/WXL/EventScript.h"

namespace wxl::scripts::equipextension
{
    class EquipExtension : public wxl::events::EventScript
    {
    public:
        EquipExtension();

    private:
        void OnItemSlotChange(const wxl::events::ItemSlotChangeArgs& a);
        void OnItemSlotClear(const wxl::events::ItemSlotClearArgs& a);
        void OnM2SkinFinalize(const wxl::events::M2SkinFinalizeArgs& a);
        void OnM2PerFrameUpdate(const wxl::events::M2PerFrameUpdateArgs& a);
        void OnBuildBonePalette(const wxl::events::BuildBonePaletteArgs& a);

        // OnWeaponVisualChange and OnItemDisplayLookup formerly lived here, driving the old runtime
        // weapon model-swap path (native ItemDisplayInfo lookup + live Model1/Model2 substitution).
        // Both are removed: WeaponExtension (WeaponExtension.hpp/.cpp) now owns weapon model
        // resolution entirely, patching ItemModelData.dbc at the data level instead, the same
        // convention CreatureModelData already uses for creatures. This class no longer subscribes
        // to either event -- see EquipExtension.cpp's constructor.

        // Not model-load-related at all -- reused purely as the earliest possible, unconditional
        // trigger to kick the one-time armor sidecar load off. OnModelLoadPre fires for ANY model
        // init anywhere in the client, including the login/char-select glue scene's own preview
        // models, well before OnItemSlotChange ever fires for the char-select preview character
        // (see CreatureExtension.hpp's identical reasoning -- same fix, same rationale, applied
        // here for consistency). LoadSidecarModels is idempotent (no-op after the first call), so
        // this is purely additive: the existing OnItemSlotChange call site stays in place as a
        // redundant safety net.
        void OnModelLoadPre(const wxl::events::ModelLoadArgs& a);
    };

    /// Constructs the extension so its handlers bind to the event bus. Called once from wxl::equip::Init().
    void Create();
}
