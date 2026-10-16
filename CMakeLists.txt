cmake_minimum_required(VERSION 3.16)
project(ignify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network DBus)

add_executable(ignify src/main.cpp src/api.cpp src/ui.cpp src/views.cpp src/mpris.cpp)
target_link_libraries(ignify PRIVATE Qt6::Widgets Qt6::Network Qt6::DBus)

enable_testing()
add_executable(ignify_test src/test.cpp)
target_link_libraries(ignify_test PRIVATE Qt6::Core)
add_test(NAME ignify_test COMMAND ignify_test)

include(GNUInstallDirs)
install(TARGETS ignify)
# Desktop integration: app menu entry with an absolute Exec path, and the flame icon at every size.
# For a per-user install (no sudo): cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local
configure_file(data/ignify.desktop.in ignify.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ignify.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
foreach(size 16 24 32 48 64 128 256 512)
  install(FILES data/icons/${size}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
          RENAME ignify.png)
endforeach()
