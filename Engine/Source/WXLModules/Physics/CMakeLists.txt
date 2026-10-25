# WXL physics module: wxl_physics.dll, loaded from <client folder>\WXL\Modules\ by the 64-bit client.
# It is built against Engine/Source/WXL/Sdk/wxl_api.h and its own folder only: no engine header, no import
# library of the client. physics_domino.* is third-party code (see README.md).
if (NOT WOWDECOMP_ARCH_64)
    message(STATUS "WXL modules are 64-bit only: wxl_physics is not built")
    return()
endif ()

file(GLOB PHYSICS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(wxl_physics SHARED ${PHYSICS_SOURCES})

target_include_directories(wxl_physics
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine/Source/WXL/Sdk
)

set_target_properties(wxl_physics PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # "$<1:...>" keeps Visual Studio from adding a Debug/Release sub-folder: the loader looks in WXL\Modules
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_SOURCE_DIR}/bin/Win/WXL/Modules>"
)
