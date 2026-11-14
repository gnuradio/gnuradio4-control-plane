# API Routes

Every route the server answers, with its gRPC form. The REST routes are registered in `register_routes()` in `src/api/http_server.cpp`, and the gRPC contract is `proto/gr4cp/v1/control_plane.proto`. Both transports call the same application services, so they see the same sessions.

The REST API listens on `GR4CP_PORT`, 8080 by default. The gRPC API is served by a build with `GR4CP_ENABLE_GRPC=ON` when `GR4CP_GRPC_PORT` is set.

## Routes

| REST route                                          | gRPC RPC                                   | Request                                                        | Success                                                                          |
| --------------------------------------------------- | ------------------------------------------ | -------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| `GET /healthz`                                      | `ServerService.GetHealth`                  | none                                                           | 200 with `ok` and `server_instance_id`                                           |
| `GET /version`                                      | `ServerService.GetVersion`                 | none                                                           | 200 with `version` and `server_instance_id`                                      |
| `POST /sessions`                                    | `SessionService.CreateSession`             | `grc` (required), `name`, `scheduler_id`                       | 201 with the session, stopped                                                    |
| `GET /sessions`                                     | `SessionService.ListSessions`              | none                                                           | 200 with every session, each with its `streams`                                  |
| `GET /sessions/{id}`                                | `SessionService.GetSession`                | none                                                           | 200 with the session and its `streams`                                           |
| `DELETE /sessions/{id}`                             | `SessionService.DeleteSession`             | none                                                           | 204, empty                                                                       |
| `POST /sessions/{id}/start`                         | `SessionService.StartSession`              | none                                                           | 200 with the session                                                             |
| `POST /sessions/{id}/stop`                          | `SessionService.StopSession`               | none                                                           | 200 with the session                                                             |
| `POST /sessions/{id}/restart`                       | `SessionService.RestartSession`            | none                                                           | 200 with the session                                                             |
| `GET /sessions/{id}/blocks/{unique_name}/settings`  | `BlockSettingsService.GetBlockSettings`    | none                                                           | 200 with `settings`                                                              |
| `POST /sessions/{id}/blocks/{unique_name}/settings` | `BlockSettingsService.UpdateBlockSettings` | a settings object; `mode` is `staged` (default) or `immediate` | 200 with `session_id`, `block`, `applied_via` and `accepted`                     |
| `GET /blocks`                                       | `CatalogService.ListBlocks`                | none                                                           | 200 with every block                                                             |
| `GET /blocks/{id}`                                  | `CatalogService.GetBlock`                  | none                                                           | 200 with the block                                                               |
| `GET /schedulers`                                   | `CatalogService.ListSchedulers`            | none                                                           | 200 with every scheduler                                                         |
| `GET /schedulers/{id}`                              | `CatalogService.GetScheduler`              | none                                                           | 200 with the scheduler                                                           |
| `GET /sessions/{id}/streams/{stream_id}/http`       | none                                       | none                                                           | the stream's current payload, with the status and content type the runtime gives |
| `GET /sessions/{id}/streams/{stream_id}/ws`         | none                                       | a WebSocket upgrade                                            | 101, then the stream's frames; 426 without the upgrade                           |

Notes on the REST form:

- `{id}` of a session takes letters, digits and `_`. A block `{id}`, a `{unique_name}` and a `{stream_id}` are percent-encoded.
- Request and response bodies are JSON. The settings `mode` is a query parameter, `?mode=immediate`.
- A method other than `GET`, `POST` or `DELETE` answers 405.

Notes on the gRPC form:

- A path parameter becomes a request field: `session_id`, `block_unique_name`, `block_id`, `scheduler_id`.
- The settings `mode` is the `BlockSettingsMode` enum, and an unset mode is staged.
- A 64-bit integer setting reads as an integer, where REST returns a decimal string.
- A settings `Value` with no kind set is `INVALID_ARGUMENT`; explicit `null_value` is the null setting.
- A block parameter with no enum labels returns an empty `enum_labels` map, where REST omits the object.
- `streams` are set on `GetSession` and `ListSessions` only, as in REST. Stream data has no gRPC form. A client reads it from the REST `path` of each stream.
- The standard `grpc.health.v1.Health` service is served as well, for tools that probe it.

## Errors

A REST error is a JSON body, `{"error": {"code": ..., "message": ...}}`. A gRPC error carries the same message as its status message, and the same `code` in the trailing metadata key `gr4cp-error-code`.

| Cause                                                    | REST status and `code`                           | gRPC status           |
| -------------------------------------------------------- | ------------------------------------------------ | --------------------- |
| A malformed request, or a block refused a settings patch | 400 `validation_error`                           | `INVALID_ARGUMENT`    |
| An unknown session, block or scheduler                   | 404 `not_found`                                  | `NOT_FOUND`           |
| The session's state does not allow the call              | 409 `invalid_state`                              | `FAILED_PRECONDITION` |
| The runtime failed                                       | 500 `runtime_error`                              | `INTERNAL`            |
| A catalog failed to load                                 | 500 `catalog_error` or `scheduler_catalog_error` | `INTERNAL`            |
| Any other failure                                        | 500 `internal_error`                             | `INTERNAL`            |
| The public listener could not reach its internal route   | 502 `bad_gateway`                                | none                  |
| The client cancelled the call                            | none                                             | `CANCELLED`           |
| The client's deadline passed                             | none                                             | `DEADLINE_EXCEEDED`   |

A gRPC client gets `CANCELLED` or `DEADLINE_EXCEEDED` at once. A call the server has not begun does no work. A call that is already running in the server completes there, so after a deadline on a call that changes a session, read the session to learn its state.

The server keeps gRPC's default limit of 4 MiB for a request message. A `CreateSession` whose flowgraph is larger fails with `RESOURCE_EXHAUSTED`, without a `gr4cp-error-code`; send such a flowgraph through `POST /sessions`. A client applies its own limit to each response, also 4 MiB by default, and can raise it if `ListBlocks` outgrows it.

## More Detail

- Session fields and lifecycle rules: `README.md`, "Session Lifecycle", and `docs/design.md`, "Application Boundary"
- Stream descriptors: `README.md`, "Runtime Stream Descriptors"
- Settings values and modes: `README.md`, "Live Block Settings"
- Every gRPC message and field: `proto/gr4cp/v1/control_plane.proto`
