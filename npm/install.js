#!/usr/bin/env node
"use strict";

const https = require("https");
const http = require("http");
const fs = require("fs");
const path = require("path");
const os = require("os");
const { execSync } = require("child_process");

const VERSION = require("./package.json").version;
const REPO = "sethdford/seaclaw";

function getPlatformKey() {
  const platform = os.platform();
  const arch = os.arch();

  if (platform === "darwin" && arch === "arm64") return "macos-aarch64";
  if (platform === "darwin" && arch === "x64") return "macos-aarch64";
  if (platform === "linux" && arch === "x64") return "linux-x86_64";

  throw new Error(`Unsupported platform: ${platform}-${arch}`);
}

function download(url) {
  return new Promise((resolve, reject) => {
    const client = url.startsWith("https") ? https : http;
    client
      .get(url, { headers: { "User-Agent": "seaclaw-npm" } }, (res) => {
        if (
          res.statusCode >= 300 &&
          res.statusCode < 400 &&
          res.headers.location
        ) {
          return download(res.headers.location).then(resolve, reject);
        }
        if (res.statusCode !== 200) {
          return reject(new Error(`HTTP ${res.statusCode} for ${url}`));
        }
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => resolve(Buffer.concat(chunks)));
        res.on("error", reject);
      })
      .on("error", reject);
  });
}

async function main() {
  const key = getPlatformKey();
  const binDir = path.join(__dirname, "bin");
  const binPath = path.join(binDir, "seaclaw");

  if (fs.existsSync(binPath)) {
    console.log("seaclaw binary already exists, skipping download.");
    return;
  }

  const tag = `v${VERSION}`;
  const url = `https://github.com/${REPO}/releases/download/${tag}/seaclaw-${key}.bin`;

  console.log(`Downloading seaclaw ${tag} for ${key}...`);

  try {
    const data = await download(url);
    fs.mkdirSync(binDir, { recursive: true });
    fs.writeFileSync(binPath, data);
    fs.chmodSync(binPath, 0o755);
    console.log(
      `Installed seaclaw to ${binPath} (${(data.length / 1024).toFixed(0)} KB)`,
    );
  } catch (err) {
    console.error(`Failed to download seaclaw: ${err.message}`);
    console.error(`You can build from source: https://github.com/${REPO}`);
    process.exit(1);
  }
}

main();
