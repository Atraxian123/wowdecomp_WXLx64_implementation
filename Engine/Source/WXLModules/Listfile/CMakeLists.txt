# WXL listfile module: wxl_listfile.dll, loaded from <client folder>\WXL\Modules\ by the 64-bit client.
# File id -> file path from <client folder>\WXL\listfile.csv, with stand-ins for files that are not there. It is built
# against Engine/Source/WXL/Sdk (wxl_api.h, wxl_listfile_api.h) and its own folder only: no engine header, no import
# library of the client. Other modules use it through the service table in wxl_listfile_api.h.
if (NOT WOWDECOMP_ARCH_64)
    message(STATUS "WXL modules are 64-bit only: wxl_listfile is not built")
    return()
endif ()

file(GLOB LISTFILE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(wxl_listfile SHARED ${LISTFILE_SOURCES})

target_include_directories(wxl_listfile
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine/Source/WXL/Sdk
)

set_target_properties(wxl_listfile PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # "$<1:...>" keeps Visual Studio from adding a Debug/Release sub-folder: the loader looks in WXL\Modules
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_SOURCE_DIR}/bin/Win/WXL/Modules>"
)
