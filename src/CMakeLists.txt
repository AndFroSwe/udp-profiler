add_executable(udp-profile cli/udp-profile.cpp)
target_link_libraries(udp-profile
  PRIVATE ws2_32 winmm CLI11::CLI11
)
target_include_directories(udp-profile
  PRIVATE include
)

add_executable(rtt-server cli/rtt-server.cpp)
target_link_libraries(rtt-server
  PRIVATE ws2_32 winmm CLI11::CLI11
)
target_include_directories(rtt-server
  PRIVATE include
)

add_executable(rtt-client cli/rtt-client.cpp)
target_link_libraries(rtt-client
  PRIVATE ws2_32 winmm CLI11::CLI11
)
target_include_directories(rtt-client
  PRIVATE include
)
