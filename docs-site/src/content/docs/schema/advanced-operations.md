---
title: Advanced operations
description: "Polling async endpoints, fanning out over a list, asserting on responses, running JS hooks, and uploading files."
---

Five capabilities beyond a plain request. Each is a block on an operation, and
none requires a plugin.

## Polling

For endpoints that return `202 Accepted` and finish the work later. The engine
sends the initial request, then polls until a predicate matches.

```yaml
  generate:
    method: POST
    path: /api/v1/reports
    actor: admin
    expect_status: [202, 200]
    poll_until:
      method: GET
      path: /api/v1/reports/{{report.report_id}}
      success_when: "$.status == 'ready'"
      fail_when: "$.status == 'failed'"
      interval: 2s
      timeout: 5m
      max_attempts: 60
    extract:
      download_url: $.data.url
```

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `method` | HTTP method | `GET` | |
| `path` | template | — | May reference `{{response.headers.Location}}` |
| `actor` | actor id | the parent operation's actor | |
| `success_when` | predicate | — | Required in practice |
| `fail_when` | predicate | — | Wins over `success_when` on the same response |
| `interval` | duration | `2s` | Fixed delay between attempts |
| `backoff` | `{ base, max }` | max `30s` | When `base` is set, `interval` is ignored |
| `timeout` | duration | `60s` | Wall-clock cap |
| `max_attempts` | integer | `30` | Whichever cap hits first ends the loop |

:::caution[`expect_status` must include the async status]
Polling only engages if the *initial* response status matches `expect_status`.
An endpoint that returns `202` and then `200` needs the list form —
`expect_status: [202, 200]`. With a bare `expect_status: 200`, the initial `202`
is treated as a mismatch and the step fails before polling starts.
:::

Extractions run against the **final** poll response, not the initial one — so
`download_url` above comes from the completed report, which is what you want.

Durations take `ms`, `s`, `m`, `h`, or `d` (`500ms`, `2s`, `5m`). A bare number is
whole seconds, so `interval: 2` waits two seconds.

`success_when` and `fail_when` use the [predicate grammar](#predicate-grammar)
and are checked when the project loads. `{{…}}` in them is substituted
[as values](#templates-in-predicates), once, before the first poll attempt; the
poll `path` is resolved like any other template.

Use exponential backoff instead of a fixed interval when the wait is long and
unpredictable:

```yaml
    poll_until:
      path: /api/v1/jobs/{{job.job_id}}
      success_when: "$.state == 'complete'"
      backoff:
        base: 500ms
        max: 30s
      timeout: 10m
```

Timing out gives you `E_POLL_TIMEOUT`; exhausting attempts gives
`E_POLL_MAX_ATTEMPTS_EXCEEDED`; a `fail_when` match gives
`E_POLL_FAIL_PREDICATE`.

## Fanning out with `for_each`

Run one operation once per item in a list. First, extract a list with a `[*]`
path:

```yaml title="resources/products.yaml"
name: product
operations:
  list:
    method: GET
    path: /api/v1/products
    actor: customer
    extract:
      product_id: $.data[*].id
```

The `[*]` makes each match a separate **instance** of the resource. Then fan out
over it:

```yaml title="resources/reviews.yaml"
name: review
operations:
  create:
    method: POST
    path: /api/v1/products/{{product.product_id}}/reviews
    actor: customer
    depends_on: [product.list]
    for_each:
      over: product
      continue_on_error: true
    body:
      rating: 5
      comment: "Bulk review"
```

During iteration *k*, `{{product.product_id}}` resolves to the *k*-th instance
rather than the most recent one.

| Key | Type | Default |
| --- | --- | --- |
| `over` | resource id | — required |
| `continue_on_error` | bool | `false` |

With `continue_on_error: false` the fan-out stops at the first failed iteration.
With `true` every item is attempted and the parent step fails if any did.

Iterations appear indented under their parent in the summary:

```ansi
  OK     review.create (412ms) err=—
    iter #1  OK     review.create (98ms)
    iter #2  OK     review.create (104ms)
    iter #3  FAIL   review.create (89ms)
```

There is no `limit`, no alias, and no parallelism — iterations run in order.
`for_each` without `over` fails the load.

`for_each` iterates the items the producing operation returned the last time it
ran. Running `product.list` again replaces the items it produced before rather
than adding to them, so a fan-out never sees a stale list.

## Assertions

`assert:` checks the response beyond its status code. A failing assertion fails
the step with `E_ASSERTION_FAILED`.

```yaml
  create:
    method: POST
    path: /api/v1/orders
    actor: customer
    expect_status: 201
    assert:
      - "$.data.status == 'pending'"
      - expr: "$.data.total > 0"
        name: "total is positive"
      - "$.data.currency in ['USD', 'EUR']"
      - "$.data.id matches '^ord_[a-z0-9]+$'"
```

Items are either a bare predicate or a `{ expr, name }` map. Named assertions
read better in reports:

```ansi
  OK     order.create (164ms) err=—
         ✓ assert: total is positive
```

`assert:` must be a list. Written as a map it fails the load with
`'assert' must be a list, got a map`.

### Predicate grammar

The same grammar powers `assert`, `poll_until.success_when`, and
`poll_until.fail_when`:

| Form | Example |
| --- | --- |
| Comparison | `$.data.total == 100`, `$.count >= 1` |
| Inequality | `$.status != 'draft'` |
| Membership | `$.role in ['admin', 'owner']` |
| Regex | `$.id matches '^ord_'` |
| Boolean | `$.a == 1 && $.b == 2`, `$.x == 1 \|\| $.y == 2` |
| Grouping | `($.a == 1 \|\| $.b == 2) && $.c == 3` |
| Status shortcut | `$.status_code == 201` |
| Truthiness | `$.data.verified` |

The details:

- **Operators.** `==`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `matches`, and the
  logical `&&` / `||`, also spelled `and` / `or`. There is no negation operator —
  write `$.status != 'draft'` or `$.flag == false`.
- **Precedence.** `&&` binds tighter than `||`, so `a || b && c` means
  `a || (b && c)`. Use parentheses to group.
- **Values.** Strings in single or double quotes, numbers, `true`, `false`,
  `null`, and lists in `[ ]`.
- **Paths.** `$.field`, `$.a.b`, `$.items[0]`, and `$['odd key']`. Wildcards,
  filters, and `$..` are not part of predicate paths.
- **`matches`** searches the string for the pattern — `'^ord_'` needs the `^` to
  anchor it. Patterns follow the [regex limits](#regular-expressions).
- **A bare path** is a truthiness check: false for `null`, `false`, `0`, an empty
  string, or a missing field.
- **Nesting** of parentheses and lists is limited to 64 levels.

Every assertion and poll condition is parsed when the project loads, so a
malformed one fails `reqloom lint` with `E_SCHEMA_INVALID` instead of failing
after a request has gone out.

### Templates in predicates

`{{…}}` in an assertion or poll condition is substituted, as a value that can
never change the structure of the predicate:

```yaml
    assert:
      - "$.data.id == '{{order.order_id}}'"
      - "$.data.total == {{env.expected_total}}"
      - "$.data.owner == {{user.user_id}}"
```

- Each placeholder is resolved like any other template. In a poll condition that
  happens once, before the first attempt.
- **Inside a quoted string literal** the value is escaped for that literal, so a
  quote or backslash in it stays part of the string.
- **Outside a literal** it becomes exactly one operand: bare when the value is
  exactly a number (`42`, `-1.5`), `true`, `false` or `null`, otherwise a quoted,
  escaped string. A value such as `1 || true` is compared as the text
  `"1 || true"`, never evaluated.
- A reference that does not resolve fails the step with `E_VAR_UNRESOLVED`,
  naming the variable.

References in assertions and poll conditions count like references anywhere
else: they are checked when the project loads (an unknown scope is
`E_REF_UNDEFINED`), and a resource they name pulls its producer into the chain.
The assertion's text in reports and events is the expression as written, with
the braces.

### JSONPath in extractions

`extract:` paths support a fixed subset of JSONPath:

| Form | Example |
| --- | --- |
| Field access | `$.data.id`, `$.data.user.email` |
| Array index | `$.items[0].id`, `$.matrix[0][1]` |
| Filter — first match | `$.items[?(@.status == 'open')].id` |
| Every element | `$.data[*].id` — one resource instance per match |

A filter uses the predicate grammar with `@` for the current element, and may
contain brackets of its own: `$.items[?(@.tags[0] == 'x')].id`.

Anything else is an error rather than an empty result. `$..name` (recursive
descent), quoted keys, and negative indices fail the step with
`E_EXTRACTION_FAILED`, as does a path that matches nothing. Response bodies that
nest deeper than 256 levels are rejected with `E_RESPONSE_PARSE`.

A path that finds JSON `null` does not fail the step. A later reference to that
field is unresolved, and the failure says why — for example "The last
user.create response returned null for 'user_id'". It does not fall back to a
value from an earlier run.

XPath extraction (`source: xpath`) is not implemented: it never produces a value,
so anything that references the variable fails with `E_VAR_UNRESOLVED`.

### Regular expressions

Regex extraction (`source: regex`) and the `matches` operator share one engine:
C++ `std::regex` with the ECMAScript grammar. Lookahead and backreferences work;
lookbehind does not. Extraction returns capture group 1, or the whole match when
there is no group.

| Limit | Value |
| --- | --- |
| Pattern length | 1024 bytes |
| Text searched | 1 MiB |
| Nested repeats | refused |

A pattern with a repeated group that itself ends in an unbounded repeat —
`(a+)+`, `(\w*)*` — is refused, because it can take exponential time. Bound the
inner repeat (`(a{1,64})+`) or drop the outer one. Refused and invalid patterns
fail `reqloom lint` with `E_SCHEMA_INVALID`; a pattern built at run time that is
refused makes `matches` false and fails a regex extraction.

There is no time limit on matching, so an unusual slow pattern the check misses
can still be slow.

## Hooks

Two hook points run JavaScript around a request: `pre_request` and
`post_response`. Inline for one-liners:

```yaml
  create:
    method: POST
    path: /api/v1/orders
    pre_request: |
      ctx.request.headers['X-Request-Id'] = ctx.hex.encode(ctx.request.url);
```

Or a file, which is better for anything real:

```yaml
    pre_request: ./hooks/sign-request.js
    post_response: ./hooks/normalise.js
```

A value counts as a file reference when it starts with `./` or `../`, or ends in
`.js`/`.mjs` without looking like code. File hooks are sandboxed:

- Paths must be **relative** and stay **inside the project root** — `../../etc/x.js`
  and absolute paths are rejected at load time with `E_SCHEMA_INVALID`
- Maximum **1 MiB** per script

Every hook runs in a fresh sandbox with no filesystem, network, timers, or
`require`:

| Limit | Value |
| --- | --- |
| Run time | 1 second per hook, including any promises it waits on |
| Memory | 64 MiB |
| Stack | 8 MiB |

A hook may be `async` and may return a promise. Pending promise jobs run within
the same 1-second budget and the returned promise must settle: a rejected
promise fails the hook with its reason, and one that never settles fails it too.

A hook reads and changes the request through `ctx.request`, and in
`post_response` the response through `ctx.response`. It sees only what its
operation uses:

- `ctx.secret` holds the secrets named by the operation, by that operation's
  actor, by environment values the operation references, and by the hook script
  itself (`ctx.secret.NAME` or `ctx.secret["NAME"]` written literally).
- `ctx.actors` holds only the operation's own actor's session.
- `ctx.env` holds the active environment's values.

Reading any other secret or actor gives `undefined`.

A hook that leaves the response alone leaves it byte for byte: the body, and
every header, including repeated ones such as several `Set-Cookie` lines. A body
or header value that is not valid UTF-8 is exposed as a string with one
character per byte. Changing a header replaces all of that header's entries
with the one new value.

A throwing hook, a rejected promise, or running out of memory fails the step with
`E_HOOK_FAILURE`; running past 1 second fails it with `E_HOOK_TIMEOUT`. These two
hook points are the only ones — there are no project-level or actor-level hooks.

## File uploads

Uploads go through `body_form`, using the `@` prefix that curl and Postman use.
There is no `files:` key.

```yaml
  upload_avatar:
    method: POST
    path: /api/v1/users/me/avatar
    actor: customer
    body_form:
      avatar: "@./fixtures/avatar.png"
      caption: "Profile photo"
```

How a value is read depends only on how it is **written** in the YAML, never on
what a variable resolves to:

| Written as | Sent as |
| --- | --- |
| `"@./fixtures/avatar.png"` — one leading `@` | the file at that path |
| `"@@mention"` — two leading `@` | the text `@mention` (one `@` removed) |
| anything else | text, whatever it resolves to |

So a variable can't turn a text field into a file upload: `caption:
"{{env.caption}}"` is sent as text even if the variable holds `@/etc/passwd`. A
variable may appear inside a file path — `"@./fixtures/{{env.file}}"` — because
the leading `@` is written in the YAML.

Where the file is looked up:

- An **absolute** path is used as written.
- A **relative** path resolves against the project folder — the one holding
  `reqloom.yaml` — not the directory you ran Reqloom from.
- `~` is **not** expanded. Write the full path.

The encoding is chosen for you:

- **`multipart/form-data`** when any field is a file, or when you set a
  `Content-Type: multipart/form-data` header explicitly
- **`application/x-www-form-urlencoded`** otherwise

So the same `body_form` block covers both plain form posts and uploads:

```yaml
  login:
    method: POST
    path: /api/v1/auth/token
    body_form:
      grant_type: password
      username: "{{env.user}}"
      password: "{{env.pass}}"
```

Each file caps at **50 MiB**, and all the files in one request at **256 MiB**
(text fields don't count). A missing, unreadable, or oversized file — or an empty
path after `@` — fails the step with `E_UPLOAD_FILE_UNREADABLE` before anything
is sent.

:::note[Upload paths are not sandboxed]
Unlike hook scripts, `@` paths may point outside the project root — fixtures
often live elsewhere. That also means a schema you didn't write can read any
file the process can. Review `body_form` entries in imported or third-party
schemas.
:::

## Next

- [Secrets, TLS & timeouts](/schema/secrets-and-transport/) — retries and transport
- [Variables & references](/reference/variables/) — the full `{{...}}` grammar
- [Error codes](/reference/error-codes/) — every `E_*` these can produce
