# Plan — Navigation review follow-up (high-value findings + #5)

Derived from the 2026-06-05 review of `mod-dungeon-clear` and the DungeonClear
addon. Scope: high-value findings **#1** (decompose `Advance::Execute`), **#2**
(raid-correct addon transport), **#3** (hoist duplicated geometry/LOS code), and
medium-value point **#5** (defer `CorridorCenter` in the strided fallback).

Ordering is deliberate: do the **low-risk, mechanical** work first (#2, #3, #5)
so the tree is clean and the shared helpers exist, then tackle the **structural**
refactor (#1) on top of them.

Guardrail for all phases: the gtest suite (`sudo bash t/run_tests.sh` from the
module root, 21 tests over math/util/status) must stay green. Build only when
explicitly asked; C++ changes need a `build.sh` recompile + worldserver restart
to take effect, Lua changes are live on `/reload`.

---

## Phase 0 — DONE: Finding #2, addon raid transport

The client→server direction was broken for raids: the addon sent on `"PARTY"`
(subgroup-only) and the hook rejected `CHAT_MSG_RAID`. The server→client push
path was already raid-safe (it unicasts via `GetRealPlayersInGroup()`).

**Applied:**
- `DungeonClear.lua` `SendDcCommand` — send on `"RAID"` when
  `GetNumRaidMembers() > 0`, else `"PARTY"`.
- `DungeonClearAddonHook.cpp` — accept `CHAT_MSG_RAID` / `CHAT_MSG_RAID_LEADER`
  in addition to the party channels; doc comment updated.

**Remaining to verify (in-game, raid of ≥2 subgroups with a tank bot):**
- On/Off/Skip/Pause/Go from the addon reach the leader tank when the human is
  in a *different subgroup* than the tank.
- STATUS/BOSS pushes still render for every raid member (expected — push path
  unchanged).
- Plain 5-man party path is unchanged (still uses `"PARTY"`).

Requires a worldserver rebuild for the C++ hook change.

---

## Phase 1 — Finding #3: hoist duplicated geometry / LOS code

Pure mechanical de-duplication; no behavior change. Highest payoff-to-risk.

**Duplication to collapse** (currently copy-pasted, code comments admit it):
- `LOSCleanPrefixCount` + the `losClear` lambda + `LOS_Z_BUMP` / `LOS_MIN_HOP` /
  `LOS_GRAZE_BRIDGE` — identical in `StridedPathfinder.cpp` and
  `LongRangePathfinder.cpp`.
- `CorridorCenter::ChordClear` — a third copy of the same VMAP raycast.
- `Dist2D` / `Dist3D` / `Dist3DSq` helpers — scattered across producers and the
  follower.
- The `{y,z,x}` Detour ↔ G3D coordinate swaps, repeated inline everywhere.

**Steps.**
1. New TU `Util/DungeonClearGeometry.{h,cpp}` with:
   - `float LosPrefixCount(Player*, std::vector<G3D::Vector3> const&)` (the
     graze-bridged LOS prefix walk) and a `ChordClear(Player*, a, b)` primitive
     it shares with `CorridorCenter`.
   - `Dist2D/Dist3D/Dist3DSq` and the LOS tuning constants as the single source.
   - Small `ToDetour(x,y,z)` / `FromDetour(...)` inline helpers.
2. Replace the three copies; keep the exact current constants so geometry is
   byte-identical.
3. Wire into `mod-dungeon-clear.cmake` (auto-globbed today — confirm the new
   files are picked up).
4. Run gtest. Add a focused unit test for `LosPrefixCount`'s graze-bridge /
   truncation behavior if not already covered (it's the trickiest piece).

**Risk:** low. **Net:** removes the documented "keep these two in sync" drift
hazard; later tuning happens in one place.

---

## Phase 2 — Point #5: defer CorridorCenter in the strided fallback

`StridedPathfinder::TryProbe` runs `CorridorCenter::Center` (raycast-heavy)
*before* the LOS screen, and `TryProbe` runs for **rejected** tiers too
(direct + 3 bee-line reaches + 4 arcs × 3 reaches + N spawn-graph nodes). On a
worst-case route that's a dozen-plus centerings whose work is thrown away.

Only reached when the *primary* long-range producer already failed, so this is a
narrow efficiency win — sequenced after Phase 1 so it builds on the shared
helpers.

**Approach (pick one during implementation):**
- **Option A (minimal):** keep centering coupled to the LOS screen but cache the
  centered result per `(probe-start, probe-end)` within a single `Build` call so
  repeated tiers toward the same snapped point don't recompute. Lowest risk,
  preserves the current "verify the centered line" contract.
- **Option B (deferred):** LOS-screen the *un-centered* line to accept/reject a
  tier, and only center the **chosen** stride's polyline once it's committed to
  `result.segments`, then re-screen that one. Bigger win, but changes which
  exact line is verified — needs the in-game wall-grazing check below.

**Verification.** Centering quality is visual: run a dungeon where the fallback
is known to engage (primary long-range fails — e.g. a multi-level seam) and
confirm the tank still hugs corridor centers, doesn't clip walls, and doesn't
regress the door-approach stutter (`dc-event-door-force-open`,
`dc-posstuck-false-positive` are the relevant past bugs). If Option B shows any
wall-clip, fall back to Option A.

**Risk:** low–medium (Option A low, Option B medium). Gate on in-game check.

---

## Phase 3 — Finding #1: decompose `Advance::Execute`

The structural payoff and the riskiest phase — done last, on a clean tree.
`DungeonClearActions.cpp:830-1581` is one ~750-line method running ~16 sequential
decision phases and juggling five failure counters (`stuck`, `posStuck`,
`rebuildAttempts`, `doneNotEngagedTicks`, `pursuitFailTicks`) with reset rules
scattered throughout. Several past freeze/livelock bugs live in the *interaction*
between these.

**Step 3a — extract phases (no behavior change).**
Pull each phase into a named helper returning a small result enum:

```cpp
enum class AdvanceStep { Handled, FallThrough, Stall };
```

Candidate helpers (in execution order):
- `ResolveNextBoss` → live-boss position + engage range/atBoss predicates.
- `TryEngageHold` (the at-boss park).
- `TryLootYield` (loot lifecycle + commit timeout).
- `TryBetweenPullsRest`.
- `TryBossGridStreamIn` (the not-present-but-far case).
- `UpdateStuckBookkeeping` + `RecoverPosStuck`.
- `TryDirectPursuit` (live-boss LOS pursuit + fail latch).
- `DriveLongPath` (EnsureLongPath → off-path resnap → hop → jump/spline/MoveTo).

Each helper takes `(bot, ctx, next, …)` and the shared computed predicates.
`Execute` becomes a short ladder of `if (step == Handled) return …;`.
**Keep counter semantics identical** in this step — move code, don't change it.
Run gtest after each extraction.

**Step 3b — unify the counters.**
Replace the five loose counters with one `ApproachHealth` struct holding the
distinct concerns and explicit transitions:
- movement-not-issued (`stuck`), no-displacement (`posStuck`),
  consecutive-rebuilds (`rebuildAttempts`), path-ends-short
  (`doneNotEngagedTicks`), pursuit-unreachable latch (`pursuitFailTicks`).
- Centralize the resets (boss change, entered-engage-range, real forward
  progress) into `ApproachHealth::OnBossChange()` /
  `OnEnteredEngageRange()` / `OnForwardProgress()` so the scattered reset sites
  collapse to three method calls.

**Verification.** This is the one phase that must be regression-tested against
the known failure scenarios, since those bugs lived here:
- vertically-separated boss (`dc-boss-grid-load-deadlock`),
- close-but-outside-pull-range freeze (`dc-direct-pursuit-freeze`),
- route-ends-short livelock (`dc-path-ends-short-livelock`),
- hallway/door stutter (`dc-posstuck-false-positive`),
- loot↔advance oscillation (`dungeon-clear-loot-oscillation`).
Walk a multi-level dungeon end-to-end with the DC debug log channel on and
confirm none recur. Extend the gtest suite where a phase is now pure enough to
unit-test (e.g. `ApproachHealth` transitions in isolation).

**Risk:** medium–high. Mitigation: land 3a (mechanical) and 3b (semantic)
as separate commits so a regression bisects cleanly to one or the other.

---

## Suggested commit sequence

1. `fix(addon): route DC commands over RAID channel in raids` — Phase 0 (Lua repo).
2. `fix(DungeonClear): accept RAID addon channel in command hook` — Phase 0 (module).
3. `refactor(DungeonClear): hoist shared LOS/geometry into one TU` — Phase 1.
4. `perf(DungeonClear): avoid centering rejected strided probes` — Phase 2.
5. `refactor(DungeonClear): extract Advance::Execute phase helpers` — Phase 3a.
6. `refactor(DungeonClear): unify approach-health counters` — Phase 3b.

Each is independently buildable/testable; stop and verify between 5 and 6.

## Out of scope (tracked separately)

The cross-run route cache, auto-anchor generation, and unified movement-intent
ideas live in `IDEAS-pathfinding-future.md`. Idea C (movement intent) is a
natural successor to Phase 3 and should only be attempted on top of the
extracted phase helpers.
