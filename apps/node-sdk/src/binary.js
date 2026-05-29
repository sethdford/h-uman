/**
 * @human/hula-sdk binary helper — version-pinned binary auto-download.
 *
 * Exports ensureBinary(version) to automatically download the correct
 * precompiled human binary for the current platform. Downloads are
 * cached under a platform-specific user cache dir and verified to be
 * executable before use.
 *
 * Supported platforms:
 *   - Linux x86_64
 *   - Linux aarch64
 *   - macOS aarch64
 *
 * Example:
 *   import { ensureBinary } from "@human/hula-sdk/binary";
 *   const humanPath = await ensureBinary("0.1.0");
 *   // Use humanPath to run the human CLI
 */

import { promises as fs } from "node:fs";
import { homedir, platform, arch, tmpdir } from "node:os";
import https from "node:https";
import path from "node:path";
import { URL } from "node:url";


/**
 * Get the platform-specific cache directory for the human binary.
 * Uses XDG_CACHE_HOME on Linux, ~/Library/Caches on macOS, or ~/.cache.
 */
function getCacheDir() {
  let cacheBase;
  if (process.env.XDG_CACHE_HOME) {
    cacheBase = process.env.XDG_CACHE_HOME;
  } else if (platform() === "darwin") {
    cacheBase = path.join(homedir(), "Library", "Caches");
  } else {
    cacheBase = path.join(homedir(), ".cache");
  }
  return path.join(cacheBase, "human-sdk");
}


/**
 * Map the current platform to the release asset name and download URL.
 *
 * The h-uman release workflow produces binaries named:
 *   human-linux-x86_64.bin
 *   human-linux-aarch64.bin
 *   human-macos-aarch64.bin
 *
 * @param {string} version - The SDK version (e.g., "0.1.0")
 * @returns {{assetName: string, assetUrl: string}}
 * @throws {Error} if the platform is not supported
 */
function platformToAssetName(version) {
  const sys = platform();
  let machine = arch();

  // Normalize machine names
  if (machine === "x64") machine = "x86_64";
  if (machine === "arm64") machine = "aarch64";

  let assetName;
  if (sys === "linux" && machine === "x86_64") {
    assetName = "human-linux-x86_64.bin";
  } else if (sys === "linux" && machine === "aarch64") {
    assetName = "human-linux-aarch64.bin";
  } else if (sys === "darwin" && machine === "aarch64") {
    assetName = "human-macos-aarch64.bin";
  } else {
    throw new Error(
      `Unsupported platform: ${sys} ${machine}. ` +
      "Supported: Linux x86_64, Linux aarch64, macOS aarch64. " +
      "Build the human binary from source: " +
      "https://github.com/sethdford/h-uman"
    );
  }

  // Release assets are published to GitHub Releases
  const baseUrl = "https://github.com/sethdford/h-uman/releases/download";
  const tag = `v${version}`;
  const assetUrl = `${baseUrl}/${tag}/${assetName}`;

  return { assetName, assetUrl };
}


/**
 * Download from HTTPS URL with built-in error handling.
 *
 * @param {string} url - Must be HTTPS only
 * @returns {Promise<Buffer>} The downloaded data
 * @throws {Error} if download fails or URL is not HTTPS
 */
function downloadHttps(url) {
  return new Promise((resolve, reject) => {
    const parsedUrl = new URL(url);
    if (parsedUrl.protocol !== "https:") {
      reject(new Error(
        `URL must use HTTPS, got ${parsedUrl.protocol}: ${url}`
      ));
      return;
    }

    const request = https.get(url, (response) => {
      if (response.statusCode !== 200) {
        reject(new Error(
          `HTTP ${response.statusCode} downloading ${url}`
        ));
        return;
      }

      const chunks = [];
      response.on("data", (chunk) => chunks.push(chunk));
      response.on("end", () => resolve(Buffer.concat(chunks)));
      response.on("error", reject);
    });

    request.on("error", reject);
    request.setTimeout(30000, () => {
      request.destroy();
      reject(new Error(`Timeout downloading ${url}`));
    });
  });
}


/**
 * Download and cache the human binary for the current platform.
 *
 * Checks the cache for an existing binary; if not present or invalid,
 * downloads from the release URL (HTTPS only). The binary is verified
 * to be executable before return. Subsequent calls for the same version
 * use the cached copy (no re-download).
 *
 * @param {string} version - The SDK version to download (default: "0.1.0")
 * @returns {Promise<string>} Absolute path to the cached binary
 * @throws {Error} if the platform is unsupported or download fails
 */
export async function ensureBinary(version = "0.1.0") {
  const { assetName, assetUrl } = platformToAssetName(version);
  const cacheDir = getCacheDir();
  const cachePath = path.join(cacheDir, `human-${version}`);

  // Create cache directory if it doesn't exist
  try {
    await fs.mkdir(cacheDir, { recursive: true });
  } catch (err) {
    throw new Error(`Failed to create cache directory ${cacheDir}: ${err.message}`);
  }

  // If cached binary exists and is executable, return it.
  try {
    const stat = await fs.stat(cachePath);
    // Check if file is executable (mode & 0o111 != 0)
    if ((stat.mode & 0o111) !== 0) {
      return cachePath;
    }
  } catch {
    // File doesn't exist or isn't accessible; continue to download
  }

  // Download from the release URL (HTTPS only).
  let binaryData;
  try {
    binaryData = await downloadHttps(assetUrl);
  } catch (err) {
    throw new Error(
      `Failed to download human binary from ${assetUrl}: ${err.message}. ` +
      "Ensure the release exists and your network allows HTTPS."
    );
  }

  // Write to cache and make executable
  try {
    await fs.writeFile(cachePath, binaryData);
    // Make executable: user + group can read/execute, others can read
    await fs.chmod(cachePath, 0o755);
  } catch (err) {
    throw new Error(
      `Failed to write cached binary to ${cachePath}: ${err.message}`
    );
  }

  // Verify it's executable before returning
  try {
    const stat = await fs.stat(cachePath);
    if ((stat.mode & 0o111) === 0) {
      throw new Error("Downloaded binary is not executable");
    }
  } catch (err) {
    throw new Error(
      `Failed to verify binary permissions at ${cachePath}: ${err.message}`
    );
  }

  return cachePath;
}
