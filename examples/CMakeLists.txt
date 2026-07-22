# Examples over the PUBLIC interface only: they link the library and see no
# core header. These targets existing and linking is itself the check that
# the fytim_ surface covers the agent's needs.
add_executable(agent_inline agent_inline.c)
target_link_libraries(agent_inline PRIVATE fytimui)
target_include_directories(agent_inline PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(ASAN_C_FLAGS)
    target_compile_options(agent_inline PRIVATE ${ASAN_C_FLAGS})
    target_link_options(agent_inline PRIVATE ${ASAN_C_FLAGS})
endif()

# The full stack: HOST-side libfymd4c markdown rendering published through
# the public surface. Only when libfymd4c's CMake package is installed.
find_package(libfymd4c CONFIG QUIET)
if(libfymd4c_FOUND)
    add_executable(agent_md agent_md.c)
    target_link_libraries(agent_md PRIVATE fytimui libfymd4c::fymd4c)
    target_include_directories(agent_md PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(ASAN_C_FLAGS)
        target_compile_options(agent_md PRIVATE ${ASAN_C_FLAGS})
        target_link_options(agent_md PRIVATE ${ASAN_C_FLAGS})
    endif()
endif()
