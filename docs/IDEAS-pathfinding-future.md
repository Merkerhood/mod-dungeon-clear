# Dungeon-Clear — Future Pathfinding Ideas

Speculative, larger-scope directions captured during the 2026-06-05 navigation
review. These are **not** scheduled work — they are here to explore later. Each
entry states the idea, why it could matter, a rough sketch, and the open
questions that need answering before it becomes a real plan.

Related current architecture:
- Primary producer: `LongRangePathfinder` (own 65 535-node Detour A*, whole
  route in one call, offloaded to `DcPathWorker`).
- Fallbacks: anchor routes (`DungeonClearRouteRegistry`), then the
  bee-line / arc / spawn-graph tiers in `StridedPathfinder::Build`.

---

## Idea A — Cross-run route cache (highest leverage)

**Idea.** Memoize successful long-range corridors keyed on
`(mapId, difficulty, fromBossEntry → toBossEntry)`. The first group through a
dungeon pays the 65 k-node A*; every later group (and every later leg the same
group repeats) reuses the cached polyline. Optionally persist the cache so it
survives restarts.

**Why it matters.** Routes between two encounters are *static* per map and
difficulty. Today every run re-derives them from scratch on the worker thread.
A shared cache turns the heavy A* into a one-time cost per route per server
lifetime (or forever, if persisted), which:
- removes nearly all steady-state pathfinding cost on a busy realm,
- makes the `DC_ASYNC_PATH_PENDING_TIMEOUT_MS` world-thread sync-fallback
  effectively never fire,
- is, in effect, *auto-generating the hand-tuned anchor routes we already
  support* (see Idea B).

**Sketch.**
- Cache value = the `RawResult` (plain floats, no `Player*`/mesh) so it stays
  worker-safe and can be `Finalize()`d against whichever bot needs it.
- Key on snapped boss coords, not bot start position — the route is
  boss→boss; the leading bot-to-firstNode hop is re-derived per call (cheap).
- Store in a process-global guarded map (same locking discipline as
  `DcPathWorker`'s mailbox). Invalidate the whole map on mmap/version change.
- A persisted form could live in a small `acore_world`-side table or a flat
  file under the module, written lazily on first successful build.

**Open questions.**
- Boss positions for *wandering* bosses aren't static (the live-boss tracking
  in `Advance` exists precisely because of this). Cache only the static-spawn
  leg and keep the live final-approach (`DC_DIRECT_PURSUIT_RANGE`) dynamic?
- Cache key when a boss has multiple valid approach sides (multi-wing maps,
  `DungeonWingRegistry`)? Likely fold the wing into the key.
- Memory bound: how many `(map, diff, from, to)` pairs realistically exist, and
  do we cap/LRU it? (Probably small — bosses per dungeon is single digits.)
- Cross-difficulty navmesh identity: is the mesh identical for normal/heroic of
  the same map? If yes, drop `difficulty` from the key.

---

## Idea B — Promote hot auto-routes into anchor suggestions

**Idea.** When the long-range producer rebuilds the *same* corridor N times (or
on an operator command like `.dc dumproute`), emit it in `WaypointHint` format
ready to paste into `DungeonClearRouteRegistry`.

**Why it matters.** The hand-authored anchor registry is the real fix for the
recurring "path ends short on a ledge / pull from the wrong side of a doorway"
class of stalls — but authoring coordinates by hand is slow and error-prone.
A data-driven dump turns observed-good routes into registry candidates, so the
registry fills itself from real play instead of manual surveying.

**Sketch.**
- Pairs naturally with Idea A's cache: the cache already holds the canonical
  polyline; a dump command just formats it.
- Down-sample the smoothed polyline to a handful of anchors (corners /
  elevation changes), since anchors are sparse by design.
- Tag jump/door legs from the geometry heuristics already present
  (`JUMP_DZ`, door detection) so the emitted hints carry the right flags.

**Open questions.**
- Auto-applying is risky (a bad observed route becomes a permanent anchor);
  keep it suggestion-only with human review?
- How to detect a "corner worth anchoring" vs. ordinary smoothing noise?
- Where does the dump go — log channel, a file, an in-game whisper?

---

## Idea C — Unify movement issuance behind a single "intent"

**Idea.** Replace the three independent movement-issuing paths in
`Advance::Execute` (direct-pursuit `MoveTo`, long-path escort spline,
final-approach `MoveTo`) with one `MovementIntent { target, kind }` that is
recomputed each tick and re-issued only when the target moves beyond a dedup
threshold.

**Why it matters.** The `StopActiveSplineGlide` + pursuit-latch +
spline-reissue-guard dance exists because multiple code paths can each issue
movement and must defensively cancel each other. That arbitration is the
subject of several past bugfixes (`dc-direct-pursuit-freeze`,
`dungeon-clear-spline-advance`, the path-ends-short livelock). A single
intent recomputed per tick, with dedup instead of stop/relaunch, would collapse
those three paths into one place and likely retire the whole freeze/stutter
family.

**Sketch.**
- `kind ∈ { PursueLiveBoss, FollowLongPath, FinalApproach, Recover }`.
- Each tick: pick the intent, compare to the in-flight one; if the target
  drifted < dedup-radius and the generator is healthy, leave it; else (re)issue.
- The escort-spline window becomes just the issuance mechanism for
  `FollowLongPath`, not a separate control flow with its own guards.

**Open questions.**
- This is closely tied to the Finding #1 decomposition of `Advance::Execute` —
  best done *after* that refactor lands, on top of the extracted phase helpers.
- Does the engine's `MoveTo` dedup (`IsDuplicateMove`) already give us enough
  of the "don't relaunch" behavior to lean on, or do we need our own
  target-drift threshold?
- How does combat preemption (`AttackAction` clearing NORMAL-priority moves)
  interact with a persistent intent — does the intent need to survive a combat
  interruption and re-assert, or be recomputed fresh?

---

## Smaller spin-off thoughts

- **Boss "room entrance" targeting.** Instead of aiming at the boss centroid,
  aim at the navmesh poly where the boss's room connects to the corridor, then
  let the live final-approach close the gap. Could reduce ledge/gap dead-ends
  without hand anchors. Overlaps with Idea A/B.
- **Spawn-graph → real waypoint graph.** The current spawn-graph fallback
  (`DungeonSpawnGraph::FindCorridor`) is a per-call linear projection of creature
  spawns onto the start→target line. A precomputed connectivity graph between
  spawn clusters would follow non-straight corridors better — but this only ever
  runs when the primary producer already failed, so the payoff is narrow.
