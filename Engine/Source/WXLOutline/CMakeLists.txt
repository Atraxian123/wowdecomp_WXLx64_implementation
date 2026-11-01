# WarcraftXL unit outline, in-tree. A render script: it subscribes to the core's render events and draws through
# the D3D9 device the events hand it. Kept apart from WXLCore because it needs the engine (object manager, unit
# models, the Gx device's state shadow) and the engine already links WXLCore.
file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(WXLOutline STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(WXLOutline
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(WXLOutline
    PUBLIC
        WXLCore
)
