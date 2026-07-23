cmake_minimum_required(VERSION 3.12)

project(libfytimui
    VERSION "0.0.1"
    HOMEPAGE_URL "https://github.com/pantoniou/libfytimui"
    LANGUAGES C
)

option(BUILD_FYTIMUI_TESTS "Build the test suites" ON)
option(BUILD_TIMUI_CORE_TESTS "Register the vendored timui core's own tests with CTest" ON)
option(ENABLE_ASAN "Enable ASAN support" OFF)

if(WIN32)
    option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)
else()
    option(BUILD_SHARED_LIBS "Build using shared libraries" ON)
endif()

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo MinSizeRel)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE $ENV{CMAKE_BUILD_TYPE})
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release")
    endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES GNU|Clang)
    add_compile_options(-Wall -Wextra -Wsign-compare -Wshadow)

    # Newest C standard the compiler supports, mirroring libfyaml/libfymd4c.
    include(CheckCCompilerFlag)
    set(_fytim_std "")
    check_c_compiler_flag(-std=gnu2x FYTIM_HAS_GNU2X)
    if(FYTIM_HAS_GNU2X)
        set(_fytim_std -std=gnu2x)
    else()
        check_c_compiler_flag(-std=c2x FYTIM_HAS_C2X)
        if(FYTIM_HAS_C2X)
            set(_fytim_std -std=c2x)
        else()
            check_c_compiler_flag(-std=gnu11 FYTIM_HAS_GNU11)
            if(FYTIM_HAS_GNU11)
                set(_fytim_std -std=gnu11)
            endif()
        endif()
    endif()
    if(_fytim_std)
        add_compile_options(${_fytim_std})
    endif()

    add_compile_definitions(_GNU_SOURCE)
elseif(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W3)
endif()

set(ASAN_C_FLAGS)
if(ENABLE_ASAN)
    set(ASAN_C_FLAGS -fsanitize=address -fsanitize=undefined
                     -fno-omit-frame-pointer)
endif()

# ---------------------------------------------------------------------------
# Vendored timui core.
#
# The core is a single translation unit: core/include/timui.h textually
# includes every core/timui_*.c under TIMUI_IMPLEMENTATION, so core/timui.c is
# the only file compiled. It is built with hidden visibility and linked into
# libfytimui statically, so no timui_* symbol reaches the public ABI. The
# public headers never include timui.h.
# ---------------------------------------------------------------------------
add_library(fytimui-core OBJECT core/src/timui.c)
target_include_directories(fytimui-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(fytimui-core PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
# stb_image.h declares a handful of STBIDEF (== static, under STB_IMAGE_STATIC)
# entry points it never defines. The diagnostic is emitted at end-of-translation-
# unit, so the pragma push/pop around the #include in timui_images.c cannot
# suppress it -- the core is a single TU. Silence it on the core object only.
include(CheckCCompilerFlag)
check_c_compiler_flag(-Wno-unused-function HAVE_WNO_UNUSED_FUNCTION)
if(HAVE_WNO_UNUSED_FUNCTION)
    target_compile_options(fytimui-core PRIVATE -Wno-unused-function)
endif()
if(ASAN_C_FLAGS)
    target_compile_options(fytimui-core PRIVATE ${ASAN_C_FLAGS})
endif()

add_subdirectory(src)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libfytimui-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libfytimui-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libfytimui)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/libfytimui-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT libfytimui-targets
    FILE libfytimui-targets.cmake
    NAMESPACE libfytimui::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libfytimui)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libfytimui-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libfytimui-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libfytimui)

if(BUILD_FYTIMUI_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Internal scaffolding for eyeballing layout; built with the tests.
if(BUILD_FYTIMUI_TESTS)
    add_subdirectory(examples)
endif()
