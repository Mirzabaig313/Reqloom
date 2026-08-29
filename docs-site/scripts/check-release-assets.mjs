// Self-check for src/lib/release-assets.mjs.
//
//   node scripts/check-release-assets.mjs
//
// Asserts against the exact artifact names produced by
// .github/workflows/release.yml. If that workflow renames an artifact, this
// fails — which is the point: the download page would otherwise silently drop
// a platform.

import assert from "node:assert/strict";
import {
    pickRelease,
    groupAssets,
    displayName,
    megabytes,
    shortDigest,
} from "../src/lib/release-assets.mjs";

const TAG = "v0.1.0-alpha.1";
const asset = (name) => ({ name, size: 54_413_816, digest: "sha256:f3e90901901b34fe" });

// Every artifact release.yml uploads, in the order the workflow writes them.
const release = {
    tag_name: TAG,
    draft: false,
    prerelease: true,
    assets: [
        asset(`Reqloom-${TAG}-linux-x86_64.AppImage`),
        asset(`Reqloom-${TAG}-linux-x86_64.tar.gz`),
        asset(`Reqloom-${TAG}-macos.dmg`),
        asset(`Reqloom-${TAG}-macos.zip`),
        asset(`Reqloom-${TAG}-windows-x64-setup.exe`),
        asset(`Reqloom-${TAG}-windows-x64.zip`),
    ],
};

// --- pickRelease -----------------------------------------------------------

assert.equal(pickRelease([release]), release, "a versioned release is featured");

assert.equal(
    pickRelease([{ tag_name: "test4-v0.1.0-alpha.1", draft: false, assets: [] }]),
    null,
    "a scratch tag must never be featured",
);

assert.equal(
    pickRelease([{ tag_name: TAG, draft: true, assets: [] }]),
    null,
    "a draft must never be featured",
);

assert.equal(
    pickRelease([
        { tag_name: "test4-v0.1.0-alpha.1", draft: false, assets: [] },
        release,
    ]),
    release,
    "a versioned release is found past a scratch tag",
);

assert.equal(pickRelease([]), null, "no releases yields the fallback");
assert.equal(pickRelease(null), null, "a malformed payload yields the fallback");

// --- groupAssets -----------------------------------------------------------

const groups = groupAssets(release);
assert.deepEqual(
    groups.map((g) => g.id),
    ["macos", "linux", "windows"],
    "all three platforms are matched, in display order",
);

// Every asset must land in exactly one bucket — no drops, no duplicates.
const grouped = groups.flatMap((g) => g.assets.map((a) => a.name));
assert.equal(grouped.length, release.assets.length, "no asset is dropped");
assert.equal(new Set(grouped).size, grouped.length, "no asset is double-counted");

// The recommended file leads each bucket.
assert.match(groups[0].assets[0].name, /\.dmg$/, "macOS leads with the .dmg");
assert.match(groups[1].assets[0].name, /\.AppImage$/, "Linux leads with the AppImage");
assert.match(groups[2].assets[0].name, /setup\.exe$/, "Windows leads with the installer");

// A release missing a platform drops that card instead of rendering it empty.
assert.deepEqual(
    groupAssets({ tag_name: TAG, assets: [asset(`Reqloom-${TAG}-macos.dmg`)] }).map(
        (g) => g.id,
    ),
    ["macos"],
    "platforms with no asset are omitted",
);

assert.deepEqual(groupAssets(null), [], "no release yields no groups");

// --- formatting ------------------------------------------------------------

assert.equal(
    displayName(`Reqloom-${TAG}-macos.dmg`, TAG),
    "macos.dmg",
    "the repo/tag prefix is stripped from link text",
);
assert.equal(megabytes(54_413_816), "54.4 MB");
assert.equal(shortDigest("sha256:f3e90901901b34fe"), "f3e90901901b");
assert.equal(shortDigest(undefined), null, "a missing digest is tolerated");

console.log("check-release-assets: all assertions passed");
