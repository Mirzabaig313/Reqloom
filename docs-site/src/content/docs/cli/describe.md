---
title: reqloom describe
description: "One operation in full: actor, method, path, each value it consumes and where it comes from, what it extracts, its assertions, and which operations depend on it."
---

```bash
reqloom describe <resource.operation> [--project <path>] [--env <name>] [--format text|json]
```

Prints templates and names, never values: no `{{…}}` is resolved, no secret is
read, nothing is sent.

```ansi
refund.approve POST /api/v1/admin/refunds/{{refund.refund_id}}/approve
  actor: admin
  defined at: resources/refunds.yaml:23
  consumes:
    {{refund.refund_id}}  extracted by refund.request  [path]
  extracts: nothing
  expect_status: 200
  assertions: none
  depends on: refund.request
  needed by: none
```

`consumes` lists each `{{…}}` the operation's path, headers, query, body, inline
auth, assertions and poll refer to, the field it is used in, and its source:

| `source` | Where the value comes from |
| --- | --- |
| `extraction` | Extracted by an earlier operation; `producers` names it |
| `actor_session` | The session of the actor named in `from` |
| `environment` | The environment value `from`; `via_secrets` names the secret it holds when it is `!secret NAME` (taken from `--env`, else the default) |
| `secret` | `REQLOOM_SECRET_<NAME>`, or the keychain |
| `process_environment` | `{{$.env.NAME}}`, listed in `allowed_env` |
| `builtin` | `{{$.uuid}}`, `{{$.now}}` and the other built-ins |

Values injected by the actor (its `inject` headers) are not listed here; see the
actor file. `depends on` and `needed by` are direct neighbours only; use
[`reqloom plan`](/cli/plan/) for the whole chain.

## JSON output

```json
{
  "schema_version": 1,
  "contract_version": "1.2",
  "command": "describe",
  "ok": true,
  "operation": {
    "id": "order.pay",
    "actor": "customer",
    "method": "POST",
    "path": "/api/v1/orders/{{order.order_id}}/pay",
    "file": "resources/orders.yaml",
    "line": 19,
    "consumes": [{"token": "order.order_id", "source": "extraction", "from": "order", "producers": ["order.create"], "via_secrets": [], "used_in": ["path"]}],
    "extracts": [{"variable": "order.payment_id", "path": "$.data.payment.id"}],
    "expect_status": [200],
    "assertions": [],
    "depends_on": ["order.create"],
    "dependents": ["order.ship", "refund.request", "review.create"]
  }
}
```

`assertions` items are `{ "name", "expr" }` with `name` `null` when unnamed.

## Exit codes

`0` described; `1` the project does not validate; `2` invalid invocation,
unknown operation or environment; `3` the output could not be written.
