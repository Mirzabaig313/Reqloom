# Reqloom AI Importer

One prompt: **[`reqloom-import.md`](reqloom-import.md)**.

Paste it into your model, paste your API description after it, and you get a
Reqloom project back in one pass.

```
your OpenAPI / Postman / Markdown / curl logs
        │
        ▼
  reqloom-import.md  ──►  a plan + open questions, then the YAML files
        │
        ▼
  reqloom lint --project <dir>   ──►  paste errors back, iterate
        │
        ▼
  reqloom run <op>               ──►  the only real proof
```

Typically 1–3 calls for a 50-endpoint API. No API key, no integration — it's a
Markdown file you paste.

## Why one prompt

This started as a six-stage suite: discover, plan, generate actors, generate
resources, generate environment, fix lint. The staging existed to stop a model
hallucinating detail and truncating long output.

It was replaced because the friction outweighed the benefit. Six stages meant
8–15 calls, manual copy-paste of intermediate artifacts, and a workflow nobody
finished. The original prompts are in git history if you want them.

What actually prevents hallucination is **giving the model the schema** rather
than splitting the work. `reqloom-import.md` embeds the full key reference, a
worked example, and an explicit "ask instead of guessing" rule — so there is very
little left to invent. The plan-before-YAML review gate survives as Part 1 of the
single response.

## What still needs a human

The prompt cannot know two things, and neither can any spec:

- **Which credentials are real.** Passwords and keys come out as `!secret`
  placeholders you populate in your OS keychain.
- **Whether a status code or body is right.** Lint validates structure only. One
  real run per chain is the proof.

Read the **Open questions** section it produces. Every ungrounded inference lands
there by design, and answering them is cheaper than debugging generated YAML.

## Licence

The prompt is a designated paid component — not covered by the Apache 2.0 licence
over the engine, CLI, schema, and desktop app. See [`LICENSE`](../../LICENSE).
