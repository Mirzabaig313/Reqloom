---
title: reqloom plan
description: "The chain reqloom run would execute, without running it: steps in order, the actor of each, sessions, secret names and the hosts that would receive credentials, with whether each is approved."
---

The answer to "what must exist before I can call this endpoint?". `plan`
resolves the chain exactly as [`reqloom run`](/cli/run/) does and stops there:
**nothing is sent, no secret is read, nobody is asked, and nothing is
approved.**

```bash
reqloom plan <resource.operation> [--project <path>] [--env <name>] [--format text|json]
```

```ansi
Plan for refund.approve in environment local (7 steps; nothing is sent)
  1. product.create POST /api/v1/vendors/{{vendor.vendor_id}}/products  as vendor
  2. product.publish POST /api/v1/products/{{product.product_id}}/publish  as vendor
  3. cart.add_item POST /api/v1/cart/items  as customer
  4. order.create POST /api/v1/orders  as customer
  5. order.pay POST /api/v1/orders/{{order.order_id}}/pay  as customer
  6. refund.request POST /api/v1/orders/{{order.order_id}}/refunds  as customer
  7. refund.approve POST /api/v1/admin/refunds/{{refund.refund_id}}/approve  as admin  <- target
Sessions (the CLI keeps none between runs):
  vendor (simple): signs in; secrets: VENDOR_PASSWORD
  customer (chain): signs in
  admin (simple): signs in; secrets: ADMIN_PASSWORD
Secrets (names only):
  ADMIN_PASSWORD  REQLOOM_SECRET_ADMIN_PASSWORD not set
  VENDOR_PASSWORD  REQLOOM_SECRET_VENDOR_PASSWORD not set
Hosts receiving credentials:
  http://localhost:3000  not_approved  (the credentials of actor 'vendor'; …: product.create, …, sign-in of actor 'admin')
Next: Each host in hosts[] with status not_approved must be approved before credentials go there. Ask the person you work for to approve it themselves (`reqloom hosts approve <host>` at their terminal, or the Reqloom app); do not approve hosts on their behalf.
```

## What it reports

- **Steps** in execution order, each with its actor, the secrets the operation
  itself refers to, whether its request carries a credential, and its host.
- **Sessions** each actor needs. Every `reqloom run` starts with no session, so
  each actor signs in; `cached` is always `false` from the CLI.
- **Secrets**, by name only, following environment values such as
  `admin_password: !secret ADMIN_PASSWORD`. For each, whether its
  `REQLOOM_SECRET_<NAME>` variable is set. The keychain is not read, so a secret
  only in the keychain shows as `env_set: false`.
- **Hosts** that would receive a credential, judged by the same
  [host approval](/cli/run/#approving-hosts) rules a run applies, without asking:

| `status` | Means |
| --- | --- |
| `approved` | Approved for this project and stored |
| `approved_for_run` | Approved by `REQLOOM_APPROVED_HOSTS` for a run in this shell |
| `not_approved` | A run asks at a terminal, and otherwise fails with `E_HOST_NOT_APPROVED` before sending |
| `refused` | Outside an actor's `allowed_hosts` (or not the `baseUrl` host), or the approval store refuses; a run fails. `detail` says why |
| `unknown` | The host comes from a template, so it is known only when the request is built; the run checks it then |

A request's host is read from the environment's `baseUrl` (operations, polls,
sign-in and refresh steps are all sent to `baseUrl` + path), and from the token
URL of an OAuth 2 actor or inline OAuth 2 auth, only when it is written
literally. Whether a request carries a credential is judged from what it refers
to (an actor, a secret, `$.env`, inline auth, a value the project sends as a
credential), as the run judges it before sending.

Only a run can judge what is decided while sending: a credential typed literally
into a header, requests a hook makes or rewrites, redirect hops, and TLS or
proxy settings an actor or inline auth adds (environment `transport:` settings
are included). `ready: true` is therefore necessary for a run to send, not proof
that it will.

## JSON output

```json
{
  "schema_version": 1,
  "contract_version": "1.2",
  "command": "plan",
  "ok": true,
  "target": "refund.approve",
  "environment": "local",
  "ready": false,
  "steps": [
    {"order": 1, "op": "product.create", "method": "POST", "path": "/api/v1/vendors/{{vendor.vendor_id}}/products", "actor": "vendor", "target": false, "depends_on": [], "secrets": [], "carries_credential": true, "host": "http://localhost:3000"}
  ],
  "sessions": [{"actor": "vendor", "strategy": "simple", "cached": false, "secrets": ["VENDOR_PASSWORD"]}],
  "secrets": [{"name": "VENDOR_PASSWORD", "env_variable": "REQLOOM_SECRET_VENDOR_PASSWORD", "env_set": false}],
  "hosts": [{"host": "http://localhost:3000", "status": "not_approved", "credential": "the credentials of actor 'vendor'", "used_by": ["product.create", "sign-in of actor 'vendor'"], "detail": null}],
  "next_action": "…"
}
```

`ready` is `false` when any host is `not_approved` or `refused`. `order` is the
1-based position in the chain (a run's `steps[].index` counts rows instead, poll
attempts and `for_each` iterations included). `host` is `null` when it comes
from a template; `env_variable` is `null` when no environment variable can carry
the secret's name.

`next_action` is one line built from Reqloom's own data. It points at fields of
the document (a refused or unapproved host first, then the secrets whose
variable is not set, then the `reqloom run` command) rather than repeating host
or secret names from the project.

## Exit codes

`0` planned, also when hosts are not approved (read `ready`); `1` the project
does not validate; `2` invalid invocation, unknown operation or environment, or
a bad `REQLOOM_APPROVED_HOSTS`; `3` the output could not be written.
