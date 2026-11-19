---
title: reqloom list
description: "Every actor, resource and operation in a project, with method, path, actor and where each operation is defined. Read-only."
---

Prints what a project contains. Nothing is sent and nothing is written.

```bash
reqloom list [--project <path>] [--format text|json]
```

```ansi
MarketplaceAPI: 3 actors, 5 resources, 27 operations; environments: local (default local)
actors: admin (simple), customer (chain), vendor (simple)
cart
  cart.add_item                POST    /api/v1/cart/items  as customer
  cart.remove_item             DELETE  /api/v1/cart/items/{{cart.cart_item_id}}  as customer
order
  order.create                 POST    /api/v1/orders  as customer
  order.pay                    POST    /api/v1/orders/{{order.order_id}}/pay  as customer
```

Paths are printed as written, with `{{…}}` left unresolved.

## JSON output

```json
{
  "schema_version": 1,
  "contract_version": "1.2",
  "command": "list",
  "ok": true,
  "project": "MarketplaceAPI",
  "default_environment": "local",
  "environments": ["local"],
  "actors": [{"id": "admin", "strategy": "simple"}],
  "resources": [
    {"id": "order", "operations": [{"id": "order.pay", "method": "POST", "path": "/api/v1/orders/{{order.order_id}}/pay", "actor": "customer", "file": "resources/orders.yaml", "line": 19}]}
  ]
}
```

| Field | Meaning |
| --- | --- |
| `actors[].strategy` | The actor's `auth.strategy` |
| `operations[].actor` | The actor it runs as, or `null` |
| `operations[].file`, `line` | Where the operation's key is, relative to the project folder |

When the command cannot answer, the document has `ok: false` and an `error`
object; see [errors](/cli/overview/#json-errors).

## Exit codes

`0` listed; `1` the project does not validate; `2` invalid invocation or no
`reqloom.yaml`; `3` the output could not be written.
