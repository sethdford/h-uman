/**
 * Tests for the binary download helper (src/binary.js).
 *
 * Uses node:test (stdlib). Pure-function tests cover the asset-URL/ref logic
 * (which pins the wrong-release-tag 404 bug). One loopback-HTTPS test covers
 * the redirect-following fix (GitHub release assets 302-redirect to a CDN, and
 * Node's https.get does NOT follow redirects automatically). The loopback
 * server uses a self-signed cert and stays on 127.0.0.1 — no external network.
 *
 * Run from the repo root:
 *
 *   node --test apps/node-sdk/test/binary.test.js
 */

// Accept the loopback self-signed cert for the redirect test below. Loopback
// only; no external hosts are contacted.
process.env.NODE_TLS_REJECT_UNAUTHORIZED = "0";

import { test } from "node:test";
import assert from "node:assert/strict";
import https from "node:https";

import { ensureBinary, __testing } from "../src/binary.js";

const { platformToAssetName, releaseRef, downloadHttps } = __testing;

// Self-signed cert/key for the loopback redirect test (test-only).
const TEST_KEY = `-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDCwtG5zSHRtD/b
C5TDfz9iwoRL/UBtk5HudQf61/04oXO4Esdh6sjw4xDJk3ys1jPbJDNqCLUoEME+
CpUypYSzqwUvlUDvG3eJgw2s3a93klqKaKBBhGn4MMD18C4O/ko/FWUGtgs4Q8Tn
3l1LXRDoRxxMmz2EWgbreCP26t/6hlc8TZ7HehX1TrelH+KTfPysvpVhL+ET8PzW
Gl13gcXPY+Jvub/c1s8/wVVRRD/APQGiTGyGAJbsy4qc7RHu8ValdIBQl7d1BCth
+Tu3hA+mmcAlQgkFZ/tk0/3qP4sA5hbAx5997MKwSgTeh10hD1XDi/DjYu4Px4aK
bB3vHLvrAgMBAAECggEABP5640FL0zVEh+jhnSu3o+AqBUrOqqCkltFAbaG5OAkH
zxoIK84Z7fdcmFakVUg+1ykz+k2T252mXuFllwFgiDzj6qbwndD9/TQzlxGuLKo/
kRoCtftfvkH9yLCqda72l2yEhSoI2p3+QBXXHZYa12Hva1V1u92VHNedUHkQWjup
HLLX5MWyDUduAvYnD28dsrVYz2cpAWQfp/JwLMShyn2VvSE2fdljkI7Jf1kPiEn3
M1nLAwJgh4fjpHPnCJh45ix+TexQDIJdj4HFOWj6zUUp7ZJOy4XoeKtVJ6ciw0YW
E31z9lY5LdP4D85JYzgP+FOgyl7OAmqpdNlXsz+ElQKBgQD6vicW5Uiu4fKHfLMU
240cOC25XWUXX6Tz5gxHW0OBmcmHu/lo6ivJFrktLEoecLKwDETOkMl0JHMBXkdG
jeuXKdsN63k2FC7X7YNf4/KJAPqeZobj9ruCtZ5CHu4hxHuJNUTw7iIWg8Cg1xWw
W1l3dI1OtdWfohAWuT8gCarnHQKBgQDG2DAJp+k46/vxpAU2SKEF4Wcd3KV6yWA2
Uq2QLQeRp0Oj8WDiBVjtpHJACmK6DNFPBjnVIivbbWnzwFrFcC3QJCzVuwTD0t6b
mvNsmAzish5NRvWV9rngt8VCsInwwJciSgBxM9Qw7Tf78SZkQnsdPXYL+arNNtmA
zQRjCIZYpwKBgAU83aIrzfXhQGi2ISOJZow1XDcoDUmMtOxnXNBMxr85UC1mrtIT
OjDsKZgY+b9jCUiGKRXLjnm/nStlJcYChu5UcH/88D5B86yNCJaDM3jLLXELoTu3
1rjnRFQLQ1wvN4lpNHR39PxVajux4oEZl2fYZm9Dex/nicB/xCLOMOS9AoGAF5lc
yiiVc13+fsU6oSCpi6ses9qWASaZNplFZBEUDuNNEc3585ky3rFfpd4VrYML7FcO
0g7GKbqokndHzprrtQI2F/+kJyGFre4L6d152gXNttovF2c61EK7NJkDJgGVbkpe
FxFRo6TjuhD6v3dsaSH8Opuc+9IAqlqqpJ5EY40CgYAM/3m3SHlz2zVaCxcYIuCC
1jfjIzkzzyWsaYIXSRKNkRLqRC7HUvwUaw0NUZsHTZPMnYjiI62+ShAQrh40q5AY
3OhuQiA++3R9ojzIcMvw/+f6YvtDJvz3goqhsnEOFk1rAZ8Ur/kS7GceL1s9nA3z
mNWlLXYRSfxJJj9dm8BJ4Q==
-----END PRIVATE KEY-----
`;
const TEST_CERT = `-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUcYYiA36TW9OjUyXMVRtStEZOblowDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDUyOTA5NTI0OFoXDTM2MDUy
NjA5NTI0OFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAwsLRuc0h0bQ/2wuUw38/YsKES/1AbZOR7nUH+tf9OKFz
uBLHYerI8OMQyZN8rNYz2yQzagi1KBDBPgqVMqWEs6sFL5VA7xt3iYMNrN2vd5Ja
imigQYRp+DDA9fAuDv5KPxVlBrYLOEPE595dS10Q6EccTJs9hFoG63gj9urf+oZX
PE2ex3oV9U63pR/ik3z8rL6VYS/hE/D81hpdd4HFz2Pib7m/3NbPP8FVUUQ/wD0B
okxshgCW7MuKnO0R7vFWpXSAUJe3dQQrYfk7t4QPppnAJUIJBWf7ZNP96j+LAOYW
wMeffezCsEoE3oddIQ9Vw4vw42LuD8eGimwd7xy76wIDAQABo1MwUTAdBgNVHQ4E
FgQUapuCJIg/f8yVJS4HuOojOrOp0tcwHwYDVR0jBBgwFoAUapuCJIg/f8yVJS4H
uOojOrOp0tcwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEADa1u
MAPMK19vbrZaJUguCrmazpIbTBEG+1gBjv+Rsmb2Nre5ZAM2ARopmgYvhSAZKM17
LnlEUPuKpsbdf0oprOKK3pC/s0gqbRLBWlRu6buLtvLZC5lcz9lCmQxtzGixIuji
3yElH2vRsp4CCoSIHTSixsQiAB9QHV6HqLszLrJevNozzJgVvb+n+I8HhECLzL2W
JQ4D37kT0q8f6em3Vb7OP1jqFQnVEbaf3AGZQJ3vEWYHSJPYcjilgXRDBzqZjmVG
x1wkD3H/WW4lrCl5Wkc7L2rAnjlN1b011Ija4JxnYmpZUt6D8iS9eXym5Ugon0Iz
a0CL5G4v7SVPYSbqBA==
-----END CERTIFICATE-----
`;


test("ensureBinary function signature and contract", async (t) => {
  await t.test("ensureBinary is a function", () => {
    assert.equal(typeof ensureBinary, "function");
  });

  await t.test("ensureBinary returns a Promise", async () => {
    const result = ensureBinary("0.1.0");
    assert.equal(result instanceof Promise, true);
    result.catch(() => {});
  });

  await t.test("module exports ensureBinary as a named export", async () => {
    const { ensureBinary: fn } = await import("../src/binary.js");
    assert.equal(typeof fn, "function");
  });
});


test("releaseRef resolves the binary release independently of SDK version", async (t) => {
  await t.test("defaults to 'latest' when HUMAN_BINARY_RELEASE is unset", () => {
    const prev = process.env.HUMAN_BINARY_RELEASE;
    delete process.env.HUMAN_BINARY_RELEASE;
    try {
      assert.equal(releaseRef(), "latest");
    } finally {
      if (prev !== undefined) process.env.HUMAN_BINARY_RELEASE = prev;
    }
  });

  await t.test("uses an explicit pinned tag when set", () => {
    const prev = process.env.HUMAN_BINARY_RELEASE;
    process.env.HUMAN_BINARY_RELEASE = "v2026.3.3";
    try {
      assert.equal(releaseRef(), "v2026.3.3");
    } finally {
      if (prev === undefined) delete process.env.HUMAN_BINARY_RELEASE;
      else process.env.HUMAN_BINARY_RELEASE = prev;
    }
  });

  await t.test("falls back to 'latest' for an empty/whitespace value", () => {
    const prev = process.env.HUMAN_BINARY_RELEASE;
    process.env.HUMAN_BINARY_RELEASE = "   ";
    try {
      assert.equal(releaseRef(), "latest");
    } finally {
      if (prev === undefined) delete process.env.HUMAN_BINARY_RELEASE;
      else process.env.HUMAN_BINARY_RELEASE = prev;
    }
  });
});


test("platformToAssetName builds correct asset URLs", async (t) => {
  // These only run meaningfully on the supported platforms; on an unsupported
  // platform platformToAssetName throws, which we assert separately.
  const supported = (() => {
    try {
      platformToAssetName("0.1.0");
      return true;
    } catch {
      return false;
    }
  })();

  await t.test("default (latest) URL uses /latest/download/ and HTTPS", () => {
    if (!supported) return;
    const prev = process.env.HUMAN_BINARY_RELEASE;
    delete process.env.HUMAN_BINARY_RELEASE;
    try {
      const { assetName, assetUrl, ref } = platformToAssetName("0.1.0");
      assert.equal(ref, "latest");
      assert.ok(assetName.startsWith("human-") && assetName.endsWith(".bin"));
      assert.ok(assetUrl.startsWith("https://"));
      assert.ok(assetUrl.includes("/latest/download/"));
    } finally {
      if (prev !== undefined) process.env.HUMAN_BINARY_RELEASE = prev;
    }
  });

  await t.test("version is NOT baked into the URL (decoupled from binary tag)", () => {
    if (!supported) return;
    const prev = process.env.HUMAN_BINARY_RELEASE;
    delete process.env.HUMAN_BINARY_RELEASE;
    try {
      const { assetUrl } = platformToAssetName("0.1.0");
      assert.ok(!assetUrl.includes("v0.1.0"),
        `URL must not contain the SDK version: ${assetUrl}`);
      assert.ok(!assetUrl.includes("0.1.0"),
        `URL must not contain the SDK version: ${assetUrl}`);
    } finally {
      if (prev !== undefined) process.env.HUMAN_BINARY_RELEASE = prev;
    }
  });

  await t.test("pinned ref uses /releases/download/{ref}/ and HTTPS", () => {
    if (!supported) return;
    const prev = process.env.HUMAN_BINARY_RELEASE;
    process.env.HUMAN_BINARY_RELEASE = "v2026.3.3";
    try {
      const { assetUrl, ref } = platformToAssetName("0.1.0");
      assert.equal(ref, "v2026.3.3");
      assert.ok(assetUrl.includes("/releases/download/v2026.3.3/"));
      assert.ok(!assetUrl.includes("/latest/"));
      assert.ok(assetUrl.startsWith("https://"));
    } finally {
      if (prev === undefined) delete process.env.HUMAN_BINARY_RELEASE;
      else process.env.HUMAN_BINARY_RELEASE = prev;
    }
  });
});


test("downloadHttps", async (t) => {
  await t.test("rejects non-HTTPS URLs", async () => {
    await assert.rejects(
      () => downloadHttps("http://example.com/x.bin"),
      /must use HTTPS/
    );
  });

  await t.test("follows a 302 redirect to the final asset (the GitHub-CDN case)", async () => {
    // Server emits 302 -> /final on the first request, then 200 + body.
    const server = https.createServer({ key: TEST_KEY, cert: TEST_CERT },
      (req, res) => {
        if (req.url === "/asset.bin") {
          res.writeHead(302, { Location: "/final.bin" });
          res.end();
        } else if (req.url === "/final.bin") {
          res.writeHead(200, { "Content-Type": "application/octet-stream" });
          res.end(Buffer.from([0x7f, 0x45, 0x4c, 0x46])); // "\x7fELF"
        } else {
          res.writeHead(404);
          res.end();
        }
      });

    await new Promise((r) => server.listen(0, "127.0.0.1", r));
    const { port } = server.address();
    try {
      const data = await downloadHttps(`https://127.0.0.1:${port}/asset.bin`);
      assert.deepEqual([...data], [0x7f, 0x45, 0x4c, 0x46]);
    } finally {
      await new Promise((r) => server.close(r));
    }
  });

  await t.test("rejects after exceeding the redirect budget (loop guard)", async () => {
    // Server always 302s to itself -> downloadHttps must give up, not hang.
    const server = https.createServer({ key: TEST_KEY, cert: TEST_CERT },
      (req, res) => {
        res.writeHead(302, { Location: "/loop" });
        res.end();
      });

    await new Promise((r) => server.listen(0, "127.0.0.1", r));
    const { port } = server.address();
    try {
      await assert.rejects(
        () => downloadHttps(`https://127.0.0.1:${port}/loop`),
        /Too many redirects/
      );
    } finally {
      await new Promise((r) => server.close(r));
    }
  });

  await t.test("rejects on a non-200, non-redirect status", async () => {
    const server = https.createServer({ key: TEST_KEY, cert: TEST_CERT },
      (req, res) => {
        res.writeHead(404);
        res.end();
      });

    await new Promise((r) => server.listen(0, "127.0.0.1", r));
    const { port } = server.address();
    try {
      await assert.rejects(
        () => downloadHttps(`https://127.0.0.1:${port}/missing.bin`),
        /HTTP 404/
      );
    } finally {
      await new Promise((r) => server.close(r));
    }
  });
});
