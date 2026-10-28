cmake_minimum_required(VERSION 3.16)

project(MiniGameServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 单配置生成器（MinGW Makefiles / Ninja）把可执行文件放到 build/bin。
if(NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

function(minigame_set_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(MINIGAME_ENABLE_SANITIZERS)
    target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
  endif()
  if(MINIGAME_ENABLE_TSAN)
    target_compile_options(${target_name} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target_name} PRIVATE -fsanitize=thread)
  endif()
endfunction()

option(MINIGAME_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MINIGAME_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(MINIGAME_ENABLE_SANITIZERS AND MINIGAME_ENABLE_TSAN)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()

add_library(minigame_core
  src/Logger.cpp
  src/Protocol.cpp
  src/TicTacToeGame.cpp
  src/Room.cpp
  src/RoomManager.cpp
  src/TcpSocket.cpp
  src/LineBuffer.cpp
  src/OutputBuffer.cpp
  src/Session.cpp
  src/Server.cpp
  src/ThreadPool.cpp
  src/SessionLane.cpp
  src/ResponseQueue.cpp
)

if(UNIX AND NOT APPLE)
  target_sources(minigame_core PRIVATE
    src/Epoll.cpp
    src/Connection.cpp
    src/Reactor.cpp
  )
  find_path(MYSQL_INCLUDE_DIR mysql.h PATHS /usr/include/mysql)
  find_library(MYSQL_CLIENT mysqlclient)
  if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT)
    target_sources(minigame_core PRIVATE src/Database.cpp)
    target_include_directories(minigame_core PUBLIC ${MYSQL_INCLUDE_DIR})
    target_compile_definitions(minigame_core PUBLIC MINIGAME_HAS_MYSQL)
    target_link_libraries(minigame_core PUBLIC ${MYSQL_CLIENT} crypt)
  endif()
endif()

target_include_directories(minigame_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(minigame_core PUBLIC Threads::Threads)

if(WIN32)
  target_compile_definitions(minigame_core PUBLIC _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(minigame_core PUBLIC ws2_32)
endif()

minigame_set_warnings(minigame_core)

add_executable(minigame_server src/main.cpp)
target_link_libraries(minigame_server PRIVATE minigame_core)
minigame_set_warnings(minigame_server)

add_executable(minigame_client client/main.cpp)
target_link_libraries(minigame_client PRIVATE minigame_core)
minigame_set_warnings(minigame_client)

add_executable(minigame_tests tests/unit_tests.cpp)
target_link_libraries(minigame_tests PRIVATE minigame_core)
minigame_set_warnings(minigame_tests)

add_executable(minigame_integration tests/integration_test.cpp)
target_link_libraries(minigame_integration PRIVATE minigame_core)
minigame_set_warnings(minigame_integration)

add_executable(minigame_stream_tests tests/stream_tests.cpp)
target_link_libraries(minigame_stream_tests PRIVATE minigame_core)
minigame_set_warnings(minigame_stream_tests)

if(UNIX AND NOT APPLE)
  add_executable(minigame_phase3_tests tests/phase3_tests.cpp)
  target_link_libraries(minigame_phase3_tests PRIVATE minigame_core)
  minigame_set_warnings(minigame_phase3_tests)
  if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT)
    add_executable(minigame_phase4_tests tests/phase4_tests.cpp)
    target_link_libraries(minigame_phase4_tests PRIVATE minigame_core)
    minigame_set_warnings(minigame_phase4_tests)
  endif()
endif()

add_executable(minigame_phase5_limits tests/phase5_limits.cpp)
target_link_libraries(minigame_phase5_limits PRIVATE minigame_core)
minigame_set_warnings(minigame_phase5_limits)

add_executable(minigame_reactor_tests tests/reactor_tests.cpp)
target_link_libraries(minigame_reactor_tests PRIVATE minigame_core)
minigame_set_warnings(minigame_reactor_tests)

enable_testing()
add_test(NAME unit COMMAND minigame_tests)
add_test(NAME integration COMMAND minigame_integration)
add_test(NAME stream COMMAND minigame_stream_tests)
add_test(NAME reactor COMMAND minigame_reactor_tests)
add_test(NAME phase5_limits COMMAND minigame_phase5_limits)
if(UNIX AND NOT APPLE)
  add_test(NAME phase3 COMMAND minigame_phase3_tests)
  if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT)
    add_test(NAME phase4 COMMAND minigame_phase4_tests)
  endif()
endif()
set_tests_properties(unit PROPERTIES TIMEOUT 30)
set_tests_properties(integration PROPERTIES TIMEOUT 30)
set_tests_properties(stream PROPERTIES TIMEOUT 30)
set_tests_properties(reactor PROPERTIES TIMEOUT 60)
set_tests_properties(phase5_limits PROPERTIES TIMEOUT 30)
if(UNIX AND NOT APPLE)
  set_tests_properties(phase3 PROPERTIES TIMEOUT 60)
  if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT)
    set_tests_properties(phase4 PROPERTIES TIMEOUT 120)
  endif()
endif()
