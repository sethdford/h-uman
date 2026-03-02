import sharp from "sharp";
import { mkdirSync, writeFileSync } from "fs";
import { join, dirname } from "path";
import { fileURLToPath } from "url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const pub = join(__dirname, "..", "public");

const clawSvg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128"><path fill-rule="evenodd" d="M81 36 64 0 47 36l-1 2-9-10a6 6 0 0 0-9 9l10 10h-2L0 64l36 17h2L28 91a6 6 0 1 0 9 9l9-10 1 2 17 36 17-36v-2l9 10a6 6 0 1 0 9-9l-9-9 2-1 36-17-36-17-2-1 9-9a6 6 0 1 0-9-9l-9 10v-2Zm-17 2-2 5c-4 8-11 15-19 19l-5 2 5 2c8 4 15 11 19 19l2 5 2-5c4-8 11-15 19-19l5-2-5-2c-8-4-15-11-19-19l-2-5Z" clip-rule="evenodd"/><path d="M118 19a6 6 0 0 0-9-9l-3 3a6 6 0 1 0 9 9l3-3Zm-96 4c-2 2-6 2-9 0l-3-3a6 6 0 1 1 9-9l3 3c3 2 3 6 0 9Zm0 82c-2-2-6-2-9 0l-3 3a6 6 0 1 0 9 9l3-3c3-2 3-6 0-9Zm96 4a6 6 0 0 1-9 9l-3-3a6 6 0 1 1 9-9l3 3Z"/></svg>`;

const whiteClaw = clawSvg.replace(
  /<\/svg>/,
  "<style>path{fill:#fff}</style></svg>",
);
const coralClaw = clawSvg.replace(
  /<\/svg>/,
  "<style>path{fill:#F97066}</style></svg>",
);

async function generateFavicons() {
  for (const size of [16, 32, 48]) {
    await sharp(Buffer.from(coralClaw))
      .resize(size, size)
      .png()
      .toFile(join(pub, `favicon-${size}.png`));
  }
  await sharp(Buffer.from(coralClaw))
    .resize(180, 180)
    .png()
    .toFile(join(pub, "apple-touch-icon.png"));
  console.log("Favicons generated.");
}

async function generateOgImage() {
  const w = 1200,
    h = 630;
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}">
    <defs>
      <radialGradient id="g1" cx="30%" cy="40%" r="60%">
        <stop offset="0%" stop-color="rgba(249,112,102,0.15)"/>
        <stop offset="100%" stop-color="transparent"/>
      </radialGradient>
      <radialGradient id="g2" cx="75%" cy="65%" r="50%">
        <stop offset="0%" stop-color="rgba(59,130,246,0.08)"/>
        <stop offset="100%" stop-color="transparent"/>
      </radialGradient>
    </defs>
    <rect width="${w}" height="${h}" fill="#0a0a0a"/>
    <rect width="${w}" height="${h}" fill="url(#g1)"/>
    <rect width="${w}" height="${h}" fill="url(#g2)"/>

    <g transform="translate(100,200)">
      ${whiteClaw.replace('viewBox="0 0 128 128"', 'viewBox="0 0 128 128" width="72" height="72"')}
    </g>

    <text x="200" y="230" fill="#f5f5f7" font-family="system-ui,-apple-system,sans-serif" font-size="56" font-weight="700" letter-spacing="-2">Bring AI to every</text>
    <text x="200" y="295" fill="#f5f5f7" font-family="system-ui,-apple-system,sans-serif" font-size="56" font-weight="700" letter-spacing="-2">device on Earth.</text>

    <text x="200" y="360" fill="#a1a1a6" font-family="system-ui,-apple-system,sans-serif" font-size="22">282 KB binary · 50+ providers · 20 channels · 47 tools</text>

    <text x="200" y="420" fill="#F97066" font-family="system-ui,-apple-system,sans-serif" font-size="20" font-weight="600">SeaClaw</text>
    <text x="310" y="420" fill="#6e6e73" font-family="system-ui,-apple-system,sans-serif" font-size="18">Pure C11 · Zero dependencies · MIT licensed</text>

    <line x1="200" y1="490" x2="1000" y2="490" stroke="rgba(255,255,255,0.06)" stroke-width="1"/>
    <text x="200" y="530" fill="#86868b" font-family="ui-monospace,monospace" font-size="14">sethdford.github.io/seaclaw</text>
  </svg>`;

  await sharp(Buffer.from(svg)).png().toFile(join(pub, "og-image.png"));
  console.log("OG image generated.");
}

await generateFavicons();
await generateOgImage();
console.log("All assets generated.");
