# gr4-control-plane

`gr4-control-plane` is a small C++23 REST service for creating and controlling GNU Radio 4 sessions.

It is designed as a focused control surface:

- create, list, inspect, start, stop, restart, and delete sessions
- update and read live block settings on running sessions
- expose a read-only GNU Radio 4-backed block catalog
- keep session state in memory
- keep HTTP and CLI layers thin over application services

The service is not a multi-tenant platform, graph editor, diagnostics system, plugin manager, or observability API. The core application boundary is `SessionService`; HTTP routes translate requests into service calls and serialize the result.

## Repository Layout

- `include/gr4cp/domain`, `src/domain`: session and catalog domain types
- `include/gr4cp/storage`, `src/storage`: in-memory session repository
- `include/gr4cp/runtime`, `src/runtime`: GNU Radio 4 runtime integration plus test stub runtime
- `include/gr4cp/app`, `src/app`: application services
- `include/gr4cp/api`, `src/api`: HTTP adapter
- `include/gr4cp/cli`, `src/cli`: REST API client CLI
- `test`: unit, HTTP, CLI, and smoke tests
- `docs/design.md`: architecture notes

## Requirements

- CMake 3.25 or newer
- C++23 compiler
- GNU Radio 4 installation with CMake package files
- Boost headers
- nlohmann-json
- cpp-httplib
- GTest
- Zlib
- pkg-config

The production block catalog is GNU Radio 4-backed only. With the default `GR4CP_ENABLE_GR4_CATALOG=ON`, configuration requires a working `gnuradio4` package. Server startup also validates GNU Radio 4 plugin loading and catalog reflection before accepting requests. If initialization fails, the server exits instead of serving placeholder catalog data.

## Build And Test

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If GNU Radio 4 is installed in a non-standard prefix:

```bash
cmake -S . -B build -DGR4CP_GNURADIO4_PREFIX=/path/to/gr4/prefix
```

`GR4CP_GNURADIO4_PREFIX` can also be provided as an environment variable.

Useful build options:

- `GR4CP_ENABLE_GR4_CATALOG=ON`: require the GNU Radio 4-backed catalog provider, enabled by default
- `GR4CP_SUPPRESS_IMPORTED_WERROR=ON`: strip inherited `-Werror` from imported GNU Radio 4 targets when needed
- `USE_CCACHE=ON`: use ccache when available, enabled by default

Built executables:

- `build/gr4cp_server`
- `build/gr4cp-cli`

Install the runtime with:

```bash
cmake --install build --prefix /path/to/prefix
```

The install includes `libgr4cp_core` and `libgr4cp_cli`. The installed server
and CLI use relative runtime paths to find those libraries under the same
prefix, so they do not depend on the build tree.

## Run The Server

```bash
./build/gr4cp_server
```

The server listens on `0.0.0.0:8080` by default. Override the port with `GR4CP_PORT`:

```bash
GR4CP_PORT=8090 ./build/gr4cp_server
```

Launchers can request an available loopback port and receive the selected port
through a file. The server reserves the socket before loading the catalogs, so
the port cannot be taken while startup is in progress:

```bash
GR4CP_PORT=0 GR4CP_PORT_FILE=/tmp/gr4cp.port ./build/gr4cp_server
```

`GR4CP_PORT_FILE` is only used when `GR4CP_PORT=0`.

Temporary health check:

```bash
curl http://127.0.0.1:8080/healthz
```

## HTTP API

The public product API is limited to these endpoints:

- `POST /sessions`
- `GET /sessions`
- `GET /sessions/{id}`
- `DELETE /sessions/{id}`
- `POST /sessions/{id}/start`
- `POST /sessions/{id}/stop`
- `POST /sessions/{id}/restart`
- `POST /sessions/{id}/blocks/{unique_name}/settings`
- `GET /sessions/{id}/blocks/{unique_name}/settings`
- `GET /blocks`
- `GET /blocks/{id}`

`GET /healthz` is a temporary bootstrap endpoint and is not part of the product API.

Errors are returned as JSON:

```json
{
  "error": {
    "code": "validation_error",
    "message": "request body must be valid JSON"
  }
}
```

## Session Lifecycle

Create a session from inline GRC content:

```bash
curl -X POST http://127.0.0.1:8080/sessions \
  -H 'Content-Type: application/json' \
  -d '{"name":"demo","grc":"<inline grc content>"}'
```

`grc` is required and must be a non-empty string. `name` is optional and defaults to an empty string.

List sessions:

```bash
curl http://127.0.0.1:8080/sessions
```

Inspect a session:

```bash
curl http://127.0.0.1:8080/sessions/<id>
```

Start, stop, or restart a session:

```bash
curl -X POST http://127.0.0.1:8080/sessions/<id>/start
curl -X POST http://127.0.0.1:8080/sessions/<id>/stop
curl -X POST http://127.0.0.1:8080/sessions/<id>/restart
```

Delete a session:

```bash
curl -X DELETE http://127.0.0.1:8080/sessions/<id>
```

Session responses include `id`, `name`, `state`, `created_at`, `updated_at`, and `last_error`. A running session may also include runtime-derived stream descriptors when the submitted graph contains supported managed stream metadata.

## Runtime Stream Descriptors

When a running session contains supported authored stream-export metadata, session responses may include a `streams` array. This is runtime-derived session metadata, not a separate stream-management API.

Supported authored transport values:

- `http_snapshot`
- `http_poll`
- `websocket`

Authored stream metadata is read from graph block parameters. The preferred shape is nested under `parameters.stream`; compatibility flattened fields such as `stream_id`, `transport`, and `payload_format` are also accepted.

Each descriptor includes:

- `id`
- `block_instance_name`
- `transport`
- `payload_format`
- `path`
- `ready`

Behavior notes:

- `streams` appears only for running sessions.
- stored graph content is not rewritten when runtime bindings are derived.
- authored endpoint values may remain in graph content, but runtime-owned bindings define the active session descriptor when supported stream metadata is present.
- invalid supported stream metadata fails start or restart explicitly.
- sessions without supported stream metadata omit `streams`.

## Live Block Settings

Live block settings operate only on running sessions. Blocks are addressed by runtime `unique_name`.

Apply a partial settings update:

```bash
curl -X POST http://127.0.0.1:8080/sessions/<id>/blocks/src0/settings \
  -H 'Content-Type: application/json' \
  -d '{"frequency":1250.0,"amplitude":0.5}'
```

By default, updates use the staged GR4 settings path. Use `mode=immediate` for the immediate GR4 property endpoint:

```bash
curl -X POST 'http://127.0.0.1:8080/sessions/<id>/blocks/src0/settings?mode=immediate' \
  -H 'Content-Type: application/json' \
  -d '{"frequency":1250.0}'
```

Request bodies must be JSON objects. Supported values are `null`, boolean, integer, floating point, string, and nested object values. Arrays are rejected.

Successful updates return:

```json
{
  "session_id": "sess_0123456789abcdef",
  "block": "src0",
  "applied_via": "staged_settings",
  "accepted": true
}
```

Read effective runtime settings:

```bash
curl http://127.0.0.1:8080/sessions/<id>/blocks/src0/settings
```

Response:

```json
{
  "settings": {
    "frequency": 1250.0,
    "amplitude": 0.5
  }
}
```

Stopped or errored sessions return a conflict error for this surface. The service does not fabricate settings from stored GRC content.

## Block Catalog

The block catalog is read-only metadata for client tooling such as future `gr4-studio` integration. It is populated from GNU Radio 4 plugin loading, registry enumeration, and reflection.

```bash
curl http://127.0.0.1:8080/blocks
curl http://127.0.0.1:8080/blocks/blocks.math.add_ff
```

Catalog responses describe block IDs, names, categories, summaries, ports, and parameters. The catalog is independent of running sessions and is not runtime graph inspection.

## CLI

`gr4cp-cli` is a thin client for the session lifecycle API. It talks to a running server and defaults to `http://127.0.0.1:8080`.

Supported commands:

```text
gr4cp-cli sessions create --file <path> [--name <name>] [--url <url>]
gr4cp-cli sessions list [--url <url>]
gr4cp-cli sessions get <id> [--url <url>]
gr4cp-cli sessions start <id> [--url <url>]
gr4cp-cli sessions stop <id> [--url <url>]
gr4cp-cli sessions restart <id> [--url <url>]
gr4cp-cli sessions delete <id> [--url <url>]
```

Examples:

```bash
./build/gr4cp-cli sessions create --file demo.grc --name demo
./build/gr4cp-cli sessions list
./build/gr4cp-cli sessions start sess_0123456789abcdef
./build/gr4cp-cli sessions delete sess_0123456789abcdef
```

## Architecture Notes

The mandatory layers are explicit:

- `domain`: core data types and validation helpers
- `storage`: repository interface and in-memory implementation
- `runtime`: session runtime manager boundary and GNU Radio 4 implementation
- `app`: service layer and lifecycle semantics
- `api`: HTTP adapter

The HTTP layer parses, delegates, serializes, and maps errors. The CLI is only a REST API client and does not duplicate lifecycle logic. The block catalog remains separate from session lifecycle and uses a provider/service boundary.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for the full text.
