#include <cstdint>
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"

// STATIC: 0x00B2F9EC
CVar* CCharacterComponent::g_componentTextureLevelVar;

// STATIC: 0x00B2F9F0
CVar* CCharacterComponent::g_componentThreadVar;

// STATIC: 0x00B2F9F4
CVar* CCharacterComponent::g_componentCompressVar;

// WXL (no original address)
CVar* CCharacterComponent::g_npcRuntimeCompositeVar;

// VERIFY: 00bba183 auto
// OFFSET: 0x008A1420
bool ComponentVarHandler(CVar* var, const char* oldValue, const char* value, void* arg) {
    return true;
}
