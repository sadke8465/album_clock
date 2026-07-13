import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import {
  parseCsv,
  albumsFromCsv,
  pickShuffledAlbum,
  rgbaToRgb565,
  generatedFallbackFrame,
  pickLastfmImage,
  FRAME_BYTES,
  DISPLAY,
} from "../src/helpers.ts";

const csvPath = fileURLToPath(new URL("../../data/albums.csv", import.meta.url));
const csv = readFileSync(csvPath, "utf-8");

test("parseCsv handles quoted fields with embedded commas", () => {
  const rows = parseCsv(
    'Name,Artist,Album,URL,ID\n"Quando, Quando, Quando",Tony,Made In Italy,http://x,123\n'
  );
  assert.equal(rows.length, 1);
  assert.equal(rows[0]["name"], "Quando, Quando, Quando");
  assert.equal(rows[0]["id"], "123");
});

test("albumsFromCsv loads the real catalog", () => {
  const albums = albumsFromCsv(csv);
  assert.ok(albums.length > 200, `expected >200 albums, got ${albums.length}`);
  // Every row should carry an Apple id we can resolve artwork from.
  assert.ok(albums.every((a) => a.appleId.length > 0));
  // Hebrew metadata should survive intact.
  assert.ok(albums.some((a) => /[֐-׿]/.test(a.album || a.name)));
});

test("pickShuffledAlbum is stable within a window and avoids immediate repeats", () => {
  const albums = albumsFromCsv(csv);
  for (let w = 0; w < 500; w++) {
    const a = pickShuffledAlbum(albums, w);
    const again = pickShuffledAlbum(albums, w);
    const prev = pickShuffledAlbum(albums, w - 1);
    assert.deepEqual(a, again, "same window must be deterministic");
    assert.notDeepEqual(a, prev, `window ${w} repeated the previous album`);
  }
});

test("pickShuffledAlbum spreads across the catalog", () => {
  const albums = albumsFromCsv(csv);
  const seen = new Set<string>();
  for (let w = 0; w < 300; w++) {
    seen.add(pickShuffledAlbum(albums, w)!.appleId);
  }
  assert.ok(seen.size > 100, `poor spread: only ${seen.size} distinct albums`);
});

test("rgbaToRgb565 produces exactly FRAME_BYTES little-endian", () => {
  const rgba = new Uint8Array(DISPLAY * DISPLAY * 4);
  // Pure red pixel 0.
  rgba[0] = 255;
  rgba[1] = 0;
  rgba[2] = 0;
  rgba[3] = 255;
  const out = rgbaToRgb565(rgba, 1.0, 1.0);
  assert.equal(out.length, FRAME_BYTES);
  // Pure red in RGB565 = 0xF800, little-endian => [0x00, 0xF8].
  assert.equal(out[0], 0x00);
  assert.equal(out[1], 0xf8);
});

test("generatedFallbackFrame is a valid frame", () => {
  assert.equal(generatedFallbackFrame(0).length, FRAME_BYTES);
});

test("pickLastfmImage skips the placeholder cover", () => {
  const images = [
    { size: "small", "#text": "http://x/2a96cbd8b46e442fc41c2b86b821562f.png" },
  ];
  assert.equal(pickLastfmImage(images), "");
  const real = [{ size: "large", "#text": "http://x/real.png" }];
  assert.equal(pickLastfmImage(real), "http://x/real.png");
});
