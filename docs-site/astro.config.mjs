// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import remarkBaseLinks from "./src/plugins/remark-base-links.mjs";

// Site URL is set per-deploy via the SITE/BASE env vars (see deploy-docs.yml).
// Defaults target the project page at https://mirzabaig313.github.io/Reqloom/
// so a local or non-CI build still produces correct, prefixed internal links.
// CI overrides these:
//   SITE = https://<user>.github.io   BASE = /Reqloom/   (project page)
//   SITE = https://<user>.github.io   BASE = /            (org page)
const site = process.env.SITE ?? "https://mirzabaig313.github.io";
const base = process.env.BASE ?? "/Reqloom/";

const description =
    "Reqloom is a workflow-aware API testing tool. Define actors and resources " +
    "once; run any endpoint and the engine resolves the whole prerequisite chain.";

const ogImage = `${site}${base}og-image.png`;

export default defineConfig({
    site,
    base,
    markdown: {
        remarkPlugins: [[remarkBaseLinks, { base }]],
    },
    integrations: [
        starlight({
            title: "Reqloom",
            description,
            tagline: "Workflow-aware API testing",
            logo: { src: "./src/assets/logo.svg", replacesTitle: false },
            favicon: "/favicon.svg",
            lastUpdated: true,
            social: [
                {
                    icon: "github",
                    label: "GitHub",
                    href: "https://github.com/Mirzabaig313/Reqloom",
                },
            ],
            editLink: {
                baseUrl:
                    "https://github.com/Mirzabaig313/Reqloom/edit/main/docs-site/",
            },
            customCss: ["./src/styles/custom.css"],
            // H2 + H3 only. Reference pages nest four deep and a full-depth
            // TOC becomes an unreadable second sidebar.
            tableOfContents: { minHeadingLevel: 2, maxHeadingLevel: 3 },
            expressiveCode: {
                themes: ["github-dark-default", "github-light-default"],
                styleOverrides: {
                    borderRadius: "0.625rem",
                    borderColor: "var(--rl-border-subtle)",
                    codeFontFamily: "var(--sl-font-mono)",
                    uiFontFamily: "var(--sl-font)",
                    frames: {
                        shadowColor: "transparent",
                    },
                },
            },
            head: [
                {
                    tag: "meta",
                    attrs: { property: "og:type", content: "website" },
                },
                {
                    tag: "meta",
                    attrs: { property: "og:image", content: ogImage },
                },
                {
                    tag: "meta",
                    attrs: { property: "og:image:width", content: "1200" },
                },
                {
                    tag: "meta",
                    attrs: { property: "og:image:height", content: "630" },
                },
                {
                    tag: "meta",
                    attrs: {
                        property: "og:image:alt",
                        content: "Reqloom — your API is a graph. Test it like one.",
                    },
                },
                {
                    tag: "meta",
                    attrs: {
                        name: "twitter:card",
                        content: "summary_large_image",
                    },
                },
                {
                    tag: "meta",
                    attrs: { name: "twitter:image", content: ogImage },
                },
                // Matches the app's canvas colours so mobile browser chrome
                // blends with the page instead of flashing default white.
                {
                    tag: "meta",
                    attrs: {
                        name: "theme-color",
                        content: "#EAECF5",
                        media: "(prefers-color-scheme: light)",
                    },
                },
                {
                    tag: "meta",
                    attrs: {
                        name: "theme-color",
                        content: "#0F2226",
                        media: "(prefers-color-scheme: dark)",
                    },
                },
            ],
            sidebar: [
                {
                    label: "Start Here",
                    items: [
                        { label: "What is Reqloom?", slug: "start/overview" },
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
                        {
                            label: "Advanced operations",
                            slug: "schema/advanced-operations",
                        },
                        {
                            label: "Secrets, TLS & timeouts",
                            slug: "schema/secrets-and-transport",
                        },
                        { label: "Common pitfalls", slug: "schema/pitfalls" },
                        { label: "Cheat sheet", slug: "schema/cheatsheet" },
                    ],
                },
                {
                    label: "CLI",
                    items: [
                        { label: "Overview", slug: "cli/overview" },
                        { label: "reqloom run", slug: "cli/run" },
                        { label: "reqloom lint", slug: "cli/lint" },
                        { label: "reqloom import", slug: "cli/import" },
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
