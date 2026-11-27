import { DurableObject } from "cloudflare:workers";
import { PhotonImage, SamplingFilter, crop, resize } from "@cf-wasm/photon";
import albumsCsv from "../../data/albums.csv";
import { radioMatchScore, radioCompilation, rankRadioReleases, parseRadioTrack, canPublishRadio, radioSearchTitle, hebrewSearchVariants, radioReleaseQuality, radioAlbumKey, radioMarket, type RadioTrack } from "./radio-match";
import { pokemonAssets, preparePokemonSprite, POKEMON_SIZES, MAX_PACKET } from "./pokemon";
import {
  FRAME_BYTES,
  albumsFromCsv,
  parseCsv,
  generatedFallbackFrame,
  pickLastfmImage,
  rgbaToRgb565,
  sha256hex,
} from "./helpers";
import {
  fallbackIndex,
  parseByteRange,
  parsePackHeader,
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
const DETAIL_SIZE = 512;
const DETAIL_BYTES = DETAIL_SIZE * DETAIL_SIZE * 2;
const DETAIL_CATALOG = new Map(parseCsv(albumsCsv).map(row => [row.id, row]));
const RADIO_POLICY = 4;

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

function processToRgb565(bytes: ArrayBuffer, size = 64): Uint8Array {
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
      size,
      size,
      SamplingFilter.Lanczos3
    );
    const frame = rgbaToRgb565(resized.get_raw_pixels());
    if (frame.byteLength !== size * size * 2) {
      throw new Error(`Bad processed frame length ${frame.byteLength}`);
    }
    return frame;
  } finally {
    resized?.free();
    square?.free();
    source.free();
  }
}

function similarCover(a: Uint8Array,b: Uint8Array): boolean {
  if(a.length!==FRAME_BYTES||b.length!==FRAME_BYTES)return false;
  let sum=0;
  for(let i=0;i<a.length;i+=2) {
    const x=a[i]|a[i+1]<<8,y=b[i]|b[i+1]<<8;
    sum+=Math.abs((x>>11)-(y>>11))*8+Math.abs(((x>>5)&63)-((y>>5)&63))*4+Math.abs((x&31)-(y&31))*8;
  }
  return sum/(64*64*3)<25;
}

async function storeDetail(env: Env,key:string,detail:Uint8Array):Promise<void>{
  await env.FRAMES.put(key,detail,{httpMetadata:{contentType:"application/octet-stream"},
    customMetadata:{sha256:await hashBytes(detail)}});
}
async function serveDetail(env:Env,key:string):Promise<Response>{
  const object=await env.FRAMES.get(key);
  if(!object||object.size!==DETAIL_BYTES||!object.customMetadata?.sha256)
    return jsonResponse({error:"detail_unavailable"},{status:404});
  return new Response(object.body,{headers:{"content-type":"application/octet-stream",
    "content-length":String(DETAIL_BYTES),"x-detail-sha256":object.customMetadata.sha256,
    "cache-control":"public,max-age=31536000,immutable",...corsHeaders()}});
}
async function fallbackDetail(env:Env,version:string,index:number):Promise<Response>{
  const key=`detail/fallback/${version}/${index}.rgb565`;
  if(await env.FRAMES.head(key))return serveDetail(env,key);
  const packKey=`fallback/packs/${version}.acpk`;
  const h=await env.FRAMES.get(packKey,{range:{offset:0,length:16}});
  const header=h&&parsePackHeader(await h.bytes());
  if(!header||index>=header.count||header.metadataLength>500000)return jsonResponse({error:"invalid_pack"},{status:404});
  const meta=await env.FRAMES.get(packKey,{range:{offset:16,length:header.metadataLength}});
  const entries=meta&&asRecord(await meta.json())?.entries;
  const entry=Array.isArray(entries)?asRecord(entries[index]):null;
  const id=stringValue(entry?.id).replace(/^apple:/,"");
  const row=DETAIL_CATALOG.get(id);
  if(!row||!/^\d+$/.test(id))return jsonResponse({error:"detail_unavailable"},{status:404});
  const base=await env.FRAMES.get(packKey,{range:{offset:16+header.metadataLength+index*FRAME_BYTES,length:FRAME_BYTES}});
  if(!base)return jsonResponse({error:"detail_unavailable"},{status:404});
  const original=await base.bytes();
  const apple=async(country:string)=>{
    const result=asRecord(await fetchJson("https://itunes.apple.com/lookup?id="+id+"&country="+country,env,2200));
    const items=Array.isArray(result?.results)?result.results:[];
    const match=items.map(asRecord).find(value=>String(value?.trackId)===id);
    return stringValue(match?.artworkUrl100).replace(/100x100(?:bb)?/,"600x600bb");
  };
  // Older entries were originally resolved via MusicBrainz first. Try that
  // source as well as Apple, but only accept a visually matching rendition.
  const candidates=row.image_url?[row.image_url]:(await Promise.allSettled([
    apple("il"),apple("us"),musicbrainzCover(row.artist,row.album,env,2200),
    deezerCover(row.artist,row.album,env,2200)
  ])).flatMap(r=>r.status==="fulfilled"&&r.value?[r.value]:[]);
  const jobs=[...new Set(candidates)].map(async url=>{
    const image=await fetchImage(url,env,Date.now()+2500);
    if(!similarCover(original,processToRgb565(image)))throw Error("cover mismatch");
    return processToRgb565(image,DETAIL_SIZE);
  });
  try{
    await storeDetail(env,key,await Promise.any(jobs));
    return serveDetail(env,key);
  }catch{return jsonResponse({error:"detail_unavailable"},{status:404});}
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

  // This instance is dedicated to Kan 88 (separate from the Last.fm state).
  // Both displays see one strongly consistent cover and share a single lookup.
  private radioLast: RadioCover | null = null;
  private radioLoaded: Promise<void> | null = null;
  private radioTrack: RadioTrack | null = null;
  private radioCheckAt = 0;
  private radioRefresh: Promise<void> | null = null;
  private radioJobs = new Map<string, Promise<void>>();
  private radioRetryAt = new Map<string, number>();
  private radioAttempts = new Map<string,number>();
  private radioPublishedAt = 0;

  async getRadioState(): Promise<JsonRecord> {
    if (!this.radioLoaded) this.radioLoaded = (async () => {
      this.radioLast = await this.ctx.storage.get<RadioCover>("radio-last-v3") ??
        await this.env.KV.get<RadioCover>("kan88:last-frame:v1", "json");
    })().catch(()=>{ this.radioLoaded=null; });
    await this.radioLoaded;
    if (!this.radioRefresh && Date.now() - this.radioCheckAt >= 1200) {
      this.radioCheckAt = Date.now();
      this.radioRefresh = this.refreshRadio().finally(() => { this.radioRefresh = null; });
      this.ctx.waitUntil(this.radioRefresh);
    }
    // Always return the last good cover immediately, even while the official
    // feed or catalogue is slow. Never hold a client request open for search.
    const track = this.radioTrack&&Date.now()<=this.radioTrack.end+15000?this.radioTrack:null;
    return { ...this.radioLast,
      cover_url:this.radioLast?.sha256?`/v1/radio/cover/${this.radioLast.sha256}`:undefined,
      status: track && this.radioLast?.track_key === track.key && this.radioLast.policy===RADIO_POLICY ? "ready" : track ? "searching" : this.radioLast ? "holding" : "waiting",
      current_title: track?.title ?? "", current_artists: track?.artists ?? [],
      current_start: track?.start ?? null, current_end: track?.end ?? null,
      checked_at: this.radioCheckAt, published_at:this.radioPublishedAt||null,
      lookup_attempt:track?this.radioAttempts.get(track.key)??0:0,retry_after_ms: 1500 };
  }

  private async refreshRadio(): Promise<void> {
    try {
      const track = await getRadioTrack(this.env);
      this.radioTrack = track;
      const same=track&&this.radioLast?.track_key===track.key&&this.radioLast.policy===RADIO_POLICY;
      if (!track || (same&&this.radioLast?.release_quality==="original") || this.radioJobs.has(track.key) ||
          (this.radioRetryAt.get(track.key) ?? 0) > Date.now()) return;
      const job = this.resolveRadio(track,!!same).finally(() => { this.radioJobs.delete(track.key); });
      this.radioJobs.set(track.key, job);
      this.ctx.waitUntil(job);
    } catch { /* Feed failures retain the last verified artwork. */ }
  }

  private async resolveRadio(track: RadioTrack, improve=false): Promise<void> {
    const started=Date.now(),attempt=(this.radioAttempts.get(track.key)??0)+1;
    this.radioAttempts.set(track.key,attempt);
    if(this.radioAttempts.size>64)this.radioAttempts.delete(this.radioAttempts.keys().next().value!);
    try {
      const value = await resolveRadioCover(this.env, track,improve);
      if (!value) {
        // No-match is deliberately short-lived; transient search failures
        // should never lock out a song for its entire airtime.
        this.radioRetryAt.set(track.key, Date.now()+Math.min(30_000,8_000*attempt));
        if (this.radioRetryAt.size > 64) this.radioRetryAt.delete(this.radioRetryAt.keys().next().value!);
        return;
      }
      if (!canPublishRadio(track,this.radioTrack,Date.now())) return;
      // Never flip between two fallback compilations. Replace a tentative
      // cover when an actual original becomes available on a subsequent pass.
      if(!improve||value.release_quality==="original"){
        this.radioLast={...value,lookup_ms:Date.now()-started};
        this.radioPublishedAt=Date.now();
        this.ctx.waitUntil(Promise.allSettled([
          this.ctx.storage.put("radio-last-v3",this.radioLast),
          radioCacheKey(track).then(key=>this.env.KV.put(key,JSON.stringify(value),{expirationTtl:value.release_quality==="original"?2592000:3600}))
        ]).then(()=>{}));
      }
      if(value.release_quality!=="original")this.radioRetryAt.set(track.key,Date.now()+Math.min(45000,22000*attempt));
    } catch { this.radioRetryAt.set(track.key, Date.now()+4_000); }
  }

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


interface RadioCover { title: string; artists: string[]; album: string; sha256: string; track_key?: string; cover_url?: string; source?: string; score?: number; policy?:number; release_quality?:'original'|'fallback'; album_date?:string; lookup_ms?:number; }
interface RadioCandidate { image: string; album: string; score: number; source: string; date: string; compilation: boolean; single: boolean; albumDate?:string; appleId?:string; order?:number; }

async function radioCacheKey(track:RadioTrack):Promise<string>{
  return `kan88:track:v${RADIO_POLICY}:`+await hashBytes(new TextEncoder().encode(track.key));
}

// Unlike the legacy header-only timeout, these deadlines cover the response
// body too. A slow catalogue/CDN cannot leave the shared resolver busy forever.
async function radioJson(url:string,env:Env,timeout:number):Promise<unknown>{
  const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),timeout);
  try{
    const response=await fetch(url,{signal:controller.signal,headers:{"User-Agent":ua(env),Accept:"application/json","Cache-Control":"no-cache"}});
    if(response.status===204)return null;
    if(!response.ok)throw new Error(`GET ${url} -> ${response.status}`);
    return await response.json();
  }finally{clearTimeout(timer);}
}
async function radioImage(url:string,env:Env,controller:AbortController):Promise<ArrayBuffer>{
  const timer=setTimeout(()=>controller.abort(),2800);
  try{
    const response=await fetch(url,{signal:controller.signal,headers:{"User-Agent":ua(env),Accept:"image/jpeg,image/png,image/webp,image/*;q=0.8"},cf:{cacheEverything:true,cacheTtl:86400}});
    if(!response.ok)throw new Error(`Artwork HTTP ${response.status}`);
    const bytes=await response.arrayBuffer();if(bytes.byteLength>8*1024*1024)throw new Error("Artwork too large");return bytes;
  }finally{clearTimeout(timer);}
}

async function getRadioTrack(env: Env): Promise<RadioTrack | null> {
  const data=await radioJson("https://www.kan.org.il/api/arc-cloud/get-live-track-data?channelId=4",env,2500);
  return parseRadioTrack(data,Date.now());
}

function radioImageType(bytes: ArrayBuffer): string {
  const b=new Uint8Array(bytes);
  if(b[0]===255&&b[1]===216)return "image/jpeg";
  if(b[0]===137&&b[1]===80&&b[2]===78&&b[3]===71)return "image/png";
  if(b[0]===82&&b[1]===73&&b[2]===70&&b[3]===70&&b[8]===87&&b[9]===69&&b[10]===66&&b[11]===80)return "image/webp";
  throw new Error("Unsupported radio artwork format");
}

async function resolveRadioCover(env: Env, track: RadioTrack, improve=false): Promise<RadioCover | null> {
  const {title,artists}=track;
  const searchTitle=radioSearchTitle(title)||title;
  const market=radioMarket(searchTitle);
  const key=await radioCacheKey(track);
  const cached=improve?null:await env.KV.get<RadioCover>(key,"json").catch(()=>null);
  if(cached?.sha256&&cached.cover_url&&cached.policy===RADIO_POLICY)return cached;
  const add=(rowTitle:unknown,rowArtist:unknown,image:string,album:string,source:string,date="",isCompilation=false,isSingle=false):RadioCandidate[]=>{
    const score=radioMatchScore(title,artists,rowTitle,rowArtist);
    return score&&/^https:\/\//.test(image)?[{image,album,score,source,date,compilation:isCompilation||radioCompilation(album),single:isSingle||/\s-\s(?:single|ep)$/i.test(album)}]:[];
  };
  const apple=async(term:string,country:string):Promise<RadioCandidate[]>=>{
    const data=asRecord(await radioJson("https://itunes.apple.com/search?"+new URLSearchParams({term,country,entity:"song",limit:"50"}),env,3300));
    const rows=(Array.isArray(data?.results)?data.results:[]).map(asRecord);
    const aliases=new Map<string,string>();
    // Apple Japan/Korea/Taiwan localize artist names as well as song names.
    // Resolve the unchanged numeric artist ID to its US catalogue name;
    // this is verified identity evidence, not a title-only guess.
    if(["jp","kr","tw"].includes(country)){
      const ids=[...new Set(rows.filter(r=>r&&typeof r.artistId==="number"&&stringValue(r.artistName)&&
        radioMatchScore(title,[stringValue(r.artistName)],r.trackName,r.artistName)>0&&
        !radioMatchScore(title,artists,r.trackName,r.artistName)).map(r=>String(r!.artistId)))].slice(0,8);
      if(ids.length)try{
        const names=asRecord(await radioJson("https://itunes.apple.com/lookup?"+new URLSearchParams({id:ids.join(","),country:"us",entity:"musicArtist"}),env,2000));
        for(const item of Array.isArray(names?.results)?names.results:[]){const r=asRecord(item);if(r&&typeof r.artistId==="number")aliases.set(String(r.artistId),stringValue(r.artistName));}
      }catch{}
    }
    return rows.flatMap((row,order)=>{
      const artist=radioMatchScore(title,artists,row?.trackName,row?.artistName)?row?.artistName:aliases.get(String(row?.artistId))||row?.artistName;
      return add(row?.trackName,artist,stringValue(row?.artworkUrl100).replace(/100x100(?:bb)?/,"600x600bb"),
        stringValue(row?.collectionName),"Apple Music "+country.toUpperCase(),stringValue(row?.releaseDate))
        .map(c=>({...c,appleId:typeof row?.collectionId==="number"?String(row.collectionId):undefined,order}));
    });
  };
  const deezer=async(queryTitle=searchTitle):Promise<RadioCandidate[]>=>{
    const data=asRecord(await radioJson("https://api.deezer.com/search?"+new URLSearchParams({q:artists[0]+" "+queryTitle,limit:"40"}),env,2800));
    return (Array.isArray(data?.data)?data.data:[]).flatMap((item,order)=>{
      const row=asRecord(item),artist=asRecord(row?.artist),album=asRecord(row?.album);
      return add(row?.title,artist?.name,stringValue(album?.cover_big)||stringValue(album?.cover_medium),stringValue(album?.title),"Deezer").map(c=>({...c,order}));
    });
  };
  const musicbrainz=async():Promise<RadioCandidate[]>=>{
    const quote=(v:string)=>v.replace(/[\\"]/g,"\\$&");
    const q=new URLSearchParams({query:'recording:"'+quote(searchTitle)+'" AND artist:"'+quote(artists[0])+'"',fmt:"json",limit:"15"});
    const data=asRecord(await radioJson("https://musicbrainz.org/ws/2/recording/?"+q,env,2600));
    return (Array.isArray(data?.recordings)?data.recordings:[]).flatMap(item=>{
      const rec=asRecord(item), credits=Array.isArray(rec?.["artist-credit"])?rec["artist-credit"]:[];
      const artist=credits.map(v=>{const c=asRecord(v);return stringValue(c?.name)+stringValue(c?.joinphrase);}).join("");
      return (Array.isArray(rec?.releases)?rec.releases:[]).slice(0,15).flatMap(item=>{
        const rel=asRecord(item), group=asRecord(rel?.["release-group"]), id=stringValue(rel?.id);
        if(!/^[0-9a-f-]{36}$/.test(id))return [];
        const types=Array.isArray(group?.["secondary-types"])?group["secondary-types"]:[];
        const date=stringValue(rel?.date),recorded=stringValue(rec?.["first-release-date"])||date;
        const candidates=add(rec?.title,artist,"https://coverartarchive.org/release/"+id+"/front-500",stringValue(rel?.title),"MusicBrainz",
          recorded,types.includes("Compilation"),group?.["primary-type"]==="Single"||group?.["primary-type"]==="EP").map(c=>({...c,albumDate:date}));
        const groupId=stringValue(group?.id);
        if(/^[0-9a-f-]{36}$/.test(groupId))candidates.push(...candidates.map(c=>({...c,image:"https://coverartarchive.org/release-group/"+groupId+"/front-500"})));
        return candidates;
      });
    });
  };
  // Start fewer requests per Apple storefront to avoid rate limiting. Other
  // markets, title-only, and alternate artists are used on the second pass.
  const term=artists[0]+" "+searchTitle;
  const results=await Promise.allSettled([apple(term,market),apple(term,"us"),deezer(),musicbrainz()]);
  const candidates=results.flatMap(r=>r.status==="fulfilled"?r.value:[]);
  // Israeli stations sometimes give only the transliterated title while
  // catalogue search indexes only Hebrew. Use phonetic queries as a bounded
  // second pass only when the normal searches found no confident match.
  if(improve||!candidates.some(c=>c.score>=190&&!c.compilation)){
    const alternates=candidates.some(c=>c.score>=180)?[]:hebrewSearchVariants(searchTitle);
    const jobs=[apple(searchTitle,market),apple(term,"gb"),...alternates.map(t=>apple(artists[0]+" "+t,"il"))];
    if(alternates[0])jobs.push(deezer(alternates[0]));
    if(artists[1])jobs.push(apple(artists[1]+" "+(alternates[0]||searchTitle),"il"));
    const additional=await Promise.allSettled(jobs.slice(0,6));
    candidates.push(...additional.flatMap(r=>r.status==="fulfilled"?r.value:[]));
  }
  // Begin downloading the most plausible covers while verifying collection
  // dates. The final ranking still determines what gets published.
  rankRadioReleases(candidates);
  const downloads=new Map<string,{job:Promise<ArrayBuffer|null>;controller:AbortController}>();
  const download=(url:string)=>{
    const prior=downloads.get(url);if(prior)return prior.job;
    const controller=new AbortController(),job=radioImage(url,env,controller).catch(()=>null);
    downloads.set(url,{job,controller});return job;
  };
  [...new Set(candidates.slice(0,4).map(c=>c.image))].slice(0,2).forEach(url=>download(url));
  // Track releaseDate is often the same on a 1976 studio album and a 2002
  // greatest-hits compilation. A single batched album lookup reveals the
  // actual collection's release date and avoids this false equivalence.
  const ids=[...new Set(candidates.filter(c=>c.score>=180&&c.appleId).map(c=>c.appleId!))].slice(0,16);
  if(ids.length){
    try{
      const data=asRecord(await radioJson("https://itunes.apple.com/lookup?"+new URLSearchParams({id:ids.join(","),entity:"album",country:market}),env,2300));
      const albums=new Map((Array.isArray(data?.results)?data.results:[]).map(v=>{const row=asRecord(v);return [String(row?.collectionId),row] as const;}));
      for(const c of candidates)if(c.appleId){
        const row=albums.get(c.appleId);if(!row)continue;
        c.albumDate=stringValue(row.releaseDate);
        if(typeof row.trackCount==="number"&&row.trackCount>45)c.compilation=true;
        if(typeof row.trackCount==="number"&&row.trackCount<=3)c.single=true;
      }
    }catch{ /* Unavailable release metadata is not fabricated. */ }
  }
  // Prefer original studio albums, then earliest release, for comparably
  // confident matches. An exact compilation still beats a weak wrong song.
  rankRadioReleases(candidates);
  const seen=new Set<string>(),albumCounts=new Map<string,number>();
  const unique=candidates.filter(c=>{
    const album=radioAlbumKey(c.album);
    if(seen.has(c.image)||(album&&(albumCounts.get(album)??0)>=3))return false;
    seen.add(c.image);albumCounts.set(album,(albumCounts.get(album)??0)+1);return true;
  });
  // Start the ranked downloads together, but publish as soon as the highest
  // ranked viable cover finishes; never wait for unrelated slow downloads.
  for(let batch=0;batch<Math.min(8,unique.length);batch+=4){
    const checked=unique.slice(batch,batch+4).map(candidate=>download(candidate.image).then(image=>image?{candidate,image}:null));
    for(const job of checked){const item=await job;if(!item)continue;
    const {candidate,image}=item;
    let contentType:string,frame:Uint8Array;
    try{contentType=radioImageType(image);frame=processToRgb565(image);}catch{continue;}
    for(const [url,request] of downloads)if(url!==candidate.image)request.controller.abort();
    const sha256=await hashBytes(frame);
    await Promise.all([
      env.FRAMES.put(`frames/${sha256}.rgb565`,frame,{httpMetadata:{contentType:"application/octet-stream"}}),
      env.FRAMES.put(`radio/cover/${sha256}`,image,{httpMetadata:{contentType}})
    ]);
    const value:RadioCover={title,artists,album:candidate.album,sha256,track_key:track.key,cover_url:`/v1/radio/cover/${sha256}`,
      source:candidate.source,score:candidate.score,policy:RADIO_POLICY,release_quality:radioReleaseQuality(candidate,candidates),album_date:candidate.albumDate};
    return value;
    }
  }
  return null;
}

async function kan88State(env: Env): Promise<Response> {
  const state=await env.NOW_PLAYING.getByName("kan88-live-v3",{locationHint:"me"}).getRadioState();
  return jsonResponse(state,{headers:{"cache-control":"no-store"}});
}

async function radioDetail(env:Env,hash:string):Promise<Response>{
  const key=`detail/radio/${hash}.rgb565`;
  if(await env.FRAMES.head(key))return serveDetail(env,key);
  const cover=await env.FRAMES.get(`radio/cover/${hash}`);
  if(!cover)return jsonResponse({error:"detail_unavailable"},{status:404});
  try{await storeDetail(env,key,processToRgb565(await cover.arrayBuffer(),DETAIL_SIZE));return serveDetail(env,key);}
  catch{return jsonResponse({error:"detail_unavailable"},{status:404});}
}

async function serveRadioCover(env:Env,hash:string,headOnly:boolean):Promise<Response>{
  const object=await env.FRAMES.get(`radio/cover/${hash}`);
  if(object)return new Response(headOnly?null:object.body,{headers:{
    "content-type":object.httpMetadata?.contentType??"image/jpeg","content-length":String(object.size),
    "cache-control":"public,max-age=31536000,immutable","x-content-type-options":"nosniff",...corsHeaders()}});
  // Previous firmware stored only RGB565. Recover its high-resolution crop
  // for the CRT so the two displays match even before the first new lookup.
  const detail=await env.FRAMES.get(`detail/radio/${hash}.rgb565`);
  const legacy=detail?.size===DETAIL_BYTES?detail:await env.FRAMES.get(`frames/${hash}.rgb565`);
  if(!legacy||![DETAIL_BYTES,FRAME_BYTES].includes(legacy.size))return jsonResponse({error:"cover_not_found"},{status:404});
  const side=legacy.size===DETAIL_BYTES?DETAIL_SIZE:64,raw=await legacy.bytes(),rgba=new Uint8Array(side*side*4);
  for(let i=0;i<side*side;i++){
    const v=raw[i*2]|raw[i*2+1]<<8,r=v>>11,g=(v>>5)&63,b=v&31;
    rgba[i*4]=(r<<3)|(r>>2);rgba[i*4+1]=(g<<2)|(g>>4);rgba[i*4+2]=(b<<3)|(b>>2);rgba[i*4+3]=255;
  }
  const image=new PhotonImage(rgba,side,side);
  try{return new Response(headOnly?null:image.get_bytes(),{headers:{"content-type":"image/png","cache-control":"public,max-age=60",...corsHeaders()}});}
  finally{image.free();}
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
      if (url.pathname === "/v1/radio/kan88/state") return kan88State(env);
      const pokemonMatch=url.pathname.match(/^\/v1\/pokemon\/sprite\/(\d{1,2})\/(\d{1,3})\/(\d)\/(16|22|32|64)\.pka$/);
      if(pokemonMatch){
        const game=Number(pokemonMatch[1]),id=Number(pokemonMatch[2]),variant=Number(pokemonMatch[3]),size=Number(pokemonMatch[4]),animated=url.searchParams.get('animated')==='1';
        if(!POKEMON_SIZES.includes(size as typeof POKEMON_SIZES[number])||!pokemonAssets(game,id,variant,animated).length)
          return jsonResponse({error:'sprite_unavailable'},{status:404});
        const key=`pokemon/pka1/${game}/${id}/${variant}/${size}/${Number(animated)}.pka`;
        let object=await env.FRAMES.get(key);
        if(!object){
          const packet=await preparePokemonSprite(game,id,variant,size,animated);
          if(!packet)return jsonResponse({error:'sprite_unavailable'},{status:404});
          const digest=await hashBytes(packet);
          await env.FRAMES.put(key,packet,{httpMetadata:{contentType:'application/octet-stream'},customMetadata:{sha256:digest}});
          return new Response(request.method==='HEAD'?null:packet,{headers:{'content-type':'application/octet-stream','content-length':String(packet.length),'x-pokemon-sha256':digest,'cache-control':'public,max-age=31536000,immutable',...corsHeaders()}});
        }
        if(object.size>MAX_PACKET||!object.customMetadata?.sha256)return jsonResponse({error:'invalid_sprite'},{status:502});
        return new Response(request.method==='HEAD'?null:object.body,{headers:{'content-type':'application/octet-stream','content-length':String(object.size),'x-pokemon-sha256':object.customMetadata.sha256,'cache-control':'public,max-age=31536000,immutable',...corsHeaders()}});
      }
      if(url.pathname==="/v1/firmware/latest.json"){
        const object=await env.FRAMES.get("firmware/latest.json");
        if(!object)return jsonResponse({error:"firmware_unavailable"},{status:404});
        return new Response(object.body,{headers:{"content-type":"application/json","content-length":String(object.size),"cache-control":"no-store"}});
      }
      const firmwareMatch=url.pathname.match(/^\/v1\/firmware\/([a-f0-9]{64})\.bin$/);
      if(firmwareMatch){
        const object=await env.FRAMES.get(`firmware/releases/${firmwareMatch[1]}.bin`);
        if(!object)return jsonResponse({error:"firmware_unavailable"},{status:404});
        return new Response(object.body,{headers:{"content-type":"application/octet-stream","content-length":String(object.size),"cache-control":"public,max-age=31536000,immutable"}});
      }
      const radioCover=url.pathname.match(/^\/v1\/radio\/cover\/([a-f0-9]{64})$/);
      if(radioCover)return serveRadioCover(env,radioCover[1],request.method==="HEAD");
      const radioDetailMatch=url.pathname.match(/^\/v1\/detail\/radio\/([a-f0-9]{64})\.rgb565$/);
      if(radioDetailMatch)return radioDetail(env,radioDetailMatch[1]);
      const randomDetail=url.pathname.match(/^\/v1\/detail\/fallback\/([a-f0-9]{64})\/(\d{1,4})\.rgb565$/);
      if(randomDetail)return fallbackDetail(env,randomDetail[1],Number(randomDetail[2]));
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
