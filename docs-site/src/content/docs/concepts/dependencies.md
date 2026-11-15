---
title: Dependency resolution
description: "How Reqloom turns one operation into an ordered chain: implicit edges from references, explicit depends_on, topological sort, and what gets skipped."
---

You ask for one operation. The engine works out everything that has to happen
first and runs it in order. This is the whole point of the tool, and it comes
from two kinds of edge in a graph.

## Two kinds of edge

### Implicit — a reference

Writing `{{order.order_id}}` says "I need a value from the `order` resource".
Whichever operation extracts it becomes a prerequisite:

```yaml
  pay:
    path: /api/v1/orders/{{order.order_id}}/pay    # ← implies order.create
```

Most of your chain comes from this. You don't declare it; you just use the value.
A reference counts wherever it appears: the path, headers, query, body, form, the
inline `auth:` block, the poll path and conditions, and `assert:`. So an
assertion such as `"$.owner == '{{user.user_id}}'"` pulls `user.create` into the
chain too.

### When several operations extract the same value

A resource often has more than one operation that returns its id: `create`,
`get` and `update` may all extract `order_id`. The engine picks the producer of
`{{order.order_id}}` like this:

1. Take every other operation in the resource that extracts `order_id`.
2. Drop any that also **use** `{{order.order_id}}` themselves — they need the
   value already, so they can't be where it first comes from.
3. One left: that's the producer. Several left: the ones the operation lists in
   `depends_on` are used. None of them listed: the load fails.

So `get` and `update` on `/orders/{{order.order_id}}` drop out at step 2 and
`create` is the producer. When two operations that don't use the id both extract
it — say `create` and an `import` endpoint — the reference is ambiguous:

```ansi
LINT FAIL [E_REF_UNDEFINED]: Operation 'order.delete': ambiguous producer for
{{order.order_id}}: candidates order.create, order.import — add depends_on
```

Add the one you mean to `depends_on` and the ambiguity is gone:

```yaml
  delete:
    method: DELETE
    path: /orders/{{order.order_id}}
    depends_on: [order.create]
```

If every operation that extracts the value also uses it, there is no producer at
all, and the load fails with `no producer for {{order.order_id}}`. Operations that
extract the same variable are not a cycle on their own.

### Explicit — `depends_on`

For prerequisites that produce nothing you read, but must still happen:

```yaml
  add_item:
    path: /api/v1/cart/items
    depends_on: [product.publish]      # returns nothing we use
    body:
      product_id: "{{product.product_id}}"
```

Publishing a product returns no value this operation needs — but an unpublished
product can't be added to a cart. State changes like publish, activate, approve,
and verify are the usual reason to reach for `depends_on`.

`depends_on` takes a sequence, or a single operation id as one item. A map fails
the load.

## Worked example

The sample project's `refund.approve` declares only one dependency:

```yaml title="resources/refunds.yaml"
  approve:
    method: POST
    path: /api/v1/admin/refunds/{{refund.refund_id}}/approve
    actor: admin
    depends_on: [refund.request]
```

Follow the edges outward — `refund.request` needs `order.pay`, which needs
`order.create`, which needs `cart.add_item`, which needs `product.publish`, which
needs `product.create`:

```bash
reqloom run refund.approve --project samples/marketplace
```

```ansi
Running: refund.approve (chain of 7 steps, env=local)
  [1] Running: product.create (attempt 1)
  [2] Running: product.publish (attempt 1)
  [3] Running: cart.add_item (attempt 1)
  [4] Running: order.create (attempt 1)
  [5] Running: order.pay (attempt 1)
  [6] Running: refund.request (attempt 1)
  [7] Running: refund.approve (attempt 1)
```

Seven steps from one declared dependency. Nobody wrote that list — it fell out of
the graph.

## How the order is chosen

Kahn's topological sort, with a **lexicographic tie-break** when several
operations are ready at once. That second part matters: the order is
deterministic, so the same schema produces the same chain every run, and a
diff in CI output means something actually changed.

Independent branches are therefore ordered predictably rather than arbitrarily —
useful when you're comparing runs, less useful as a guarantee to rely on. If two
operations must happen in a specific order, say so with `depends_on` instead of
depending on the sort.

## Cycles are a load-time error

A cycle can't be ordered, so it's rejected before anything runs:

```ansi
LINT FAIL [E_CYCLE]: Circular dependency detected: order.one → order.two
```

`reqloom lint` catches this, which is why it's worth running in a pre-commit
hook. The message prints the path so you can see which edge to remove.

## Nothing runs twice

Within a run, an operation that's already satisfied is skipped rather than
repeated. If two branches both need `product.create`, it runs once:

```ansi
  OK     product.create (113ms) err=—
  SK     product.create (0ms) err=—
```

`SK` means skipped — the value was already available.

To force an operation to execute every time it appears, mark it:

```yaml
  create:
    force: true
```

Useful when each run must create fresh data rather than reuse what's already
there.

## When a step fails, the rest is blocked

Downstream operations aren't attempted — they're marked `BLOCK`:

```ansi
  FAIL   product.create (0ms) err=E_SESSION_REFRESH_FAILED
  BLOCK  product.publish (0ms) err=—
  BLOCK  cart.add_item (0ms) err=—
  BLOCK  order.create (0ms) err=—
```

`BLOCK` steps have no error code because they never ran. Fix the first failure —
the other six are consequences, not separate problems.

## Actor logins are not chain steps

Three actors appear in that seven-step chain, and their logins don't show up in
the numbering. Authentication happens inside the step that needs it, cached per
actor for the session lifetime. A seven-step chain across three actors performs
three logins, invisibly.

## What the resolver validates at load

Before any request, and therefore during `reqloom lint`:

1. Every referenced **scope** exists — an actor, resource, environment variable,
   or secret by that name — including references inside function calls such as
   `{{$.base64.encode(order.order_id)}}`
2. Every operation's and poll's `actor`, and every `for_each` resource, is
   defined
3. Every `depends_on` target is a real operation
4. Each referenced value has one clear producer
5. Assertions, poll conditions, and regex patterns parse; `$.faker` names exist;
   `$.env` names are listed in `allowed_env`
6. The graph is acyclic
7. Every operation's chain resolves to an executable order

:::caution[Scopes are checked, fields are not]
`{{order.missing_var}}` passes validation as long as a resource named `order`
exists. Nothing verifies that an upstream operation actually extracts
`missing_var` — that's a run-time `E_VAR_UNRESOLVED`. See
[pitfalls](/schema/pitfalls/#lint-validates-the-scope-not-the-field).
:::

:::tip[Seeing and editing the graph]
The [desktop app](/desktop/overview/) shows the resolved chain as a strip above the
editor and as a layered graph you can click through. Its **Chain** tab edits every
step's dependencies and extractions in one table, then saves them together — so
rewiring a seven-step chain is one screen rather than seven files. See
[editing the whole chain](/desktop/editing/#editing-the-whole-chain-at-once).
:::

## Keeping chains short

A seven-step chain is fine. A thirty-step chain is a smell, and usually means
prerequisite data is being created that could be assumed. Two ways out:

- **Seed the data.** Put long-lived fixtures in the environment
  (`{{env.test_product_id}}`) instead of creating them per run.
- **Test the endpoint, not the story.** If you only care that
  `refund.approve` rejects a bad payload, an operation that references a seeded
  refund id needs no chain at all.

Reqloom builds the chain you declared. Declaring less is the way to a shorter one.

## Next

- [Variables & references](/concepts/variables/) — the values that flow along the edges
- [Sessions & caching](/concepts/sessions/) — why logins don't repeat
- [`reqloom run`](/cli/run/) — reading the chain output
