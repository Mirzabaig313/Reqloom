# Security Policy

ChainAPI is a local developer tool: a CLI (`chainapi`) and a Qt desktop app
built on a pure C++ engine. It parses untrusted input (OpenAPI specs,
`chainapi.yaml` project files), executes user-supplied JavaScript hooks, and
handles API credentials via the OS keychain. We take reports against any of
these surfaces seriously.

## Supported versions

ChainAPI is pre-0.1.0 and ships from `main`. Security fixes land on `main` and
in the next tagged release. Older tags do not receive backports until a
stable release line exists.

| Version | Supported |
|---------|-----------|
| `main` / latest release | ✅ |
| older pre-0.1.0 tags | ❌ |

## Reporting a vulnerability

**Do not open a public issue, pull request, or discussion for a security
problem.** Public disclosure before a fix puts every user at risk.

Report privately through GitHub's private vulnerability reporting:

1. Go to **https://github.com/Mirzabaig313/ChainAPI/security/advisories/new**
2. Or: repo **Security** tab → **Report a vulnerability**

If you cannot use GitHub Security Advisories, contact the maintainer
(**@Mirzabaig313**) directly and ask for a private channel before sending
any details.

Please include:

- Affected component (engine / CLI / desktop) and version or commit SHA.
- A clear description of the issue and its impact.
- Reproduction steps, a proof-of-concept project, or a minimal `chainapi.yaml`
  / OpenAPI spec that triggers it.
- Your assessment of severity and any known mitigation.

Do **not** include real secrets, production credentials, or customer data in
a report. Redact tokens and use placeholders.

## What to expect

| Stage | Target |
|-------|--------|
| Acknowledgement of your report | within 3 business days |
| Initial assessment (confirm / triage / severity) | within 7 business days |
| Fix or mitigation plan communicated | within 30 days of confirmation |

These are good-faith targets for a small open-source project, not a
contractual SLA. We will keep you updated on progress and let you know when a
fix ships.

## Disclosure policy

- We follow **coordinated disclosure**. Please give us a reasonable window to
  release a fix before any public write-up.
- We will credit reporters in the advisory and release notes unless you ask to
  remain anonymous.
- Once a fix is released, we will publish a GitHub Security Advisory describing
  the issue, affected versions, and the fixed version.

## Scope

In scope:

- The engine library, CLI, and desktop app in this repository.
- Untrusted-input handling: OpenAPI import, `chainapi.yaml` parsing.
- The JavaScript hook runner (sandbox escape, resource exhaustion).
- Secret handling: keychain storage, header/log redaction.
- The HTTP client's TLS verification behavior.
- Supply-chain issues in our build, CI, or release process.

Out of scope:

- Vulnerabilities in third-party dependencies that are already publicly known
  and have an upstream fix — instead, open a regular issue to bump the
  dependency.
- Findings that require a user to deliberately run a project they know to be
  malicious with security controls explicitly disabled (e.g. a user passing an
  `--insecure` TLS flag against their own choice). Report it anyway if the
  control is missing or fails silently.
- Social engineering, physical access, and issues in services we do not
  operate.

## Safe harbor

We will not pursue or support legal action against researchers who:

- Make a good-faith effort to follow this policy.
- Avoid privacy violations, data destruction, and service disruption.
- Only test against their own projects and installations.

Thank you for helping keep ChainAPI and its users safe.
