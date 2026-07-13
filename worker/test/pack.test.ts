import test from "node:test";
import assert from "node:assert/strict";

import {
  fallbackIndex,
  hashString,
  parsePackHeader,
  parseByteRange,
  shuffledOrder,
  xorshift32,
} from "../src/pack.ts";
import type { PackManifest } from "../src/service-types.ts";

function header(count = 3, metadataLength = 120): Uint8Array {
  const bytes = new Uint8Array(16);
  bytes.set(new TextEncoder().encode("ACPK"));
  const view = new DataView(bytes.buffer);
  view.setUint16(4, 1, true);
  view.setUint16(6, 64, true);
  view.setUint16(8, 64, true);
  view.setUint16(10, count, true);
  view.setUint32(12, metadataLength, true);
  return bytes;
}

test("parsePackHeader validates the v1 binary contract", () => {
  assert.deepEqual(parsePackHeader(header()), {
    formatVersion: 1,
    width: 64,
    height: 64,
    count: 3,
    metadataLength: 120,
  });
  const corrupt = header();
  corrupt[0] = 0;
  assert.equal(parsePackHeader(corrupt), null);
});

test("xorshift fallback order is deterministic and complete", () => {
  assert.equal(xorshift32(123)(), xorshift32(123)());
  const order = shuffledOrder(234, hashString("catalog"));
  assert.equal(order.length, 234);
  assert.equal(new Set(order).size, 234);
});

test("fallbackIndex is stable and accepts explicit manual seeds", () => {
  const version = "a".repeat(64);
  const manifest: PackManifest = {
    api_version: 1,
    format_version: 1,
    version,
    sha256: version,
    size: 100000,
    count: 234,
    metadata_length: 1000,
    pack_key: `fallback/packs/${version}.acpk`,
    pack_url: `/v1/fallback/packs/${version}.acpk`,
    generated_at: 0,
    skipped_count: 0,
  };
  assert.equal(fallbackIndex(manifest, 44), fallbackIndex(manifest, 44));
  assert.equal(
    fallbackIndex(manifest, 44, "manual"),
    fallbackIndex(manifest, 999, "manual")
  );
});

test("fallback pack range parsing supports resumable downloads", () => {
  assert.deepEqual(parseByteRange("bytes=8192-", 20000), {
    offset: 8192,
    length: 11808,
  });
  assert.deepEqual(parseByteRange("bytes=10-19", 20000), {
    offset: 10,
    length: 10,
  });
  assert.equal(parseByteRange("bytes=20000-", 20000), null);
  assert.equal(parseByteRange("items=1-2", 20000), null);
});
