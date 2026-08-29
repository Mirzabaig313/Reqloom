---
title: GitHub REST
description: "A validation case study: 22 endpoints across 7 resources modelling the GitHub REST API, and the schema gaps it exposed."
---

Before the engine was built, the schema format was validated by hand-authoring
real APIs and recording what broke. GitHub was one of them: **22 endpoints across
7 resources** (`repo`, `branch`, `content`, `pull`, `issue`, `team`) and 2 actors
(`user`, `admin`). The findings below drove real changes to the schema.

:::note[This is a case study, not a bundled project]
The schema was authored for the validation exercise and isn't shipped in the
repository — the only runnable sample is
[marketplace](/examples/marketplace/). The patterns below are real and worth
reading; the file paths aren't ones you can open.
:::

## Why GitHub

It stresses things a marketplace API doesn't:

- **Pre-issued credentials.** A personal access token isn't obtained by logging
  in; you already have it. The "auth" call verifies it rather than producing it.
- **Header-based pagination.** `Link: <...>; rel="next"` rather than a cursor in
  the body.
- **Base64 in a JSON body.** The create-file endpoint wants file content encoded
  inside a JSON field.
- **A genuinely deep chain.** repo → branch → content → pull → merge.

## The chain it models

```
repo.create → branch.create → content.create → pull.create → pull.merge
```

Authored as five operations with one `depends_on` each, which the resolver expands
into the full ordering — the same mechanic as
[marketplace's 7-step chain](/examples/marketplace/#running-the-flagship-chain).

Multi-actor review flows composed cleanly: a pull request opened by `user`,
reviewed by `admin`, merged by `user`, expressed purely through `actor:` plus
`depends_on:`.

## Pre-issued credentials

A GitHub PAT lives in your keychain, not in a login response. The pattern that
works today:

```yaml title="actors/user.yaml"
name: user
description: GitHub user authenticated with a personal access token
auth:
  strategy: bearer
  token: "{{secret.GITHUB_PAT}}"
session:
  ttl: 24h
```

`strategy: bearer` makes no network call — the token is attached directly. And
because `inject:` and actor auth config resolve `env` and `secret` like anywhere
else, no synthetic "extract the value I already had" step is needed.

That was **Finding 1** in the validation: the original spec assumed `inject:`
only saw actor session variables, which would have forced a hook on every
operation. Relaxing it to the general resolution rules fixed the class of problem,
and it's how the engine behaves now.

Note the `ttl: 24h`. Per-actor TTLs matter here: a PAT is good for a day, where a
short-lived JWT actor wants 15m.

## Unique names per run

Creating a repo needs a name nobody has used:

```yaml
  create:
    method: POST
    path: /user/repos
    actor: user
    body:
      name: "reqloom-test-repo-{{$.uuid}}"
      private: true
    expect_status: 201
    extract:
      repo_name: $.name
      owner: $.owner.login
```

`{{$.uuid}}` per request is exactly the case builtins exist for — see
[variables](/concepts/variables/#unique-values-two-different-needs).

## Base64 in a body

GitHub's create-file endpoint wants encoded content. The validation flagged this
as **Finding 3** and proposed YAML transformer tags (`!base64`, `!json`, `!file`).
Those tags were **not** implemented — `!secret` remains the only YAML tag. What
shipped instead was an encoding function:

```yaml
  create:
    method: PUT
    path: /repos/{{repo.owner}}/{{repo.repo_name}}/contents/REQLOOM.md
    actor: user
    body:
      message: "Add REQLOOM.md"
      content: "{{$.base64.encode('Hello from Reqloom')}}"
      branch: "{{branch.branch_name}}"
    expect_status: 201
```

Same outcome, different spelling. `$.base64`, `$.hex`, and `$.url` all have
`encode`/`decode` — see [encoding functions](/reference/variables/#encoding-functions).

## Header pagination — still partly open

GitHub paginates with a `Link` header. Extracting a header works:

```yaml
    extract:
      link_header: $.headers.Link
```

But `Link` is a comma-separated list of `<url>; rel="next"` tuples, and you want
only the URL whose `rel` is `next`. The validation asked for named parsers
(`link-header[rel=next]`) as **Finding 2**. The header *source* shipped; **the
named-parser library did not.** There is no `parse:` key.

Today you'd extract the raw header and pick it apart in a `post_response`
[hook](/schema/advanced-operations/#hooks), or use a regex extraction:

```yaml
    extract:
      next_url:
        path: '<([^>]+)>;\s*rel="next"'
        source: regex
```

This is a real remaining gap, and it's on the [roadmap](/dev/roadmap/#known-gaps)
territory rather than something the schema solves elegantly yet.

## What needed no escape hatch

The validation's conclusion was **pass with three required changes**. Everything
else in GitHub's surface — multi-actor PR flows, team management, issue
lifecycles, per-actor token lifetimes — was expressible declaratively, with no JS
hooks.

The comparison that mattered: hand-authoring this chain in YAML was dramatically
more compact than the equivalent Postman collection, because the prerequisites are
declared once instead of scripted per request.

## Takeaways for your own schema

| GitHub pattern | What to use |
| --- | --- |
| Pre-issued token (PAT, API key) | `strategy: bearer` + `{{secret.X}}`, no login call |
| Long-lived credential | A longer `session.ttl` — `24h` |
| Unique resource name per run | `{{$.uuid}}` in the body |
| Base64 a body field | `{{$.base64.encode(...)}}` |
| Header value | `$.headers.<Name>` extraction |
| Structured header value | Regex extraction, or a `post_response` hook |

## Next

- [Marketplace](/examples/marketplace/) — the runnable sample
- [Stripe](/examples/stripe/) — the multi-tenant case study
- [Auth strategies](/schema/auth-strategies/) — all eleven
