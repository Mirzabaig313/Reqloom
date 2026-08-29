---
title: File structure
description: "How a Reqloom project is laid out, how imports and globs actually resolve, and the two accepted shapes for actor, resource, and environment files."
---

A project is a directory containing `reqloom.yaml`. Everything else is optional
and glued on with `imports:`.

## Single file

Fine for a handful of operations, and what the importer's output starts as:

```yaml title="reqloom.yaml"
version: 1
name: MyAPI
default_environment: local

environment:
  baseUrl: http://localhost:3000
  api_token: !secret API_TOKEN

actors:
  user:
    auth:
      strategy: bearer
      token: "{{env.api_token}}"

resources:
  order:
    operations:
      create:
        method: POST
        path: /orders
        actor: user
        expect_status: 201
        extract:
          order_id: $.data.id
```

## Multi-file

Past about five resources, split it. This is the layout the sample project and
the importer both use:

```
my-api/
├── reqloom.yaml              version, name, imports
├── environments/
│   ├── local.yaml
│   └── staging.yaml
├── actors/
│   ├── admin.yaml
│   ├── vendor.yaml
│   └── customer.yaml
└── resources/
    ├── products.yaml
    ├── orders.yaml
    └── refunds.yaml
```

```yaml title="reqloom.yaml"
version: 1
name: MarketplaceAPI
default_environment: local
imports:
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
```

Splitting keeps diffs readable and means two people editing different resources
don't conflict.

## How imports resolve

Two rules here are easy to get wrong.

### Directory names are load-bearing

A file's **relative path prefix** decides how it's parsed. Only three prefixes
are recognised:

| Prefix | Parsed as |
| --- | --- |
| `actors/` | an actor |
| `resources/` | a resource |
| `environments/` | an environment |

A file imported from anywhere else is read and then **silently discarded**. So
this fails quietly:

```yaml
imports:
  - shared/common-actors.yaml     # loaded, then thrown away
```

Put it in `actors/`, or don't import it.

### Globs are narrow

Only a trailing `*.yaml` or `*.yml` expands. Anything else is treated as a
literal filename:

```yaml
imports:
  - resources/*.yaml              # expands, sorted alphabetically
  - resources/*.yml               # expands
  - resources/orders.yaml         # a single literal file
  - resources/**/*.yaml           # NOT recursive — treated as a literal path
  - resources/order?.yaml         # NOT a pattern — literal
```

There is no `**`, no `?`, no character classes. Nested subdirectories need an
explicit line each:

```yaml
imports:
  - resources/*.yaml
  - resources/admin/*.yaml
```

`imports:` also accepts a map, whose values are collapsed into one list — useful
only as documentation:

```yaml
imports:
  environments: [environments/*.yaml]
  actors: [actors/*.yaml]
  resources: [resources/*.yaml]
```

## Two accepted file shapes

Actor, resource, and environment files each accept two forms. The sample project
uses the flat form throughout.

### Actors and resources

```yaml title="resources/orders.yaml — flat (name: at top level)"
name: order
description: Customer orders
operations:
  create:
    method: POST
    path: /api/v1/orders
```

```yaml title="resources/orders.yaml — wrapped (single top-level key)"
order:
  description: Customer orders
  operations:
    create:
      method: POST
      path: /api/v1/orders
```

Both give a resource with id `order`. With neither form, the **filename stem** is
used — so `orders.yaml` would give you `orders`.

:::caution[The id is `name:`, not the filename]
`resources/orders.yaml` declaring `name: order` produces `order.create`, and
references are `{{order.order_id}}`. This trips almost everyone once. `reqloom lint`
prints the counts it parsed — the quickest way to see what the ids actually are.
:::

### Environments

```yaml title="environments/local.yaml — wrapped"
name: local
variables:
  baseUrl: http://localhost:3000
  admin_password: !secret ADMIN_PASSWORD
transport:
  connect_timeout: 2s
```

```yaml title="environments/local.yaml — flat"
baseUrl: http://localhost:3000
admin_password: !secret ADMIN_PASSWORD
```

In the flat form, `name` and `transport` are treated as structure, not variables.
Without `name:`, the filename stem is the environment name — so `staging.yaml`
becomes `staging`, which is what `--env staging` selects.

## Environments are files, not a root block

There is **no `environments:` (plural) root key.** The singular root
`environment:` map defines exactly one environment — the one named by
`default_environment`:

```yaml title="reqloom.yaml"
default_environment: local
environment:                  # becomes the `local` environment
  baseUrl: http://localhost:3000
```

Additional environments only exist as files under `environments/`. So a
multi-environment project always uses imports:

```yaml
imports:
  - environments/*.yaml
```

```bash
reqloom run order.create --env staging
```

A `--env` name with no matching environment doesn't error — the run proceeds with
zero variables. See [pitfalls](/schema/pitfalls/#a-misspelled---env-runs-with-no-variables).

## Root-level keys

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `version` | integer | — | Required. Must be 1–3 |
| `name` | string | `Unnamed Project` | Shown in output and the desktop |
| `default_environment` | string | `local` | Used when `--env` is absent |
| `imports` | sequence or map | — | Glob patterns |
| `environment` | map | — | Variables for the default environment |
| `actors` | map | — | Inline actors |
| `resources` | map | — | Inline resources |
| `auth` | map | — | Project default for `auth: { type: inherit }` |
| `transport` | map | — | Applies to the **default environment only** |
| `latency_slo` | `{ p95_ms }` | — | Surfaced on the desktop latency chart |

Inline `actors:` / `resources:` and imported files coexist — imports are
processed first, so an inline definition with the same id wins.

## Limits

| Limit | Value |
| --- | --- |
| Schema file size | 8 MiB per file |
| YAML nesting depth in a body | 64 |
| Hook script size | 1 MiB |
| Upload file size | 50 MiB |

## Where to put hooks and fixtures

Hook scripts must be **relative and inside the project root**:

```
my-api/
├── reqloom.yaml
├── hooks/
│   └── sign-request.js
└── fixtures/
    └── avatar.png
```

```yaml
    pre_request: ./hooks/sign-request.js
    body_form:
      avatar: "@./fixtures/avatar.png"
```

An absolute or escaping hook path is rejected at load with `E_SCHEMA_INVALID`.
Upload paths are *not* sandboxed — see
[uploads](/schema/advanced-operations/#file-uploads).

## Version control

Commit everything except secrets, which never touch the filesystem anyway:

```gitignore
# nothing schema-related to ignore — secrets live in the OS keychain
```

Keep environment files committed with non-sensitive values and `!secret` tags for
the rest. That way a new contributor clones, adds their keychain entries, and
runs.

## Next

- [Authoring guide](/schema/authoring/) — build a project up from scratch
- [Secrets, TLS & timeouts](/schema/secrets-and-transport/) — the `!secret` tag
- [Common pitfalls](/schema/pitfalls/) — silent failures in this area
