# WarcraftXL, in-tree. The event bus and the extension base class, compiled straight into the client.
# No detours and no offsets: a hook point is an ev::Emit() call placed in the decompiled function.
# The target is named WXLCore because an unrelated "Event" target already exists in this tree.
file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(WXLCore STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(WXLCore
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

# BlpTranscode.cpp uses std::span (C++20); the rest of the tree is C++17. Only this target moves up, and
# its public headers (Event.h, Hooks.h, Blp.h, Log.h, WXL.h) stay C++17-clean.
set_target_properties(WXLCore PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

target_include_directories(WXLCore
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Blp
)
