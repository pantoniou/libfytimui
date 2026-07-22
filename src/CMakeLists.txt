# ---------------------------------------------------------------------------
# Targets, following the libfymd4c layout:
#
#   fytimui         - primary library (shared when BUILD_SHARED_LIBS)
#   fytimui_static  - always a static archive
#
# Both are built from the same sources and both absorb the vendored timui core
# objects, which were compiled with hidden visibility. Only FYTIM_EXPORT-tagged
# fytim_* symbols are public; no timui_* symbol is reachable from either.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

set(_fytimui_sources
    libfytimui.c
    fytim_layout.c
    fytim_sgr.c
)

set(_fytimui_incdir ${CMAKE_SOURCE_DIR}/include)

add_library(fytimui        ${_fytimui_sources} $<TARGET_OBJECTS:fytimui-core>)
add_library(fytimui_static STATIC ${_fytimui_sources} $<TARGET_OBJECTS:fytimui-core>)

# Keep the archive named libfytimui.a beside libfytimui.so on ELF; use a
# distinct name where the two would collide.
set(_fytimui_static_output fytimui_static)
if(BUILD_SHARED_LIBS AND NOT WIN32)
    set(_fytimui_static_output fytimui)
endif()

include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-Wl,--exclude-libs,ALL")
check_c_source_compiles("int main(void){return 0;}" FYTIMUI_HAVE_EXCLUDE_LIBS)
unset(CMAKE_REQUIRED_FLAGS)

foreach(_t fytimui fytimui_static)
    # The public headers are the only thing consumers see. The vendored core's
    # include dir is PRIVATE on purpose: timui.h must never be reachable from a
    # consumer's include path.
    target_include_directories(${_t} PUBLIC
        "$<BUILD_INTERFACE:${_fytimui_incdir}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_include_directories(${_t} PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${_t} PRIVATE
        "FYTIM_VERSION_STRING=\"${PROJECT_VERSION}\"")
    target_compile_definitions(${_t} PRIVATE "$<$<CONFIG:Debug>:DEBUG>")
    if(CMAKE_C_COMPILER_ID MATCHES GNU|Clang)
        target_compile_options(${_t} PRIVATE -fvisibility=hidden)
        if(FYTIMUI_HAVE_EXCLUDE_LIBS)
            target_link_options(${_t} PRIVATE "-Wl,--exclude-libs,ALL")
        endif()
    endif()
    if(ASAN_C_FLAGS)
        target_compile_options(${_t} PRIVATE ${ASAN_C_FLAGS})
        target_link_options(${_t} PRIVATE ${ASAN_C_FLAGS} INTERFACE ${ASAN_C_FLAGS})
    endif()
endforeach()

set_target_properties(fytimui PROPERTIES
    OUTPUT_NAME fytimui
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER "${_fytimui_incdir}/libfytimui.h"
    EXPORT_NAME fytimui
)
set_target_properties(fytimui_static PROPERTIES
    OUTPUT_NAME ${_fytimui_static_output}
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME fytimui_static
)

add_library(libfytimui::libfytimui        ALIAS fytimui)
add_library(libfytimui::libfytimui_static ALIAS fytimui_static)

install(TARGETS fytimui fytimui_static
    EXPORT libfytimui-targets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${_fytimui_incdir}/libfytimui
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
