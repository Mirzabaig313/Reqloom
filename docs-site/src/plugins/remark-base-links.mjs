// Rewrites author-written root-absolute links (e.g. `/start/tour/`) so they
// include the configured Astro `base`. Astro applies `base` to its own
// generated links (nav, sidebar) but NOT to absolute links written by hand in
// Markdown/MDX content, so those 404 on project-page deploys served under a
// subpath. This plugin closes that gap at build time, keeping the content
// source clean and base-agnostic.
import { visit } from "unist-util-visit";

/// Join `base` and an absolute `target` without producing a double slash.
function withBase(base, target) {
    const normalizedBase = base.endsWith("/") ? base.slice(0, -1) : base;
    return `${normalizedBase}${target}`;
}

/// @param {{ base?: string }} options  The site base (defaults to "/").
export default function remarkBaseLinks(options = {}) {
    const base = options.base ?? "/";

    // Root-page deploys need no rewriting.
    if (base === "/" || base === "") {
        return () => { };
    }

    const prefix = base.endsWith("/") ? base : `${base}/`;

    return (tree) => {
        visit(tree, ["link", "image", "definition"], (node) => {
            const url = node.url;
            if (typeof url !== "string") {
                return;
            }
            // Only rewrite root-absolute, in-site links. Skip protocol-relative
            // (`//host`), external (`https://`), anchors, and links already
            // carrying the base prefix.
            if (!url.startsWith("/") || url.startsWith("//")) {
                return;
            }
            if (url.startsWith(prefix)) {
                return;
            }
            node.url = withBase(base, url);
        });
    };
}
