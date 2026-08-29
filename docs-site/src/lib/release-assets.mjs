// Release-asset selection: which published file belongs to which platform, and
// which one a reader should be offered first.
//
// Split out of DownloadCards.astro so it can be exercised without a build —
// see scripts/check-release-assets.mjs. The asset names it matches are produced
// by .github/workflows/release.yml; if that workflow renames an artifact, the
// check fails and this file needs the same edit.

export const REPO = "Mirzabaig313/Reqloom";
export const RELEASES_URL = `https://github.com/${REPO}/releases`;

/** Platform buckets, matched against the asset filename. */
export const PLATFORMS = [
    {
        id: "macos",
        label: "macOS",
        note: "Apple silicon and Intel",
        match: (n) => n.endsWith(".dmg") || n.endsWith("macos.zip"),
        preferred: (n) => n.endsWith(".dmg"),
    },
    {
        id: "linux",
        label: "Linux",
        note: "x86_64",
        match: (n) => n.endsWith(".AppImage") || n.includes("linux-x86_64.tar.gz"),
        preferred: (n) => n.endsWith(".AppImage"),
    },
    {
        id: "windows",
        label: "Windows",
        note: "x64",
        match: (n) => n.endsWith(".exe") || n.includes("windows-x64.zip"),
        preferred: (n) => n.endsWith(".exe"),
    },
];

/**
 * Pick the release to feature.
 *
 * Only versioned tags qualify. This deliberately excludes scratch tags such as
 * `test4-v0.1.0-alpha.1` so a throwaway build can never become the headline
 * download. Prereleases DO qualify — `v0.1.0-alpha.1` is the current shipping
 * line, and GitHub's own `/releases/latest` would skip it.
 */
export function pickRelease(releases) {
    if (!Array.isArray(releases)) {
        return null;
    }
    return (
        releases.find(
            (r) => r && !r.draft && /^v\d+\.\d+\.\d+/.test(r.tag_name ?? ""),
        ) ?? null
    );
}

/**
 * Group a release's assets by platform, preferred file first. Platforms with no
 * matching asset are dropped rather than rendered empty.
 */
export function groupAssets(release) {
    if (!release) {
        return [];
    }
    const assets = Array.isArray(release.assets) ? release.assets : [];
    return PLATFORMS.map((platform) => ({
        ...platform,
        assets: assets
            .filter((a) => platform.match(a.name))
            .sort(
                (a, b) =>
                    Number(platform.preferred(b.name)) -
                    Number(platform.preferred(a.name)),
            ),
    })).filter((group) => group.assets.length > 0);
}

/** Strip the repo/tag prefix so the link text is just the interesting part. */
export function displayName(assetName, tagName) {
    return assetName
        .replace(`Reqloom-${tagName}-`, "")
        .replace(`Reqloom-${tagName}`, "")
        .replace("Reqloom-", "");
}

export const megabytes = (bytes) => `${(bytes / 1_000_000).toFixed(1)} MB`;

/** Trim the `sha256:` prefix the API returns. */
export const shortDigest = (digest) =>
    typeof digest === "string" && digest.startsWith("sha256:")
        ? digest.slice(7, 19)
        : null;
