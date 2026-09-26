# Design

## Product Shape

`gr4-control-plane` is a focused C++23 REST service for creating and controlling GNU Radio 4 sessions. It is intended to be built on as the session-control boundary.

The product surface is deliberately small:

- session lifecycle control
- live block settings for running sessions
- read-only GNU Radio 4-backed block catalog
- thin HTTP and CLI adapters over application services

The service is not a general platform layer. It does not provide multi-tenant orchestration, graph editing, diagnostics, observability, history, audit, or public plugin management APIs.

## Architecture Layers

The mandatory layers are:

- `domain`: core types and pure validation helpers
- `storage`: repository interfaces and persistence implementations
- `runtime`: GNU Radio 4 execution boundary
- `app`: application services and lifecycle semantics
- `api`: HTTP transport adapter

Supporting namespaces:

- `catalog`: provider-backed read-only GNU Radio 4 metadata
- `cli`: command-line REST API client

New work should extend these layers instead of adding side channels or broad platform abstractions.

## Application Boundary

`SessionService` is the core application boundary. It owns session lifecycle semantics and coordinates repository state with runtime operations.

Lifecycle states:

- `stopped`
- `running`
- `error`

Each session stores:

- `id`
- `name`
- `grc_content`
- `state`
- optional `last_error`
- `created_at`
- `updated_at`
- optional scheduler alias used by runtime preparation

Lifecycle rules:

- `create` validates non-empty GRC content and stores a new stopped session.
- `start` prepares and starts runtime execution, then marks the session running.
- `stop` succeeds when already stopped and otherwise stops runtime execution before persisting stopped state.
- `restart` performs stop-complete-then-start behavior.
- `remove` stops a running session before destroying runtime state and deleting the session.
- runtime failures are recorded on the session as `error` with `last_error`, then surfaced as application errors.

Business rules belong in `SessionService` or the specific application service responsible for the feature. They do not belong in route handlers or the CLI.

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

`GET /healthz` is temporary bootstrap infrastructure and is not part of the product API.

No other product endpoints should be added without an explicit scope decision.

The HTTP layer stays thin:

- parse JSON bodies and path parameters
- call application services
- serialize successful responses
- map application errors to HTTP responses

It must not contain lifecycle state machines, runtime orchestration, catalog curation, settings conversion rules, or compatibility policy.

## CLI

`gr4cp-cli` is a client of the REST API. It reads local GRC files for `sessions create`, sends requests to a running server, and prints server responses.

It must not duplicate `SessionService` lifecycle behavior, inspect runtime state locally, or implement a second control plane.

## Storage

The current repository implementation is thread-safe and in memory. This matches the current appliance shape:

- no persistence
- no distributed coordination
- no cross-process state sharing

Future persistence should be added behind the existing repository boundary. It should not change HTTP route behavior or move lifecycle rules out of `SessionService`.

## Runtime

The runtime layer isolates application lifecycle semantics from GNU Radio 4 execution details.

`RuntimeManager` owns the execution boundary:

- prepare runtime resources
- start execution
- stop execution
- destroy runtime resources
- apply live block settings
- read live block settings

`Gr4RuntimeManager` is the production runtime implementation. It loads submitted GRC content through GNU Radio 4, prepares scheduler execution, and uses GNU Radio 4 runtime messaging for live settings. The stub runtime exists for focused tests and must not become a second product behavior path.

Runtime-specific details must stay out of `domain`, `storage`, and `api`.

## Block Catalog

The block catalog is a read-only metadata surface for client tooling. It is independent of session lifecycle and running graphs.

The catalog path uses a provider/service boundary:

- `domain`: block, port, and parameter descriptor types
- `catalog`: `BlockCatalogProvider` plus the GNU Radio 4-backed provider
- `app`: `BlockCatalogService` for snapshot caching and deterministic list/get behavior

The production catalog source is GNU Radio 4:

- load plugins once at startup through one GNU Radio 4 plugin loader, from `GNURADIO4_PLUGIN_DIRECTORIES` and then the installation's plugin directory; a shared object in a searched directory that is neither a plugin nor a block library stops startup with a line that names the directory and the file
- enumerate registered block types
- instantiate block models for reflection
- translate reflected metadata into stable `BlockDescriptor` values

If GNU Radio 4 plugin loading, registry enumeration, or reflection cannot initialize, server startup must fail. The production server must not fall back to static placeholder catalog data.

The catalog must remain read-only. It must not depend on running sessions, runtime graph inspection, or custom plugin platform abstractions.

## Live Block Settings

Live block settings are the narrow runtime control surface for active graphs:

- `POST /sessions/{id}/blocks/{unique_name}/settings`
- `GET /sessions/{id}/blocks/{unique_name}/settings`

Rules:

- settings calls are session-scoped
- the target block is addressed by runtime `unique_name`
- the session must be running
- stopped or errored sessions return conflict errors
- stored GRC content is not inspected to fabricate values

`POST /settings` accepts a JSON object and translates it into GNU Radio 4 runtime settings messages:

- default mode: staged settings
- `mode=immediate`: immediate settings

Supported JSON value shapes are:

- `null`
- boolean
- integer
- floating point
- string
- nested object

Arrays are rejected. Unsupported runtime reply shapes are surfaced as errors.

## Stream Metadata

Session responses may include runtime-derived stream descriptors for running sessions when submitted graph content contains supported stream-export metadata. This is response data associated with session lifecycle, not a separate public stream-management API.

Supported authored transport values:

- `http_snapshot`
- `http_poll`
- `websocket`

Authored stream metadata may be expressed in either of these forms:

- preferred nested metadata under `parameters.stream`
- compatibility flattened fields such as `stream_id`, `transport`, and `payload_format`

Runtime stream descriptors are serialized in session responses under `streams` only when the session is running and the runtime has active stream bindings. Each descriptor contains:

- `id`
- `block_instance_name`
- `transport`
- `payload_format`
- `path`
- `ready`

Stream handling remains runtime-owned:

- authored graph content stores stream intent
- runtime preparation derives active bindings
- active descriptors are scoped to the running session generation
- stored graph content is not rewritten during runtime preparation
- authored endpoint values may remain in graph content, but runtime-owned bindings define the active descriptor for supported managed stream metadata
- invalid supported stream metadata fails start or restart explicitly
- absence of supported stream metadata leaves the session on the no-stream-descriptor path

Do not introduce public stream, websocket, SSE, diagnostics, or history surfaces as part of this feature area.

## Error Handling

Application services raise typed errors for validation, not-found, invalid-state, runtime, timeout, catalog, and scheduler-catalog failures. The HTTP layer maps those errors into stable JSON error responses.

Route handlers should not inspect exception internals beyond this mapping. Application services should provide messages that are useful to operators while preserving the narrow API contract.

## Scope Boundaries

Out of scope:

- graph abstractions or graph editing APIs
- generic parameter patching beyond live block settings
- multi-tenant control planes
- orchestration layers
- observability, diagnostics, history, audit, metrics, or admin APIs
- public plugin management APIs
- SSE, websocket, or event-stream product surfaces
- runtime graph inspection as a catalog source
- compatibility layers for older platform designs

Prefer deletion over expansion. Add abstractions only when they remove real complexity inside the existing layer boundaries.
