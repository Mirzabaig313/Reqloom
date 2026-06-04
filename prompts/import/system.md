# AI Importer — System Prompt (placeholder)

This is the system prompt template used by the AI importer (PRD §10).
The full prompt is filled in during Phase 3 of the roadmap. The
expected structure is:

1. Reqloom schema spec excerpt (so the model knows the output format).
2. Few-shot examples (`few-shot/` neighbors).
3. Instructions for confidence reporting per inference.
4. Privacy footer reminding that no input data is sent to Reqloom's
   servers — the LLM call goes directly from the user's machine to
   their chosen provider.

See PRD §10.2 and FR-9.x for the contract this prompt must satisfy.
