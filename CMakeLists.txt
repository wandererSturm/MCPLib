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
  udsutil.h udsutil.cpp
  cancommands.h cancommands.cpp
  doipcommands.h doipcommands.cpp
  hsfzcommands.h hsfzcommands.cpp
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

# Optional: gs_usb-protocol CAN/CAN-FD adapters (candleLight, canable, etc.)
# have no OS-level CAN driver on Windows, so cancommands.cpp talks to them
# directly over USB via libusb. Degrades gracefully if libusb isn't found.
find_path(LIBUSB1_INCLUDE_DIR NAMES libusb-1.0/libusb.h
  HINTS "$ENV{USERPROFILE}/vcpkg/installed/x64-windows/include")
find_library(LIBUSB1_LIBRARY NAMES libusb-1.0 usb-1.0
  HINTS "$ENV{USERPROFILE}/vcpkg/installed/x64-windows/lib")
find_file(LIBUSB1_DLL NAMES libusb-1.0.dll
  HINTS "$ENV{USERPROFILE}/vcpkg/installed/x64-windows/bin")

if(LIBUSB1_INCLUDE_DIR AND LIBUSB1_LIBRARY)
  message(STATUS "gs_usb CAN-FD support: enabled (libusb ${LIBUSB1_LIBRARY})")
  target_sources(mcpserverlib PRIVATE gsusbcanbus.h gsusbcanbus.cpp)
  target_include_directories(mcpserverlib PRIVATE ${LIBUSB1_INCLUDE_DIR})
  target_link_libraries(mcpserverlib PRIVATE ${LIBUSB1_LIBRARY})
  target_compile_definitions(mcpserverlib PRIVATE MCPSERVERLIB_HAVE_GSUSB)
  if(LIBUSB1_DLL)
    add_custom_command(TARGET mcpserverlib POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBUSB1_DLL}" "$<TARGET_FILE_DIR:mcpserverlib>"
      COMMENT "Copying libusb-1.0.dll next to mcpserverlib")
  endif()
else()
  message(STATUS "gs_usb CAN-FD support: disabled (libusb not found)")
endif()
