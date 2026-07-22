# ---------------------------------------------------------------------------
# Vendored timui core tests.
#
# These are the upstream timui.h unit tests, carried over verbatim so a core
# sync can be validated here rather than only in the upstream tree. They link
# the core directly (not libfytimui) because they exercise timui's own API,
# which is deliberately not part of this library's public surface.
#
# Each case is registered with CTest individually. The names come from the
# built runner's own --list output rather than from parsing a source file:
# test.h declares cases that live in other binaries (vterm round-trip, ConPTY,
# no-images), and test_main.c's table is partly guarded by preprocessor
# conditionals (TIMUI_WITH_VTERM_TESTS), so only the binary knows the true set.
# ---------------------------------------------------------------------------
if(BUILD_TIMUI_CORE_TESTS)
    set(TIMUI_CORE_TEST_SRCS
        test_main.c test_rect.c test_result.c test_arena.c test_strings.c
        test_id_stack.c test_msgq.c test_mpsc.c test_transport.c test_screen.c
        test_input.c test_mouse.c test_termios.c test_size.c test_caps.c
        test_kitty.c test_sync.c test_cells.c test_utf8.c test_draw.c
        test_render.c test_cursor.c test_frame.c test_interact.c test_theme.c
        test_stylesheet.c test_button.c test_widgets.c test_input_widget.c
        test_listbox.c test_grid_widget.c test_dialog.c test_fuzz.c
        test_clip.c test_menus.c test_modal.c test_hyperlink.c
        test_esc_timeout.c test_scroll.c test_v02_batch.c test_v02_widgets.c
        test_v02_more.c test_images_pty.c test_review_critical.c
        test_snapshot.c test_coverage_z7.c test_render_stream.c
        test_external_poll.c test_mouse_drag.c
        test_inline.c test_local_core.c)

    # Tier A visual tests: only when libvterm is available. The escapes are
    # replayed into a real VT emulator and the screen grid is compared, which
    # catches renderer bugs that byte-substring checks cannot.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(VTERM QUIET vterm)
    endif()
    if(VTERM_FOUND)
        list(APPEND TIMUI_CORE_TEST_SRCS test_vt_roundtrip.c)
    endif()

    add_executable(timui_core_tests ${TIMUI_CORE_TEST_SRCS}
                   $<TARGET_OBJECTS:fytimui-core>)
    target_include_directories(timui_core_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR})
    if(VTERM_FOUND)
        target_compile_definitions(timui_core_tests PRIVATE TIMUI_WITH_VTERM_TESTS)
        target_include_directories(timui_core_tests PRIVATE ${VTERM_INCLUDE_DIRS})
        target_link_libraries(timui_core_tests PRIVATE ${VTERM_LIBRARIES})
    endif()
    if(ASAN_C_FLAGS)
        target_compile_options(timui_core_tests PRIVATE ${ASAN_C_FLAGS})
        target_link_options(timui_core_tests PRIVATE ${ASAN_C_FLAGS})
    endif()

    # Several cases read fixtures (goldens, drive scripts) by relative path.
    set(TIMUI_TEST_WORKDIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # After the runner links, ask it for its case list and generate the
    # add_test() calls; CTest reads them via TEST_INCLUDE_FILES at test time.
    set(_disc ${CMAKE_CURRENT_BINARY_DIR}/timui_core_tests_cases.cmake)
    add_custom_command(TARGET timui_core_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -D RUNNER=$<TARGET_FILE:timui_core_tests>
                -D OUTFILE=${_disc}
                -D WORKDIR=${TIMUI_TEST_WORKDIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/DiscoverTests.cmake
        VERBATIM)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${_disc})
endif()

# ---- libfytimui's own tests ------------------------------------------------
# These link the sources directly rather than the library, so internal units
# (the SGR parser) can be tested without exporting them.
add_executable(test_fytim_sgr test_fytim_sgr.c ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c)
target_include_directories(test_fytim_sgr PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(ASAN_C_FLAGS)
    target_compile_options(test_fytim_sgr PRIVATE ${ASAN_C_FLAGS})
    target_link_options(test_fytim_sgr PRIVATE ${ASAN_C_FLAGS})
endif()

add_executable(test_fytim_layout test_fytim_layout.c
               ${CMAKE_SOURCE_DIR}/src/fytim_layout.c)
target_include_directories(test_fytim_layout PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src)
if(ASAN_C_FLAGS)
    target_compile_options(test_fytim_layout PRIVATE ${ASAN_C_FLAGS})
    target_link_options(test_fytim_layout PRIVATE ${ASAN_C_FLAGS})
endif()

foreach(t full_size only_transcript_grows width_propagates
          multirow_prompt_grows multirow_prompt_sheds_last
          minimum_untruncated degrades_in_priority_order
          no_trailing_separator prompt_always_present degenerate_geometry_rejected
          null_out_safe extreme_dimensions_safe deterministic)
    add_test(NAME fytim.layout.${t} COMMAND test_fytim_layout ${t})
endforeach()

# The band tests exercise the PUBLIC surface, but still link the sources
# directly (with the core objects) like the other fytim.* suites, so they run
# without an installed library.
add_executable(test_fytim_band test_fytim_band.c
               ${CMAKE_SOURCE_DIR}/src/libfytimui.c
               ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
               ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
               $<TARGET_OBJECTS:fytimui-core>)
target_include_directories(test_fytim_band PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/core/include)
if(ASAN_C_FLAGS)
    target_compile_options(test_fytim_band PRIVATE ${ASAN_C_FLAGS})
    target_link_options(test_fytim_band PRIVATE ${ASAN_C_FLAGS})
endif()

foreach(t create_paints_inline commit_batches_into_pump
          commit_rejects_disallowed line_event_on_enter esc_emits_quit
          history_recall completion chrome_and_workband
          workband_lifecycle workband_caps_last_rows workband_top_bottom
          workband_order_and_independence workband_rejects_disallowed
          workband_commit_defers_during_stream
          workband_defer_survives_empty_tail
          workband_commit_payload workband_commit_payload_defers
          regression_no_shrink_while_streaming
          workband_shed_oldest ctrl_g_edit_and_suspend
          regression_idle_band_keeps_status size_accessor sync_brackets_updates
          transcript_tail tail_apply null_safety)
    add_test(NAME fytim.band.${t} COMMAND test_fytim_band ${t})
endforeach()

# Visual tests over libvterm: the public surface replayed into a real VT
# emulator, asserting on the screen grid. Only when libvterm is available;
# uses the pkg-config probe from the core-test section above.
if(VTERM_FOUND)
    add_executable(test_fytim_vt test_fytim_vt.c
                   ${CMAKE_SOURCE_DIR}/src/libfytimui.c
                   ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
                   ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
                   $<TARGET_OBJECTS:fytimui-core>)
    target_include_directories(test_fytim_vt PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/core/include
        ${VTERM_INCLUDE_DIRS})
    target_link_libraries(test_fytim_vt PRIVATE ${VTERM_LIBRARIES} util)
    if(ASAN_C_FLAGS)
        target_compile_options(test_fytim_vt PRIVATE ${ASAN_C_FLAGS})
        target_link_options(test_fytim_vt PRIVATE ${ASAN_C_FLAGS})
    endif()
    foreach(t regression_workband_cap_keeps_chrome
              regression_resize_repaints_width
              regression_settle_keeps_chrome_row
              regression_sgr_carries_across_rows
              regression_sgr_survives_freeze_cut)
        add_test(NAME fytim.vt.${t} COMMAND test_fytim_vt ${t})
    endforeach()
endif()

foreach(t plain_text bold_run combined_params truecolor indexed_color
          style_persists_across_feeds escape_split_across_feeds
          disallowed_sequences_flagged osc8_hyperlink_allowed
          other_osc_still_disallowed malformed_input_safe
          overlong_escape_safe null_and_empty_safe
          style_emit_roundtrip style_emit_default_and_tiny)
    add_test(NAME fytim.sgr.${t} COMMAND test_fytim_sgr ${t})
endforeach()
