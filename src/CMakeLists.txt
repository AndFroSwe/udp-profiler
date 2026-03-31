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
add_library(connection)
target_include_directories(connection
  PUBLIC connection
)
if(WIN32)
  target_sources(connection PRIVATE connection/connection_win.cpp)
  target_link_libraries(connection
  PRIVATE ws2_32
  )
else()
  message(FATAL_ERROR "not implemented on OS")
endif()
