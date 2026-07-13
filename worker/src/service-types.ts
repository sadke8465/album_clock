export type PlaybackState = "playing" | "idle" | "unknown";
export type ServiceStatus = "ready" | "preparing" | "degraded";
export const RETRY_DELAYS_MS = [2_000, 5_000, 10_000, 30_000] as const;

export function retryDelayMs(retryIndex: number): number {
  return RETRY_DELAYS_MS[
    Math.min(Math.max(0, retryIndex), RETRY_DELAYS_MS.length - 1)
  ];
}

export function idleDebounceReady(
  idleCount: number,
  idleSince: number,
  now: number,
  debounceMs = 6_000
): boolean {
  return idleCount >= 2 && idleSince > 0 && now - idleSince >= debounceMs;
}

export interface TrackInfo {
  key: string;
  title: string;
  artist: string;
  album: string;
  imageUrl: string;
}

export interface FrameReference {
  sha256: string;
  url: string;
  bytes: number;
  readyAt: number;
}

export interface ReadyDisplay {
  mode: "lastfm" | "fallback";
  generation: number;
  track: Omit<TrackInfo, "imageUrl"> | null;
  frame: FrameReference | null;
}

export interface PackManifest {
  api_version: 1;
  format_version: 1;
  version: string;
  sha256: string;
  size: number;
  count: number;
  metadata_length: number;
  pack_key: string;
  pack_url: string;
  generated_at: number;
  skipped_count: number;
  report_key?: string;
}

export interface CoordinatorState {
  api_version: 1;
  revision: number;
  generation: number;
  service_status: ServiceStatus;
  playback: PlaybackState;
  checked_at: number;
  last_success_at: number;
  display: ReadyDisplay;
  pending: Omit<TrackInfo, "imageUrl"> | null;
  fallback: PackManifest | null;
  retry_after_ms: number;
  error: { code: string; message: string; at: number } | null;
  idle_since: number;
  idle_count: number;
  next_retry_at: number;
  retry_index: number;
}

export interface PublicCoordinatorState {
  api_version: 1;
  revision: number;
  generation: number;
  service_status: ServiceStatus;
  playback: PlaybackState;
  checked_at: number;
  last_success_at: number;
  display: ReadyDisplay;
  pending: Omit<TrackInfo, "imageUrl"> | null;
  fallback: PackManifest | null;
  retry_after_ms: number;
  error: { code: string; message: string; at: number } | null;
}

export function publicState(state: CoordinatorState): PublicCoordinatorState {
  return {
    api_version: 1,
    revision: state.revision,
    generation: state.generation,
    service_status: state.service_status,
    playback: state.playback,
    checked_at: state.checked_at,
    last_success_at: state.last_success_at,
    display: state.display,
    pending: state.pending,
    fallback: state.fallback,
    retry_after_ms: state.retry_after_ms,
    error: state.error,
  };
}

export function initialState(now = Date.now()): CoordinatorState {
  return {
    api_version: 1,
    revision: 1,
    generation: 0,
    service_status: "ready",
    playback: "unknown",
    checked_at: 0,
    last_success_at: 0,
    display: { mode: "fallback", generation: 0, track: null, frame: null },
    pending: null,
    fallback: null,
    retry_after_ms: 2000,
    error: null,
    idle_since: now,
    idle_count: 0,
    next_retry_at: 0,
    retry_index: 0,
  };
}

export function isPackManifest(value: unknown): value is PackManifest {
  if (!value || typeof value !== "object") return false;
  const item = value as Record<string, unknown>;
  return (
    item.api_version === 1 &&
    item.format_version === 1 &&
    typeof item.version === "string" &&
    /^[a-f0-9]{64}$/.test(item.version) &&
    item.sha256 === item.version &&
    typeof item.size === "number" &&
    typeof item.count === "number" &&
    typeof item.metadata_length === "number" &&
    typeof item.pack_key === "string" &&
    typeof item.pack_url === "string"
  );
}
