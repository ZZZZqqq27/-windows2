cmake_minimum_required(VERSION 3.15)
project(p2p_video_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(app
  src/main.cpp
  src/config.cpp
  src/crypto_utils.cpp
  src/logger.cpp
  src/chunk_store.cpp
  src/hex_utils.cpp
  src/secure_transport.cpp
  src/sha256.cpp
  src/tcp_transport.cpp
  src/routing_table.cpp
)

target_include_directories(app PRIVATE include)

find_package(OpenSSL REQUIRED)
target_link_libraries(app PRIVATE OpenSSL::Crypto)

add_executable(mvp2_tests
  tests/test_placeholder.cpp
  src/chunk_store.cpp
  src/config.cpp
  src/crypto_utils.cpp
  src/hex_utils.cpp
  src/secure_transport.cpp
  src/sha256.cpp
  src/logger.cpp
  src/routing_table.cpp
)

target_include_directories(mvp2_tests PRIVATE include)
target_link_libraries(mvp2_tests PRIVATE OpenSSL::Crypto)
