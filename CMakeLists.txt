cmake_minimum_required(VERSION 3.16)

project(mcpserverlib LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Network HttpServer SerialBus)
find_package(Threads REQUIRED)

add_library(mcpserverlib SHARED
  mcpserverlib_global.h
  mcpcommand.h
  mcpcommandregistry.h mcpcommandregistry.cpp
  mcpserver.h mcpserver.cpp
  cancommands.h cancommands.cpp
)

target_include_directories(mcpserverlib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mcpserverlib PUBLIC
  Qt6::Core
  Qt6::Concurrent
  Qt6::Network
  Qt6::HttpServer
  Qt6::SerialBus
  Threads::Threads
)

target_compile_definitions(mcpserverlib PRIVATE MCPSERVERLIB_LIBRARY)
