file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(WoWCommon STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(WoWCommon
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(WoWCommon PUBLIC WXLCore)
