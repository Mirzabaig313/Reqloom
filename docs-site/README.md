# ChainAPI Docs Site

The documentation site at **chainapi.github.io** (and any equivalent
project-pages URL). Built with [Astro Starlight](https://starlight.astro.build/).

## Local development

```bash
cd docs-site
npm install
npm run dev          # → http://localhost:4321
```

## Production build

```bash
npm run build
npm run preview      # serve the built site locally
```

## Deployment

`.github/workflows/deploy-docs.yml` builds and deploys to GitHub Pages
on every push to `main` that touches:

- `docs-site/**`
- `doc/**`
- `prompts/import/**`
- The workflow file itself

Manual trigger is available from the GitHub Actions tab.

The workflow auto-detects whether the repo is an **org page**
(`<owner>.github.io`) or a **project page** (`<owner>.github.io/<repo>/`)
and configures `SITE` + `BASE` accordingly. No config change needed
when moving between the two.

## Adding a page

1. Create the markdown file under `src/content/docs/<section>/<slug>.md`
   (or `.mdx` for component support)
2. Add it to the sidebar in `astro.config.mjs`
3. Frontmatter requires `title:` and `description:`

```markdown
---
title: My new page
description: One-line summary that appears in the sidebar tooltip and OG card.
---

Content here.
```

## Editing existing pages

Each page has an "Edit page on GitHub" link in the footer (configured
in `astro.config.mjs`). Contributors don't need to clone — they can
edit straight from GitHub.

## Layout

```
docs-site/
├── astro.config.mjs       Site config + sidebar
├── package.json
├── tsconfig.json
├── public/                Static assets served at /
│   └── favicon.svg
├── scripts/
│   └── scaffold-stubs.sh  Regenerate placeholder pages for new nav slugs
└── src/
    ├── assets/            Images referenced from MDX (logo, hero)
    ├── styles/
    │   └── custom.css     Brand colours, layout tweaks
    └── content/docs/      Every page in the site
        ├── index.mdx                  Landing page (splash template)
        ├── start/
        ├── concepts/
        ├── schema/
        ├── cli/
        ├── ai-importer/
        ├── examples/
        ├── reference/
        └── dev/
```

## Brand colours

In `src/styles/custom.css`. Currently a deep-blue/violet placeholder
matching the Material 3 swatch from PRD §9.2. Real palette decision is
a Phase 4 polish item.

## Migrating canonical content

Some sections (the schema authoring guide, AI importer playbook) have
a canonical version in `doc/local/` and a copy in `docs-site/`. The
copy in `docs-site/` is the one users see. When updating, edit the
`docs-site/` version; the `doc/local/` version is kept for offline
reference but should be considered secondary.

## Why Astro Starlight

Considered alternatives:

- **Docusaurus** — heavier, MDX-first by default but verbose config
- **VitePress** — lighter but Vue-coupled, less out-of-the-box (no
  built-in OG generation, search, i18n scaffolding)
- **mdBook** — too minimal for a product site
- **Jekyll + just-the-docs** — minimal, but JS ecosystem (search,
  custom components) is awkward

Starlight wins on: built-in search, OG image generation, sidebar
auto-collapse, dark/light mode by default, MDX support, and very
short config. Used by Bun, Cloudflare, Tauri, Astro itself.
