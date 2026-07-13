# Album Clock Cloudflare service

The production service coordinates Last.fm state in one SQLite-backed Durable
Object per username. Ready 64×64 RGB565 frames and immutable fallback packs live
in R2; KV is used only for long-lived artwork URL lookups.

## Protocol

| Endpoint | Purpose |
| --- | --- |
| `GET /v1/state` | Versioned display state. Supports `If-None-Match` and `304`. |
| `GET /v1/frames/<sha256>.rgb565` | Immutable, checksum-addressed 8,192-byte frame. |
| `GET /v1/fallback/current.json` | Active fallback-pack manifest. |
| `GET /v1/fallback/packs/<sha256>.acpk` | Immutable fallback pack, including range requests. |
| `GET /v1/status` | Stored diagnostics only; never starts artwork work. |
| `GET /health` | Local service/binding health only. |
| `GET /frame.rgb565` | Compatibility endpoint for old firmware and Pages backup. |
| `GET /frame.rgb565?rand=<seed>` | Deterministic forced fallback, even while playing. |

Unknown routes return JSON `404`; they never return accidental binary data.

Incoming state requests trigger a refresh at most once every two seconds. The
Durable Object serializes concurrent refreshes, debounces idle playback across
two observations and six seconds, and retains the last ready frame through
upstream errors. New frames are written and verified in R2 before the display
generation changes.

Artwork preparation uses the Last.fm image first. When absent or invalid,
Deezer and MusicBrainz race within the resolver budget. iTunes is only attempted
on a later retry. Retry delays are 2, 5, 10, then 30 seconds.

## Provision and deploy

```bash
cd worker
npm ci
npx wrangler login
npx wrangler r2 bucket create albumclock-frames
npx wrangler kv namespace create ALBUMCLOCK
# Put the returned KV id in wrangler.jsonc if it differs from the configured id.
npx wrangler secret put LAST_FM_API_KEY
npm run typecheck
npm test
npx wrangler deploy --dry-run --outdir dist
npx wrangler deploy
```

The first deploy applies the `NowPlayingCoordinator` SQLite Durable Object
migration declared in `wrangler.jsonc`.

Verify the deployment:

```bash
BASE=https://albumclock.<your-subdomain>.workers.dev
curl --fail "$BASE/health"
curl --fail -D- "$BASE/v1/state"
curl --fail "$BASE/v1/status"
```

The repository workflows can deploy this service and publish fallback packs.
Configure `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` as narrowly scoped
GitHub secrets and `WORKER_BASE_URL` as a repository variable.

## Development

```bash
npm run typecheck
npm test
npx wrangler deploy --dry-run --outdir dist
npm run dev
```

`LAST_FM_API_KEY` belongs in `.dev.vars` for local work and in a Wrangler secret
for production. Never commit it.
