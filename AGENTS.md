# AGENTS.md

## Scope Rules

- This repository is a GNU Radio 4 session control MVP.
- Only these public endpoints are allowed:
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
  - `GET /schedulers`
  - `GET /schedulers/{id}`
- No other product endpoints are allowed beyond temporary bootstrap `GET /healthz`.
- Current scope allows exactly those 13 product endpoints plus temporary `GET /healthz`, and nothing else.
- The optional gRPC API (`proto/gr4cp/v1/control_plane.proto`) is a second transport for the existing operations. It must not gain an RPC that has no REST counterpart.
- No platform features: no multi-tenant control plane, no orchestration layer, no observability surface, no diagnostics stream, or history APIs. There is no public plugin management surface.

## Architecture Rules

- Keep the HTTP layer thin. It should translate HTTP to service calls and nothing else.
- Keep the HTTP layer thin even after product endpoints exist: parse, delegate, serialize, and map errors only.
- Keep the gRPC layer as thin as the HTTP layer: convert, delegate, and map errors only.
- Keep the CLI thin as well: it is only a client of the existing REST API and must not duplicate `SessionService` lifecycle logic.
- `SessionService` is the core application boundary and owns lifecycle and runtime error semantics.
- Keep the block catalog separate from session lifecycle logic. It is a read-only metadata feature for future gr4-studio integration.
- The production block catalog is GR4-backed only. If GR4 plugin loading, registry enumeration, or reflection cannot initialize, startup must fail rather than serving static placeholder catalog data.
- Couple the block catalog directly to GR4 plugin loading, registry enumeration, and reflection rather than introducing broad internal platform layers.
- The real session core now lives in `domain`, `storage`, `runtime`, and `app`. New work should extend those layers instead of inventing side channels.
- Keep the mandatory layers explicit: `domain`, `storage`, `runtime`, `app`, `api`.
- Prefer deletion over expansion. Remove speculative abstractions instead of growing them.
- Keep HTTP as a thin wrapper over `SessionService`; do not move business rules into route handlers.
- The in-memory repository and stub runtime manager remain the MVP implementation until real runtime integration is needed.
- Use a provider/service seam for the block catalog. Do not turn it into runtime graph inspection or session-dependent reflection in this phase.

## Out Of Scope

- Graph abstractions
- Generic graph editing or parameter patching APIs beyond the narrow live block settings surface
- Compatibility layers
- SSE, websockets, or event streams
- Diagnostics, history, audit, metrics, or admin APIs
- Observables or reactive state layers
- Runtime graph inspection or catalog generation from running sessions
- Replacing the read-only block catalog with session-dependent introspection or custom plugin platform abstractions
