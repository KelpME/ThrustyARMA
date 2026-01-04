cmake_minimum_required(VERSION 3.16)
project(twcs_mapper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(EVDEV REQUIRED libevdev)

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
)

target_link_libraries(twcs_mapper config_lib ${EVDEV_LIBRARIES})
target_include_directories(twcs_mapper PRIVATE ${EVDEV_INCLUDE_DIRS})
target_compile_options(twcs_mapper PRIVATE ${EVDEV_CFLAGS_OTHER})
target_compile_definitions(twcs_mapper PRIVATE _GNU_SOURCE)

# Install targets
include(GNUInstallDirs)

install(TARGETS twcs_select twcs_mapper
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Set output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)