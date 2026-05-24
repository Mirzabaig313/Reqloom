# Design Partner Outreach Email

> Use this template for cold outreach to qualified candidates. Personalize the first line; the rest can stay verbatim.

---

## Version A — for someone you have spoken with before (preferred)

**Subject:** Quick question about your Postman/Bruno workflow

Hi {first_name},

You came up because I'm building a developer tool around a frustration I've heard from {context — "you mentioned at $event" / "your tweet about" / "we worked together on $project"}: the manual chain of "log in as admin, log in as vendor, create product, create order, pay, then test the actual endpoint" that backend devs run through every morning to test multi-role APIs.

I'm calling it ChainAPI. The core idea is treating an API as a graph: define each role and each resource once, and the tool auto-resolves the chain when you click any endpoint. No more copying tokens between tabs.

Before I commit to building this for ten weeks I'm validating the design with ten engineers who actually live this pain. Would you be up for a 30-minute call this or next week? I'll show you the schema and the proposed UI; you tell me whether it would replace your current setup or whether I've missed something.

Happy to share notes from the conversation back to you. I'll buy you coffee or its remote equivalent.

{your_name}

---

## Version B — for cold outreach (no prior contact)

**Subject:** Backend API testing — would love your input

Hi {first_name},

I'm researching the workflow developers use to test multi-role APIs (admin + vendor + customer kind of thing) and you fit the profile I'm looking for from your {LinkedIn / GitHub / X}. I'd value 30 minutes of your time on a call.

Quick context: I've been building a tool that treats an API as a dependency graph — define each actor's auth flow once, define each resource once, and the tool auto-resolves "to test refund.approve I need to log in as 3 actors and create 7 prerequisite records" without any manual scripting. No code yet; I'm validating the design before I commit.

If you currently use Postman or Bruno for API testing and have ever felt the "copy-paste tokens between tabs" pain, I'd love to show you what I have and get your honest take.

30 minutes, on Zoom or a call, your timezone. I'll share my findings with you regardless of what you tell me.

{your_name}

---

## Notes

- Keep the email under 150 words. Long pitch emails get deleted.
- The "honest take" framing matters. People give better feedback when they don't think they're being sold to.
- If they don't respond in 5 days, **do not** send a follow-up. The non-response is data; respect their time.
- Track every send in `results-template.md` even if no response. Response rate is a useful signal.
