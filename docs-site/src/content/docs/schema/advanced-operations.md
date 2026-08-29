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
| `backoff` | `{ base, max }` | — | When `base` is set, `interval` is ignored |
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
`for_each` without `over` is dropped silently.

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

:::caution[`assert:` must be a sequence]
Written as a map it is **silently ignored** — no error, no warning, and the
step passes. If your assertions never seem to fire, check that each line starts
with `-`.
:::

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
| Status shortcut | `$.status_code == 201` |
| Truthiness | `$.data.verified` |

## Hooks

Two hook points run JavaScript around a request: `pre_request` and
`post_response`. Inline for one-liners:

```yaml
  create:
    method: POST
    path: /api/v1/orders
    pre_request: |
      request.headers['X-Signature'] = sign(request.body);
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

A throwing or hanging hook fails the step with `E_HOOK_FAILURE` or
`E_HOOK_TIMEOUT`. These two hook points are the only ones — there are no
project-level or actor-level hooks.

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

The encoding is chosen for you:

- **`multipart/form-data`** when any value starts with `@`, or when you set a
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

Files cap at **50 MiB**. A missing or unreadable file fails the step with
`E_UPLOAD_FILE_UNREADABLE`.

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
