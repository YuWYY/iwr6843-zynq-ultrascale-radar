set(APP ${CMAKE_CURRENT_LIST_DIR}/../radar_runtime/src)
add_executable(radar_autoboot_tests test_autoboot.c ${APP}/app/radar_autoboot.c)
target_include_directories(radar_autoboot_tests PRIVATE mock_bsp vendor_amd
    ${APP}/app ${APP}/hal ${APP}/capture ${APP}/config ${APP}/processing ${APP}/display)
target_compile_definitions(radar_autoboot_tests PRIVATE SDT)
target_compile_options(radar_autoboot_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME deployment_autoboot_state_machine COMMAND radar_autoboot_tests)
add_library(amd_sg_host STATIC
    vendor_amd/xaxidma.c vendor_amd/xaxidma_bd.c vendor_amd/xaxidma_bdring.c)
target_include_directories(amd_sg_host PUBLIC mock_bsp vendor_amd)
target_compile_definitions(amd_sg_host PUBLIC SDT)
# Real upstream driver is preserved verbatim, warning policy not rewritten.
target_compile_options(amd_sg_host PRIVATE -Wall -Wextra)
add_library(radar_host_app STATIC
    ${APP}/hal/radar_control.c ${APP}/hal/radar_cli.c ${APP}/app/radar_console.c
    ${APP}/app/radar_sw2.c ${APP}/app/radar_sw3.c ${APP}/hal/radar_dma.c
    ${APP}/capture/radar_capture_profile.c ${APP}/capture/radar_snapshot.c
    ${APP}/capture/radar_session.c ${APP}/capture/radar_crc32.c ${APP}/capture/radar_export.c
    ${APP}/hal/radar_dsp.c ${APP}/hal/radar_dsp_platform.c)
target_include_directories(radar_host_app PUBLIC ${APP}/app ${APP}/hal ${APP}/capture ${APP}/config ${APP}/processing ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(radar_host_app PUBLIC amd_sg_host)
target_compile_options(radar_host_app PRIVATE -Wall -Wextra -Werror)
add_executable(radar_runtime_tests test_runtime.c model.c)
target_link_libraries(radar_runtime_tests PRIVATE radar_host_app)
target_compile_options(radar_runtime_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME runtime_regressions COMMAND radar_runtime_tests)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME tools_regressions COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_tools.py)
set_tests_properties(runtime_regressions PROPERTIES
    ENVIRONMENT "RADAR_TEST_EXPORT_LOG=${CMAKE_BINARY_DIR}/host_export.log")
set_tests_properties(tools_regressions PROPERTIES
    DEPENDS runtime_regressions
    ENVIRONMENT "RADAR_TEST_EXPORT_LOG=${CMAKE_BINARY_DIR}/host_export.log")

add_executable(radar_finite_profile_tests test_finite_profile.c model.c)
target_link_libraries(radar_finite_profile_tests PRIVATE radar_host_app)
target_compile_options(radar_finite_profile_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME finite_profile_runtime COMMAND radar_finite_profile_tests)
set_tests_properties(finite_profile_runtime PROPERTIES
    ENVIRONMENT "RADAR_FINITE_EXPORT_LOG=${CMAKE_BINARY_DIR}/finite_export.log"
    FIXTURES_SETUP finite_export)
add_test(NAME finite_profile_generator COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_finite_profile.py)
add_test(NAME finite_export_decode COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_finite_export.py ${CMAKE_BINARY_DIR}/finite_export.log)
set_tests_properties(finite_export_decode PROPERTIES FIXTURES_REQUIRED finite_export)
add_test(NAME adc_decoder_regressions COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_adc.py)
add_executable(radar_display_edid_tests test_display_edid.c)
target_compile_options(radar_display_edid_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME display_edid_regressions COMMAND radar_display_edid_tests)
include(${CMAKE_CURRENT_LIST_DIR}/range_tests.cmake)
add_executable(radar_spatial_tests test_spatial_core.c
    ${APP}/processing/radar_spatial.c ${APP}/processing/radar_range.c)
target_include_directories(radar_spatial_tests PRIVATE ${APP}/processing)
target_compile_options(radar_spatial_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(radar_spatial_tests PRIVATE m)
add_test(NAME spatial_dsp_regressions COMMAND radar_spatial_tests)
add_test(NAME spatial_python_reference COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_spatial.py)
add_executable(radar_range_quality_tests test_range_quality.c ${APP}/processing/radar_range_quality.c)
target_include_directories(radar_range_quality_tests PRIVATE ${APP}/processing)
target_compile_options(radar_range_quality_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(radar_range_quality_tests PRIVATE m)
add_test(NAME range_quality_regressions COMMAND radar_range_quality_tests)
add_executable(radar_view_schedule_tests test_view_schedule.c ${APP}/display/radar_view_schedule.c)
target_include_directories(radar_view_schedule_tests PRIVATE ${APP}/display ${APP}/processing)
target_compile_options(radar_view_schedule_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME view_scheduler_regressions COMMAND radar_view_schedule_tests)

add_executable(radar_scan_tests test_scan.c
    ${APP}/processing/radar_scan.c ${APP}/capture/radar_crc32.c
    ${APP}/hal/radar_dsp.c ${APP}/processing/radar_frame_queue.c ${APP}/processing/radar_freshness.c)
target_include_directories(radar_scan_tests PRIVATE mock_bsp vendor_amd
    ${APP}/app ${APP}/hal ${APP}/capture ${APP}/config ${APP}/processing)
target_compile_definitions(radar_scan_tests PRIVATE SDT)
target_compile_options(radar_scan_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME scan_scheduler_regressions COMMAND radar_scan_tests)
target_link_libraries(radar_scan_tests PRIVATE m)

add_executable(radar_dsp_driver_tests test_dsp_driver.c ${APP}/hal/radar_dsp.c)
target_include_directories(radar_dsp_driver_tests PRIVATE ${APP}/hal ${APP}/processing)
target_compile_options(radar_dsp_driver_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME pl_dsp_driver_contract COMMAND radar_dsp_driver_tests)
add_executable(radar_pl_import_tests test_pl_import.c ${APP}/processing/radar_range.c ${APP}/processing/radar_spatial.c)
target_include_directories(radar_pl_import_tests PRIVATE ${APP}/processing)
target_compile_options(radar_pl_import_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(radar_pl_import_tests PRIVATE m)
add_test(NAME pl_dsp_import_scaling COMMAND radar_pl_import_tests)
add_executable(radar_frame_view_queue_tests test_frame_view_queue.c ${APP}/processing/radar_frame_queue.c ${APP}/display/radar_view_cache.c)
target_include_directories(radar_frame_view_queue_tests PRIVATE ${APP}/processing ${APP}/display)
target_compile_options(radar_frame_view_queue_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME immutable_frame_view_queues COMMAND radar_frame_view_queue_tests)
add_executable(radar_scope_time_tests test_scope_time.c ${APP}/processing/radar_range_quality.c ${APP}/display/radar_font_zh.c)
target_include_directories(radar_scope_time_tests PRIVATE ${APP}/processing ${APP}/display)
target_compile_options(radar_scope_time_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(radar_scope_time_tests PRIVATE m)
add_test(NAME scope_timestamp_quality COMMAND radar_scope_time_tests)
add_test(NAME dsp_queue_capacity_model COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_dsp_queue_model.py)
add_executable(radar_freshness_tests test_freshness.c ${APP}/processing/radar_freshness.c ${APP}/display/radar_view_cache.c)
target_include_directories(radar_freshness_tests PRIVATE ${APP}/processing ${APP}/display)
target_compile_options(radar_freshness_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME immutable_sample_freshness COMMAND radar_freshness_tests)

add_executable(radar_stream_capture_tests test_stream_capture.c model.c
    ${APP}/capture/radar_stream.c ${APP}/capture/radar_raw_slots.c
    ${APP}/processing/radar_range.c)
target_link_libraries(radar_stream_capture_tests PRIVATE radar_host_app m)
target_compile_options(radar_stream_capture_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME stream_capture_producer COMMAND radar_stream_capture_tests)
set_tests_properties(stream_capture_producer PROPERTIES TIMEOUT 60)
add_executable(radar_stream_scan_tests test_stream_scan.c
    ${APP}/processing/radar_stream_scan.c ${APP}/processing/radar_range.c
    ${APP}/processing/radar_spatial.c ${APP}/processing/radar_scene.c
    ${APP}/processing/radar_freshness.c ${APP}/capture/radar_raw_slots.c
    ${APP}/capture/radar_capture_profile.c ${APP}/capture/radar_crc32.c
    ${APP}/hal/radar_dsp.c ${APP}/hal/radar_time_bridge.c)
target_include_directories(radar_stream_scan_tests PRIVATE mock_bsp vendor_amd
    ${APP}/app ${APP}/hal ${APP}/capture ${APP}/config ${APP}/processing ${APP}/display)
target_compile_definitions(radar_stream_scan_tests PRIVATE SDT)
target_compile_options(radar_stream_scan_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(radar_stream_scan_tests PRIVATE m)
add_test(NAME stream_scan_dualslot COMMAND radar_stream_scan_tests)
set_tests_properties(stream_scan_dualslot PROPERTIES TIMEOUT 60)
add_test(NAME stream_profile_generator COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_stream_profile.py)

option(RADAR_UBSAN "Instrument host model and application for undefined behavior" OFF)
if(RADAR_UBSAN)
    foreach(t amd_sg_host radar_host_app radar_runtime_tests radar_finite_profile_tests radar_display_edid_tests radar_scan_tests radar_spatial_tests radar_view_schedule_tests radar_range_quality_tests radar_dsp_driver_tests radar_pl_import_tests radar_frame_view_queue_tests radar_scope_time_tests radar_freshness_tests radar_stream_capture_tests radar_stream_scan_tests)
        target_compile_options(${t} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    endforeach()
    foreach(t radar_runtime_tests radar_finite_profile_tests radar_display_edid_tests radar_scan_tests radar_spatial_tests radar_view_schedule_tests radar_range_quality_tests radar_dsp_driver_tests radar_pl_import_tests radar_frame_view_queue_tests radar_scope_time_tests radar_freshness_tests radar_stream_capture_tests radar_stream_scan_tests)
        target_link_options(${t} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    endforeach()
endif()
