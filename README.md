# mcpserverlib

A small Qt6 library that exposes tools to MCP (Model Context Protocol) clients
from any Qt application — console, QML, or Widgets — over two built-in
transports: stdio and Streamable HTTP.

## Features

- MCP JSON-RPC 2.0 handling: `initialize`, `tools/list`, `tools/call`
- Two transports running simultaneously:
  - **stdio** — newline-delimited JSON on stdin/stdout, for clients that
    spawn your binary directly (Claude Code, Codex, OpenCode, ...)
  - **Streamable HTTP** — `POST /mcp`, for clients that connect over the network
- Tool calls run on a background thread pool, so a slow tool never blocks the
  caller (the HTTP I/O thread, or your app's main/UI thread if embedded
  directly in a QML or Widgets app)
- Dynamic tool registration at runtime, with a `notifications/tools/list_changed`
  push to stdio clients when the tool set changes
- Thread-safe command registry

## Requirements

- Qt 6 — `Core`, `Concurrent`, `Network`, `HttpServer`
- CMake 3.16+
- C++17

## Adding it to your project

```cmake
add_subdirectory(path/to/mcpserverlib)
target_link_libraries(your_app PRIVATE mcpserverlib)
```

Headers are exported via `target_include_directories(... PUBLIC ...)`, so no
manual `include_directories()` call is needed.

### Just the tools, without the server

The same sources also build **`mcpcommands`**: a static library with
`McpCommand`, the registry and every tool (CAN/UDS, DoIP, HSFZ) but no MCP
server — for running the tools inside an application of your own. It needs
only Qt `Core`, `Network` and `SerialBus`; turn the server off to skip Qt
HttpServer entirely:

```cmake
set(MCPLIB_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/mcpserverlib)
target_link_libraries(your_app PRIVATE mcpcommands)
mcplib_deploy_libusb(your_app) # copies libusb-1.0.dll next to it, when gs_usb is built in
```

The tool managers (`CanManager`, `DoipManager`, `HsfzManager`) wait for
vehicle responses in nested event loops, so give them a thread of their own
rather than your UI thread. Each has a `status()` — the open connection and
its running tester presents — that is safe to call from any thread without
waiting for the manager's.

### gs_usb adapters (libusb)

gs_usb CAN/CAN-FD adapters (candleLight, CANable, …) are reached over libusb
when it's found at configure time. Point `LIBUSB_ROOT` at an install prefix
(with `include/`, `lib/`, `bin/`), e.g. vcpkg's
`-DLIBUSB_ROOT=C:/vcpkg/installed/x64-windows`; `VCPKG_ROOT` and
`%USERPROFILE%/vcpkg` are searched too. libusb is plain C, so the MSVC-built
vcpkg package works from MinGW builds as well.

## Usage

```cpp
#include "mcpcommand.h"
#include "mcpcommandregistry.h"
#include "mcpserver.h"

class GetStatusCommand : public McpCommand {
public:
    QJsonObject definition() const override {
        return QJsonObject{
            {"name", "get_status"},
            {"description", "Retrieve app status"},
            {"inputSchema", QJsonObject{{"type", "object"}}}
        };
    }
    QJsonObject execute(const QJsonObject &args) override {
        return QJsonObject{{"content", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "OK"}}
        }}};
    }
};

McpCommandRegistry registry;
registry.addCommand(QSharedPointer<GetStatusCommand>::create());

McpServer server(&registry, 3768); // HTTP on 127.0.0.1:3768; stdio is always on
```

## Writing a tool

Implement `McpCommand`:

- `definition()` — the MCP tool schema (`name`, `description`, `inputSchema`)
- `execute(args)` — runs on a worker thread; must be safe to call
  concurrently with itself

## Transports

| Transport | Endpoint | Notes |
|---|---|---|
| stdio | stdin / stdout | newline-delimited JSON-RPC; app logging must go to stderr, never stdout |
| HTTP | `POST http://127.0.0.1:<port>/mcp` | one JSON response per request; notifications get `202 Accepted` with an empty body |

## Dynamic tools

Call `registry.addCommand()` / `registry.removeCommand()` at any time.
Connected **stdio** clients get a `notifications/tools/list_changed` push
automatically. HTTP clients don't — that transport is plain request/response
with no open stream to push over; call `tools/list` again if you suspect the
tool set changed.

## Limitations

- No SSE stream on the HTTP transport, so server-initiated notifications only
  reach stdio clients
- Only the `tools` capability is implemented — no MCP resources or prompts
