# WXL outline module: wxl_outline.dll, loaded from <client folder>\WXL\Modules\ by the 64-bit client.
# Built against Engine/Source/WXL/Sdk (wxl_api.h, wxl_module.hpp), its own folder, and QuestMatch.h (engine-free, shared
# with the core side: Engine/Source/WXLOutline) for the colour parser. D3D9 comes from the Windows SDK; the module does
# not link d3d9.lib or d3dcompiler (it uses the client's device and loads the compiler at run time).
if (NOT WOWDECOMP_ARCH_64)
    message(STATUS "WXL modules are 64-bit only: wxl_outline is not built")
    return()
endif ()

file(GLOB MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(wxl_outline SHARED ${MODULE_SOURCES})

target_include_directories(wxl_outline
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine/Source/WXL/Sdk
        ${CMAKE_SOURCE_DIR}/Engine/Source/WXLOutline
)

set_target_properties(wxl_outline PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_SOURCE_DIR}/bin/Win/WXL/Modules>"
)
