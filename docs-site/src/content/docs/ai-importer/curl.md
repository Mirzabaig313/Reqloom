---
title: Importing curl logs
description: "The hardest input and the most truthful one. How to prepare curl or HAR captures, and why it will ask you a lot of questions."
---

Curl logs have no schema, no metadata, and no declared structure. There's no direct
importer for them — this is the AI importer's actual purpose.

The trade is worth understanding: a curl log is the **least structured** input and
the **most truthful** one. A spec says what the API is supposed to do; a capture
shows what it did.

## When this is the right input

- There is no OpenAPI spec, or the spec is stale enough to mislead
- The API is undocumented or third-party
- You have browser DevTools output or a HAR export of a real session
- You want the actual request order, which a spec never records

## Preparing the input

Quality of output tracks quality of input closely. Four things to do first:

**Capture a complete flow, in order.** One coherent journey — log in, create,
update, delete — beats 50 unrelated requests. The order *is* the dependency
information.

**Include responses.** A request alone tells the model nothing about what to
extract. This is the single highest-value addition:

```bash
curl -i -X POST https://api.example.com/orders \
  -H "Authorization: Bearer eyJ..." \
  -H "Content-Type: application/json" \
  -d '{"total": 100}'
```

`-i` includes response headers. Better still, capture the body too.

**Redact credentials.** Replace real tokens with placeholders before pasting
anything into a model:

```bash
-H "Authorization: Bearer <REDACTED>"
```

The schema will reference `{{secret.X}}` anyway, so the model never needs the real
value.

**Note which identity made each request.** If your capture spans an admin session
and a customer session, say so. The model cannot infer it, and it's what determines
your actors.

## From DevTools

Chrome and Firefox both offer **Copy as cURL** on a network request, and **Save all
as HAR** for a whole session. HAR is verbose but complete — requests, responses,
headers, timings. Redact it before use; HAR files contain every cookie and token
from the session.

## Expect questions

The prompt will surface a long list of open questions on curl input
— far more than for OpenAPI. That's the prompt working correctly, not failing.
Typical ones:

- Is `/api/v1/orders/8821` an id from the previous response, or a fixture?
- Two requests use different tokens — two actors, or one whose token was refreshed?
- Is this `POST /orders/8821/pay` a prerequisite for the refund, or unrelated?
- What status does this endpoint return on success? The capture shows one case.

Answering these in chat takes ten minutes and saves hours. Every unanswered question
becomes a guess in the generated YAML, and guesses in `expect_status` or
`depends_on` produce chains that fail in confusing ways.

## What the model will get wrong

Even with good input, check these:

| Likely error | Why |
| --- | --- |
| Literal ids left in paths | `/orders/8821` looks like a valid path |
| Missing `depends_on` | Adjacency in a log isn't proof of dependency |
| Invented `expect_status` | Your capture shows one response; the model generalises |
| One actor where there are two | Different tokens can look like the same identity |
| Over-broad resources | Every path under `/admin` grouped into one resource |

The literal-id one is the most common. `path: /orders/8821` is syntactically fine
and will "work" against a database that still contains order 8821 — then break
silently later. Search generated output for digits in paths.

## After generating

```bash
reqloom lint --project my-api
```

Iterate with the model until lint is clean, then **run one chain for real**. Lint cannot tell
you that a status code or a body field is wrong — only a real request can. Mark
what you've confirmed:

```yaml
    _provenance:
      source: ai_import
      verified_against: live_capture     # was: synthetic
```

## An honest alternative

For a small API, hand-writing the schema from a curl log is often faster than three
rounds of prompting. Five endpoints and one actor is maybe twenty minutes with the
[authoring guide](/schema/authoring/) open, and you'll understand the result.

The AI importer earns its cost at scale — 30+ endpoints, or an API you don't know
well.

## Next

- [AI importer](/ai-importer/playbook/) — the prompt and the workflow
- [Overview & playbook](/ai-importer/playbook/) — the end-to-end workflow
- [Authoring guide](/schema/authoring/) — writing it yourself
