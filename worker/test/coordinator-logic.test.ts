import test from "node:test";
import assert from "node:assert/strict";

import {
  idleDebounceReady,
  initialState,
  publicState,
  retryDelayMs,
} from "../src/service-types.ts";

test("idle requires two observations spanning six seconds", () => {
  assert.equal(idleDebounceReady(1, 1_000, 20_000), false);
  assert.equal(idleDebounceReady(2, 1_000, 6_999), false);
  assert.equal(idleDebounceReady(2, 1_000, 7_000), true);
});

test("preparation retries use bounded 2/5/10/30 second backoff", () => {
  assert.deepEqual([0, 1, 2, 3, 4, 99].map(retryDelayMs), [2_000, 5_000, 10_000, 30_000, 30_000, 30_000]);
});

test("public state does not expose coordinator bookkeeping", () => {
  const state = initialState(123);
  const visible = publicState(state) as unknown as Record<string, unknown>;
  assert.equal(visible.api_version, 1);
  assert.equal("idle_since" in visible, false);
  assert.equal("retry_index" in visible, false);
});
