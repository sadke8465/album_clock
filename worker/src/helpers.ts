// Pure, dependency-free helpers. Kept isolated from the fetch handler and the
// photon/WASM image path so they can be unit-tested under plain Node.

export const DISPLAY = 64;
export const FRAME_BYTES = DISPLAY * DISPLAY * 2; // 8192

// Last.fm's "no cover" placeholder image hash. Treat those URLs as empty.
const LASTFM_PLACEHOLDER = "2a96cbd8b46e442fc41c2b86b821562f";

export interface Album {
  name: string;
  artist: string;
  album: string;
  appleId: string;
}

/**
 * Minimal RFC-4180-ish CSV parser: handles quoted fields, embedded commas and
 * escaped double-quotes. Header row is required. Blank lines and `#` comments
 * are skipped.
 */
export function parseCsv(text: string): Record<string, string>[] {
  const rows: string[][] = [];
  let field = "";
  let record: string[] = [];
  let inQuotes = false;

  const pushField = () => {
    record.push(field);
    field = "";
  };
  const pushRecord = () => {
    pushField();
    rows.push(record);
    record = [];
  };

  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (inQuotes) {
      if (c === '"') {
        if (text[i + 1] === '"') {
          field += '"';
          i++;
        } else {
          inQuotes = false;
        }
      } else {
        field += c;
      }
    } else if (c === '"') {
      inQuotes = true;
    } else if (c === ",") {
      pushField();
    } else if (c === "\n") {
      pushRecord();
    } else if (c === "\r") {
      // ignore; handled by \n
    } else {
      field += c;
    }
  }
  if (field.length > 0 || record.length > 0) pushRecord();

  if (rows.length === 0) return [];
  const header = rows[0].map((h) => h.trim().toLowerCase());
  const out: Record<string, string>[] = [];
  for (let r = 1; r < rows.length; r++) {
    const cells = rows[r];
    if (cells.length === 1 && cells[0].trim() === "") continue;
    if (cells[0].trim().startsWith("#")) continue;
    const obj: Record<string, string> = {};
    header.forEach((key, idx) => {
      obj[key] = (cells[idx] ?? "").trim();
    });
    out.push(obj);
  }
  return out;
}

export function albumsFromCsv(text: string): Album[] {
  return parseCsv(text)
    .map((row) => ({
      name: row["name"] || "",
      artist: row["artist"] || "",
      album: row["album"] || "",
      appleId: row["id"] || "",
    }))
    .filter((a) => a.artist || a.album || a.appleId);
}

// Small deterministic PRNG so a given seed always shuffles the same way.
export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return function () {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/**
 * Pick a fallback album for a given rotation window using a shuffled order.
 * The window index seeds the shuffle, so within a window the choice is stable
 * (idempotent across polls) but consecutive windows land on different albums.
 * A one-step lookback avoids the rare seed collision that would repeat the
 * previous window's album.
 */
export function pickShuffledAlbum(
  albums: Album[],
  windowIndex: number
): Album | null {
  if (albums.length === 0) return null;
  if (albums.length === 1) return albums[0];

  const firstOf = (seed: number): number => {
    const order = albums.map((_, i) => i);
    const rand = mulberry32(seed >>> 0);
    for (let i = order.length - 1; i > 0; i--) {
      const j = Math.floor(rand() * (i + 1));
      [order[i], order[j]] = [order[j], order[i]];
    }
    return order[0];
  };

  let idx = firstOf(windowIndex);
  const prev = firstOf(windowIndex - 1);
  if (idx === prev) {
    // Nudge to the next distinct album so we never repeat back-to-back.
    idx = firstOf(windowIndex + 0x9e3779b9);
    if (idx === prev) idx = (prev + 1) % albums.length;
  }
  return albums[idx];
}

export function pickLastfmImage(images: unknown): string {
  if (!Array.isArray(images)) return "";
  for (let i = images.length - 1; i >= 0; i--) {
    const item = images[i] as Record<string, unknown>;
    const url = typeof item?.["#text"] === "string" ? (item["#text"] as string) : "";
    if (url && !url.includes(LASTFM_PLACEHOLDER)) return url;
  }
  return "";
}

function clamp8(v: number): number {
  return v < 0 ? 0 : v > 255 ? 255 : v;
}

/**
 * Convert an RGBA pixel buffer (length DISPLAY*DISPLAY*4) to little-endian
 * RGB565 (FRAME_BYTES). Applies mild, natural contrast/saturation to match the
 * look of the Python generator — no CRT/scanline effects.
 */
export function rgbaToRgb565(
  rgba: Uint8Array,
  contrast = 1.1,
  saturation = 1.06
): Uint8Array {
  const out = new Uint8Array(FRAME_BYTES);
  const px = DISPLAY * DISPLAY;
  for (let i = 0; i < px; i++) {
    let r = rgba[i * 4];
    let g = rgba[i * 4 + 1];
    let b = rgba[i * 4 + 2];

    // Contrast around mid-gray.
    r = (r - 128) * contrast + 128;
    g = (g - 128) * contrast + 128;
    b = (b - 128) * contrast + 128;

    // Saturation around luma.
    const luma = 0.299 * r + 0.587 * g + 0.114 * b;
    r = clamp8(luma + (r - luma) * saturation);
    g = clamp8(luma + (g - luma) * saturation);
    b = clamp8(luma + (b - luma) * saturation);

    const value =
      ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
    out[i * 2] = value & 0xff;
    out[i * 2 + 1] = (value >> 8) & 0xff;
  }
  return out;
}

// Pleasant deterministic gradient used only when no artwork can be resolved.
export function generatedFallbackFrame(seed = 0): Uint8Array {
  const rgba = new Uint8Array(DISPLAY * DISPLAY * 4);
  for (let y = 0; y < DISPLAY; y++) {
    for (let x = 0; x < DISPLAY; x++) {
      const i = (y * DISPLAY + x) * 4;
      rgba[i] = 8 + Math.floor((x / DISPLAY) * 40) + (seed & 7);
      rgba[i + 1] = 18 + Math.floor((y / DISPLAY) * 60);
      rgba[i + 2] = 28 + Math.floor(((x + y) / (2 * DISPLAY)) * 90);
      rgba[i + 3] = 255;
    }
  }
  return rgbaToRgb565(rgba, 1.0, 1.0);
}

export async function sha256hex(input: string): Promise<string> {
  const data = new TextEncoder().encode(input);
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}
