# CLi functions
add_executable(udp-profile cli/udp-profile.cpp)
target_link_libraries(udp-profile
  PRIVATE CLI11::CLI11 connection
)
target_include_directories(udp-profile
  PRIVATE include
)

add_executable(rtt-server cli/rtt-server.cpp)
target_link_libraries(rtt-server
  PRIVATE CLI11::CLI11 connection
)
target_include_directories(rtt-server
  PRIVATE include
)

add_executable(rtt-client cli/rtt-client.cpp)
target_link_libraries(rtt-client
  PRIVATE CLI11::CLI11 connection
)
target_include_directories(rtt-client
  PRIVATE include
)

# libraries
add_library(connection connection/connection.cpp)
target_link_libraries(connection
  PUBLIC ws2_32
)
target_include_directories(connection
  PUBLIC connection
)
