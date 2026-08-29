// gen-og.mjs — render the social card to public/og-image.png.
//
// Run from docs-site/ after changing the tagline or the brand palette:
//
//   node scripts/gen-og.mjs
//
// The output is committed. It is deliberately not part of `npm run build`:
// the card changes maybe twice a year, and wiring sharp into every CI build
// buys nothing.

import sharp from "sharp";
import { mkdir } from "node:fs/promises";

const W = 1200;
const H = 630;

// Palette mirrors the app's dark "Morphous Abalone" theme (desktop/src/theming/Theme.cpp).
// `fill="none"` on the root matters: without it every open <path> gets an
// implicit black fill and the curved chain renders with a dark sliver.
const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" fill="none">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="${W}" y2="${H}" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#0F2226"/>
      <stop offset="1" stop-color="#071417"/>
    </linearGradient>
    <radialGradient id="wash" cx="1010" cy="90" r="560" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#18A6B4" stop-opacity="0.32"/>
      <stop offset="1" stop-color="#18A6B4" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="thread" x1="130" y1="560" x2="700" y2="470" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#2FBFCB"/>
      <stop offset="1" stop-color="#8BEDF0"/>
    </linearGradient>
    <linearGradient id="mark" x1="8" y1="8" x2="56" y2="56" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#8BEDF0"/>
      <stop offset="1" stop-color="#18A6B4"/>
    </linearGradient>
  </defs>

  <rect width="${W}" height="${H}" fill="url(#bg)"/>
  <rect width="${W}" height="${H}" fill="url(#wash)"/>

  <!-- Loom lattice -->
  <g stroke="#4FD4DA" stroke-width="1" opacity="0.08">
    ${Array.from({ length: 13 }, (_, i) => `<path d="M${i * 100} 0V${H}"/>`).join("")}
    ${Array.from({ length: 7 }, (_, i) => `<path d="M0 ${i * 105}H${W}"/>`).join("")}
  </g>

  <!-- Mark: same interlace as src/assets/logo.svg, scaled to 58px -->
  <g transform="translate(86 72) scale(0.9)">
    <g stroke="url(#mark)" stroke-width="5" stroke-linecap="round">
      <path d="M22 8.5V37.5"/><path d="M22 46.5V55.5"/>
      <path d="M42 8.5V17.5"/><path d="M42 26.5V55.5"/>
      <path d="M8.5 22H17.5"/><path d="M26.5 22H55.5"/>
      <path d="M8.5 42H55.5"/>
    </g>
    <circle cx="42" cy="42" r="9" fill="url(#mark)"/>
    <circle cx="42" cy="42" r="3.2" fill="#F4FDFD" opacity="0.95"/>
  </g>

  <text x="176" y="122" font-family="Geist, Inter, Helvetica, Arial, sans-serif"
        font-size="40" font-weight="600" fill="#F2FBFC" letter-spacing="-0.6">Reqloom</text>

  <text x="88" y="290" font-family="Geist, Inter, Helvetica, Arial, sans-serif"
        font-size="76" font-weight="600" fill="#F2FBFC" letter-spacing="-2.6">Your API is a graph.</text>
  <text x="88" y="378" font-family="Geist, Inter, Helvetica, Arial, sans-serif"
        font-size="76" font-weight="600" fill="#7DE6EA" letter-spacing="-2.6">Test it like one.</text>

  <text x="88" y="450" font-family="Geist, Inter, Helvetica, Arial, sans-serif"
        font-size="27" fill="#9DB9BE" letter-spacing="-0.3">Workflow-aware API testing that resolves request chains for you.</text>

  <!-- Resolved chain, sitting on its own baseline below the copy -->
  <path d="M120 556C210 556 244 520 320 520C396 520 430 484 506 484"
        stroke="url(#thread)" stroke-width="3.4" stroke-linecap="round" fill="none"/>
  <g fill="#4FD4DA">
    <circle cx="120" cy="556" r="7"/>
    <circle cx="320" cy="520" r="7"/>
  </g>
  <circle cx="506" cy="484" r="12" fill="#8BEDF0"/>

  <text x="552" y="493" font-family="Geist Mono, ui-monospace, Menlo, monospace"
        font-size="24" fill="#7C9CA3">reqloom run refund.approve</text>
</svg>`;

await mkdir("public", { recursive: true });
await sharp(Buffer.from(svg)).png({ compressionLevel: 9 }).toFile("public/og-image.png");
console.log(`wrote public/og-image.png (${W}x${H})`);
