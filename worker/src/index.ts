import { DurableObject } from "cloudflare:workers";
import { PhotonImage, SamplingFilter, crop, resize } from "@cf-wasm/photon";
import albumsCsv from "../../data/albums.csv";
import {
  FRAME_BYTES,
  albumsFromCsv,
  generatedFallbackFrame,
  pickLastfmImage,
  rgbaToRgb565,
  sha256hex,
} from "./helpers";
import {
  fallbackIndex,
  parseByteRange,
  readCurrentManifest,
  readFallbackFrame,
} from "./pack";
import {
  initialState,
  idleDebounceReady,
  publicState,
  retryDelayMs,
  type CoordinatorState,
  type PackManifest,
  type PublicCoordinatorState,
  type TrackInfo,
} from "./service-types";

const PROC_VERSION = "v2";
const DEFAULT_UA =
  "album-matrix-clock/2.0 (+https://github.com/sadke8465/album_clock)";
const STATE_CHECK_MS = 2_000;
const PACK_CHECK_MS = 60_000;
const IDLE_DEBOUNCE_MS = 6_000;
const UPSTREAM_GRACE_MS = 60_000;
const LASTFM_TIMEOUT_MS = 1_500;
const RESOLVER_TIMEOUT_MS = 1_500;
const IMAGE_TIMEOUT_MS = 2_500;
const PREPARE_DEADLINE_MS = 6_000;
const ALBUMS = albumsFromCsv(albumsCsv);

type JsonRecord = Record<string, unknown>;

function asRecord(value: unknown): JsonRecord | null {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as JsonRecord)
    : null;
}

function stringValue(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function ua(env: Env): string {
  return env.USER_AGENT || DEFAULT_UA;
}

async function fetchWithTimeout(
  url: string,
  init: RequestInit,
  timeoutMs: number
): Promise<Response> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), Math.max(timeoutMs, 1));
  try {
    return await fetch(url, { ...init, signal: controller.signal });
  } finally {
    clearTimeout(timeout);
  }
}

async function fetchJson(
  url: string,
  env: Env,
  timeoutMs: number
): Promise<unknown> {
  const response = await fetchWithTimeout(
    url,
    { headers: { "User-Agent": ua(env), Accept: "application/json" } },
    timeoutMs
  );
  if (!response.ok) throw new Error(`GET ${url} -> ${response.status}`);
  return response.json<unknown>();
}

async function hashBytes(bytes: Uint8Array): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)]
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

function cleanAlbumName(album: string): string {
  let value = album.replace(/\s*[([][^)\]]*[)\]]/g, " ");
  value = value.replace(
    /\s*-\s*(EP|Single|Remaster(ed)?.*|Deluxe.*|Expanded.*)$/i,
    ""
  );
  return value.replace(/\s+/g, " ").trim();
}

function trackKey(artist: string, album: string, title: string): string {
  return [artist, album, title]
    .map((part) => part.normalize("NFKC").trim().toLocaleLowerCase())
    .join("\u001f");
}

async function getNowPlaying(env: Env): Promise<TrackInfo | null> {
  const key = env.LAST_FM_API_KEY?.trim();
  const user = env.LAST_FM_USER?.trim();
  if (!key || !user) throw new Error("Last.fm credentials are not configured");
  const url =
    "https://ws.audioscrobbler.com/2.0/?method=user.getrecenttracks" +
    `&user=${encodeURIComponent(user)}&api_key=${encodeURIComponent(key)}` +
    "&format=json&limit=1";
  const root = asRecord(await fetchJson(url, env, LASTFM_TIMEOUT_MS));
  if (!root) throw new Error("Last.fm returned invalid JSON");
  if (typeof root.error === "number") {
    throw new Error(
      `Last.fm error ${root.error}: ${stringValue(root.message)}`
    );
  }
  const recent = asRecord(root.recenttracks);
  let tracks: unknown[] = [];
  if (recent) {
    tracks = Array.isArray(recent.track)
      ? recent.track
      : recent.track
        ? [recent.track]
        : [];
  }
  const track = asRecord(tracks[0]);
  if (!track) return null;
  const attr = asRecord(track["@attr"]);
  if (attr?.nowplaying !== "true") return null;
  const artist = stringValue(asRecord(track.artist)?.["#text"]);
  const album = stringValue(asRecord(track.album)?.["#text"]);
  const title = stringValue(track.name);
  return {
    key: trackKey(artist, album, title),
    title,
    artist,
    album,
    imageUrl: pickLastfmImage(track.image),
  };
}

async function deezerCover(
  artist: string,
  album: string,
  env: Env,
  timeoutMs: number
): Promise<string> {
  const query = artist ? `artist:"${artist}" album:"${album}"` : album;
  if (!query.trim()) return "";
  const root = asRecord(
    await fetchJson(
      `https://api.deezer.com/search/album?limit=5&q=${encodeURIComponent(query)}`,
      env,
      timeoutMs
    )
  );
  const albums = root && Array.isArray(root.data) ? root.data : [];
  for (const value of albums) {
    const item = asRecord(value);
    if (!item) continue;
    const cover =
      stringValue(item.cover_big) ||
      stringValue(item.cover_xl) ||
      stringValue(item.cover_medium);
    if (cover) return cover;
  }
  return "";
}

async function coverArtForReleaseGroup(
  id: string,
  env: Env,
  timeoutMs: number
): Promise<string> {
  if (!id) return "";
  const root = asRecord(
    await fetchJson(
      `https://coverartarchive.org/release-group/${encodeURIComponent(id)}`,
      env,
      timeoutMs
    )
  );
  const images = root && Array.isArray(root.images) ? root.images : [];
  const ordered = [
    ...images.filter((value) => asRecord(value)?.front === true),
    ...images,
  ];
  for (const value of ordered) {
    const image = asRecord(value);
    const thumbnails = asRecord(image?.thumbnails);
    for (const size of ["500", "250", "large", "small"]) {
      const url = stringValue(thumbnails?.[size]);
      if (url) return url;
    }
    const original = stringValue(image?.image);
    if (original) return original;
  }
  return "";
}

async function musicbrainzCover(
  artist: string,
  album: string,
  env: Env,
  timeoutMs: number
): Promise<string> {
  if (!artist || !album) return "";
  const query = `artist:"${artist}" AND releasegroup:"${album}"`;
  const started = Date.now();
  const root = asRecord(
    await fetchJson(
      `https://musicbrainz.org/ws/2/release-group/?query=${encodeURIComponent(query)}&type=album&fmt=json&limit=2`,
      env,
      timeoutMs
    )
  );
  const groups = root && Array.isArray(root["release-groups"])
    ? root["release-groups"]
    : [];
  for (const value of groups.slice(0, 2)) {
    const remaining = timeoutMs - (Date.now() - started);
    if (remaining <= 0) break;
    try {
      const url = await coverArtForReleaseGroup(
        stringValue(asRecord(value)?.id),
        env,
        remaining
      );
      if (url) return url;
    } catch {
      // Try the next release group while the shared resolver budget remains.
    }
  }
  return "";
}

async function itunesCover(
  artist: string,
  album: string,
  env: Env,
  timeoutMs: number
): Promise<string> {
  const term = [artist, album].filter(Boolean).join(" ");
  if (!term) return "";
  const root = asRecord(
    await fetchJson(
      `https://itunes.apple.com/search?media=music&entity=album&limit=5&term=${encodeURIComponent(term)}`,
      env,
      timeoutMs
    )
  );
  const results = root && Array.isArray(root.results) ? root.results : [];
  for (const value of results) {
    const artwork = stringValue(asRecord(value)?.artworkUrl100);
    if (artwork) return artwork.replace(/100x100(?:bb)?/, "600x600bb");
  }
  return "";
}

async function resolveArtwork(
  track: TrackInfo,
  env: Env,
  deadline: number,
  allowLaterResolver = false
): Promise<{ url: string; method: string }> {
  const cacheKey = `art:${await sha256hex(
    `${track.artist.toLocaleLowerCase()}|${track.album.toLocaleLowerCase()}`
  )}`;
  const cached = await env.KV.get(cacheKey);
  if (cached) return { url: cached, method: "kv-cache" };
  if (track.imageUrl) return { url: track.imageUrl, method: "lastfm-image" };

  const album = cleanAlbumName(track.album) || track.album || track.title;
  const remaining = Math.min(
    RESOLVER_TIMEOUT_MS,
    Math.max(1, deadline - Date.now())
  );
  const requireUrl = async (
    promise: Promise<string>,
    method: string
  ): Promise<{ url: string; method: string }> => {
    const url = await promise;
    if (!url) throw new Error(`${method} returned no cover`);
    return { url, method };
  };
  try {
    const winner = await Promise.any([
      requireUrl(deezerCover(track.artist, album, env, remaining), "deezer"),
      requireUrl(
        musicbrainzCover(track.artist, album, env, remaining),
        "musicbrainz"
      ),
    ]);
    await env.KV.put(cacheKey, winner.url, { expirationTtl: 604800 });
    return winner;
  } catch {
    if (allowLaterResolver) {
      const laterRemaining = Math.min(
        RESOLVER_TIMEOUT_MS,
        Math.max(1, deadline - Date.now())
      );
      const url = await itunesCover(track.artist, album, env, laterRemaining);
      if (url) {
        await env.KV.put(cacheKey, url, { expirationTtl: 604800 });
        return { url, method: "itunes-retry" };
      }
    }
    throw new Error("No artwork resolver returned a cover");
  }
}

async function fetchImage(
  url: string,
  env: Env,
  deadline: number
): Promise<ArrayBuffer> {
  const timeout = Math.min(IMAGE_TIMEOUT_MS, deadline - Date.now());
  if (timeout <= 0) throw new Error("Artwork preparation deadline exceeded");
  const response = await fetchWithTimeout(
    url,
    {
      headers: {
        "User-Agent": ua(env),
        Accept: "image/avif,image/webp,image/png,image/jpeg,image/*;q=0.8",
      },
      cf: { cacheEverything: true, cacheTtl: 86400 },
    } as RequestInit,
    timeout
  );
  if (!response.ok) throw new Error(`image GET ${url} -> ${response.status}`);
  return response.arrayBuffer();
}

function processToRgb565(bytes: ArrayBuffer): Uint8Array {
  const source = PhotonImage.new_from_byteslice(new Uint8Array(bytes));
  let square: PhotonImage | null = null;
  let resized: PhotonImage | null = null;
  try {
    const width = source.get_width();
    const height = source.get_height();
    if (width !== height) {
      const side = Math.min(width, height);
      const x = Math.floor((width - side) / 2);
      const y = Math.floor((height - side) / 2);
      square = crop(source, x, y, x + side, y + side);
    }
    resized = resize(
      square ?? source,
      64,
      64,
      SamplingFilter.Lanczos3
    );
    const frame = rgbaToRgb565(resized.get_raw_pixels());
    if (frame.byteLength !== FRAME_BYTES) {
      throw new Error(`Bad processed frame length ${frame.byteLength}`);
    }
    return frame;
  } finally {
    resized?.free();
    square?.free();
    source.free();
  }
}

async function prepareTrackFrame(
  track: TrackInfo,
  env: Env,
  allowLaterResolver = false
): Promise<{ sha256: string; method: string }> {
  const deadline = Date.now() + PREPARE_DEADLINE_MS;
  let artwork = await resolveArtwork(track, env, deadline, allowLaterResolver);
  let bytes: ArrayBuffer;
  try {
    bytes = await fetchImage(artwork.url, env, deadline);
  } catch (firstError) {
    if (artwork.method !== "lastfm-image") throw firstError;
    const withoutDirect = { ...track, imageUrl: "" };
    artwork = await resolveArtwork(
      withoutDirect,
      env,
      deadline,
      allowLaterResolver
    );
    bytes = await fetchImage(artwork.url, env, deadline);
  }
  const frame = processToRgb565(bytes);
  const sha256 = await hashBytes(frame);
  const key = `frames/${sha256}.rgb565`;
  const existing = await env.FRAMES.head(key);
  if (!existing || existing.size !== FRAME_BYTES) {
    await env.FRAMES.put(key, frame, {
      httpMetadata: { contentType: "application/octet-stream" },
      customMetadata: {
        bytes: String(FRAME_BYTES),
        processing: PROC_VERSION,
        artworkMethod: artwork.method,
      },
    });
  }
  const stored = await env.FRAMES.head(key);
  if (!stored || stored.size !== FRAME_BYTES) {
    throw new Error("R2 did not verify the complete 8,192-byte frame");
  }
  return { sha256, method: artwork.method };
}

async function prepareWithinDeadline(
  track: TrackInfo,
  env: Env,
  allowLaterResolver: boolean
): Promise<{ sha256: string; method: string }> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      prepareTrackFrame(track, env, allowLaterResolver),
      new Promise<never>((_, reject) => {
        timer = setTimeout(
          () => reject(new Error("Artwork preparation deadline exceeded")),
          PREPARE_DEADLINE_MS
        );
      }),
    ]);
  } finally {
    if (timer) clearTimeout(timer);
  }
}

function meaningfulSignature(state: CoordinatorState): string {
  return JSON.stringify({
    service_status: state.service_status,
    playback: state.playback,
    display: state.display,
    pending: state.pending,
    fallback: state.fallback?.version ?? null,
    error: state.error,
    idle_since: state.idle_since,
    idle_count: state.idle_count,
    retry_index: state.retry_index,
  });
}

function withoutImage(track: TrackInfo): Omit<TrackInfo, "imageUrl"> {
  const { imageUrl: _imageUrl, ...publicTrack } = track;
  return publicTrack;
}

export class NowPlayingCoordinator extends DurableObject<Env> {
  private cachedState: CoordinatorState | null = null;
  private refreshPromise: Promise<CoordinatorState> | null = null;
  private lastCheckAt = 0;
  private lastPackCheckAt = 0;
  private lastPersistAt = 0;

  private async loadState(): Promise<CoordinatorState> {
    if (this.cachedState) return structuredClone(this.cachedState);
    const stored = await this.ctx.storage.get<CoordinatorState>("state");
    this.cachedState = stored ?? initialState();
    return structuredClone(this.cachedState);
  }

  private async persist(state: CoordinatorState): Promise<void> {
    await this.ctx.storage.put("state", state);
    this.cachedState = structuredClone(state);
    this.lastPersistAt = Date.now();
  }

  private async refreshPack(state: CoordinatorState, now: number): Promise<void> {
    if (now - this.lastPackCheckAt < PACK_CHECK_MS) return;
    this.lastPackCheckAt = now;
    try {
      const manifest = await readCurrentManifest(this.env.FRAMES);
      state.fallback = manifest;
    } catch (error) {
      console.error(
        JSON.stringify({
          event: "fallback_manifest_error",
          error: errorMessage(error),
        })
      );
    }
  }

  private applyFailure(state: CoordinatorState, error: unknown, now: number): void {
    const delay = retryDelayMs(state.retry_index);
    state.retry_index = Math.min(state.retry_index + 1, 3);
    state.next_retry_at = now + delay;
    state.retry_after_ms = delay;
    state.service_status = "degraded";
    state.error = { code: "upstream_error", message: errorMessage(error), at: now };
    if (
      state.display.mode === "lastfm" &&
      state.last_success_at > 0 &&
      now - state.last_success_at >= UPSTREAM_GRACE_MS
    ) {
      state.generation += 1;
      state.playback = "unknown";
      state.display = {
        mode: "fallback",
        generation: state.generation,
        track: null,
        frame: null,
      };
      state.pending = null;
    }
  }

  private async refresh(): Promise<CoordinatorState> {
    const state = await this.loadState();
    const before = meaningfulSignature(state);
    const now = Date.now();
    await this.refreshPack(state, now);
    state.checked_at = now;
    try {
      const preparationRetry = state.retry_index > 0;
      const track = await getNowPlaying(this.env);
      state.last_success_at = now;
      state.next_retry_at = 0;
      state.retry_after_ms = STATE_CHECK_MS;
      state.error = null;
      if (track) {
        state.idle_count = 0;
        state.idle_since = 0;
        state.playback = "playing";
        const currentKey = state.display.track?.key;
        if (state.display.mode !== "lastfm" || currentKey !== track.key) {
          state.pending = withoutImage(track);
          state.service_status = "preparing";
          try {
            const frame = await prepareWithinDeadline(
              track,
              this.env,
              preparationRetry
            );
            state.generation += 1;
            state.display = {
              mode: "lastfm",
              generation: state.generation,
              track: withoutImage(track),
              frame: {
                sha256: frame.sha256,
                url: `/v1/frames/${frame.sha256}.rgb565`,
                bytes: FRAME_BYTES,
                readyAt: Date.now(),
              },
            };
            state.pending = null;
            state.service_status = "ready";
            state.retry_index = 0;
            console.log(
              JSON.stringify({
                event: "frame_ready",
                generation: state.generation,
                trackKey: track.key,
                frame: frame.sha256,
                method: frame.method,
              })
            );
          } catch (error) {
            this.applyFailure(state, error, now);
          }
        } else {
          state.pending = null;
          state.service_status = "ready";
          state.retry_index = 0;
        }
      } else if (state.display.mode === "lastfm") {
        state.idle_count += 1;
        if (!state.idle_since) state.idle_since = now;
        if (idleDebounceReady(state.idle_count, state.idle_since, now, IDLE_DEBOUNCE_MS)) {
          state.generation += 1;
          state.playback = "idle";
          state.display = {
            mode: "fallback",
            generation: state.generation,
            track: null,
            frame: null,
          };
          state.pending = null;
          state.service_status = "ready";
          state.retry_index = 0;
        }
      } else {
        state.playback = "idle";
        state.idle_count = Math.max(state.idle_count, 2);
        if (!state.idle_since) state.idle_since = now;
        state.pending = null;
        state.service_status = "ready";
        state.retry_index = 0;
      }
    } catch (error) {
      this.applyFailure(state, error, now);
      console.error(
        JSON.stringify({ event: "state_refresh_error", error: errorMessage(error) })
      );
    }

    const changed = before !== meaningfulSignature(state);
    if (changed) state.revision += 1;
    if (changed || now - this.lastPersistAt >= 30_000) {
      await this.persist(state);
    } else {
      this.cachedState = structuredClone(state);
    }
    this.lastCheckAt = Date.now();
    return state;
  }

  async getState(force = false): Promise<PublicCoordinatorState> {
    const state = await this.loadState();
    const now = Date.now();
    if (!force && now < state.next_retry_at) return publicState(state);
    if (!force && now - this.lastCheckAt < STATE_CHECK_MS) {
      return publicState(state);
    }
    if (!this.refreshPromise) {
      this.refreshPromise = this.refresh().finally(() => {
        this.refreshPromise = null;
      });
    }
    return publicState(await this.refreshPromise);
  }

  async getStoredState(): Promise<PublicCoordinatorState> {
    return publicState(await this.loadState());
  }
}

function coordinator(env: Env): DurableObjectStub<NowPlayingCoordinator> {
  return env.NOW_PLAYING.getByName(env.LAST_FM_USER || "default", {
    locationHint: "me",
  });
}

function corsHeaders(): Record<string, string> {
  return { "access-control-allow-origin": "*" };
}

function jsonResponse(value: unknown, init: ResponseInit = {}): Response {
  const headers = new Headers(init.headers);
  headers.set("content-type", "application/json; charset=utf-8");
  headers.set("access-control-allow-origin", "*");
  return Response.json(value, { ...init, headers });
}

async function serveFrameObject(env: Env, sha256: string): Promise<Response> {
  if (!/^[a-f0-9]{64}$/.test(sha256)) {
    return jsonResponse({ error: "invalid_frame_id" }, { status: 400 });
  }
  const object = await env.FRAMES.get(`frames/${sha256}.rgb565`);
  if (!object || object.size !== FRAME_BYTES) {
    return jsonResponse({ error: "frame_not_found" }, { status: 404 });
  }
  return new Response(object.body, {
    headers: {
      "content-type": "application/octet-stream",
      "content-length": String(FRAME_BYTES),
      "cache-control": "public, max-age=31536000, immutable",
      etag: `"${sha256}"`,
      "x-frame-bytes": String(FRAME_BYTES),
      ...corsHeaders(),
    },
  });
}

async function servePackObject(
  env: Env,
  version: string,
  request: Request
): Promise<Response> {
  if (!/^[a-f0-9]{64}$/.test(version)) {
    return jsonResponse({ error: "invalid_pack_id" }, { status: 400 });
  }
  const key = `fallback/packs/${version}.acpk`;
  const head = await env.FRAMES.head(key);
  if (!head) return jsonResponse({ error: "pack_not_found" }, { status: 404 });
  const rangeHeader = request.headers.get("range");
  const range = parseByteRange(rangeHeader, head.size);
  if (rangeHeader && !range) {
    return new Response(null, {
      status: 416,
      headers: { "content-range": `bytes */${head.size}`, ...corsHeaders() },
    });
  }
  const object = await env.FRAMES.get(
    key,
    range ? { range: { offset: range.offset, length: range.length } } : undefined
  );
  if (!object) return jsonResponse({ error: "pack_not_found" }, { status: 404 });
  const responseLength = range?.length ?? object.size;
  const headers: Record<string, string> = {
    "content-type": "application/octet-stream",
    "content-length": String(responseLength),
    "cache-control": "public, max-age=31536000, immutable",
    "accept-ranges": "bytes",
    etag: `"${version}"`,
    ...corsHeaders(),
  };
  if (range) {
    headers["content-range"] = `bytes ${range.offset}-${range.offset + range.length - 1}/${head.size}`;
  }
  return new Response(request.method === "HEAD" ? null : object.body, {
    status: range ? 206 : 200,
    headers: {
      ...headers,
    },
  });
}

async function serveCurrentManifest(env: Env): Promise<Response> {
  const object = await env.FRAMES.get("fallback/current.json");
  if (!object) return jsonResponse({ error: "pack_not_published" }, { status: 404 });
  return new Response(object.body, {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-cache",
      ...corsHeaders(),
    },
  });
}

async function legacyFrame(request: Request, env: Env): Promise<Response> {
  const url = new URL(request.url);
  const rand = url.searchParams.get("rand");
  const state = await coordinator(env).getState(url.searchParams.has("nocache"));
  if (!rand && state.display.mode === "lastfm" && state.display.frame) {
    return serveFrameObject(env, state.display.frame.sha256);
  }
  const manifest = state.fallback ?? (await readCurrentManifest(env.FRAMES));
  let frame: Uint8Array | null = null;
  let selected = 0;
  if (manifest) {
    const rotation = Number.parseInt(env.ROTATION_SECONDS || "600", 10) || 600;
    selected = fallbackIndex(
      manifest,
      Math.floor(Date.now() / 1000 / rotation),
      rand ?? undefined
    );
    frame = await readFallbackFrame(env.FRAMES, manifest, selected);
  }
  const body = frame ?? generatedFallbackFrame(selected);
  return new Response(body, {
    headers: {
      "content-type": "application/octet-stream",
      "content-length": String(FRAME_BYTES),
      "cache-control": rand ? "no-store" : "public, max-age=2",
      "x-frame-bytes": String(FRAME_BYTES),
      "x-fallback-index": String(selected),
      ...corsHeaders(),
    },
  });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    try {
      if (request.method !== "GET" && request.method !== "HEAD") {
        return jsonResponse({ error: "method_not_allowed" }, { status: 405 });
      }
      if (url.pathname === "/v1/state") {
        const state = await coordinator(env).getState(
          url.searchParams.has("refresh")
        );
        const etag = `"state-${state.revision}"`;
        const headers = {
          etag,
          "cache-control": "no-store",
          "retry-after": String(Math.max(1, Math.ceil(state.retry_after_ms / 1000))),
          "x-checked-at": String(state.checked_at),
          ...corsHeaders(),
        };
        if (request.headers.get("if-none-match") === etag) {
          return new Response(null, { status: 304, headers });
        }
        return jsonResponse(state, { headers });
      }
      if (url.pathname === "/v1/status" || url.pathname === "/status") {
        return jsonResponse(await coordinator(env).getStoredState(), {
          headers: { "cache-control": "no-store" },
        });
      }
      if (url.pathname === "/v1/fallback/current.json") {
        return serveCurrentManifest(env);
      }
      const frameMatch = url.pathname.match(
        /^\/v1\/frames\/([a-f0-9]{64})\.rgb565$/
      );
      if (frameMatch) return serveFrameObject(env, frameMatch[1]);
      const packMatch = url.pathname.match(
        /^\/v1\/fallback\/packs\/([a-f0-9]{64})\.acpk$/
      );
      if (packMatch) return servePackObject(env, packMatch[1], request);
      if (url.pathname === "/frame.rgb565") return legacyFrame(request, env);
      if (url.pathname === "/health") {
        const state = await coordinator(env).getStoredState();
        return jsonResponse({
          ok: true,
          api_version: 1,
          albums: ALBUMS.length,
          revision: state.revision,
          generation: state.generation,
          bindings: { kv: true, r2: true, durable_object: true },
        });
      }
      return jsonResponse({ error: "not_found" }, { status: 404 });
    } catch (error) {
      const requestId = request.headers.get("cf-ray") ?? crypto.randomUUID();
      console.error(
        JSON.stringify({
          event: "request_error",
          requestId,
          path: url.pathname,
          error: errorMessage(error),
        })
      );
      return jsonResponse(
        { error: "internal_error", request_id: requestId },
        { status: 500 }
      );
    }
  },
} satisfies ExportedHandler<Env>;
