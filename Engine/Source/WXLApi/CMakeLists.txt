# The module API of the 64-bit client: the table modules call (ApiTable.cpp), the guarded engine reads behind
# it (ModelAccess.cpp) and the loader that finds WXL\Modules\*.dll (ModuleLoader.cpp). The table is built
# from what WXLCore and WXLEquip already provide, so this library sits above both. The public header for a
# module is Engine/Source/WXL/Sdk/wxl_api.h: nothing here is meant to be included by one.
file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
if (NOT WOWDECOMP_ARCH_64)
    # the module API is 64-bit only: a 32-bit client keeps just the loader's "no modules here" stub
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "(ApiTable|ModelAccess)\\.cpp$")
endif ()

add_library(WXLApi STATIC
    ${PRIVATE_SOURCES}
)

if (NOT WOWDECOMP_ARCH_64)
    target_compile_definitions(WXLApi PUBLIC WXL_ALLOW_32BIT_HEADER=1)
endif ()

target_include_directories(WXLApi
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(WXLApi
    PUBLIC
        WXLCore
        WXLEquip
)
