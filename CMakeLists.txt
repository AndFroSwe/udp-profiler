cmake_minimum_required(VERSION 3.31)

project(udp-test LANGUAGES C CXX)

# projectproject  settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add cmake scripts
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(fetch_cli11)
add_subdirectory(src)
