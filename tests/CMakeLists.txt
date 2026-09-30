find_package(GTest)
if(NOT GTest_FOUND)
    message(FATAL_ERROR "BUILD_TESTING=ON needs GoogleTest (Debian: libgtest-dev)")
endif()
find_package(Qt6 REQUIRED COMPONENTS Test)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)

# A test that neither defines main() nor makes an application object of its
# own gets gtest_qt_main.cpp, whose QCoreApplication owns the single-shot
# timers the code under test leaves pending.
function(cutefish_add_test name)
    file(STRINGS ${name}.cpp own_main REGEX "^int main\\(")
    file(STRINGS ${name}.cpp own_app
         REGEX "(new Q(Core|Gui)Application)|(Q(Core|Gui)Application (app|application)[ ({])")
    if(own_main)
        add_executable(${name} ${name}.cpp)
        target_link_libraries(${name} PRIVATE GTest::gtest ${ARGN})
    elseif(own_app)
        add_executable(${name} ${name}.cpp)
        target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main ${ARGN})
    else()
        add_executable(${name} ${name}.cpp gtest_qt_main.cpp)
        target_link_libraries(${name} PRIVATE GTest::gtest Qt6::Core ${ARGN})
    endif()
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

cutefish_add_test(image_capture_test cutefish-compositor-core cutefish-session PkgConfig::WaylandClient)
cutefish_add_test(serial_tracker_test cutefish-wayland)
cutefish_add_test(client_tracker_test cutefish-wayland)
cutefish_add_test(wayland_server_test cutefish-wayland PkgConfig::WaylandClient)
cutefish_add_test(scene_test cutefish-scene)
cutefish_add_test(shm_pool_test cutefish-compositor-core)
cutefish_add_test(buffer_test cutefish-compositor-core)
cutefish_add_test(surface_test cutefish-compositor-core)
cutefish_add_test(wayland_protocol_test cutefish-compositor-core cutefish-wayland-dmabuf PkgConfig::WaylandClient)
cutefish_add_test(xkb_tracker_test cutefish-input)
cutefish_add_test(key_translator_test cutefish-input)
cutefish_add_test(input_test cutefish-input)
cutefish_add_test(computer_control_test cutefish-computer-control)
cutefish_add_test(touch_test cutefish-input)
cutefish_add_test(pointer_acceleration_test cutefish-mouse-settings)
cutefish_add_test(touchpad_click_test cutefish-mouse-settings)
add_executable(mouse_settings_test mouse_settings_test.cpp)
target_link_libraries(mouse_settings_test PRIVATE cutefish-mouse-settings Qt6::Test GTest::gtest)
find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
add_test(NAME mouse_settings_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:mouse_settings_test>)
add_executable(touchpad_settings_test touchpad_settings_test.cpp)
target_link_libraries(touchpad_settings_test PRIVATE cutefish-mouse-settings Qt6::Test GTest::gtest)
add_test(NAME touchpad_settings_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:touchpad_settings_test>)
add_executable(touchscreen_settings_test touchscreen_settings_test.cpp)
target_link_libraries(touchscreen_settings_test PRIVATE cutefish-mouse-settings Qt6::Test GTest::gtest)
add_test(NAME touchscreen_settings_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:touchscreen_settings_test>)
add_executable(keyboard_settings_test keyboard_settings_test.cpp)
target_link_libraries(keyboard_settings_test PRIVATE cutefish-keyboard-settings Qt6::Test GTest::gtest)
add_test(NAME keyboard_settings_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:keyboard_settings_test>)
cutefish_add_test(drm_selection_test cutefish-backend)
cutefish_add_test(drm_properties_test cutefish-backend)
cutefish_add_test(drm_configuration_planner_test cutefish-fake-drm)
cutefish_add_test(drm_multi_device_planner_test cutefish-backend)
cutefish_add_test(drm_hotplug_monitor_test cutefish-backend Qt6::Test)
cutefish_add_test(libseat_session_test cutefish-backend)

# Manual wl_shm client. Not registered with ctest: it needs a running
# compositor and a screen to look at.
add_executable(cutefish-shm-demo manual/shm_client.cpp)
target_link_libraries(cutefish-shm-demo PRIVATE PkgConfig::WaylandClient)

cutefish_add_test(sub_surface_test cutefish-compositor-core)
cutefish_add_test(wm_test cutefish-wm)
cutefish_add_test(subsurface_output_protocol_test cutefish-compositor-core PkgConfig::WaylandClient)
cutefish_add_test(seat_protocol_test cutefish-wayland-input cutefish-wm
    PkgConfig::WaylandClient PkgConfig::Xkbcommon)
cutefish_add_test(xdg_positioner_test cutefish-wayland-shell)
cutefish_add_test(qt_hardware_integration_test cutefish-wayland-shell)
cutefish_add_test(shell_surface_test cutefish-decoration)
cutefish_add_test(decoration_test cutefish-decoration)
cutefish_add_test(text_test cutefish-decoration)
cutefish_add_test(xdg_shell_protocol_test cutefish-wayland-shell cutefish-wayland-input
    PkgConfig::WaylandClient)
cutefish_add_test(protocol_binding_test cutefish-wayland-globals PkgConfig::WaylandClient)
set_tests_properties(protocol_binding_test PROPERTIES TIMEOUT 60)
cutefish_add_test(xdg_foreign_protocol_test cutefish-wayland-shell cutefish-wayland-input
    PkgConfig::WaylandClient)
cutefish_add_test(xdg_toplevel_drag_protocol_test cutefish-wayland-shell cutefish-wayland-input
    PkgConfig::WaylandClient)
cutefish_add_test(foreign_toplevel_list_test cutefish-wayland-private-shell cutefish-input
    cutefish-compositor-core PkgConfig::WaylandClient)
cutefish_add_test(private_shell_protocol_test cutefish-wayland-private-shell
    cutefish-wayland-shell cutefish-wayland-input PkgConfig::WaylandClient)
cutefish_add_test(work_area_test cutefish-wm)
add_executable(dock_layout_test dock_layout_test.cpp)
target_include_directories(dock_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dock_layout_test PRIVATE cutefish-wm GTest::gtest)
add_test(NAME dock_layout_test COMMAND dock_layout_test)
cutefish_add_test(session_lock_test cutefish-session cutefish-wm cutefish-scene)
add_executable(accelerator_test accelerator_test.cpp)
target_include_directories(accelerator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(accelerator_test PRIVATE cutefish-accelerators cutefish-session cutefish-wm
    cutefish-scene Qt6::Test GTest::gtest)
add_test(NAME accelerator_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:accelerator_test>)
cutefish_add_test(xdg_decoration_protocol_test cutefish-wayland-shell cutefish-wayland-input
    cutefish-decoration PkgConfig::WaylandClient)
add_executable(cutefish-subsurface-demo manual/subsurface_client.cpp)
target_link_libraries(cutefish-subsurface-demo PRIVATE PkgConfig::WaylandClient)

# Manual xdg-shell demo. --script runs the whole toplevel state machine
# and exits, which is what the VT harness drives.
add_executable(cutefish-xdg-demo manual/xdg_client.cpp)
target_link_libraries(cutefish-xdg-demo PRIVATE PkgConfig::WaylandClient cutefish-protocols)

# Manual xdg-decoration demo. Draws only its client area, so everything
# around it on screen is the compositor's own frame.
add_executable(cutefish-decoration-demo manual/decoration_client.cpp)
target_link_libraries(cutefish-decoration-demo
    PRIVATE PkgConfig::WaylandClient cutefish-protocols)

# Development harness that plays the session's role for the shell: it obtains a
# privileged Wayland connection from a running compositor and execs a program
# with it. Not registered with ctest; it needs a compositor and a shell.
add_executable(cutefish-shell-session-launcher manual/shell_session_launcher.cpp)

# Manual input demo. Needs a running compositor, a screen and a mouse.
add_executable(cutefish-input-demo manual/input_client.cpp)
target_link_libraries(cutefish-input-demo PRIVATE PkgConfig::WaylandClient PkgConfig::Xkbcommon)
target_compile_definitions(subsurface_output_protocol_test PRIVATE
    SUBSURFACE_DEMO_PATH="$<TARGET_FILE:cutefish-subsurface-demo>")
add_dependencies(subsurface_output_protocol_test cutefish-subsurface-demo)

cutefish_add_test(output_scheduler_test cutefish-output-scheduler cutefish-compositor-core cutefish-decoration)

cutefish_add_test(clipboard_protocol_test cutefish-wayland-input PkgConfig::WaylandClient)
set_tests_properties(clipboard_protocol_test PROPERTIES TIMEOUT 30)

# Widgets and Quick are used by the external Wayland test clients.
find_package(Qt6 REQUIRED COMPONENTS Widgets Quick)
add_executable(clipboard-qt-client clipboard/qt_client.cpp)
target_link_libraries(clipboard-qt-client PRIVATE Qt6::Widgets)
cutefish_add_test(clipboard_toolkit_test cutefish-wayland-input cutefish-wayland-shell PkgConfig::WaylandClient)
target_compile_definitions(clipboard_toolkit_test PRIVATE
    QT_CLIPBOARD_CLIENT="$<TARGET_FILE:clipboard-qt-client>"
    GTK_CLIPBOARD_CLIENT="${CMAKE_CURRENT_SOURCE_DIR}/clipboard/gtk_client.py")
add_dependencies(clipboard_toolkit_test clipboard-qt-client)
set_tests_properties(clipboard_toolkit_test PROPERTIES TIMEOUT 70)
cutefish_add_test(selection_test cutefish-input)

cutefish_add_test(dmabuf_test cutefish-wayland-dmabuf cutefish-compositor-core
    PkgConfig::WaylandClient PkgConfig::Libdrm)

cutefish_add_test(shadow_test cutefish-decoration)
cutefish_add_test(shadow_controller_test cutefish-decoration)
cutefish_add_test(shadow_pixel_test cutefish-decoration cutefish-render Qt6::Gui)
set_tests_properties(shadow_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")

cutefish_add_test(motion_test cutefish-scene)
cutefish_add_test(window_animation_test cutefish-scene)
cutefish_add_test(screen_transition_node_test cutefish-scene)
cutefish_add_test(desk_switch_node_test cutefish-scene)
cutefish_add_test(desks_test cutefish-wm Qt6::Test)
cutefish_add_test(overview_layout_test cutefish-wm)
cutefish_add_test(overview_node_test cutefish-scene)
cutefish_add_test(overview_test cutefish-wm)

cutefish_add_test(cursor_test cutefish-cursor)
cutefish_add_test(output_cursor_router_test cutefish-cursor)

# DMA-BUF scanout: eligibility, plane assignment, the atomic commit
# path, explicit synchronization and the hardware cursor.
cutefish_add_test(overlay_candidate_test cutefish-scene)
cutefish_add_test(plane_assignment_test cutefish-backend PkgConfig::Libdrm)
cutefish_add_test(sync_fence_test cutefish-sync cutefish-compositor-core)

add_library(cutefish-fake-drm STATIC fake_drm_device.cpp fake_drm_device.h)
target_include_directories(cutefish-fake-drm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cutefish-fake-drm PUBLIC cutefish-backend PkgConfig::Libdrm)

cutefish_add_test(scanout_test cutefish-fake-drm cutefish-compositor-core)
cutefish_add_test(hardware_cursor_test cutefish-fake-drm cutefish-cursor cutefish-session)

cutefish_add_test(window_hints_protocol_test cutefish-wayland-shell PkgConfig::WaylandClient)
cutefish_add_test(surface_blur_protocol_test cutefish-wayland-shell PkgConfig::WaylandClient)

cutefish_add_test(dnd_protocol_test cutefish-cursor cutefish-wayland-input PkgConfig::WaylandClient)

cutefish_add_test(data_control_protocol_test cutefish-wayland-input cutefish-wayland-private-shell
    cutefish-wayland-shell PkgConfig::WaylandClient)

add_executable(dnd-qt-client dnd/qt_client.cpp)
target_link_libraries(dnd-qt-client PRIVATE Qt6::Widgets)
cutefish_add_test(dnd_toolkit_test cutefish-wayland-input cutefish-wayland-shell PkgConfig::WaylandClient)
target_compile_definitions(dnd_toolkit_test PRIVATE
    QT_DND_CLIENT="$<TARGET_FILE:dnd-qt-client>"
    GTK_DND_CLIENT="${CMAKE_CURRENT_SOURCE_DIR}/dnd/gtk_client.py")
add_dependencies(dnd_toolkit_test dnd-qt-client)
set_tests_properties(dnd_toolkit_test PROPERTIES TIMEOUT 70)

set_tests_properties(private_shell_protocol_test PROPERTIES TIMEOUT 60)

cutefish_add_test(viewporter_test cutefish-compositor-core)
cutefish_add_test(viewporter_protocol_test cutefish-compositor-core PkgConfig::WaylandClient)
set_tests_properties(viewporter_protocol_test PROPERTIES TIMEOUT 30)
cutefish_add_test(viewporter_pixel_test cutefish-compositor-core cutefish-input cutefish-render Qt6::Gui PkgConfig::Gbm PkgConfig::Libdrm)
set_tests_properties(viewporter_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")

cutefish_add_test(fractional_scale_protocol_test cutefish-compositor-core PkgConfig::WaylandClient)
set_tests_properties(fractional_scale_protocol_test PROPERTIES TIMEOUT 30)

cutefish_add_test(fractional_scale_pixel_test cutefish-compositor-core cutefish-render)
set_tests_properties(fractional_scale_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")

cutefish_add_test(output_scale_test cutefish-input)
cutefish_add_test(display_types_test cutefish-display)
cutefish_add_test(edid_test cutefish-display)
cutefish_add_test(recommended_scale_test cutefish-display)
cutefish_add_test(display_transform_test cutefish-display)
cutefish_add_test(mirror_transform_test cutefish-display)
cutefish_add_test(display_layout_test cutefish-display)
cutefish_add_test(chromeos_display_layout_test cutefish-display)
cutefish_add_test(display_configuration_validator_test cutefish-display)
cutefish_add_test(display_manager_test cutefish-display Qt6::Test)
cutefish_add_test(native_display_delegate_test cutefish-display)
target_sources(native_display_delegate_test PRIVATE
    fake_native_display_delegate.cpp fake_native_display_delegate.h)
cutefish_add_test(display_configuration_controller_test cutefish-display Qt6::Test)
target_sources(display_configuration_controller_test PRIVATE
    fake_native_display_delegate.cpp fake_native_display_delegate.h)

add_executable(scaling-quick-client scaling/quick_client.cpp)
target_link_libraries(scaling-quick-client PRIVATE Qt6::Quick)
add_executable(scaling-qt-client scaling/qt_client.cpp)
target_link_libraries(scaling-qt-client PRIVATE Qt6::Widgets)
cutefish_add_test(fractional_scale_toolkit_test cutefish-wayland-input cutefish-wayland-shell PkgConfig::WaylandClient)
target_compile_definitions(fractional_scale_toolkit_test PRIVATE QT_SCALE_CLIENT="$<TARGET_FILE:scaling-qt-client>"
    QT_QUICK_SCALE_CLIENT="$<TARGET_FILE:scaling-quick-client>")
add_dependencies(fractional_scale_toolkit_test scaling-qt-client scaling-quick-client)
set_tests_properties(fractional_scale_toolkit_test PROPERTIES TIMEOUT 30)

cutefish_add_test(presentation_test cutefish-compositor-core)
cutefish_add_test(presentation_protocol_test cutefish-compositor-core PkgConfig::WaylandClient)
add_executable(cutefish-presentation-client manual/presentation_client.cpp)
target_link_libraries(cutefish-presentation-client PRIVATE cutefish-protocols PkgConfig::WaylandClient)

cutefish_add_test(drm_output_test cutefish-backend cutefish-compositor-core PkgConfig::WaylandClient)
target_sources(drm_output_test PRIVATE fake_drm_device.cpp)
set_tests_properties(drm_output_test PROPERTIES TIMEOUT 60 ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")

cutefish_add_test(window_visual_test cutefish-decoration)
cutefish_add_test(window_visual_pixel_test cutefish-decoration cutefish-render)
set_tests_properties(window_visual_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")
add_executable(fishui-resize-client visuals/fishui_client.cpp)
target_link_libraries(fishui-resize-client PRIVATE Qt6::Quick)
cutefish_add_test(window_visual_toolkit_test cutefish-wayland-input cutefish-wayland-shell
    cutefish-decoration cutefish-render PkgConfig::WaylandClient)
target_compile_definitions(window_visual_toolkit_test PRIVATE
    QT_SCALE_CLIENT="$<TARGET_FILE:scaling-qt-client>"
    FISHUI_RESIZE_CLIENT="$<TARGET_FILE:fishui-resize-client>"
    ELECTRON_VISUAL_CLIENT="${CMAKE_CURRENT_SOURCE_DIR}/visuals/electron_client.js")
add_dependencies(window_visual_toolkit_test scaling-qt-client fishui-resize-client)
set_tests_properties(window_visual_toolkit_test PROPERTIES TIMEOUT 70
    ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")

if(CUTEFISH_ENABLE_XWAYLAND)
    cutefish_add_test(xwayland_test cutefish-xwayland cutefish-wayland-shell cutefish-wayland-input cutefish-render cutefish-decoration PkgConfig::WaylandClient)
    add_executable(xwayland-startup-fault manual/xwayland_startup_fault.cpp)
    target_compile_definitions(xwayland_test PRIVATE XWAYLAND_FAULT_PATH="$<TARGET_FILE:xwayland-startup-fault>")
    add_executable(x11-test-client xwayland/x11_client.cpp)
    target_link_libraries(x11-test-client PRIVATE PkgConfig::Xcb)
    target_compile_definitions(xwayland_test PRIVATE X11_TEST_CLIENT="$<TARGET_FILE:x11-test-client>")
    target_compile_definitions(xwayland_test PRIVATE QT_CLIPBOARD_CLIENT="$<TARGET_FILE:clipboard-qt-client>")
    target_compile_definitions(xwayland_test PRIVATE
        QT_DND_CLIENT="$<TARGET_FILE:dnd-qt-client>"
        GTK_DND_CLIENT="${CMAKE_CURRENT_SOURCE_DIR}/dnd/gtk_client.py")
    add_dependencies(xwayland_test xwayland-startup-fault x11-test-client clipboard-qt-client
        dnd-qt-client)
    set_tests_properties(xwayland_test PROPERTIES TIMEOUT 90 ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")
endif()

cutefish_add_test(display_runtime_test cutefish-display-runtime cutefish-session Qt6::Test)

cutefish_add_test(capture_pixel_test cutefish-render)
cutefish_add_test(render_internals_test cutefish-render PkgConfig::Gbm PkgConfig::Libdrm)
set_tests_properties(render_internals_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")
set_tests_properties(capture_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")
cutefish_add_test(desk_switch_pixel_test cutefish-scene cutefish-render)
set_tests_properties(desk_switch_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")
cutefish_add_test(overview_pixel_test cutefish-scene cutefish-render)
set_tests_properties(overview_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")
set_tests_properties(image_capture_test PROPERTIES TIMEOUT 15)

add_dependencies(image_capture_test cutefish-screenshot)
target_compile_definitions(image_capture_test PRIVATE SCREENSHOT_EXECUTABLE="$<TARGET_FILE:cutefish-screenshot>")

cutefish_add_test(damage_test cutefish-compositor-core)
cutefish_add_test(damage_pixel_test cutefish-scene cutefish-render)
set_tests_properties(damage_pixel_test PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;EGL_PLATFORM=surfaceless")
cutefish_add_test(damage_gbm_test cutefish-scene cutefish-render PkgConfig::Gbm)
cutefish_add_test(dmabuf_format_gbm_test cutefish-render PkgConfig::Gbm)
cutefish_add_test(capture_dmabuf_gbm_test cutefish-render PkgConfig::Gbm PkgConfig::Libdrm)

target_compile_definitions(window_visual_toolkit_test PRIVATE QT_QUICK_SCALE_CLIENT="$<TARGET_FILE:scaling-quick-client>")
add_dependencies(window_visual_toolkit_test scaling-quick-client)

cutefish_add_test(idle_notify_protocol_test cutefish-wayland-input PkgConfig::WaylandClient)

add_executable(auth_controller_test auth_controller_test.cpp)
target_link_libraries(auth_controller_test PRIVATE cutefish-auth Qt6::Test)
add_test(NAME auth_controller_test COMMAND auth_controller_test)
set_tests_properties(auth_controller_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(switcher_view_test switcher_view_test.cpp)
target_include_directories(switcher_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(switcher_view_test PRIVATE cutefish-switcher Qt6::Test)
add_test(NAME switcher_view_test COMMAND switcher_view_test)
add_executable(auth_ui_test auth_ui_test.cpp)
target_link_libraries(auth_ui_test PRIVATE cutefish-auth Qt6::Test)
add_test(NAME auth_ui_test COMMAND auth_ui_test)
add_executable(greetd_backend_test greetd_backend_test.cpp)
target_link_libraries(greetd_backend_test PRIVATE cutefish-auth Qt6::Test)
add_test(NAME greetd_backend_test COMMAND greetd_backend_test)
add_executable(screen_saver_service_test screen_saver_service_test.cpp)
target_link_libraries(screen_saver_service_test PRIVATE cutefish-session Qt6::Test)
add_test(NAME screen_saver_service_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:screen_saver_service_test>)
add_executable(applications_service_test applications_service_test.cpp)
target_include_directories(applications_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(applications_service_test PRIVATE cutefish-session Qt6::Test)
add_test(NAME applications_service_test
    COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:applications_service_test>)

add_executable(window_debug_service_test window_debug_service_test.cpp)
target_include_directories(window_debug_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(window_debug_service_test PRIVATE cutefish-session Qt6::Test)
add_test(NAME window_debug_service_test
    COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:window_debug_service_test>)

add_executable(sleep_lock_test sleep_lock_test.cpp)
target_link_libraries(sleep_lock_test PRIVATE cutefish-session Qt6::Test)
add_test(NAME sleep_lock_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:sleep_lock_test>)

add_executable(power_button_test power_button_test.cpp)
target_link_libraries(power_button_test PRIVATE cutefish-session Qt6::Test)
add_test(NAME power_button_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:power_button_test>)

cutefish_add_test(input_method_protocol_test cutefish-wayland-input PkgConfig::WaylandClient)
set_tests_properties(input_method_protocol_test PROPERTIES TIMEOUT 45)

add_executable(input_method_launched_client input_method/launched_client.cpp)
target_link_libraries(input_method_launched_client PRIVATE PkgConfig::WaylandClient)
add_executable(input_method_launcher_test input_method_launcher_test.cpp)
target_include_directories(input_method_launcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(input_method_launcher_test PRIVATE cutefish-wayland-input
    cutefish-wayland-private-shell cutefish-input-method-settings PkgConfig::WaylandClient
    Qt6::Test GTest::gtest)
target_compile_definitions(input_method_launcher_test PRIVATE
    CUTEFISH_LAUNCHED_CLIENT="$<TARGET_FILE:input_method_launched_client>")
add_dependencies(input_method_launcher_test input_method_launched_client)
add_test(NAME input_method_launcher_test
    COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:input_method_launcher_test>)
set_tests_properties(input_method_launcher_test PROPERTIES TIMEOUT 60)

# Sanitizer builds run several times slower than the per-test timeouts above
# allow for; configure with -DCUTEFISH_TEST_TIMEOUT_SCALE=5 for those.
set(CUTEFISH_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiplier for every test's TIMEOUT")
if(NOT CUTEFISH_TEST_TIMEOUT_SCALE EQUAL 1)
    get_directory_property(cutefish_all_tests TESTS)
    foreach(test ${cutefish_all_tests})
        get_test_property(${test} TIMEOUT timeout)
        if(timeout AND NOT timeout STREQUAL "NOTFOUND")
            math(EXPR scaled "${timeout} * ${CUTEFISH_TEST_TIMEOUT_SCALE}")
            set_tests_properties(${test} PROPERTIES TIMEOUT ${scaled})
        endif()
    endforeach()
endif()
