include(FetchContent)
FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# The version lives in the VERSION file at the repository root (scripts/version.mjs keeps the app's copies in step).
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION MALGRIND_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)

add_library(malgrind_host STATIC
    src/wire.cpp src/symbols.cpp src/aggregate.cpp src/http.cpp src/source.cpp src/discover.cpp src/store.cpp src/session.cpp src/pio.cpp src/backup.cpp src/projects.cpp src/github.cpp src/boards.cpp src/devices.cpp src/setup.cpp src/settings.cpp src/plan.cpp src/ops.cpp src/serve.cpp src/cli.cpp
    src/families/registry.cpp src/families/esp32.cpp
    src/os/paths.cpp src/os/process.cpp src/os/ports.cpp src/os/tools.cpp)
if(WIN32)
    target_sources(malgrind_host PRIVATE
        src/os/paths_win.cpp src/os/process_win.cpp src/os/socket_win.cpp src/os/serial_win.cpp
        src/os/ports_win.cpp src/os/usb_win.cpp src/os/access_other.cpp src/os/console_win.cpp)
    target_link_libraries(malgrind_host PUBLIC setupapi cfgmgr32 ws2_32 shell32 ole32 advapi32)
elseif(APPLE)
    target_sources(malgrind_host PRIVATE
        src/os/paths_posix.cpp src/os/process_posix.cpp src/os/socket_posix.cpp src/os/serial_posix.cpp
        src/os/ports_macos.cpp src/os/usb_macos.cpp src/os/access_other.cpp src/os/console_posix.cpp)
    target_link_libraries(malgrind_host PUBLIC "-framework IOKit" "-framework CoreFoundation")
else()
    target_sources(malgrind_host PRIVATE
        src/os/paths_posix.cpp src/os/process_posix.cpp src/os/socket_posix.cpp src/os/serial_posix.cpp
        src/os/ports_linux.cpp src/os/usb_linux.cpp src/os/access_linux.cpp src/os/console_posix.cpp)
endif()
target_include_directories(malgrind_host PUBLIC src)
target_link_libraries(malgrind_host PUBLIC malgrind_core nlohmann_json::nlohmann_json)
target_compile_features(malgrind_host PUBLIC cxx_std_20)
target_compile_definitions(malgrind_host PUBLIC MALGRIND_VERSION="${MALGRIND_VERSION}")
target_compile_options(malgrind_host PUBLIC
    "$<IF:$<CXX_COMPILER_ID:MSVC>,/W3;/permissive-;/Zc:__cplusplus;/utf-8;/EHsc,-Wall;-Wextra>")

# The live page of the terminal mode is compiled into the binary as a raw string literal.
file(READ web/flame.html FLAME_HTML)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/flame_html.inc "R\"malgrind_html(${FLAME_HTML})malgrind_html\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS web/flame.html)
target_include_directories(malgrind_host PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Where `malgrind-cli run` finds agent/<family>/ unless told otherwise. A source build uses the repository; packaging sets its own.
set(MALGRIND_DEFAULT_AGENT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "directory that contains agent/<family>/")

# Named malgrind-cli: the app's own executable is called malgrind, and both end up in /usr/bin.
add_executable(malgrind-cli src/main.cpp)
target_compile_definitions(malgrind-cli PRIVATE MALGRIND_DEFAULT_AGENT_DIR="${MALGRIND_DEFAULT_AGENT_DIR}")
target_link_libraries(malgrind-cli PRIVATE malgrind_host)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # the packaged sidecar must not need a newer libstdc++ than the user's distribution has
    target_link_options(malgrind-cli PRIVATE -static-libstdc++ -static-libgcc)
endif()

malgrind_add_test(os_paths tests/test_os_paths.cpp)
target_link_libraries(test_os_paths PRIVATE malgrind_host)

add_executable(helper_echo tests/helper_echo.cpp)
target_link_libraries(helper_echo PRIVATE malgrind_host)

add_executable(fake_tool tests/fake_tool.cpp)
target_link_libraries(fake_tool PRIVATE malgrind_host)

malgrind_add_test(os_process tests/test_os_process.cpp)
target_link_libraries(test_os_process PRIVATE malgrind_host)
target_compile_definitions(test_os_process PRIVATE MALGRIND_HELPER_ECHO="$<TARGET_FILE:helper_echo>")
add_dependencies(test_os_process helper_echo)

malgrind_add_test(os_socket tests/test_os_socket.cpp)
target_link_libraries(test_os_socket PRIVATE malgrind_host)

malgrind_add_test(os_serial tests/test_os_serial.cpp)
target_link_libraries(test_os_serial PRIVATE malgrind_host)

malgrind_add_test(os_ports tests/test_os_ports.cpp)
target_link_libraries(test_os_ports PRIVATE malgrind_host)

malgrind_add_test(host tests/test_host.cpp)
target_link_libraries(test_host PRIVATE malgrind_host)

malgrind_add_test(store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE malgrind_host)

malgrind_add_test(backup tests/test_backup.cpp)
target_link_libraries(test_backup PRIVATE malgrind_host)

malgrind_add_test(families tests/test_families.cpp)
target_link_libraries(test_families PRIVATE malgrind_host)
target_compile_definitions(test_families PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

malgrind_add_test(projects tests/test_projects.cpp)
target_link_libraries(test_projects PRIVATE malgrind_host)

malgrind_add_test(boards tests/test_boards.cpp)
target_link_libraries(test_boards PRIVATE malgrind_host)

malgrind_add_test(devices tests/test_devices.cpp)
target_link_libraries(test_devices PRIVATE malgrind_host)

malgrind_add_test(setup tests/test_setup.cpp)
target_link_libraries(test_setup PRIVATE malgrind_host)
target_compile_definitions(test_setup PRIVATE MALGRIND_FAKE_TOOL="$<TARGET_FILE:fake_tool>")
add_dependencies(test_setup fake_tool)

malgrind_add_test(github tests/test_github.cpp)
target_link_libraries(test_github PRIVATE malgrind_host)
target_compile_definitions(test_github PRIVATE MALGRIND_FAKE_TOOL="$<TARGET_FILE:fake_tool>")
add_dependencies(test_github fake_tool)

malgrind_add_test(pio tests/test_pio.cpp)
target_link_libraries(test_pio PRIVATE malgrind_host)
target_compile_definitions(test_pio PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}" MALGRIND_FAKE_TOOL="$<TARGET_FILE:fake_tool>")
add_dependencies(test_pio fake_tool)

malgrind_add_test(settings tests/test_settings.cpp)
target_link_libraries(test_settings PRIVATE malgrind_host)
target_compile_definitions(test_settings PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

malgrind_add_test(cli tests/test_cli.cpp)
target_link_libraries(test_cli PRIVATE malgrind_host)

malgrind_add_test(serve tests/test_serve.cpp)
target_link_libraries(test_serve PRIVATE malgrind_host)
target_compile_definitions(test_serve PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}" MALGRIND_FAKE_TOOL="$<TARGET_FILE:fake_tool>")
add_dependencies(test_serve fake_tool)

malgrind_add_test(session tests/test_session.cpp)
target_link_libraries(test_session PRIVATE malgrind_host)

if(NOT WIN32)
    malgrind_add_test(access tests/test_access.cpp)
    target_link_libraries(test_access PRIVATE malgrind_host)

    malgrind_add_test(flash tests/test_flash.cpp)
    target_link_libraries(test_flash PRIVATE malgrind_host)
    target_compile_definitions(test_flash PRIVATE MALGRIND_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
