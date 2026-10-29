# Finite execution and terminal settings

The existing session GET/list endpoints reconcile scheduler completion. An execution that
finishes normally reports `state: stopped`, `completion_reason: completed`; an explicit
Stop reports `stopped`. Failed execution and downstream drain timeout report `state: error`
and a distinct reason/error. `execution_id` changes for every prepared execution, including
restart after destroy. It is an opaque identity, not a JavaScript number.

`GET /sessions/{id}/blocks/{target}/settings` continues to read running settings. Once the
scheduler has joined, the runtime captures the readable reflected settings of its blocks
before destroying the graph. The same GET reads this frozen capture while the session
exists. A never-executed session still returns conflict. POST settings remains restricted
to running execution. Restart replaces the prior terminal capture; DELETE releases it.
There is no execution-history endpoint.

Terminal settings include `__gr4cp_terminal`: session/execution identity, native block unique
name, generation, terminal reason, `capture_complete`, and `unavailable_fields`. All native
uint64 and int64 values in terminal responses are decimal strings, recursively; smaller
integer types retain their existing JSON representation. Running GET serialization is
unchanged. Consumers must check identity and capture status before using a finalization map.

Capture has accounting limits of 4 MiB per block and 16 MiB per execution, depth 24,
262144 visited values per block, and 512 blocks. Captured data is native typed metadata;
these are accounting limits, not a process RSS guarantee. Unsupported existing settings
values (such as vectors outside the current settings HTTP contract) and oversized fields
are omitted with explicit bounded failure metadata. Structural execution capture failure
marks the session error and still destroys graph resources. Lookup aliases preserve native
unique names and unambiguous authored names.

Stop cancels connected stream sources under scheduler work quiescence, propagates EOS,
and lets downstream accepted data drain. Control Plane uses a five-second downstream drain
deadline before immediate scheduler stop, recording a timeout. Blocks must implement
cooperative, bounded stop hooks and work calls; C++ cannot forcibly terminate arbitrary
blocking user code. WebSocket sink queues have a separate two-second drain/close deadline
per client; clients drain concurrently. An absent client requires no waiting. The existing
Control Plane proxy forwards normal close after its previous browser write completes.

A successful socket write/close is not proof that a browser applied data. Compare the
sink publication cursor with the client's applied cursor. Sink `transport_drain_failures`
is independent of acquisition/processing counters. A disconnected or over-budget client
can need resynchronization. Terminal metadata is not a retained stream snapshot or archive.

A producer's reflected `finalization` belongs to that producer's terminal state. It is not
an additional stream transaction. Control Plane does not interpret it, produce synthetic
updates, or change closed/active region states.
