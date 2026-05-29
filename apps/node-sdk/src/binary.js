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
 * Resolve which binary release to download from.
 *
 * The SDK's own semver (e.g. 0.1.0) is DECOUPLED from the h-uman binary
 * releases, which are date-tagged (e.g. v2026.3.3). Building a tag from the
 * SDK version (v0.1.0) points at a release that does not carry the
 * human-*.bin assets and 404s. So we default to the "latest" published
 * release, which always carries the current human-*.bin assets. Operators
 * who need a reproducible pin can set HUMAN_BINARY_RELEASE to an explicit
 * tag (e.g. "v2026.3.3").
 *
 * @returns {string} the release ref ("latest" or an explicit tag)
 */
function releaseRef() {
  const ref = (process.env.HUMAN_BINARY_RELEASE || "").trim();
  return ref || "latest";
}


/**
 * Map the current platform to the release asset name and download URL.
 *
 * The h-uman release workflow produces binaries named:
 *   human-linux-x86_64.bin
 *   human-linux-aarch64.bin
 *   human-macos-aarch64.bin
 *
 * @param {string} version - The SDK version (accepted for API compatibility;
 *   the binary release is selected by releaseRef(), not this value)
 * @returns {{assetName: string, assetUrl: string, ref: string}}
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

  // Release assets are published to GitHub Releases. SDK semver is decoupled
  // from the binary release tag, so resolve the ref independently.
  //   latest : https://github.com/{owner}/{repo}/releases/latest/download/{asset}
  //   pinned : https://github.com/{owner}/{repo}/releases/download/{ref}/{asset}
  const repoBase = "https://github.com/sethdford/h-uman/releases";
  const ref = releaseRef();
  const assetUrl = ref === "latest"
    ? `${repoBase}/latest/download/${assetName}`
    : `${repoBase}/download/${ref}/${assetName}`;

  return { assetName, assetUrl, ref };
}


// GitHub release-asset URLs 302-redirect to a signed CDN host
// (release-assets.githubusercontent.com), and the /latest/download/ alias
// 302-redirects to the current tag's asset. Node's https.get does NOT follow
// redirects automatically (unlike Python urllib), so we follow them ourselves.
// Each hop is re-validated as HTTPS to preserve the HTTPS-only project rule.
const MAX_REDIRECTS = 5;

/**
 * Download from an HTTPS URL, following redirects (HTTPS-only at each hop).
 *
 * @param {string} url - Must be HTTPS only (enforced on every hop)
 * @param {number} [redirectsLeft] - Remaining redirect budget
 * @returns {Promise<Buffer>} The downloaded data
 * @throws {Error} if download fails, URL is not HTTPS, or too many redirects
 */
function downloadHttps(url, redirectsLeft = MAX_REDIRECTS) {
  return new Promise((resolve, reject) => {
    const parsedUrl = new URL(url);
    if (parsedUrl.protocol !== "https:") {
      reject(new Error(
        `URL must use HTTPS, got ${parsedUrl.protocol}: ${url}`
      ));
      return;
    }

    const request = https.get(url, (response) => {
      const status = response.statusCode;

      // Follow 3xx redirects (301/302/303/307/308) to the Location header.
      if (status >= 300 && status < 400 && response.headers.location) {
        // Drain the redirect response so the socket can be reused/closed.
        response.resume();
        if (redirectsLeft <= 0) {
          reject(new Error(`Too many redirects downloading ${url}`));
          return;
        }
        // Location may be relative; resolve against the current URL.
        const nextUrl = new URL(response.headers.location, url).toString();
        downloadHttps(nextUrl, redirectsLeft - 1).then(resolve, reject);
        return;
      }

      if (status !== 200) {
        response.resume();
        reject(new Error(`HTTP ${status} downloading ${url}`));
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
 * @param {string} version - The SDK version (default: "0.1.0"). Accepted for
 *   API compatibility; the binary release is selected by HUMAN_BINARY_RELEASE
 *   (default "latest"), NOT this value.
 * @returns {Promise<string>} Absolute path to the cached binary
 * @throws {Error} if the platform is unsupported or download fails
 */
export async function ensureBinary(version = "0.1.0") {
  const { assetUrl, ref } = platformToAssetName(version);
  const cacheDir = getCacheDir();
  // Cache is keyed on the release ref (not the SDK version) because the binary
  // is selected by the ref. Pinned refs each get an immutable cache slot.
  const cachePath = path.join(cacheDir, `human-${ref}`);

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


// Internal helpers exported for unit testing. Not part of the public API;
// consumers should use ensureBinary().
export const __testing = { platformToAssetName, releaseRef, downloadHttps };
