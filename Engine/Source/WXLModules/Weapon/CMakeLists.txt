# WXL weapon module: wxl_weapon.dll, loaded from <client folder>\WXL\Modules\ by the 64-bit client.
# Built against Engine/Source/WXL/Sdk (wxl_api.h, wxl_module.hpp) and its own folder only.
if (NOT WOWDECOMP_ARCH_64)
    message(STATUS "WXL modules are 64-bit only: wxl_weapon is not built")
    return()
endif ()

file(GLOB MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(wxl_weapon SHARED ${MODULE_SOURCES})

target_include_directories(wxl_weapon
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine/Source/WXL/Sdk
)

set_target_properties(wxl_weapon PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_SOURCE_DIR}/bin/Win/WXL/Modules>"
)
