# Design Partner Interviews

> **Goal:** answer PRD §16.2 question 2 — "≥ 6 of 10 design partners say I would switch to this from Postman" — through structured interviews.
> **Time required:** ~2 weeks wall-clock to recruit, schedule, and run; ~30 minutes per interview.

## Files in this kit

- `screening-criteria.md` — who counts as a qualified design partner, and who doesn't
- `outreach-email.md` — the cold-outreach template
- `interview-script.md` — what to actually say and ask in the call
- `results-template.md` — fill in one entry per interview, then aggregate

## Procedure

1. Build a list of **20–30 candidates** from your network (peers, ex-colleagues, X/LinkedIn contacts in backend roles). Aim for 2–3× the target count to account for no-shows and disqualifications.
2. **Screen** each candidate against `screening-criteria.md` before reaching out. Sending the email to a frontend-only dev is wasted.
3. **Send the outreach email**. Track responses in the results template.
4. **Schedule 30-min calls** with everyone who responds yes.
5. **Run the interview** following the script. Take notes in `results-template.md`.
6. After **at least 8 interviews**, compute the aggregate.

## What "yes" means for the §16.2 gate

Per the PRD: **6 of 10** must say "I would switch from Postman" with conviction. The interview script asks this twice — once at the start (after seeing the pitch) and once at the end (after seeing the schema). The answer that counts is the **end answer**, after they understand what they're committing to.

## What to do with the results

If 6+ of 10 say yes:
- Document the recurring objections — these are your roadmap shapers
- Identify the 2–3 strongest positive responses — these become beta candidates for Phase 4
- Greenlight the MVP

If <6 of 10 say yes:
- Categorize the "no" answers: is the problem the schema, the workflow, the audience, the price?
- If the schema is the issue: revisit Phase 0 schema validation (the three-API set).
- If the audience is the issue: re-run with different personas before pivoting.
- If the workflow is the issue: this is the most expensive miss — consider whether the auto-resolve concept actually solves a sufficiently painful problem.
