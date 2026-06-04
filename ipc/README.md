# IPC Scaffold (Phase B)

This directory is reserved for the post-MVP work of extracting the engine
into a separate process (PRD ADR-002 / §8.6). Empty in MVP.

## When to populate

Triggers (any one):

- The CLI binary needs to ship without the Qt runtime
- A second consumer of the engine emerges (server, browser companion)
- C++ memory-safety bugs prove painful enough to justify a Rust rewrite
  of `engine/src/`

## Plan

When triggered:

1. Switch `reqloom-engine` from `STATIC` to `SHARED` in
   `engine/CMakeLists.txt` (one-line change).
2. Add the JSON-RPC-over-stdio façade here that links `reqloom::engine`
   and exposes the public API as RPC methods.
3. Add `IpcEngineClient` to `desktop/` that spawns the IPC server and
   talks to it; replace direct `ExecutionEngine` calls in
   `Bootstrapper.cpp` with the client.
4. Optionally, port `engine/src/` to Rust over time. The C++ headers in
   `engine/include/reqloom/engine/` stay; only `.cpp` files change.

The architectural guardrails in `cmake/ReqloomBoundaryGuards.cmake`
already keep the engine free of UI dependencies, which is what makes
this extraction cheap.
