cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBTTA_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_tta.c
)

SET(LIBTTA_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libtta.a
)

SET(LIBTTA_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# libtta's build takes the x86 path (its configure knows only x86 and ARM) and
# so compiles against emscripten's SSE emulation; the filter has to be linked
# with the matching SIMD support.
add_filter(libtta
        "${LIBTTA_SRC}"
        "${LIBTTA_LIBS}"
        []
        ""
        "${LIBTTA_INC}"
        "-msimd128"
        "1")
