cmake_minimum_required(VERSION 3.31)

project(udp-test LANGUAGES C CXX)

# projectproject  settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add cmake scripts
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(fetch_cli11)
add_executable(udp-profile udp-profile.cpp)
target_link_libraries(udp-profile
  PRIVATE ws2_32 winmm CLI11::CLI11
)

add_executable(rtt-server rtt-server.cpp)
target_link_libraries(rtt-server
  PRIVATE ws2_32 winmm CLI11::CLI11
)

add_executable(rtt-client rtt-client.cpp)
target_link_libraries(rtt-client
  PRIVATE ws2_32 winmm CLI11::CLI11
)
