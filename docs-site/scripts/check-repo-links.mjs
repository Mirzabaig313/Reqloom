// check-repo-links.mjs — fail if any docs page links to a path that is not
// published in the public repository.
//
//   node scripts/check-repo-links.mjs
//
// Several working documents are kept private via .gitignore and
// .git/info/exclude — the PRD, the engine requirement, the implementation
// tracker, validation/, doc/local/, .kiro/. A docs page linking to one of those
// is a dead end for every reader, and the site build cannot detect it because
// the URL is syntactically fine.
//
// This walks every GitHub blob/tree link in src/content/docs and asks git
// whether the target is tracked.

import { readdirSync, readFileSync, statSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, relative } from "node:path";

const DOCS = "src/content/docs";
const REPO_ROOT = "..";

/** Every GitHub blob/tree URL pointing into our own repo. */
const LINK_RE =
    /https:\/\/github\.com\/Mirzabaig313\/Reqloom\/(?:blob|tree)\/main\/([^)\s"'`]+)/g;

function walk(dir) {
    return readdirSync(dir).flatMap((entry) => {
        const full = join(dir, entry);
        return statSync(full).isDirectory() ? walk(full) : [full];
    });
}

function isTracked(path) {
    // A directory link is fine if git tracks anything beneath it.
    try {
        const out = execFileSync("git", ["ls-files", "--", path], {
            cwd: REPO_ROOT,
            encoding: "utf8",
        });
        return out.trim().length > 0;
    } catch {
        return false;
    }
}

const problems = [];
const checked = new Set();

for (const file of walk(DOCS)) {
    if (!/\.mdx?$/.test(file)) {
        continue;
    }
    const text = readFileSync(file, "utf8");
    for (const [, rawPath] of text.matchAll(LINK_RE)) {
        // Strip URL fragments and a trailing slash from directory links.
        const path = decodeURIComponent(rawPath.split("#")[0]).replace(/\/$/, "");
        if (!path) {
            continue;
        }
        const key = `${file}::${path}`;
        if (checked.has(key)) {
            continue;
        }
        checked.add(key);
        if (!isTracked(path)) {
            problems.push(`${relative(".", file)} → ${path}`);
        }
    }
}

if (problems.length > 0) {
    console.error(
        "check-repo-links: these pages link to paths that are NOT published:\n" +
            problems.map((p) => `  ${p}`).join("\n") +
            "\n\nEither link something tracked, or state the fact without a link.",
    );
    process.exit(1);
}

console.log(`check-repo-links: ${checked.size} repository links, all published`);
