cmake_minimum_required(VERSION 3.16)
project(chat_system CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
  add_definitions(-D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# nlohmann/json: vendored single header at third_party/nlohmann/json.hpp.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
  ${CMAKE_SOURCE_DIR}/third_party)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

option(CHAT_NO_MYSQL "Build without MySQL persistence (uses in-memory store)" OFF)

if(NOT CHAT_NO_MYSQL)
  find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql)
  find_library(MYSQL_LIBRARY NAMES mysqlclient libmysql)
  if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(WARNING "libmysqlclient not found; falling back to in-memory store.")
    set(CHAT_NO_MYSQL ON)
  endif()
endif()

set(CORE_SOURCES
  src/common/logger.cpp
  src/common/thread_pool.cpp
  src/common/config.cpp
  src/common/util.cpp
  src/net/socket.cpp
  src/net/reactor_epoll.cpp
  src/net/reactor_select.cpp
  src/net/connection.cpp
  src/net/server.cpp
  src/protocol/framing.cpp
  src/protocol/messages.cpp
  src/auth/crypto.cpp
  src/auth/session.cpp
  src/db/mysql_pool.cpp
  src/db/repositories.cpp
  src/chat/presence.cpp
  src/chat/room_manager.cpp
  src/chat/message_router.cpp
  src/chat/chat_server.cpp
  src/handlers/handlers.cpp)

add_library(chat_core STATIC ${CORE_SOURCES})
target_include_directories(chat_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(chat_core PUBLIC nlohmann_json::nlohmann_json)

if(CHAT_NO_MYSQL)
  target_compile_definitions(chat_core PUBLIC CHAT_NO_MYSQL=1)
else()
  target_include_directories(chat_core PUBLIC ${MYSQL_INCLUDE_DIR})
  target_link_libraries(chat_core PUBLIC ${MYSQL_LIBRARY})
endif()

if(WIN32)
  target_link_libraries(chat_core PUBLIC ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(chat_core PUBLIC Threads::Threads)
endif()

add_executable(chat_server src/main.cpp)
target_link_libraries(chat_server PRIVATE chat_core)

# CLI client: tiny standalone tool. We compile the few source files it
# actually needs rather than linking the full chat_core (which would pull
# in MySQL on a normal build).
add_executable(cli_client
  client/cli_client.cpp
  src/common/util.cpp
  src/protocol/framing.cpp)
target_include_directories(cli_client PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(cli_client PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(cli_client PRIVATE ws2_32)
else()
  target_link_libraries(cli_client PRIVATE Threads::Threads)
endif()

enable_testing()
add_executable(chat_tests
  tests/test_main.cpp
  tests/test_framing.cpp
  tests/test_thread_pool.cpp
  tests/test_router.cpp)
target_include_directories(chat_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(chat_tests PRIVATE chat_core)
add_test(NAME chat_tests COMMAND chat_tests)
