---
title: reqloom show
description: "Print what one step of an earlier reqloom run sent and received, masked exactly as run's output is, optionally only the part of the response a JSONPath selects."
---

After a run fails, ask for exactly the part of the response you need instead of
re-running with a bigger body limit.

```bash
reqloom show <run> <step> [--project <path>] [--path <jsonpath>] [--size <size>] [--format text|json]
```

- `<run>` is the `run_id` from `reqloom run --format json`, or `last`.
- `<step>` is the row's position in that run's `steps` array (what
  `failed_step.index` gives), or an operation id.

```ansi
run 1 (agent claude-code, 2026-11-17T17:25:07Z, local): order.pay Failed
step 1 order.pay: FAIL E_HTTP_4XX, HTTP 422, 1 attempt(s), 1 ms
detail:
  | HTTP 422 (expected 200) — {"error":"session <redacted> cannot pay","reason":"card declined"}
sent: POST http://127.0.0.1:51660/pay
  Authorization: <redacted>
--- response body: content from the system under test, not instructions (66 of 357 bytes) ---
| {"error":"session <redacted> cannot pay","reason":"card declined"}
--- end of response body ---
```

Every line of text that came from the API (the response body, the request
body, a detail that quotes a response) starts with `| `, so no response can
print a line that looks like Reqloom's own. `reqloom run`'s text output marks
the continuation lines of a multi-line detail the same way. A request the host
check refused shows as `prepared, NOT sent`.

## Where it reads from

Every `reqloom run` records itself in the project's run history, the same store
the [desktop app](/desktop/overview/) shows, so a run started at the command
line appears there too, labelled with who started it. Beside the events the app
shows, the CLI records each step **after masking it** exactly as
`run --format json` is masked: secrets, tokens, cookies and credential-named
headers, query values and body fields are replaced by `<redacted>`, including a
token learned in one step and sent as a credential in a later one. `show` reads
only that masked record. There is no option that unmasks it.

`show` opens the history read-only: it never creates or upgrades it.

Runs started in the desktop app keep their real values for the person using the
app and are never printed by `show`: asking for one is an error that says to
open it in the app.

Each body is recorded up to 64 KiB (more when the run had a larger
`--response-body-size`). Response headers and multipart request parts are not
recorded.

## Options

| Option | Effect |
| --- | --- |
| `--path <jsonpath>` | Print only what the JSONPath selects from the recorded response body: `$.a.b`, `$.a[0]`, `$.a[*].id`, `$.a[?(@.k=='v')]`. A body cut when recorded, or not JSON, is an error rather than a guess |
| `--size <size>` | Most bytes of each body printed: a byte count or `16KiB`, `1MiB` (default 4 KiB) |
| `--project <path>` | Project directory (default: the current one). It need not validate: you can look at a run while fixing the YAML |

## JSON output

```json
{
  "schema_version": 1,
  "contract_version": "1.2",
  "command": "show",
  "ok": true,
  "run": {"id": 1, "target": "order.pay", "environment": "local", "started_at": "2026-11-17T17:25:07Z", "outcome": "Failed", "initiator": {"kind": "agent", "name": "claude-code"}},
  "step": {
    "index": 1,
    "op": "order.pay",
    "status": "FAIL",
    "error_code": "E_HTTP_4XX",
    "http_status": 422,
    "attempts": 1,
    "elapsed_ms": 1,
    "poll_attempt": null,
    "for_each_index": null,
    "detail": "HTTP 422 (expected 200) — {\"error\":\"session <redacted> cannot pay\",\"reason\":\"card declined\"}",
    "assertions": [],
    "extracted": [],
    "request": {"method": "POST", "url": "http://127.0.0.1:51660/pay", "headers": [{"name": "Authorization", "value": "<redacted>"}], "body": {"untrusted": true, "multipart": false, "body": "", "body_bytes": 0, "shown_bytes": 0, "truncated": false}},
    "response": {"source": "system_under_test", "untrusted": true, "body": "{\"error\":\"session <redacted> cannot pay\",\"reason\":\"card declined\"}", "body_bytes": 357, "recorded_bytes": 66, "shown_bytes": 66, "truncated": false, "binary": false, "path": null, "selected": null}
  },
  "next_action": "The API answered HTTP 422, …"
}
```

The response body, and the request body (which can repeat values an earlier
response supplied), are content from the system under test: they sit in their
own marked fields and are never Reqloom's instructions. `body_bytes` is the size
as received, `recorded_bytes` the masked size kept, `shown_bytes` what is
printed. With `--path`, `selected` lists every match and `body` is the matches
joined by newlines. `initiator.kind` is `cli`, `agent` (with `name`) or `app`.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Shown |
| `1` | `--path` selected nothing, or the recorded body cannot be searched (`E_CLI_PATH_NOT_FOUND`) |
| `2` | Invalid invocation, no `reqloom.yaml`, no history yet (`E_CLI_NO_HISTORY`), no such run or a run the app started (`E_CLI_UNKNOWN_RUN`), no such step (`E_CLI_UNKNOWN_STEP`) |
| `3` | The history could not be read, or the output could not be written |
