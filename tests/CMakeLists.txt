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

    # Tier A visual tests: only when libfyvterm is available. The escapes are
    # replayed into a real VT emulator and the screen grid is compared, which
    # catches renderer bugs that byte-substring checks cannot.
    #
    # A parent project may already provide the target (its own find_package or
    # add_subdirectory, e.g. when libfytimui is pulled in via fyai); reuse that
    # rather than re-resolving the library.
    if(NOT TARGET libfyvterm::libfyvterm)
        find_package(libfyvterm CONFIG QUIET)
    endif()
    if(TARGET libfyvterm::libfyvterm)
        set(VTERM_FOUND TRUE)
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
        target_link_libraries(timui_core_tests PRIVATE libfyvterm::libfyvterm)
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
target_include_directories(test_fytim_sgr PRIVATE ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include)
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

foreach(t full_size no_prompt_frees_its_rows no_prompt_degenerate_heights
          prompt_survives_when_asked_for only_transcript_grows width_propagates
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
          commit_rejects_disallowed line_event_on_enter esc_emits_interrupt
          page_key_emits_scrollback history_recall completion
          chrome_and_workband chrome_style_contract
          workband_lifecycle workband_caps_last_rows workband_top_bottom
          workband_trailing_newline_not_a_row
          workband_order_and_independence workband_rejects_disallowed
          workband_commit_defers_during_stream
          workband_defer_survives_empty_tail
          workband_commit_payload workband_commit_payload_defers
          large_commit_spills_intact sgr_carries_across_commits
          workband_shed_oldest ctrl_g_edit_and_suspend
          regression_idle_band_keeps_status size_accessor sync_brackets_updates
          transcript_tail tail_apply null_safety)
    add_test(NAME fytim.band.${t} COMMAND test_fytim_band ${t})
endforeach()

# The cell surface: the public interface, driven through pipes like the band
# tests. What a cell looks like on a real terminal is test_fytim_surface_vt.
add_executable(test_fytim_surface test_fytim_surface.c
               ${CMAKE_SOURCE_DIR}/src/libfytimui.c
               ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
               ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
               $<TARGET_OBJECTS:fytimui-core>)
target_include_directories(test_fytim_surface PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/core/include)
if(ASAN_C_FLAGS)
    target_compile_options(test_fytim_surface PRIVATE ${ASAN_C_FLAGS})
    target_link_options(test_fytim_surface PRIVATE ${ASAN_C_FLAGS})
endif()

foreach(t open_reports_its_size put_row_paints put_row_after_a_frame_paints
          close_removes_content
          composes_with_workband two_surfaces_are_independent
          granted_rows_respects_cap resize_keeps_content
          clear_blanks_the_grid chrome_rows_paint commit_keeps_the_screen
          commit_keeps_the_chrome margin_takes_columns_from_the_grid
          commit_keeps_the_margin
          chrome_survives_a_short_region
          rejects_bad_geometry
          put_row_clips_to_width cursor_is_bounded null_safety
          destroy_with_open_surface)
    add_test(NAME fytim.surface.${t} COMMAND test_fytim_surface ${t})
endforeach()

# One region that tiles the screens it holds: the grid, the chrome, and what
# each tile is told it was given.
add_executable(test_fytim_workpane test_fytim_workpane.c
               ${CMAKE_SOURCE_DIR}/src/libfytimui.c
               ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
               ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
               $<TARGET_OBJECTS:fytimui-core>)
target_include_directories(test_fytim_workpane PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/core/include)
if(ASAN_C_FLAGS)
    target_compile_options(test_fytim_workpane PRIVATE ${ASAN_C_FLAGS})
    target_link_options(test_fytim_workpane PRIVATE ${ASAN_C_FLAGS})
endif()

foreach(t two_tiles_share_the_width one_tile_takes_the_width
          the_grid_wraps a_narrow_region_stacks columns_are_settable
          zoom_takes_the_pane a_retired_zoom_is_dropped
          zoom_refuses_a_stranger
          a_margin_takes_columns_from_the_tile
          chrome_frames_the_pane_and_the_tiles
          a_separator_divides_the_columns the_cap_bounds_the_pane
          closing_tiles_empties_the_pane commit_retires_a_tile
          a_pane_composes_with_a_band a_tile_keeps_its_own_cap
          a_resized_tile_is_regranted scroll_extent_is_checked
          controls_are_off_by_default rejects_bad_geometry null_safety
          destroy_with_open_tiles
          controls_need_the_grab controls_take_a_column
          a_click_asks_to_close a_click_asks_to_zoom
          the_wheel_belongs_to_the_tile the_wheel_stays_with_the_transcript
          the_bar_follows_the_extent
          a_band_is_a_tile_too two_bands_tile a_band_tile_commits
          a_band_tile_keeps_its_cap)
    add_test(NAME fytim.workpane.${t} COMMAND test_fytim_workpane ${t})
endforeach()

# Keys handed to a surface: the encoding, and the prompt standing aside.
add_executable(test_fytim_surface_keys test_fytim_surface_keys.c
               ${CMAKE_SOURCE_DIR}/src/libfytimui.c
               ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
               ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
               $<TARGET_OBJECTS:fytimui-core>)
target_include_directories(test_fytim_surface_keys PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/core/include)
if(ASAN_C_FLAGS)
    target_compile_options(test_fytim_surface_keys PRIVATE ${ASAN_C_FLAGS})
    target_link_options(test_fytim_surface_keys PRIVATE ${ASAN_C_FLAGS})
endif()

foreach(t typed_text_reaches_the_surface enter_is_a_return
          control_chord_is_a_control_byte every_control_byte_arrives
          arrow_is_a_csi_sequence
          editing_keys_keep_their_codes keys_keep_their_order
          repeated_keys_all_arrive prompt_does_not_see_the_keys
          keys_return_to_the_prompt close_returns_the_keys
          only_one_surface_holds_the_keys content_paints_while_holding_keys
          null_safety)
    add_test(NAME fytim.surface.keys.${t}
             COMMAND test_fytim_surface_keys ${t})
endforeach()

# The terminal changed size: needs a real pty, because a pipe has no size.
add_executable(test_fytim_resize test_fytim_resize.c
               ${CMAKE_SOURCE_DIR}/src/libfytimui.c
               ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
               ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
               $<TARGET_OBJECTS:fytimui-core>)
target_include_directories(test_fytim_resize PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(test_fytim_resize PRIVATE Threads::Threads)
if(ASAN_C_FLAGS)
    target_compile_options(test_fytim_resize PRIVATE ${ASAN_C_FLAGS})
    target_link_options(test_fytim_resize PRIVATE ${ASAN_C_FLAGS})
endif()

foreach(t resize_is_seen resize_is_seen_with_keys
          granted_rows_follow_the_window
          clear_screen_erases_the_screen ctrl_l_is_reported)
    add_test(NAME fytim.resize.${t} COMMAND test_fytim_resize ${t})
endforeach()

# What a published cell looks like on a REAL terminal: only with libvterm.
if(VTERM_FOUND)
    add_executable(test_fytim_surface_vt test_fytim_surface_vt.c
                   ${CMAKE_SOURCE_DIR}/src/libfytimui.c
                   ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
                   ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
                   $<TARGET_OBJECTS:fytimui-core>)
    target_include_directories(test_fytim_surface_vt PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(test_fytim_surface_vt PRIVATE libfyvterm::libfyvterm)
    if(ASAN_C_FLAGS)
        target_compile_options(test_fytim_surface_vt PRIVATE ${ASAN_C_FLAGS})
        target_link_options(test_fytim_surface_vt PRIVATE ${ASAN_C_FLAGS})
    endif()
    foreach(t colour_reaches_the_cell prompt_leaves_while_a_surface_holds_keys
              prompt_leaves_when_it_is_not_wanted
              empty_chrome_gives_its_rows_back margin_shifts_the_grid
              wide_glyph_keeps_the_row
              combining_stays_one_cell cursor_is_a_reverse_cell
              short_region_keeps_the_last_rows two_surfaces_stack_in_order)
        add_test(NAME fytim.surface.vt.${t}
                 COMMAND test_fytim_surface_vt ${t})
    endforeach()

    # The same reading for a tiled pane: two screens on one row, in columns
    # that do not overlap. Only a terminal can say that.
    add_executable(test_fytim_workpane_vt test_fytim_workpane_vt.c
                   ${CMAKE_SOURCE_DIR}/src/libfytimui.c
                   ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
                   ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
                   $<TARGET_OBJECTS:fytimui-core>)
    target_include_directories(test_fytim_workpane_vt PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(test_fytim_workpane_vt PRIVATE libfyvterm::libfyvterm)
    if(ASAN_C_FLAGS)
        target_compile_options(test_fytim_workpane_vt PRIVATE ${ASAN_C_FLAGS})
        target_link_options(test_fytim_workpane_vt PRIVATE ${ASAN_C_FLAGS})
    endif()

    foreach(t two_tiles_stand_side_by_side the_tiles_share_every_row
              a_wide_screen_is_clipped_to_its_tile
              the_rule_sits_between_the_tiles
              a_colour_stops_at_the_tile_edge
              the_grid_puts_the_third_below a_zoomed_tile_owns_the_rows
              a_title_stays_over_its_tile
              a_head_of_two_rows_draws_both
              a_short_tile_sheds_the_last_head_row
              a_head_with_no_content_takes_no_row
              a_styled_head_is_not_dimmed
              a_margin_is_drawn_inside_the_tile
              two_band_tiles_stand_side_by_side
              a_wide_row_is_clipped_to_its_tile)
        add_test(NAME fytim.workpane.vt.${t}
                 COMMAND test_fytim_workpane_vt ${t})
    endforeach()
endif()

# The bubble invariant against the REAL renderer: only when libfymd4c is
# ALSO installed. Streams actual markdown through fymd_render_push into
# fytim_tail_apply and asserts the chrome row after every pump.
if(VTERM_FOUND)
    find_package(libfymd4c CONFIG QUIET)
    if(libfymd4c_FOUND)
        add_executable(test_fytim_md_vt test_fytim_md_vt.c
                       ${CMAKE_SOURCE_DIR}/src/libfytimui.c
                       ${CMAKE_SOURCE_DIR}/src/fytim_sgr.c
                       ${CMAKE_SOURCE_DIR}/src/fytim_layout.c
                       $<TARGET_OBJECTS:fytimui-core>)
        target_include_directories(test_fytim_md_vt PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/core/include)
        target_link_libraries(test_fytim_md_vt PRIVATE
            libfymd4c::fymd4c libfyvterm::libfyvterm util)
        if(ASAN_C_FLAGS)
            target_compile_options(test_fytim_md_vt PRIVATE ${ASAN_C_FLAGS})
            target_link_options(test_fytim_md_vt PRIVATE ${ASAN_C_FLAGS})
        endif()
        foreach(t reverse_commit_matches_direct
                  fenced_stream_matches_direct
                  emoji_table_matches_direct
                  bubble_pinned_bytes bubble_pinned_lines bubble_pinned_single)
            add_test(NAME fytim.mdvt.${t} COMMAND test_fytim_md_vt ${t})
        endforeach()
    endif()
endif()

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
        )
    target_link_libraries(test_fytim_vt PRIVATE libfyvterm::libfyvterm util)
    if(ASAN_C_FLAGS)
        target_compile_options(test_fytim_vt PRIVATE ${ASAN_C_FLAGS})
        target_link_options(test_fytim_vt PRIVATE ${ASAN_C_FLAGS})
    endif()
    foreach(t prompt_style_fills_card prompt_wraps_and_grows
              prompt_reflows_after_resize
              regression_workband_cap_keeps_chrome
              regression_resize_repaints_width
              regression_bubble_pinned_while_streaming
              regression_indexed_colors_mapped
              regression_marker_sgr
              regression_unmatched_shrink_held
              regression_settle_compacts
              regression_sgr_carries_across_rows
              regression_sgr_survives_freeze_cut)
        add_test(NAME fytim.vt.${t} COMMAND test_fytim_vt ${t})
    endforeach()
endif()

foreach(t plain_text bold_run combined_params truecolor indexed_color
          style_persists_across_feeds escape_split_across_feeds
          disallowed_sequences_flagged osc8_hyperlink_allowed
          osc8_long_url_allowed
          other_osc_still_disallowed malformed_input_safe
          overlong_escape_safe null_and_empty_safe
          style_emit_roundtrip style_emit_default_and_tiny
          runs_never_split_utf8)
    add_test(NAME fytim.sgr.${t} COMMAND test_fytim_sgr ${t})
endforeach()
