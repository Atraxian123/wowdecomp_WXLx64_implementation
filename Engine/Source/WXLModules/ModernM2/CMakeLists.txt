# WXL modern M2 module: wxl_modernm2.dll, loaded from <client folder>\WXL\Modules\ by the 64-bit client.
# Reads modern models (MD21 container, inner version 272-274) for the client's M2 loader: container demux, record
# normalization, skin contract rebuild, TXID texture names (through wxl_listfile.dll). It is built against
# Engine/Source/WXL/Sdk (wxl_api.h, wxl_module.hpp, wxl_modernm2_api.h, wxl_listfile_api.h), its own folder (which
# carries its own copy of the M2 record layout) and FlightAnim.h (constants only, engine-free, shared with the game
# layer: Engine/Source/Model2/ModernM2): no engine type, no import library of the client.
if (NOT WOWDECOMP_ARCH_64)
    message(STATUS "WXL modules are 64-bit only: wxl_modernm2 is not built")
    return()
endif ()

file(GLOB MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(wxl_modernm2 SHARED ${MODULE_SOURCES})

target_include_directories(wxl_modernm2
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine/Source/WXL/Sdk
        ${CMAKE_SOURCE_DIR}/Engine/Source/Model2/ModernM2
)

set_target_properties(wxl_modernm2 PROPERTIES
    PREFIX ""
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # "$<1:...>" keeps Visual Studio from adding a Debug/Release sub-folder: the loader looks in WXL\Modules
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_SOURCE_DIR}/bin/Win/WXL/Modules>"
)
