cmake_minimum_required(VERSION 3.16)
project(twcs_mapper VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(EVDEV REQUIRED libevdev)
pkg_check_modules(NCURSES REQUIRED ncursesw)

# Create library for config handling
add_library(config_lib STATIC
    src/config.cpp
)

target_include_directories(config_lib PUBLIC src)
target_compile_definitions(config_lib PRIVATE _GNU_SOURCE)

# Create twcs_select executable
add_executable(twcs_select
    src/twcs_select.cpp
)

target_link_libraries(twcs_select config_lib)
target_include_directories(twcs_select PRIVATE ${EVDEV_INCLUDE_DIRS})
target_compile_options(twcs_select PRIVATE ${EVDEV_CFLAGS_OTHER})
target_compile_definitions(twcs_select PRIVATE _GNU_SOURCE)

# Create twcs_mapper executable
add_executable(twcs_mapper
    src/twcs_mapper.cpp
    src/bindings.cpp
    src/virtual_device.cpp
    src/epoll_loop.cpp
)

target_link_libraries(twcs_mapper config_lib ${EVDEV_LIBRARIES})
target_include_directories(twcs_mapper PRIVATE ${EVDEV_INCLUDE_DIRS})
target_compile_options(twcs_mapper PRIVATE ${EVDEV_CFLAGS_OTHER})
target_compile_definitions(twcs_mapper PRIVATE _GNU_SOURCE)

# Create twcs_config executable
add_executable(twcs_config
    src/twcs_config.cpp
)

target_link_libraries(twcs_config config_lib ${EVDEV_LIBRARIES} ${NCURSES_LIBRARIES})
target_include_directories(twcs_config PRIVATE ${EVDEV_INCLUDE_DIRS} ${NCURSES_INCLUDE_DIRS})
target_compile_options(twcs_config PRIVATE ${EVDEV_CFLAGS_OTHER} ${NCURSES_CFLAGS_OTHER})
target_compile_definitions(twcs_config PRIVATE _GNU_SOURCE)

# Create twcs_tui executable (comprehensive TUI)
add_executable(twcs_tui
    src/tui/main.cpp
    src/tui/tui.cpp
    src/tui/device_dashboard.cpp
    src/tui/mappings_view.cpp
    src/tui/live_monitor.cpp
    src/tui/profile_manager.cpp
    src/tui/calibration_wizard.cpp
    src/bindings.cpp
)

target_link_libraries(twcs_tui config_lib ${EVDEV_LIBRARIES} ${NCURSES_LIBRARIES})
target_include_directories(twcs_tui PRIVATE ${EVDEV_INCLUDE_DIRS} ${NCURSES_INCLUDE_DIRS} src)
target_compile_options(twcs_tui PRIVATE ${EVDEV_CFLAGS_OTHER} ${NCURSES_CFLAGS_OTHER})
target_compile_definitions(twcs_tui PRIVATE _GNU_SOURCE)

# Create twcs_setup executable
add_executable(twcs_setup
    src/twcs_setup.cpp
)

target_link_libraries(twcs_setup config_lib ${EVDEV_LIBRARIES})
target_include_directories(twcs_setup PRIVATE ${EVDEV_INCLUDE_DIRS})
target_compile_options(twcs_setup PRIVATE ${EVDEV_CFLAGS_OTHER})
target_compile_definitions(twcs_setup PRIVATE _GNU_SOURCE)

# Install targets
include(GNUInstallDirs)

install(TARGETS twcs_select twcs_mapper twcs_config twcs_tui twcs_setup
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Set output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)