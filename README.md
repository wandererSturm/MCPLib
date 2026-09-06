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
