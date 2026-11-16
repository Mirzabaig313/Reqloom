---
title: reqloom import
description: "Convert an OpenAPI spec, Postman collection, Insomnia export, or six other formats into a Reqloom project. What it produces and what you finish by hand."
---

Converts an existing API definition into a Reqloom project. No LLM, no network
call — a direct structural translation.

```bash
reqloom import <spec> [options]
```

Options may come before or after the spec path.

## Options

| Flag | Value | Default |
| --- | --- | --- |
| `--out <dir>` | Directory to write the project into | current directory |
| `--project-root <dir>` | Containment root the spec must resolve under | the spec's parent directory |
| `--force` | Write into a directory that already has a `reqloom.yaml` | off |
| `--no-secrets` | Leave credentials found in the input in the YAML, in plain text, instead of moving them into the project's secrets | off |
| `--help` / `-h` | Show usage | — |

There is no `--format` flag — the format is detected from the file.

## Supported formats

All auto-detected:

| Format | Accepts |
| --- | --- |
| OpenAPI 3.x, Swagger 2.0 | `.yaml` or `.json` |
| Postman | Collection v2.1 |
| Insomnia | v4 export |
| Thunder Client | collection export |
| Hoppscotch | collection export |
| HTTPie | collection export |
| Apidog | project export |
| REST Client | `.http` / `.rest` files |
| Bruno | a collection directory, its `bruno.json`, or a single `.bru` file |

REST Client and Bruno are recognised by their file extension (or, for Bruno, a
directory or `bruno.json`). Everything else is recognised by its structure, not
its name: a JSON file by its top-level keys (`openapi`/`swagger`, Postman's
collection `info`, Insomnia's `__export_format`, and so on), a YAML file by a
top-level `openapi` or `swagger` key. A JSON Schema document is refused with a
hint to export the collection itself.

:::note[curl logs and Markdown docs go elsewhere]
`reqloom import` needs structured input. For raw curl logs, HAR captures, or
prose API documentation, use the [AI importer](/ai-importer/playbook/) — a single
prompt you paste into a model.
:::

## Example

```bash
reqloom import school-api.postman_collection.json --out school-api
```

```ansi
Imported 11 resources, 105 operations into school-api/reqloom.yaml
```

You get a multi-file project, already in the layout the
[file structure guide](/schema/file-structure/) recommends:

```
school-api/
├── reqloom.yaml              version, name, id, imports for the folders written
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

This collection kept its token in a variable, so there was no literal credential
to [move into secrets](#credentials-move-into-secrets). A collection with
`Bearer eyJ…` written into its headers would get
`Bearer {{secret.<PROJECT>_AUTHORIZATION}}` instead, and a second line on stdout
naming the secret.

`_provenance` records where each operation came from. It's pure metadata — the
runtime ignores it — but it survives editing and tells you later which
operations you've reviewed and which you haven't.

The generated `reqloom.yaml` includes an `id:`, so the project is ready for
[secrets](/schema/secrets-and-transport/#secrets). Re-importing over an existing
project keeps the id already in its `reqloom.yaml`.

## How the source is translated

The same rules apply whatever the source format:

- **Variables.** A source variable becomes an environment variable:
  `{{token}}` → `{{env.token}}`. A name that isn't a plain identifier — dotted
  (`api.url`), Insomnia's `_.api.url`, or anything with other characters — is
  rewritten to a safe key (`{{env.api_url}}`) with a warning naming the change.
- **Environments.** Each source environment keeps its own name, lowercased with
  other characters turned into `_` (`Staging EU` → `staging_eu`). For Bruno, each
  `environments/<name>.bru` becomes an environment and the first by file name is
  the default; for HTTPie, the one marked default (else the first); for
  Insomnia, the base environment becomes `default` and each sub-environment gets
  its own environment holding the base values plus its overrides. With no source
  environments you get one named `default`.
- **Path parameters.** `/users/:id` and `/users/{id}` become
  `/users/{{users.id}}` — a reference to a value an upstream operation of the same
  resource must extract — with one warning per parameter. Wire it up as shown
  [below](#what-you-finish-by-hand).
- **Auth.** No source auth setting is converted. Each one produces a single
  warning — "`bearer` auth was not imported — create an actor for it and attach
  it". Headers the source sets explicitly, `Authorization` included, are copied
  as headers, with their credential moved into secrets (below).
- **Form bodies.** A multipart form stays multipart, and a URL-encoded form
  stays URL-encoded. Thunder Client form bodies are imported too.
- **File uploads.** Only the file name of an upload path is kept
  (`/Users/x/avatar.png` → `@avatar.png`), with a warning: place the file next to
  `reqloom.yaml`, or choose it again in the desktop form editor. A file name built
  from a variable is imported as an empty file slot (`@`) to be filled in, and a
  run fails with `E_UPLOAD_FILE_UNREADABLE` until it is.
- **Form text starting with `@`** is imported as `@@…`, so it is still sent as
  text.
- **Bruno** entries disabled with a leading `~` are skipped.

## Credentials move into secrets

An import stores the credentials it finds in the project's
[secrets](/schema/secrets-and-transport/#secrets), in the OS keychain, and
writes `{{secret.NAME}}` in their place. Exactly these count:

- a variable the source marks as secret — Postman `"type": "secret"`, HTTPie
  `isSecret`;
- the credential in a literal `Authorization` or `Proxy-Authorization` header —
  the part after the scheme word, so `Bearer abc123` becomes
  `Bearer {{secret.NAME}}`;
- the whole value of a literal `X-API-Key` header.

Nothing else is moved: a password in a body, a token in a query string, or a
variable the source doesn't flag stays as it is. A value that already holds a
`{{…}}` reference is not literal and is left alone. OpenAPI, Insomnia, Thunder
Client, Hoppscotch, Apidog, REST Client and Bruno have no secret flag the
importer reads, so for them only the header rules apply.

Secret names are upper snake case:

| Credential | Name |
| --- | --- |
| A flagged variable | its own name — `api_key` → `API_KEY`; `<ENV>_<VAR>` when its value differs between environments |
| A header value used by one operation | `<RESOURCE>_<OPERATION>_<HEADER>` — `ORDERS_CREATE_AUTHORIZATION` |
| The same header value on several operations | one shared secret, `<PROJECT NAME>_<HEADER>` — `SHOP_API_AUTHORIZATION` |

The command prints the names it moved on stdout, never the values. An existing
secret with the same name and a different value is never overwritten. A
credential that can't be stored — no keychain, a refused
[folder check](/schema/secrets-and-transport/#each-project-id-belongs-to-one-folder),
a secret of that name that holds a different value — stays in the YAML in plain
text, with a warning on stderr giving the reason and saying what to store and
where; no credential is lost.

`--no-secrets` keeps every credential in the YAML and prints that warning for
each:

```ansi
Left 1 credential in the YAML in plain text:
  - operation shop_api.create_order header `Authorization`: a credential was left in the YAML in plain text (moving credentials into secrets was turned off). To move it into secrets later, store it as secret `SHOP_API_CREATE_ORDER_AUTHORIZATION` (desktop: Manage Secrets) and write `{{secret.SHOP_API_CREATE_ORDER_AUTHORIZATION}}` in its place
```

If writing the project fails, the secrets the import stored are removed
again. The desktop app's **File → Import** does the same.

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
overwriting your edits:

```ansi
reqloom import: school-api already has a reqloom.yaml (pass --force to overwrite it)
```

The exit code is `1`. A project that couldn't be written to disk is exit code
`3`, with `E_WRITE_FAILED`.

Pass `--force` when you genuinely want to re-import. It replaces `reqloom.yaml`
and every `actors/`, `resources/`, or `environments/` file with the same name as
one the import writes — so dependencies you added to an imported resource file
are lost. It never deletes other files: actors you wrote yourself, and resources
the new import doesn't produce, stay where they are. Re-importing into a scratch
directory and diffing is usually safer.

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

Read its `WARNING:` lines, then [common pitfalls](/schema/pitfalls/) — imported
schemas are large enough that nobody reads every line.

## Next

- [Actors](/concepts/actors/) — replace imported bearer tokens with real logins
- [Dependency resolution](/concepts/dependencies/) — wire up the chains
- [AI importer](/ai-importer/playbook/) — for curl logs and prose docs
