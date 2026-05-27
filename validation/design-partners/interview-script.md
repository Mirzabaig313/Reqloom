# Design Partner Interview Script

> 30-minute call. Use this as a guide, not a checklist. Let them talk; you'll learn more from a tangent than from a perfectly-followed agenda.

## Pre-call (the day of)

- Re-read the candidate's screening notes
- Have ready in tabs:
  - PRD §1.1 (the refund.approve example chain)
  - PRD §5 (a sample of the schema)
  - PRD §9.1 (the UI mockup)
- Set up screen recording (with their permission, ask at the start)
- Have `results-template.md` open in a separate window for note-taking

## Call structure (30 min)

### 0:00–0:03 — Setup

- "Mind if I record? It's just for me; I'll share my notes with you after."
- "I'm going to ask about your current workflow first, then show you what I've been building, then ask whether it solves a real problem for you. Total 30 minutes."

### 0:03–0:10 — Their current workflow (open-ended)

The most important seven minutes. Listen more than you talk. Suggested prompts:

- "Walk me through the last time you tested a backend feature. Where did you start? What tools were open?"
- "When you log in as a test user in Postman, how does that token get into your other requests?"
- "Has there ever been a time when you spent more than 10 minutes on setup before you could actually test the thing you wanted to test?"
- (If they mention Postman scripts) "How often do those scripts break? Who maintains them?"

What you're listening for:
- Concrete pain references (specific examples > general complaints)
- Whether they've worked around the pain or accepted it as normal
- Other tools or workflows they've tried and abandoned

### 0:10–0:13 — Pitch (60–90 seconds)

Don't over-explain. The pitch is:

> "I'm building ChainAPI. It treats your API as a graph. You define each actor's auth flow once and each resource's endpoints and dependencies once. Then when you click any endpoint, ChainAPI auto-resolves the entire prerequisite chain — login as admin, log in as customer, create the order, pay, request the refund — and runs the chain. No copy-paste of tokens. No 200-request Postman folder."

Pause. Let them ask questions. Their first question is usually their biggest concern.

### 0:13–0:14 — First gauge (early signal)

Single question: **"On a 1–10 scale, would you switch from your current tool to this if it shipped tomorrow?"**

Write down the number. Don't argue with it. This is your "before they see the implementation" baseline.

### 0:14–0:24 — Schema walkthrough

Share screen. Show:

1. The MarketplaceAPI sample schema (`samples/marketplace/`) — actors, then resources
2. Walk through the `refund.approve` chain explicitly
3. Show one of the validation findings (e.g. "Stripe Connect's same-credential-multi-actor pattern" from the Stripe findings)
4. Show the proposed UI mockup from PRD §9.1

Throughout, ask:
- "Does this look like something you could write?"
- "Where would you get stuck?"
- "Is there anything in your current API that this couldn't express?"

### 0:24–0:28 — The real question

**"On the same 1–10 scale, would you switch from your current tool to this — given everything you've now seen?"**

Then probe:
- If their score is **8+**: ask what would push it to 10. ("Two more features and a stable release?")
- If their score is **5–7**: ask what's missing. The honest answer here is your roadmap signal.
- If their score is **<5**: ask what they expected the tool to be. You're learning what mental model they had vs. what you actually have.

### 0:28–0:30 — Wrap

- "Anything I should be asking that I'm not?"
- "Anyone else you think I should talk to?" (referrals are gold)
- "Would you want to be a beta user when this lands?"
- Thank them. Send a follow-up email with notes within 24 hours.

## After the call

- Within 1 hour: fill in `results-template.md` while it's fresh
- Within 24 hours: send a thank-you email with a 5-line summary of what you heard

## What to NOT do

- **Don't argue** with negative feedback. The data is more valuable than your ego.
- **Don't pitch.** You're researching, not selling. If they want to use it, they'll ask.
- **Don't promise features.** Every "yes I'll add that" turns into either a broken promise or a scope creep.
- **Don't rush past concerns.** A 60-second tangent on "my team won't switch tools without buy-in" is more valuable than 5 more pre-canned questions.
