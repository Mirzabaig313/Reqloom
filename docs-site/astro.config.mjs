// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";

// Site URL is set per-deploy via the SITE/BASE env vars (see deploy-docs.yml).
// Defaults target the project page at https://mirzabaig313.github.io/ChainAPi/
// so a local or non-CI build still produces correct, prefixed internal links.
// CI overrides these:
//   SITE = https://<user>.github.io   BASE = /ChainAPi/   (project page)
//   SITE = https://<user>.github.io   BASE = /            (org page)
const site = process.env.SITE ?? "https://mirzabaig313.github.io";
const base = process.env.BASE ?? "/ChainAPi/";

export default defineConfig({
    site,
    base,
    integrations: [
        starlight({
            title: "ChainAPI",
            description:
                "Workflow-aware API testing tool that auto-resolves request dependency chains.",
            logo: { src: "./src/assets/logo.svg", replacesTitle: false },
            favicon: "/favicon.svg",
            social: [
                {
                    icon: "github",
                    label: "GitHub",
                    href: "https://github.com/Mirzabaig313/ChainAPi",
                },
            ],
            editLink: {
                baseUrl:
                    "https://github.com/Mirzabaig313/ChainAPi/edit/main/docs-site/",
            },
            customCss: ["./src/styles/custom.css"],
            head: [
                {
                    tag: "meta",
                    attrs: {
                        property: "og:image",
                        content: `${site}${base}og-image.png`,
                    },
                },
            ],
            sidebar: [
                {
                    label: "Start Here",
                    items: [
                        { label: "What is ChainAPI?", slug: "start/overview" },
                        { label: "Installation", slug: "start/install" },
                        { label: "5-minute tour", slug: "start/tour" },
                    ],
                },
                {
                    label: "Concepts",
                    items: [
                        { label: "The mental model", slug: "concepts/mental-model" },
                        { label: "Actors", slug: "concepts/actors" },
                        { label: "Resources & operations", slug: "concepts/resources" },
                        { label: "Dependency resolution", slug: "concepts/dependencies" },
                        { label: "Variables & references", slug: "concepts/variables" },
                        { label: "Sessions & caching", slug: "concepts/sessions" },
                    ],
                },
                {
                    label: "Schema Authoring",
                    items: [
                        { label: "Authoring guide", slug: "schema/authoring" },
                        { label: "File structure", slug: "schema/file-structure" },
                        { label: "Auth strategies", slug: "schema/auth-strategies" },
                        { label: "Common pitfalls", slug: "schema/pitfalls" },
                        { label: "Cheat sheet", slug: "schema/cheatsheet" },
                    ],
                },
                {
                    label: "CLI",
                    items: [
                        { label: "Overview", slug: "cli/overview" },
                        { label: "chainapi run", slug: "cli/run" },
                        { label: "chainapi lint", slug: "cli/lint" },
                        { label: "chainapi import", slug: "cli/import" },
                    ],
                },
                {
                    label: "AI Importer",
                    items: [
                        { label: "Overview & playbook", slug: "ai-importer/playbook" },
                        { label: "Multi-stage prompt suite", slug: "ai-importer/prompts" },
                        { label: "Importing OpenAPI", slug: "ai-importer/openapi" },
                        { label: "Importing Postman", slug: "ai-importer/postman" },
                        { label: "Importing curl logs", slug: "ai-importer/curl" },
                    ],
                },
                {
                    label: "Examples",
                    items: [
                        { label: "Marketplace API", slug: "examples/marketplace" },
                        { label: "GitHub REST", slug: "examples/github" },
                        { label: "Stripe", slug: "examples/stripe" },
                    ],
                },
                {
                    label: "Reference",
                    items: [
                        { label: "Schema spec", slug: "reference/schema-spec" },
                        { label: "Variable syntax", slug: "reference/variables" },
                        { label: "Error codes", slug: "reference/error-codes" },
                        {
                            label: "Engine requirement (full spec)",
                            slug: "reference/engine-requirement",
                        },
                    ],
                },
                {
                    label: "Development",
                    items: [
                        { label: "Architecture", slug: "dev/architecture" },
                        { label: "Building from source", slug: "dev/building" },
                        { label: "Contributing", slug: "dev/contributing" },
                        { label: "Roadmap", slug: "dev/roadmap" },
                    ],
                },
            ],
        }),
    ],
});
