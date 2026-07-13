import { FRAME_BYTES } from "./helpers.ts";
import { isPackManifest, type PackManifest } from "./service-types.ts";

export const PACK_HEADER_BYTES = 16;
export const PACK_MAGIC = "ACPK";

export interface PackHeader {
  formatVersion: number;
  width: number;
  height: number;
  count: number;
  metadataLength: number;
}

export function parseByteRange(
  header: string | null,
  size: number
): { offset: number; length: number } | null {
  if (!header) return null;
  const match = header.match(/^bytes=(\d+)-(\d*)$/);
  if (!match) return null;
  const offset = Number.parseInt(match[1], 10);
  const requestedEnd = match[2] ? Number.parseInt(match[2], 10) : size - 1;
  const end = Math.min(requestedEnd, size - 1);
  if (!Number.isSafeInteger(offset) || offset < 0 || offset >= size || end < offset) {
    return null;
  }
  return { offset, length: end - offset + 1 };
}

export function parsePackHeader(bytes: Uint8Array): PackHeader | null {
  if (bytes.byteLength !== PACK_HEADER_BYTES) return null;
  const magic = new TextDecoder().decode(bytes.subarray(0, 4));
  if (magic !== PACK_MAGIC) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const header = {
    formatVersion: view.getUint16(4, true),
    width: view.getUint16(6, true),
    height: view.getUint16(8, true),
    count: view.getUint16(10, true),
    metadataLength: view.getUint32(12, true),
  };
  if (
    header.formatVersion !== 1 ||
    header.width !== 64 ||
    header.height !== 64 ||
    header.count < 1 ||
    header.metadataLength < 2
  ) {
    return null;
  }
  return header;
}

export async function readCurrentManifest(
  bucket: R2Bucket
): Promise<PackManifest | null> {
  const object = await bucket.get("fallback/current.json");
  if (!object) return null;
  const value: unknown = await object.json();
  return isPackManifest(value) ? value : null;
}

export async function readFallbackFrame(
  bucket: R2Bucket,
  manifest: PackManifest,
  index: number
): Promise<Uint8Array | null> {
  if (index < 0 || index >= manifest.count) return null;
  const offset = PACK_HEADER_BYTES + manifest.metadata_length + index * FRAME_BYTES;
  if (offset + FRAME_BYTES > manifest.size) return null;
  const object = await bucket.get(manifest.pack_key, {
    range: { offset, length: FRAME_BYTES },
  });
  if (!object) return null;
  const bytes = await object.bytes();
  return bytes.byteLength === FRAME_BYTES ? bytes : null;
}

export function hashString(value: string): number {
  let hash = 2166136261;
  for (let i = 0; i < value.length; i++) {
    hash ^= value.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

export function xorshift32(seed: number): () => number {
  let state = seed >>> 0 || 0x9e3779b9;
  return () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return (state >>> 0) / 4294967296;
  };
}

export function shuffledOrder(count: number, seed: number): number[] {
  const order = Array.from({ length: count }, (_, index) => index);
  const random = xorshift32(seed);
  for (let i = order.length - 1; i > 0; i--) {
    const j = Math.floor(random() * (i + 1));
    [order[i], order[j]] = [order[j], order[i]];
  }
  return order;
}

export function fallbackIndex(
  manifest: PackManifest,
  rotationWindow: number,
  seedOverride?: string
): number {
  const seed = hashString(
    `${manifest.version}:${seedOverride ?? String(rotationWindow)}`
  );
  return shuffledOrder(manifest.count, seed)[0] ?? 0;
}
