cmake_minimum_required(VERSION 3.16)

project(mcpserverlib LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Two libraries from the same sources:
#   mcpserverlib - the MCP server (stdio + Streamable HTTP) with every tool:
#                  a shared library, as before.
#   mcpcommands  - the tools alone (McpCommand, the registry, CAN/UDS, DoIP,
#                  HSFZ), static, with no server and no Qt HttpServer: for
#                  running the tools inside an application of your own.
option(MCPLIB_BUILD_SERVER "Build mcpserverlib, the MCP server (needs Qt HttpServer)" ON)
set(LIBUSB_ROOT "" CACHE PATH
    "libusb-1.0 install prefix (with include/, lib/, bin/), e.g. <vcpkg>/installed/x64-windows")

find_package(Qt6 REQUIRED COMPONENTS Core Network SerialBus)

set(MCPLIB_COMMAND_SOURCES
  mcpserverlib_global.h
  mcpcommand.h
  mcpcommandregistry.h mcpcommandregistry.cpp
  udsutil.h udsutil.cpp
  cancommands.h cancommands.cpp
  doipcommands.h doipcommands.cpp
  hsfzcommands.h hsfzcommands.cpp
)

# Optional: gs_usb-protocol CAN/CAN-FD adapters (candleLight, canable, etc.)
# have no OS-level CAN driver on Windows, so cancommands.cpp talks to them
# directly over USB via libusb. Degrades gracefully if libusb isn't found.
# libusb is plain C, so an MSVC-built libusb (vcpkg x64-windows) works from
# MinGW builds too.
set(_mcplib_libusb_hints
  "${LIBUSB_ROOT}"
  "$ENV{VCPKG_ROOT}/installed/x64-windows"
  "$ENV{USERPROFILE}/vcpkg/installed/x64-windows")
find_path(LIBUSB1_INCLUDE_DIR NAMES libusb-1.0/libusb.h HINTS ${_mcplib_libusb_hints} PATH_SUFFIXES include)
find_library(LIBUSB1_LIBRARY NAMES libusb-1.0 usb-1.0 HINTS ${_mcplib_libusb_hints} PATH_SUFFIXES lib)
find_file(LIBUSB1_DLL NAMES libusb-1.0.dll HINTS ${_mcplib_libusb_hints} PATH_SUFFIXES bin)

if(LIBUSB1_INCLUDE_DIR AND LIBUSB1_LIBRARY)
  message(STATUS "gs_usb CAN-FD support: enabled (libusb ${LIBUSB1_LIBRARY})")
  set(MCPLIB_HAVE_GSUSB ON)
  set(MCPLIB_LIBUSB_DLL "${LIBUSB1_DLL}" CACHE INTERNAL "libusb-1.0.dll to ship next to the executable")
else()
  message(STATUS "gs_usb CAN-FD support: disabled (libusb not found; set LIBUSB_ROOT)")
  set(MCPLIB_HAVE_GSUSB OFF)
  set(MCPLIB_LIBUSB_DLL "" CACHE INTERNAL "libusb-1.0.dll to ship next to the executable")
endif()

function(_mcplib_add_gsusb target visibility)
  if(MCPLIB_HAVE_GSUSB)
    target_sources(${target} PRIVATE gsusbcanbus.h gsusbcanbus.cpp)
    target_include_directories(${target} PRIVATE ${LIBUSB1_INCLUDE_DIR})
    target_link_libraries(${target} ${visibility} ${LIBUSB1_LIBRARY})
    target_compile_definitions(${target} PRIVATE MCPSERVERLIB_HAVE_GSUSB)
  endif()
endfunction()

# Copies libusb-1.0.dll next to an executable that uses the gs_usb backend
# (a no-op when it isn't compiled in).
function(mcplib_deploy_libusb target)
  if(MCPLIB_LIBUSB_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MCPLIB_LIBUSB_DLL}" "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Copying libusb-1.0.dll next to ${target}")
  endif()
endfunction()

# --- mcpcommands: the tools, no server -------------------------------------------
add_library(mcpcommands STATIC ${MCPLIB_COMMAND_SOURCES})
target_include_directories(mcpcommands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mcpcommands PUBLIC MCPSERVERLIB_STATIC)
target_link_libraries(mcpcommands PUBLIC Qt6::Core Qt6::Network Qt6::SerialBus)
_mcplib_add_gsusb(mcpcommands PUBLIC) # static: the executable links libusb itself

# --- mcpserverlib: the MCP server with every tool --------------------------------
if(MCPLIB_BUILD_SERVER)
  find_package(Qt6 REQUIRED COMPONENTS Concurrent HttpServer)
  find_package(Threads REQUIRED)

  add_library(mcpserverlib SHARED
    ${MCPLIB_COMMAND_SOURCES}
    mcpserver.h mcpserver.cpp
  )
  target_include_directories(mcpserverlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(mcpserverlib PUBLIC
    Qt6::Core
    Qt6::Concurrent
    Qt6::Network
    Qt6::HttpServer
    Qt6::SerialBus
    Threads::Threads
  )
  target_compile_definitions(mcpserverlib PRIVATE MCPSERVERLIB_LIBRARY)
  _mcplib_add_gsusb(mcpserverlib PRIVATE)
  if(MCPLIB_LIBUSB_DLL)
    add_custom_command(TARGET mcpserverlib POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MCPLIB_LIBUSB_DLL}" "$<TARGET_FILE_DIR:mcpserverlib>"
      COMMENT "Copying libusb-1.0.dll next to mcpserverlib")
  endif()
endif()
