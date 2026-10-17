# WarcraftXL equipment extension, in-tree. Kept apart from WXLCore because it needs the engine
# (BlizzardCore for file access, later Model2 and WoWCommon for the attach calls) and the engine
# already links WXLCore, so folding it in would make the dependency circular.
file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(WXLEquip STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(WXLEquip
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(WXLEquip
    PUBLIC
        WXLCore
        BlizzardCore
)
