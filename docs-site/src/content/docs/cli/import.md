---
title: reqloom import
description: "Convert an OpenAPI spec, Postman collection, Insomnia export, or five other formats into a Reqloom project. What it produces and what you finish by hand."
---

Converts an existing API definition into a Reqloom project. No LLM, no network
call — a direct structural translation.

```bash
reqloom import <spec> [options]
```

The spec path must come first, before any flags.

## Options

| Flag | Value | Default |
| --- | --- | --- |
| `--out <dir>` | Directory to write the project into | current directory |
| `--project-root <dir>` | Containment root the spec must resolve under | the spec's parent directory |
| `--force` | Overwrite an existing `reqloom.yaml` | off |
| `--help` / `-h` | Show usage | — |

There is no `--format` flag — the format is detected from the file.

## Supported formats

All auto-detected:

| Format | Accepts |
| --- | --- |
| OpenAPI 3.x | `.yaml` or `.json` |
| Postman | Collection v2.1 |
| Insomnia | v4 export |
| Thunder Client | collection export |
| Hoppscotch | collection export |
| REST Client | `.http` / `.rest` files |
| Bruno | a collection directory, its `bruno.json`, or a single `.bru` file |

:::note[curl logs and Markdown docs go elsewhere]
`reqloom import` needs structured input. For raw curl logs, HAR captures, or
prose API documentation, use the [AI importer](/ai-importer/playbook/), which
drives an LLM through a multi-stage prompt suite instead.
:::

## Example

```bash
reqloom import RHP-School-API.postman_collection.json --out school-api
```

```ansi
Imported 11 resources, 105 operations into school-api/reqloom.yaml
```

You get a multi-file project, already in the layout the
[file structure guide](/schema/file-structure/) recommends:

```
school-api/
├── reqloom.yaml              version, name, imports
├── environments/
│   └── default.yaml          baseUrl + variables found in the collection
└── resources/
    ├── academic_years.yaml
    ├── attendance.yaml
    └── ...
```

Each operation carries its method, path, headers, and body:

```yaml title="resources/academic_years.yaml"
name: academic_years
operations:
  create_academic_year:
    method: POST
    path: /academic-years
    headers:
      Authorization: Bearer {{env.token}}
      Content-Type: application/json
    body: |
      {
        "name": "2023-2024",
        "startDate": "2023-04-01",
        "endDate": "2024-03-31",
        "isCurrent": true
      }
    _provenance:
      source: postman_import
      imported_at: 2026-08-29T08:15:47Z
      evidence:
        postman_name: Create Academic Year
```

Collection variables become environment variables, so Postman's `{{token}}`
becomes `{{env.token}}`:

```yaml title="environments/default.yaml"
name: default
variables:
  baseUrl: http://localhost:3000/api
  token: ""
```

`_provenance` records where each operation came from. It's pure metadata — the
runtime ignores it — but it survives editing and tells you later which
operations you've reviewed and which you haven't.

## What you finish by hand

The importer translates what the source file states. Source formats don't record
workflow, so two things are always missing:

```ansi
LINT OK — 0 actors, 11 resources, 105 operations. No errors.
```

**Zero actors.** A Postman collection pastes a bearer token into a variable; it
doesn't describe how to obtain one. Replace the imported `{{env.token}}` headers
with an [actor](/concepts/actors/) that logs in:

```yaml title="actors/teacher.yaml"
name: teacher
auth:
  strategy: simple
  method: POST
  path: /auth/login
  body:
    email: "{{env.teacher_email}}"
    password: "{{env.teacher_password}}"
  extract:
    token: $.data.accessToken
inject:
  headers:
    Authorization: "Bearer {{teacher.token}}"
```

Then drop the per-operation `Authorization` header and add `actor: teacher`.

**Zero dependencies.** Every operation imports as independent, and literal IDs
come through as literals — `path: /academic-years/1`. That `1` should become a
reference to an upstream extraction:

```yaml
  get_academic_year:
    method: GET
    path: /academic-years/{{academic_years.year_id}}
    depends_on: [academic_years.create_academic_year]
```

Wiring those two things is the actual work, and it's why importing a 105-endpoint
collection gets you a running start rather than a finished project. Do it for
the handful of chains you care about, not all 105.

Review notes for anything the importer couldn't resolve cleanly go to **stderr**,
so the success line on stdout stays machine-readable:

```bash
reqloom import spec.json --out api 2> review-notes.txt
```

## Overwriting

Importing into a directory that already has a `reqloom.yaml` fails rather than
clobbering your edits:

```ansi
Write error [E_SCHEMA_INVALID]: writer: reqloom.yaml exists in school-api (pass overwrite=true to replace)
  (pass --force to overwrite an existing reqloom.yaml)
```

Pass `--force` when you genuinely want to re-import — but note it discards the
actors and dependencies you added. Re-importing into a scratch directory and
diffing is usually safer.

## `--project-root`

A containment root: the spec must resolve inside it, which blocks a path
traversal from reaching files outside the tree you intended. It defaults to the
spec's own parent directory, so the flag only matters when you're importing
something from an untrusted or scripted path:

```bash
reqloom import ./specs/api.yaml --project-root ./specs --out ./generated
```

## After importing

Always lint first:

```bash
reqloom lint --project school-api
```

Then read [common pitfalls](/schema/pitfalls/) — imported schemas hit the
silent-key-drop behaviour more than hand-written ones, because they're large
enough that nobody reads every line.

## Next

- [Actors](/concepts/actors/) — replace imported bearer tokens with real logins
- [Dependency resolution](/concepts/dependencies/) — wire up the chains
- [AI importer](/ai-importer/playbook/) — for curl logs and prose docs
