/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DUNGEON_CLEAR_TUNING_H
#define _DUNGEON_CLEAR_TUNING_H

#include <cmath>

// Single source of truth for the DungeonClear tuning constants that are SHARED
// across translation units. Historically several of these were defined twice
// (once in Trigger/DungeonClearTriggers.cpp, once in
// Action/DungeonClearActions.cpp) with comments warning they "must stay in
// sync" — kept aligned only by hand. Centralizing them here makes drift
// impossible: one definition, every site includes it.
//
// Phase 1 of the constant-centralization refactor only houses the previously
// DUPLICATED constants. File-local (single-TU) constants stay where they are;
// Bucket-A consolidation and Bucket-B promotion to DcSettings are later phases.
//
// NOTE: namespace-scope constexpr has internal linkage, so each including TU
// gets its own internal copy seeded from this one definition — no ODR issue and
// no linker conflict. The DC_ names are kept at global scope (as they were
// effectively file-global via the old anonymous-namespace defs) so call sites
// stay unqualified and unchanged.

// Asymmetric ranges so a trash pack sitting just outside the boss room gets
// engaged before the at-boss trigger fires. 22yd is just outside most level-80
// elite aggro radii (~18-20yd), giving room to position before melee. The
// trigger uses this to decide "at the boss"; the action uses it for the same
// hand-off, so they must agree.
constexpr float DC_ENGAGE_RANGE = 22.0f;

// Cone scan for "blocking trash" — the geometric fallback the trigger uses and
// the action falls back to when the corridor path scan can't run (boss off-mesh,
// etc.). 35yd catches packs one tick-cycle earlier than the engage range. Both
// TUs feed it to DungeonClearUtil::FindBlockingTrash, so it is one constant
// despite the old per-context names (DC_ENGAGE_CONE_* / DC_TRASH_CONE_*).
constexpr float DC_TRASH_CONE_RANGE = 35.0f;
constexpr float DC_TRASH_CONE_HALF_ANGLE = static_cast<float>(M_PI) / 3.0f;  // 60°

// When true, evaluate "blocking trash" via the bot's actual mmap path polyline
// instead of the geometric cone. Catches packs around corners and avoids "pack
// on the other side of a wall" false positives. Falls back to the cone scan when
// path computation fails. Both TUs must agree or one path scans while the other
// does not.
constexpr bool  DC_USE_CORRIDOR_SCAN = true;
constexpr float DC_CORRIDOR_LOOKAHEAD = 35.0f;
// Half-width of the path "blocking trash" band. Widened from 8 to 18 so it
// roughly matches level-80 elite aggro radius: a pack sitting a few yards off the
// route line still aggros as the tank passes, so it must count as blocking trash.
// With the old 8yd band such a pack was never a candidate, so the selector picked
// an on-line mob farther ahead and the tank ran straight through the side pack to
// reach it. Trigger and action share the FindBlockingTrashOnPath band, so this
// must be one value.
constexpr float DC_CORRIDOR_WIDTH = 18.0f;

// Advanced-pull commit range: a pull is only STARTED once its target pack is
// within this distance, so the camp (stamped at the tank's spot) stays close
// enough that the run-in reaches the pack and the drag-back is short. The
// corridor scan sees packs out to ~35yd, so without this the camp lands in the
// run-in's overshoot dead band and every pull after the first whiffs. The pull
// trigger and the pull action must agree on when a pull starts.
constexpr float DC_PULL_START_RANGE = 26.0f;

// Max distance the tank may lead a party member before it holds the advance to
// let them catch up. Configurable; see DungeonClear.PartyMaxSpread (this is the
// default the trigger and action both read). The HP/mana recovery thresholds
// live in DungeonClearUtil::RestMin{Hp,Mp}Pct().
constexpr float DC_PARTY_MAX_SPREAD_DEFAULT = 25.0f;

#endif  // _DUNGEON_CLEAR_TUNING_H
