---
title: Resources & operations
description: "Resources group operations and own the values they extract. Why that ownership is what makes dependency chains resolve."
---

A **resource** is a thing your API manages — an order, a product, a refund. An
**operation** is something you can do to it. Together they give every request a
stable id:

```yaml title="resources/orders.yaml"
name: order                       # resource id
operations:
  create:                         # → order.create
    method: POST
    path: /api/v1/orders
    actor: customer
    expect_status: 201
    extract:
      order_id: $.data.id
  pay:                            # → order.pay
    method: POST
    path: /api/v1/orders/{{order.order_id}}/pay
    actor: customer
    depends_on: [order.create]
    expect_status: 200
    extract:
      payment_id: $.data.payment.id
```

That id — `order.create` — is what you run, what appears in output, and what
`depends_on` points at.

:::caution[The id comes from `name:`, not the filename]
`resources/orders.yaml` declaring `name: order` gives you `order.create`, not
`orders.create`. Get this wrong and you'll see `E_REF_UNDEFINED` for an operation
you're sure exists.
:::

## The resource owns the values

This is the idea that makes everything else work: **extractions belong to the
resource, not to the operation that produced them.**

`order.create` extracts `order_id`. `order.pay` extracts `payment_id`. Both are
now available as `{{order.order_id}}` and `{{order.payment_id}}` — the resource
accumulates them:

```yaml
  refund:
    path: /api/v1/orders/{{order.order_id}}/refunds
    body:
      payment: "{{order.payment_id}}"     # from a different operation
```

There is no `{{order.create.order_id}}` form. If you find yourself wanting one,
the two operations probably belong to different resources.

## References create dependencies

When `order.pay` writes `{{order.order_id}}` in its path, it has declared a
dependency: something must produce `order_id` first. The engine finds
`order.create`, which extracts it, and runs that first.

So most of your chain is implicit. `depends_on` is only for prerequisites that
hand you **no value**:

```yaml
  add_item:
    path: /api/v1/cart/items
    actor: customer
    depends_on: [product.publish]     # publishing returns nothing we need,
    body:                             # but the product must be public first
      product_id: "{{product.product_id}}"
```

`product.publish` returns nothing this operation reads — but an unpublished
product can't be added to a cart. That's what `depends_on` is for. See
[dependency resolution](/concepts/dependencies/).

## Choosing resource boundaries

Follow your API's nouns, not its URLs. If the endpoints are
`/orders`, `/orders/{id}/pay`, `/orders/{id}/ship`, that's one resource with three
operations — they all concern an order and share its ids.

Split when the lifecycle genuinely differs. In the sample project, a refund gets
its own resource even though its paths live under `/orders/{id}/refunds`:

```yaml title="resources/refunds.yaml"
name: refund
operations:
  request:
    path: /api/v1/orders/{{order.order_id}}/refunds
    actor: customer
    depends_on: [order.pay]
    extract:
      refund_id: $.data.id
  approve:
    path: /api/v1/admin/refunds/{{refund.refund_id}}/approve
    actor: admin
    depends_on: [refund.request]
```

A refund has its own id and its own state, so it's its own resource — and
`{{refund.refund_id}}` reads better than overloading `order`.

## Operation naming

Operation names become part of the id, so keep them verbs and keep them short.
`order.create`, not `order.create_new_order`. The sample uses `create`, `get`,
`pay`, `ship`, `cancel`, `list_for_customer` — the last one earns its length by
distinguishing it from `list_for_vendor`.

## What an operation can declare

The common keys:

```yaml
  create:
    method: POST                    # default GET
    path: /api/v1/orders            # appended to baseUrl
    actor: customer
    headers:
      X-Request-Source: reqloom
    query_params:
      expand: "items"
    body:
      total: 100
    expect_status: 201              # or [201, 200]
    depends_on: [cart.add_item]
    extract:
      order_id: $.data.id
    assert:
      - "$.data.status == 'pending'"
```

Note `query_params`, not `query` or `params`. There is no `url:` key — the URL is
`baseUrl` plus `path`. Full list in the [cheat sheet](/schema/cheatsheet/#operation-keys).

## Extractions

`extract` maps a variable name to where the value lives. A bare path is a
JSONPath unless the prefix says otherwise:

```yaml
    extract:
      order_id: $.data.id                 # JSON body
      location: $.headers.Location        # response header
      session: $.cookies.session_id       # cookie
      code: $.status_code                 # status code
      all_ids: $.data[*].id               # a list → many instances
```

`[*]` is special: each match becomes a separate **instance** of the resource,
which is what [`for_each`](/schema/advanced-operations/#fanning-out-with-for_each)
iterates over. Otherwise, references resolve to the most recent instance.

XPath and regex need the explicit form:

```yaml
    extract:
      title:
        path: "//book/title"
        source: xpath
```

## Instances

Every time an operation extracts values, it creates an instance of its resource.
Run `order.create` three times and there are three orders, so `{{order.order_id}}`
means "the most recent" and `{{order[2].order_id}}` means the second — counting
from 1, not 0.

Most schemas never need indexing. It matters when you deliberately create several
of something and then act on a specific one.

## Next

- [Dependency resolution](/concepts/dependencies/) — how the chain gets built
- [Variables & references](/concepts/variables/) — passing values between operations
- [Advanced operations](/schema/advanced-operations/) — polling, fan-out, assertions
