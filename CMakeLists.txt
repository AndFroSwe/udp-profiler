cmake_minimum_required(VERSION 3.31)

project(send-test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_executable(send-test send.cpp)
target_link_libraries(send-test 
  PRIVATE ws2_32 winmm
)
