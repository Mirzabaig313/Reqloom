# Reqloom — Dependency Resolution Engine: Detailed Requirement

> **Scope:** This document refines PRD §5.8 (Dependency Resolution Algorithm), §8.3 (Execution Engine Internals), §8.4 (Engine Error Handling Strategy), and the union of FR-2.1 through FR-2.8 into an implementable specification.
> **Status:** Detailed Draft v1.1
> **Source PRD:** `docs/Reqloom - PRD.md` (v0.3 — Qt 6 + C++ stack)
> **Owner:** Engine / Tech Lead

---

## 1. Purpose

The Dependency Resolution Engine is the runtime that, given a target operation, builds and executes the minimum chain of prerequisite operations needed to reach it, while caching intermediate results and surfacing every step transparently. It is the load-bearing component of Reqloom: every UI feature (FR-5 through FR-8), the CLI (FR-13), and the AI importer (FR-9) depend on its behavior being predictable and deterministic.

This document constrains that behavior tightly enough that two independent integration testers reach identical pass/fail conclusions on every scenario.

---

## 2. Glossary (Engine-Local Terms)

| Term | Definition |
|---|---|
| **Target Operation** | The operation the user explicitly requested to run. |
| **Chain** | The ordered, deduplicated sequence of operations the engine will execute to reach the target. |
| **Prerequisite** | Any operation that must run before the target, transitively. |
| **Step** | A single position in the chain (one operation execution attempt). |
| **Run** | The execution of one chain from start to terminal state (`Succeeded`, `Failed`, `Cancelled`). |
| **Run Context** | The mutable per-run state holding session cache, extraction cache, and step results. |
| **Session** | A cached actor authentication state (token + extracted variables + TTL metadata). |
| **Extraction** | Variables captured from a response into the resource's namespace. |
| **Skip** | A step that is part of the chain but did not execute because its result was satisfied by cache. |
| **Force Re-run** | Explicit user request to invalidate one or more cached results before resolving the chain. |
| **Dry Run** | A run mode that resolves and renders the chain and final request bodies without performing any HTTP I/O. |

---

## 3. Functional Acceptance Criteria

All criteria are written in Given/When/Then. Each is independently testable. IDs are stable; future amendments append rather than renumber.

### 3.1 Chain Resolution (FR-2.1, §5.8)

**AC-3.1.1 — Single-target resolution**
Given a loaded, validated project with target operation `T` whose transitive dependency closure has `N` operations,
When the user requests to run `T`,
Then the engine SHALL produce a chain that contains exactly the unique operations in the closure of `T` plus `T` itself, in topological order.

**AC-3.1.2 — Dependency definition (the two kinds, both authoritative)**
Given an operation `O` with body/path/header/query string template containing `{{resource.field}}`,
When the engine resolves `O`'s dependencies,
Then implicit edges from `{{resource.field}}` references and explicit edges from `depends_on:` SHALL both be added to the dependency graph; the union is the operation's dependency set.

**AC-3.1.3 — Topological order is deterministic**
Given a chain with two or more valid topological orderings,
When the engine resolves the chain twice for the same target on the same schema,
Then the produced order SHALL be byte-identical across both resolutions (stable tie-break by `resource.operation` lexicographic order).

**AC-3.1.4 — Cycle detection at schema load**
Given a project YAML whose dependency graph (implicit + explicit edges) contains a cycle,
When the project loads,
Then the engine SHALL refuse to load the project, SHALL report each cycle by listing the operations in cycle order, and SHALL NOT permit any operation to be run until the cycle is resolved. (Aligns with §8.4 row "Circular dependency detected".)

**AC-3.1.5 — Self-dependency is a cycle**
Given an operation `O` that references its own resource's prior extractions in a way that creates `O → O`,
When the project loads,
Then the engine SHALL treat this as a cycle per AC-3.1.4.

**AC-3.1.6 — Unknown reference fails load**
Given any operation references `{{X.Y}}` where `X` is not a defined actor/resource/env/secret/builtin,
When the project loads,
Then the engine SHALL reject the project with an error citing the file and line of the unresolvable reference.

### 3.2 Single-Op and Flow Execution (FR-2.1, FR-2.2)

**AC-3.2.1 — Single-op run executes chain in order**
Given a resolved chain `[s1, s2, …, sN, T]`,
When the run starts,
Then the engine SHALL execute steps strictly in the resolved order; SHALL NOT begin step `sk+1` until `sk` has terminated (succeeded or skipped); AND SHALL NOT execute `T` until all prerequisites have terminated successfully or been skipped.

**AC-3.2.2 — Flow is sequential composition of single-op runs**
Given a flow `F = [T1, T2, T3]`,
When the user runs `F`,
Then the engine SHALL execute `T1`'s chain to completion, then `T2`'s chain (reusing cached results from `T1`'s run unless reset), then `T3`'s chain, in order; AND SHALL halt the flow on the first non-recovered failure.

**AC-3.2.3 — Flow halts on first failure**
Given a flow `F = [T1, T2, T3]` and `T1`'s chain fails terminally,
When the run reaches that failure,
Then the engine SHALL NOT start `T2` or `T3`, SHALL mark them as `Blocked`, AND SHALL surface `T1`'s failed step in the execution log.

### 3.3 Session Caching (FR-2.3)

**AC-3.3.1 — Session cache hit skips auth**
Given an actor `A` with a live session whose `expires_at` is strictly after the current monotonic clock,
When a chain step is `A`'s auth operation,
Then the engine SHALL mark that step `Skipped(SessionValid)` and SHALL NOT issue the auth HTTP request.

**AC-3.3.2 — Session expiry triggers refresh-or-relogin**
Given a session whose `expires_at` is at or before the current monotonic clock,
When a chain step requires that session,
Then the engine SHALL execute the actor's `session.refresh` block if defined, otherwise SHALL re-execute the actor's full auth chain, before issuing any operation that requires that session.

**AC-3.3.3 — Inline mid-chain refresh on 401**
Given a chain step receives HTTP 401 from a non-auth operation while using actor `A`,
When the response is received,
Then the engine SHALL invalidate `A`'s session, SHALL execute `A`'s refresh-or-relogin once, and SHALL retry the failing step exactly once with the new session; if the retry returns 401 again, the step fails with `E_SESSION_REFRESH_FAILED` (per §8.4 row "401 on actor session"). The session refresh and the retry SHALL count as separate steps in the execution log.

**AC-3.3.4 — TTL boundary crossed mid-chain**
Given a session valid at chain start whose TTL expires while the chain is executing but before the step that needs it,
When that step is reached,
Then the engine SHALL refresh inline per AC-3.3.2 (not wait until the next chain).

**AC-3.3.5 — Refresh failure**
Given a session refresh attempt that fails (network error, non-2xx response, or extraction failure),
When the failure is detected,
Then the engine SHALL fail the current step with `E_SESSION_REFRESH_FAILED`, SHALL halt the chain, AND SHALL NOT silently fall back to an unauthenticated request.

**AC-3.3.6 — Per-actor session isolation**
Given two actors `A` and `B`,
When `A`'s session is invalidated,
Then `B`'s session SHALL remain unaffected.

### 3.4 Resource Extraction Caching (FR-2.4)

**AC-3.4.1 — Run-lifetime extraction cache**
Given a resource operation `R.op` that has executed successfully in the current run and produced extracted variable `R.x`,
When a later step in the same run references `{{R.x}}`,
Then the engine SHALL substitute the cached value AND SHALL NOT re-execute `R.op`.

**AC-3.4.2 — Manual reset invalidates extractions**
Given the user invokes "Reset Cache" (Cmd/Ctrl+Shift+R),
When the next chain runs,
Then all resource extractions from prior runs SHALL be discarded; sessions are not reset by this action (see AC-3.4.3).

**AC-3.4.3 — Reset Cache scope (extractions only) vs Send Cleanly (full)**
Given the user invokes "Send Cleanly" (Cmd/Ctrl+Shift+Enter),
When the next chain resolves,
Then both the session cache AND the extraction cache SHALL be discarded for this run; the next chain re-executes all auth and prerequisite operations from scratch.

**AC-3.4.4 — Indexed reference resolution**
Given `{{order[2].order_id}}` and the run has produced two or more `order` resources via successful executions of `order.create`,
When the reference is resolved,
Then the engine SHALL substitute the `order_id` extracted from the second successful `order.create` execution in this run (1-indexed); IF fewer than 2 executions exist, the step fails with `E_INDEXED_REF_OUT_OF_RANGE`.

**AC-3.4.5 — No automatic invalidation on external state change**
Given a resource extraction `R.x` cached in the run, and the underlying database row was deleted between the producing call and a later reference,
When the later reference is used,
Then the engine SHALL NOT detect this; the substitution proceeds with the cached value; the downstream operation will surface a 404 or 4xx, which is reported per the error taxonomy (§5).
*Rationale: The engine has no oracle for external state. Detection of stale data is out of scope; "Reset Cache" or "Send Cleanly" is the user remedy.*

### 3.5 Retry (FR-2.5)

**AC-3.5.1 — Per-operation retry config**
Given an operation declares `retry: { max: M, backoff: B }` (M ≥ 0),
When the operation fails with a retryable cause (network timeout, 5xx, connection reset),
Then the engine SHALL retry up to `M` additional attempts, sleeping `B` between attempts; non-retryable causes (4xx other than 401, missing variable, hook error, JSON parse error on declared JSON response) SHALL NOT be retried.

**AC-3.5.2 — Retry config defaults**
Given an operation does not declare `retry`,
When the operation fails,
Then the engine SHALL use the project-level default retry policy. *(OPEN QUESTION OQ-7 — default values pending.)*

**AC-3.5.3 — Retry log entries**
Given a retried step,
When each attempt completes,
Then the execution log SHALL contain one entry per attempt with attempt number, outcome, and elapsed time.

### 3.6 Halt-on-Failure & Execution Log (FR-2.6)

**AC-3.6.1 — Halt on terminal failure**
Given any step terminates in `Failed` state after exhausting retries,
When the failure is recorded,
Then the engine SHALL NOT execute any subsequent step in the chain, SHALL mark all unexecuted steps as `Blocked`, AND SHALL transition the run to `Failed`.

**AC-3.6.2 — Full execution log preserved**
Given any run that has started (regardless of outcome),
When the run reaches a terminal state,
Then the engine SHALL persist a log to local storage containing, per step: operation id, attempt count, request (method, URL, headers with secrets masked, body), response (status, headers, body up to a configured size limit), extracted variables, skip reason if skipped, error code and error class if failed, elapsed wall-clock and monotonic durations, and start/end timestamps in UTC.

**AC-3.6.3 — Secret masking in logs**
Given any value sourced from `{{secret.X}}` or `inject.headers` containing a token,
When that value appears in the execution log,
Then it SHALL be replaced with a fixed-length masked placeholder; raw secret values SHALL NEVER be persisted to disk or shown in the timeline UI.

### 3.7 Dry Run (FR-2.8)

**AC-3.7.1 — Dry run produces resolved chain without HTTP I/O**
Given the user invokes "Dry Run" on target `T`,
When the engine executes,
Then the engine SHALL resolve the chain, SHALL produce the fully variable-substituted request line, headers, and body for every step in the chain, and SHALL NOT issue any HTTP request, SHALL NOT mutate the session cache, and SHALL NOT mutate the extraction cache.

**AC-3.7.2 — Dry run requires variable closure**
Given a dry run where some `{{resource.x}}` cannot be resolved because the producing op has never been executed in any prior run AND the extraction cache has no entry,
When the dry run renders that step,
Then the engine SHALL render the step with an explicit `<UNRESOLVED: resource.x>` placeholder, SHALL flag the step as "would-fail-at-runtime", AND SHALL NOT abort the dry run for other steps.

**AC-3.7.3 — Dry run does not execute hooks**
Given any operation in the chain has `pre_request` or `post_response` hooks,
When dry run renders that step,
Then the engine SHALL NOT execute hook scripts; the rendered request SHALL show the request body BEFORE `pre_request` runs, with an annotation that hooks are skipped. *(OPEN QUESTION OQ-3 — confirm whether dry run should also evaluate `pre_request` since it can mutate the request body.)*

### 3.8 Cancellation (Esc)

**AC-3.8.1 — Cancellation transitions run to `Cancelling`**
Given a run is in `Running` state,
When the user presses Esc (or invokes cancel via API/CLI),
Then the engine SHALL transition the run to `Cancelling` within 100 ms.

**AC-3.8.2 — In-flight HTTP request is aborted**
Given a step is awaiting an HTTP response,
When cancellation is requested,
Then the engine SHALL abort the underlying HTTP request via the HTTP client's cancellation primitive (e.g., `libcurl` `multi`-handle abort or `QNetworkReply::abort()`), SHALL NOT wait for the upstream response, AND SHALL record that step's outcome as `Cancelled` (not `Failed`).

**AC-3.8.3 — Cached results from completed steps are preserved**
Given steps `s1…sk` completed before cancellation,
When the run is cancelled,
Then their session and extraction cache writes SHALL be retained for the current run scope; subsequent runs in the same project session can reuse them per AC-3.3.1 and AC-3.4.1.

**AC-3.8.4 — Hook in-flight on cancel**
Given a `pre_request` or `post_response` hook is currently executing in QuickJS,
When cancellation is requested,
Then the engine SHALL allow the hook to complete or hit its 1-second timeout (whichever is sooner); SHALL NOT issue the HTTP request that the `pre_request` hook was preparing if cancellation arrived before the request was sent; the step SHALL be recorded as `Cancelled`.

**AC-3.8.5 — Subsequent unexecuted steps marked `Cancelled`**
Given cancellation occurs while step `sk` is in-flight,
When the run terminates,
Then `sk` is `Cancelled`; `sk+1…T` are `Cancelled` (not `Blocked` — `Blocked` is reserved for failure).

### 3.9 Concurrency

**AC-3.9.1 — Default execution model is sequential**
By default, the engine SHALL execute chain steps sequentially (one HTTP call at a time per run).

**AC-3.9.2 — Optional intra-chain parallelism (opt-in)**
*(See OQ-1.)* If parallelism is enabled, the engine SHALL identify maximally independent step sets via topological levels (Kahn's algorithm) and SHALL execute steps within the same level concurrently up to a configured concurrency limit; SHALL NOT begin a step until all of its dependencies have terminated.

**AC-3.9.3 — Same-actor session writes are serialized**
Given two parallel-eligible steps both requiring actor `A` and `A` has no live session,
When the engine begins the level,
Then auth for `A` SHALL run exactly once (deduped); the two steps SHALL wait on the same session future.

**AC-3.9.4 — Cache writes are atomic**
Given two parallel steps successfully producing extractions for the same resource,
When their results are recorded,
Then the cache SHALL serialize writes in deterministic order (completion-order of the underlying HTTP responses), and indexed references resolve in that order.

### 3.10 Variable Resolution (per §5.7)

**AC-3.10.1 — Reference resolution order**
Given a `{{X.y}}` reference,
When the engine resolves it,
Then resolution SHALL be attempted in this order: (1) builtins (`$.now`, `$.uuid`, `$.faker.*`, `$.env.*`), (2) actor sessions, (3) resource extractions (most-recent or indexed), (4) environment variables, (5) secrets; the first match wins; if no match, fail with `E_VAR_UNRESOLVED` per §8.4.

**AC-3.10.2 — Missing variable halts pre-send**
Given any required `{{X.y}}` cannot be resolved,
When the engine prepares the request,
Then the engine SHALL halt before sending and SHALL surface `missing: X.y; last set by: never | <op_id>@<timestamp>` (matches §8.4 row "Variable missing").

**AC-3.10.3 — Variable scope**
Given an extracted variable `R.x`,
When the run terminates,
Then `R.x` SHALL remain in the run-scoped extraction store for reuse by the next chain in the same project session, until invalidated by Reset Cache, Send Cleanly, project close, or schema reload (per AC-3.13.x). *(OPEN QUESTION OQ-6 — confirm whether extractions persist across app restart.)*

### 3.11 Force Re-run

**AC-3.11.1 — Force re-run target only**
Given the user invokes "Force Re-run" on operation `T`,
When the chain resolves,
Then `T` SHALL be re-executed; prerequisites SHALL use cache per default rules; the engine SHALL NOT cascade invalidation upward. *(OPEN QUESTION OQ-2 — confirm default scope; alternatives: target only, target + direct deps, full chain.)*

### 3.12 Hooks (FR-2.7 interaction with the engine)

**AC-3.12.1 — `pre_request` failure equals step failure**
Given an operation with a `pre_request` hook whose script throws or exceeds 1-second timeout,
When the hook fails,
Then the step SHALL fail with `E_HOOK_FAILURE`, the HTTP request SHALL NOT be issued, and the chain SHALL halt per AC-3.6.1.

**AC-3.12.2 — `post_response` failure equals step failure**
Given an operation with a `post_response` hook whose script throws or times out,
When the hook fails after a 2xx HTTP response,
Then the step SHALL fail with `E_HOOK_FAILURE`; extractions SHALL NOT be applied to the cache; the chain SHALL halt.

**AC-3.12.3 — Hooks are not retried separately**
Given a step is retried,
When `pre_request` exists,
Then the hook SHALL re-run on every attempt. *(OPEN QUESTION OQ-9 — confirm: is hook idempotency the user's responsibility, or should the engine cache hook output across retries?)*

### 3.13 Schema Reload

**AC-3.13.1 — Hot reload preserves session cache, discards extraction cache**
Given the project YAML changes on disk and the file watcher fires,
When the engine reloads the schema,
Then session caches SHALL be retained for unchanged actors, session caches SHALL be discarded for actors whose definitions changed, and the extraction cache SHALL be fully discarded. *(OPEN QUESTION OQ-10 — confirm; alternative is to discard everything.)*

**AC-3.13.2 — Reload during running chain**
Given a run is in `Running` state,
When the schema file changes,
Then the engine SHALL queue the reload until the run reaches a terminal state; the running chain SHALL use the pre-reload schema snapshot.

---

## 4. State Machines

### 4.1 Operation Step State Machine

| From | Event | To | Notes |
|---|---|---|---|
| `Pending` | Resolution complete | `Ready` | All upstream deps satisfied |
| `Ready` | Cache hit (session valid OR extraction present) | `Skipped` | Terminal |
| `Ready` | Variable unresolved | `Failed(E_VAR_UNRESOLVED)` | Terminal |
| `Ready` | Begin execution | `PreparingRequest` | |
| `PreparingRequest` | `pre_request` hook OK or absent | `Sending` | |
| `PreparingRequest` | Hook error/timeout | `Failed(E_HOOK_FAILURE)` | Terminal |
| `Sending` | HTTP response received | `ProcessingResponse` | |
| `Sending` | Connection error / timeout / 5xx | `RetryWaiting` | If retries remain |
| `Sending` | Cancellation signal | `Cancelled` | Terminal |
| `Sending` | 401 (non-auth op) | `RefreshingSession` | Once per step |
| `RefreshingSession` | Refresh OK | `Sending` | One retry only |
| `RefreshingSession` | Refresh fail | `Failed(E_SESSION_REFRESH_FAILED)` | Terminal |
| `RetryWaiting` | Backoff elapsed, retries remain | `Sending` | |
| `RetryWaiting` | Retries exhausted | `Failed(<cause>)` | Terminal |
| `ProcessingResponse` | `post_response` OK or absent, extraction OK | `Succeeded` | Terminal; cache updated |
| `ProcessingResponse` | Hook error | `Failed(E_HOOK_FAILURE)` | Terminal |
| `ProcessingResponse` | Extraction fail | `Failed(E_EXTRACTION_FAILED)` | Terminal |
| `ProcessingResponse` | `expect_status` mismatch | `Failed(E_STATUS_MISMATCH)` | Terminal |
| any non-terminal | Cancellation | `Cancelled` | Terminal |
| `Pending` / `Ready` | Upstream failed | `Blocked` | Terminal |

Terminal states: `Succeeded`, `Skipped`, `Failed`, `Cancelled`, `Blocked`.

### 4.2 Session State Machine (per Actor)

| From | Event | To |
|---|---|---|
| `None` | Auth required | `Authenticating` |
| `Authenticating` | Auth chain success + extractions OK | `Live(expires_at = now + ttl)` |
| `Authenticating` | Any failure | `None` |
| `Live` | `now < expires_at` and used | `Live` |
| `Live` | `now ≥ expires_at` and used | `Refreshing` |
| `Live` | 401 on dependent op | `Refreshing` |
| `Live` | "Send Cleanly" or actor schema changed | `None` |
| `Refreshing` | Refresh block success | `Live(new expires_at)` |
| `Refreshing` | Refresh block fail OR no refresh block defined | `Authenticating` (full re-login attempt) |
| `Authenticating` (re-login from refresh fail) | Fail | `None` + step fails `E_SESSION_REFRESH_FAILED` |

### 4.3 Resource Extraction Cache State Machine (per Resource Instance)

| From | Event | To |
|---|---|---|
| `Empty` | Successful extraction | `Cached(value, instance_index = N+1)` |
| `Cached` | Reference `{{R.x}}` (most-recent) | `Cached` (returns most-recent value) |
| `Cached` | Reference `{{R[k].x}}` for k ≤ N | `Cached` (returns kth value) |
| `Cached` | Reference `{{R[k].x}}` for k > N | step fails `E_INDEXED_REF_OUT_OF_RANGE` |
| `Cached` | Reset Cache / Send Cleanly / Schema reload | `Empty` |
| `Cached` | Force re-run scope includes `R.op` | `Empty` for that resource |

### 4.4 Run State Machine

| From | Event | To |
|---|---|---|
| `Idle` | User invokes run | `Resolving` |
| `Resolving` | Resolution OK | `Running` |
| `Resolving` | Cycle / unresolvable schema | `Failed(E_SCHEMA_INVALID)` |
| `Running` | All steps terminal-success | `Succeeded` |
| `Running` | Any step `Failed` | `Failed` |
| `Running` | Cancellation signal | `Cancelling` |
| `Cancelling` | All in-flight steps terminal | `Cancelled` |

### 4.5 Schema Load State Machine

| From | Event | To |
|---|---|---|
| `Unloaded` | Open project | `Parsing` |
| `Parsing` | YAML parse fail | `LoadFailed(E_YAML_PARSE)` |
| `Parsing` | Parse OK | `Validating` |
| `Validating` | Cycle | `LoadFailed(E_CYCLE)` |
| `Validating` | Unresolvable reference | `LoadFailed(E_REF_UNDEFINED)` |
| `Validating` | Schema version unsupported | `LoadFailed(E_SCHEMA_VERSION)` |
| `Validating` | OK | `Loaded` |
| `Loaded` | File change | `Parsing` (queued if a run is active) |
| `Loaded` | Project closed | `Unloaded` |

---

## 5. Error Taxonomy

Every failure mode the engine produces. Codes are stable; UI/CLI render them; QA asserts on them.

| Code | Class | Trigger | Retryable | Engine Behavior | User Indication | State Preservation |
|---|---|---|---|---|---|---|
| `E_SCHEMA_INVALID` | Schema | Generic schema validation failure | No | Refuse load | Error with file:line | Project not loaded |
| `E_YAML_PARSE` | Schema | Malformed YAML | No | Refuse load | Parse error with line | Project not loaded |
| `E_CYCLE` | Schema | Cycle in dependency graph | No | Refuse load | Cycle path listed | Project not loaded |
| `E_REF_UNDEFINED` | Schema | `{{X.y}}` references undefined symbol | No | Refuse load | Missing symbol | Project not loaded |
| `E_SCHEMA_VERSION` | Schema | `version:` not in supported set (last 3 majors) | No | Refuse load with migration hint | Migration prompt | Project not loaded |
| `E_VAR_UNRESOLVED` | Resolution | Variable cannot be substituted at send time | No | Halt step pre-send | "missing: X.y; last set by: never\|op@ts" | Cache untouched |
| `E_INDEXED_REF_OUT_OF_RANGE` | Resolution | `{{R[k].x}}` with k > available instances | No | Halt step pre-send | "needed instance k, have N" | Cache untouched |
| `E_NETWORK_TIMEOUT` | Network | Connect/read/write timeout exceeded | Yes | Retry per policy, then fail | Timeout + URL | Cache untouched for this step |
| `E_NETWORK_DNS` | Network | DNS resolution failure | Yes | Retry per policy | DNS error + host | Untouched |
| `E_NETWORK_TLS` | Network | TLS handshake failure | No | Fail immediately | TLS error + suggestion to toggle verification | Untouched |
| `E_HTTP_5XX` | HTTP | Server-side 5xx | Yes | Retry per policy | Status + body excerpt | Untouched |
| `E_HTTP_4XX` | HTTP | Non-401 4xx on dependency | No | Halt chain | Status + body excerpt | Cache untouched for failed step |
| `E_STATUS_MISMATCH` | HTTP | Response status ≠ `expect_status` | No | Halt step | Expected vs actual | Untouched |
| `E_SESSION_REFRESH_FAILED` | Auth | Refresh attempt on 401 failed | No | Halt chain | "Session refresh failed for actor X" | Session reset to `None` |
| `E_HOOK_FAILURE` | Hook | `pre_request` / `post_response` threw or timed out | No | Halt step | Stack trace from QuickJS | Cache untouched |
| `E_HOOK_TIMEOUT` | Hook | Hook exceeded 1 s | No | Halt step | "hook timeout (1s)" | Cache untouched |
| `E_EXTRACTION_FAILED` | Extraction | JSONPath/XPath/regex did not match required field | No | Halt step | Path that failed + response excerpt | Cache untouched |
| `E_RESPONSE_PARSE` | Extraction | Body declared JSON but not parseable | No | Halt step, allow raw inspection | "Response was not valid JSON" | Untouched |
| `E_CANCELLED` | Run | User cancellation | No | Mark step + chain `Cancelled` | "Cancelled by user" | Completed step caches preserved |

Cross-references to PRD §8.4: this table strictly extends and refines that table — every PRD row maps to one or more codes above.

---

## 6. Concurrency and Cancellation Semantics

### 6.1 Concurrency model
- Default: one run at a time per project; one HTTP call at a time within a run.
- Multiple runs across multiple projects (different tabs) are independent and have isolated `RunContext`s.
- Within a single run, parallel execution of independent levels is opt-in (OQ-1) and bounded by a configured concurrency limit.

### 6.2 Same-actor coalescing
When parallelism is enabled and two parallel-eligible steps both depend on actor `A`'s session in `None` state, the engine MUST coalesce: a single auth attempt occurs and both steps await its completion. A second attempt is not initiated even on transient races.

### 6.3 Cancellation propagation
- Cancellation is hierarchical: cancelling a run cancels all in-flight steps and the queued tail of the chain.
- Cancelling a flow cancels the currently-running run only; previously-completed runs in the flow remain `Succeeded`.
- The cancel signal MUST reach an in-flight HTTP request via the client's cancellation primitive within 100 ms of user input.
- Cancellation NEVER triggers compensating actions. The engine has no concept of rollback; partial side-effects on the system under test are the user's responsibility (this is a testing tool, not a transactional one).

### 6.4 Idempotency
- The engine does not assert idempotency on the SUT. It executes operations as declared.
- The `Idempotency-Key` header generation is the user's responsibility (via `{{$.uuid}}` builtins or hooks).

---

## 7. Edge Cases — Resolution Catalogue

| # | Edge Case | Engine Behavior |
|---|---|---|
| 1 | Empty chain (target has no deps and no auth) | Run executes only the target; success path normal. |
| 2 | Target IS an auth operation | Engine treats it as both a session producer and the target; one execution, session cached. |
| 3 | Two operations both extract the same variable name into the same resource | Both writes are honored; later writes shadow earlier ones for "most-recent" resolution; both indexable. |
| 4 | Optional dependency (operation `X` references `{{Y.z}}` only inside an optional `body` field) | Engine treats reference as required if the field is templated and present; if the field is conditionally rendered via hook, the dependency is implicit only when the value is referenced at render time. *(OQ-11 — confirm whether hook-injected references count.)* |
| 5 | Same operation appears in chain via two distinct dependency paths | Deduped; executed once. |
| 6 | Operation has both `expect_status` and `extract` blocks; status matches but extraction path missing | `E_EXTRACTION_FAILED`; status pass alone is not enough. |
| 7 | Network reaches server, server returns 200 with empty body, op declares JSON extractions | `E_RESPONSE_PARSE` (empty != valid JSON). |
| 8 | TTL is 0 or negative | Treated as "always expired"; auth runs every chain. |
| 9 | TTL larger than session-issued token's actual lifetime | Engine trusts declared TTL; will catch real expiry via 401 → AC-3.3.3. |
| 10 | User edits schema while chain is running | Reload queued; current chain uses snapshot (AC-3.13.2). |
| 11 | Two parallel-eligible steps write the same indexed resource (parallelism on) | Index assigned by completion order of HTTP responses (AC-3.9.4). |
| 12 | Operation has retry but is non-idempotent on the SUT | Engine still retries per declared policy; user responsibility to set `retry: { max: 0 }` for non-idempotent calls. |
| 13 | `pre_request` mutates the URL path | Allowed; the post-substitution path drives DNS/connection. The original templated path is preserved in logs alongside the mutated path. |
| 14 | Force re-run on an operation whose deps have failed in a prior run | Engine resolves chain afresh; failed-prior status does not "stick". |
| 15 | Schema reload changes a resource's `extract:` definitions mid-session | Existing extractions discarded for that resource; the new definitions apply on next run. |
| 16 | User passes Esc during cancellation | Idempotent; subsequent Esc is a no-op while in `Cancelling`. |
| 17 | Two indexed references in the same body resolve to different instances | Each `{{R[k].x}}` resolved independently per AC-3.4.4. |
| 18 | Cycle introduced via hook-injected reference | Not detectable at schema load; if such a cycle manifests at runtime via `pre_request` mutation, behavior is undefined and the user assumes responsibility. *(OQ-12 — should engine snapshot variable references after `pre_request` and re-validate?)* |

---

## 8. Test Scenarios (QA-Ready)

Each scenario lists fixture, action, and assertions. Scenarios are independent and can be ordered freely.

### 8.1 Happy paths

1. **Single op, no deps**: target with empty closure. Assert: 1 step executed, run `Succeeded`.
2. **Target with one auth dep**: actor session `None`. Assert: 2 steps; auth then target; session `Live` post-run.
3. **Marketplace `refund.approve` (PRD example)**: sample project loaded. Assert: chain order matches §5.8 example; 9 steps; first auth `Skipped` (admin pre-warmed); others execute.
4. **Reuses session across runs**: run target twice. Assert: second run's auth step is `Skipped`.
5. **Extraction reused within run**: target depends on `order.create` twice (transitively). Assert: `order.create` runs once.
6. **Indexed reference**: chain creates 3 orders, target uses `{{order[2].order_id}}`. Assert: target's request body contains the second order's id.

### 8.2 Schema-load failures

7. **Cycle detection**: schema `A → B → A`. Assert: project refuses load with `E_CYCLE` and the cycle is reported.
8. **Self-cycle**: `O` references `{{O_resource.x}}` where the only producer is `O` itself. Assert: `E_CYCLE`.
9. **Undefined reference**: `{{ghost.x}}`. Assert: `E_REF_UNDEFINED` at load.
10. **Unsupported version**: `version: 99`. Assert: `E_SCHEMA_VERSION` with migration hint.

### 8.3 Session lifecycle

11. **Expired session refresh**: TTL=0. Run target twice. Assert: auth runs both times.
12. **Mid-chain 401 refresh success**: SUT returns 401 once for `order.create`, then 200 after refresh. Assert: chain succeeds; log shows refresh step inserted.
13. **Mid-chain 401 refresh failure**: refresh endpoint returns 4xx. Assert: chain fails with `E_SESSION_REFRESH_FAILED`; subsequent steps `Blocked`.
14. **Refresh block absent, 401 occurs**: actor has no `session.refresh`. Assert: full re-login attempted; on second 401, `E_SESSION_REFRESH_FAILED`.
15. **Per-actor isolation**: invalidate vendor; admin session unaffected.

### 8.4 Extraction & cache

16. **Reset Cache button**: extractions cleared; sessions retained. Assert: next chain re-runs prerequisite operations but reuses sessions.
17. **Send Cleanly**: both caches cleared. Assert: full chain re-runs including auth.
18. **Indexed out-of-range**: `{{order[5].order_id}}` with 2 orders. Assert: `E_INDEXED_REF_OUT_OF_RANGE`.
19. **Most-recent semantics after additions**: create 2 orders, reference `{{order.order_id}}`. Assert: returns 2nd; create 3rd, re-reference. Assert: returns 3rd.

### 8.5 Retry

20. **Retry on 5xx**: SUT returns 503 twice then 200; retry policy max=2. Assert: 3 attempts, final step `Succeeded`.
21. **No retry on 4xx**: SUT returns 422; retry policy max=3. Assert: 1 attempt, step `Failed(E_HTTP_4XX)`.
22. **Retry exhaustion**: SUT returns 503 forever; max=2. Assert: 3 attempts, step `Failed(E_HTTP_5XX)`.

### 8.6 Dry run

23. **Dry run renders chain**: target with full chain; no SUT contacted. Assert: 0 outbound HTTP calls; chain rendered with all final bodies.
24. **Dry run with unresolved variable**: clear all caches. Assert: chain rendered with `<UNRESOLVED: …>` markers; no SUT calls.
25. **Dry run does not mutate caches**: pre-run snapshot caches; run dry; assert post-run caches identical.

### 8.7 Cancellation

26. **Cancel mid-flight HTTP**: SUT delays response; user cancels. Assert: in-flight HTTP request is aborted; step `Cancelled`; subsequent steps `Cancelled`.
27. **Cancel preserves prior step caches**: chain at step 4/9; cancel. Assert: extractions from steps 1–3 retained; next run uses them.
28. **Cancel during hook**: 1s hook running; cancel. Assert: hook completes or times out; HTTP not sent; step `Cancelled`.

### 8.8 Hooks

29. **Pre-request hook mutates header**: hook adds `X-Signature`. Assert: outbound headers contain it; log shows pre vs post.
30. **Post-response hook throws**: assert step `Failed(E_HOOK_FAILURE)`; extractions not applied.

### 8.9 Concurrency (only if OQ-1 resolved as opt-in)

31. **Two independent siblings parallel**: target depends on `R1.create` and `R2.create`, neither depends on the other. Assert: both fire concurrently; coalesced auth if shared actor.

---

## 9. Configuration Surface

Per-operation:
- `retry: { max: int, backoff: duration }`
- `timeout: duration` (overrides project default)
- `expect_status: int | [int]`
- `force: bool` (force re-execution even when cached — equivalent to per-op force re-run)

Per-actor:
- `session.ttl: duration`
- `session.refresh: { method, path, body, extract }`

Per-project (engine-level):
- `engine.parallelism.enabled: bool` *(OQ-1)*
- `engine.parallelism.max_concurrent: int`
- `engine.retry.default: { max, backoff }` *(OQ-7)*
- `engine.timeout.default: duration`
- `engine.dry_run.evaluate_pre_request_hooks: bool` *(OQ-3)*
- `engine.force_rerun.scope: target | direct_deps | full_chain` *(OQ-2)*
- `engine.extraction_persistence: per_run | per_session | persistent` *(OQ-6)*

All keys are read-only at runtime; changing them requires a schema reload.

---

## 10. Observability Hooks

The engine emits structured events to the application state and the local SQLite history store. Events feed the timeline UI (FR-7.6) and history (FR-12).

| Event | Fields |
|---|---|
| `RunStarted` | run_id, target_op, chain_size, env_name, timestamp |
| `StepStarted` | run_id, step_index, op_id, attempt |
| `StepSkipped` | run_id, step_index, op_id, reason (`SessionValid` \| `ExtractionCached`) |
| `RequestPrepared` | run_id, step_index, method, url, headers (masked), body_size |
| `ResponseReceived` | run_id, step_index, status, headers, body_size, elapsed_ms |
| `ExtractionApplied` | run_id, step_index, resource, variables (names only — values masked if from auth) |
| `StepFailed` | run_id, step_index, op_id, error_code, error_class, attempt |
| `StepCancelled` | run_id, step_index, op_id |
| `SessionRefreshed` | run_id, actor, trigger (`expiry` \| `401`) |
| `RunEnded` | run_id, outcome (`Succeeded` \| `Failed` \| `Cancelled`), elapsed_ms |

All events carry the `run_id` and a monotonic clock timestamp for deterministic test ordering.

---

## 11. Open Questions (Product Decisions Required)

These are intentionally not invented. Each blocks at least one acceptance criterion above.

- **OQ-1 (Concurrency default)**: Should intra-chain parallelism be off by default, on by default, or schema-declared? Affects AC-3.9.x.
- **OQ-2 (Force re-run scope)**: Default scope on right-click "Force Re-run"? Target only, target + direct deps, or full chain? Affects AC-3.11.1.
- **OQ-3 (Dry run + pre_request)**: Should dry run execute `pre_request` hooks? They can mutate the rendered request and skipping them produces a less faithful preview. Affects AC-3.7.3.
- **OQ-4 (Force re-run UI surface)**: Is force re-run a per-op toggle, a chain modifier (Cmd+Shift+Enter is "Send Cleanly"; what is force re-run's shortcut?), or both?
- **OQ-5 (Refresh fallback)**: If `session.refresh` is undefined and a 401 occurs, do we (a) attempt full re-login automatically or (b) fail with `E_SESSION_REFRESH_FAILED` immediately and require the user to declare a refresh policy? Currently spec'd as (a); confirm.
- **OQ-6 (Extraction persistence)**: Are extractions per-run (cleared at run end), per-project-session (cleared at app close), or persistent across app restarts? Affects AC-3.10.3.
- **OQ-7 (Retry defaults)**: Project-level default retry policy values? Suggested baseline aligns with global rules: max=3, base=500 ms, max=30 s with full jitter, but the engine is local and the SUT is often the user's localhost — these values may be aggressive. Affects AC-3.5.2.
- **OQ-8 (Hook re-run on retry)**: When a step retries, should `pre_request` re-run on each attempt or only once? Re-running can produce signed payloads correctly; re-running is also non-idempotent for hooks that consume randomness. Affects AC-3.12.3.
- **OQ-9 (Cancellation policy on partial fanout)**: With parallelism on, when one parallel sibling fails, do we (a) cancel its in-flight siblings, or (b) let them complete and then halt? Affects AC-3.9.x and 3.6.1.
- **OQ-10 (Schema reload cache policy)**: On hot-reload, retain unchanged actors' sessions and discard extractions, or discard everything? Affects AC-3.13.1.
- **OQ-11 (Hook-injected references count as deps?)**: If a `pre_request` hook reads `ctx.variables.X.y`, does the engine treat that as an implicit dependency? Today it cannot statically detect this. Should hooks declare deps via a frontmatter `uses: [order.create]` block?
- **OQ-12 (Runtime cycle detection after hook mutation)**: Should the engine re-validate the dependency graph after `pre_request` runs, or accept hook-introduced cycles as user error?

---

## 12. Cross-Reference: PRD → This Document

| PRD Reference | Detailed In |
|---|---|
| FR-2.1 | §3.1, §3.2, §4.1, §4.4 |
| FR-2.2 | §3.2 |
| FR-2.3 | §3.3, §4.2 |
| FR-2.4 | §3.4, §4.3 |
| FR-2.5 | §3.5 |
| FR-2.6 | §3.6, §5 |
| FR-2.8 | §3.7 |
| §5.7 (Variable Resolution) | §3.10 |
| §5.8 (Algorithm) | §3.1, §3.2, §4 |
| §8.3 (`ExecutionEngine.run`) | §3.2, §4.1, §4.4, §10 |
| §8.4 (Failure modes table) | §5 (extended) |
| §9.3 (Esc shortcut) | §3.8, §6.3 |

---

**End of Engine Requirement v1**

> Resolve OQ-1 through OQ-12 with product before engineering kickoff. Once resolved, this document is the contract between engine implementation and integration tests.
