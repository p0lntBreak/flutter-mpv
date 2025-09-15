cmake_minimum_required(VERSION 3.13)
project(runner LANGUAGES CXX C)

# Find required packages for MPV
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(GTK REQUIRED gtk+-3.0)

# Define the application target
add_executable(${BINARY_NAME}
  "main.cc"
  "my_application.cc"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "mpv_player_plugin.c"
  "mpv_player_plugin.h"
)

# Apply the standard set of build settings
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the application ID
add_definitions(-DAPPLICATION_ID="${APPLICATION_ID}")

# Include MPV headers
target_include_directories(${BINARY_NAME} PRIVATE ${MPV_INCLUDE_DIRS})
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Add compiler options for MPV
target_compile_options(${BINARY_NAME} PRIVATE ${MPV_CFLAGS_OTHER})

# Link libraries
target_link_libraries(${BINARY_NAME} PRIVATE flutter)
target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::GTK)
target_link_libraries(${BINARY_NAME} PRIVATE ${MPV_LIBRARIES})