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
- optional `completion_reason`: how the last run ended, `completed` by itself, `stopped` by a stop, or `error`; cleared when a run starts
- `created_at`
- `updated_at`
- optional scheduler alias used by runtime preparation

Lifecycle rules:

- `create` validates non-empty GRC content and stores a new stopped session.
- `start` prepares and starts runtime execution, then marks the session running.
- `stop` succeeds when already stopped and otherwise stops runtime execution before persisting stopped state. The stop cancels the run and does not drain the samples already in the graph. It also succeeds for a run that ended by itself. A run that ended with an error leaves the session in `error` with the run's error. A stop that fails leaves the session in `error`, and a later stop answers the session as it stands.
- `restart` performs stop-complete-then-start behavior.
- `remove` stops a running session before destroying runtime state and deleting the session.
- `start`, `stop`, `restart` and `remove` read the run first. A run that ended by itself is recorded before the action.
- every read of a running session observes its run. The last error a block reported becomes `last_error`. A run that ended by itself moves the session to `stopped`, or to `error` with the run's error. The ended run is destroyed after the service releases its lock.
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

- load plugins once at startup through one GNU Radio 4 plugin loader, from `GNURADIO4_PLUGIN_DIRECTORIES` and then the installation's plugin directory; a shared object in a searched directory that loads, has no plugin entry point and registers nothing is passed over, and any other shared object that fails to load stops startup with a line that names the directory and the file
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
- the target block is addressed by name, unique name or `name` setting, in that order
- the settings calls read the session as `GET /sessions/{id}` does
- an update needs a running session; stopped or errored sessions, and a run that is ending by itself, return conflict errors
- a read of a session whose run has ended, by itself or by a stop, answers with the settings the run left; the runtime keeps the ended run's block handles until the session starts again or is deleted
- a read of a session that has not run, or whose run is stopping, returns a conflict error
- a name that more than one block carries is refused with the candidates
- stored GRC content is not inspected to fabricate values

`POST /settings` stages the JSON object's values on the block, which applies them at its next work call. `applied_via` names the mode:

- default mode: `staged_settings`
- `mode=immediate`: `settings`

Supported JSON value shapes are:

- `null`
- boolean
- integer
- floating point
- string
- array, each element one of these shapes; the block converts each element to its setting's element type
- an object with exactly the two numbers `real` and `imag`, a complex value
- nested object

A read returns a list setting as an array and a complex value as the object of `real` and `imag`. A 64-bit integer, signed or unsigned, reads as its decimal string, in a running session and after its run has ended. A smaller integer reads as a number. A patch takes a 64-bit setting, or an element of a list of them, as its decimal string or as a number. The block reads the string as a decimal integer, ignoring surrounding whitespace and a trailing `#` comment. A string it cannot read, or one outside the setting's range, is refused with 400 naming the key.

## Stream Metadata

Session responses may include runtime-derived stream descriptors for running sessions when submitted graph content contains supported stream-export metadata. This is response data associated with session lifecycle, not a separate public stream-management API.

Supported authored transport values:

- `http_snapshot`
- `http_poll`
- `websocket`

Authored stream metadata may be expressed in either of these forms:

- preferred nested metadata under `parameters.stream`
- compatibility flattened fields such as `stream_id`, `transport`, and `payload_format`

The graph loader refuses a parameter the block does not declare. Before the graph loads, the nested `stream` map, and the flat `stream_id` and `payload_format` of a block that gets a binding, move under the block's `meta_information`. The binding's `endpoint` and `transport` reach the block as load overrides.

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
- the stream routes read the session as `GET /sessions/{id}` does, and a run that is ending by itself returns a conflict error
- absence of supported stream metadata leaves the session on the no-stream-descriptor path

Do not introduce public stream, websocket, SSE, diagnostics, or history surfaces as part of this feature area.

## Error Handling

Application services raise typed errors for validation, not-found, invalid-state, runtime, catalog, and scheduler-catalog failures. The HTTP layer maps those errors into stable JSON error responses.

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
