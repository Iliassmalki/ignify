cmake_minimum_required(VERSION 3.16)
project(ignify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(ignify src/main.cpp src/api.cpp src/ui.cpp)
target_link_libraries(ignify PRIVATE Qt6::Widgets Qt6::Network)

enable_testing()
add_executable(ignify_test src/test.cpp)
target_link_libraries(ignify_test PRIVATE Qt6::Core)
add_test(NAME ignify_test COMMAND ignify_test)

install(TARGETS ignify)
