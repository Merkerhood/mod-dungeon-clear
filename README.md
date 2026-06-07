# mod-dungeon-clear

Autonomous dungeon-clearing mode for **mod-playerbots** tank bots, packaged as a
drop-in AzerothCore module. A tank bot drives the party from boss to boss —
clearing blocking trash, pathing around the layout, pausing for loot, and
handling doors and stalls along the way. You sit back, deal damage, and let the
tank run the dungeon; no need to lead pulls or remember the route.

There are no waypoints or hardcoded paths — every route is generated on the fly
from the live navigation mesh, so it works in any instance, raids included.

> ## ❗ Use the companion addon
>
> [**mod-dungeon-clear-addon**](https://github.com/jrad7/mod-dungeon-clear-addon)
> is the recommended way to drive a clear: a movable in-game panel with On / Off
> / Skip / Pause buttons, a live status readout, and a per-boss list. The `dc`
> chat keywords and `.dc` command still work and are documented below, but the
> addon is far easier. See [Companion addon](#companion-addon).

## Requirements

- **mod-playerbots** installed and enabled. This is an *extension* of the
  playerbots AI engine, not a standalone module; it subclasses playerbots'
  strategy/action/trigger/value classes and links against them.
- Works against a **stock, unmodified mod-playerbots** checkout — no playerbots
  source edits required. See [How it integrates](#how-it-integrates).

## Install

1. Clone into `modules/mod-dungeon-clear/`.
2. Re-run CMake and rebuild the worldserver (`-DMODULES=static`).
3. Optionally copy `conf/mod_dungeon_clear.conf.dist` → `mod_dungeon_clear.conf`
   (only affects the DungeonClear log channel).

## Usage

Both input methods control the same behaviour and act on the group's **tank**
bot. Commands must come from a real player in the bot's group; `.dc on` requires
being inside a dungeon.

| Slash command | In-party chat keyword |
|---|---|
| `.dc on` | `dc on` / `dungeon clear on` |
| `.dc off` | `dc off` / `dungeon clear off` |
| `.dc skip` | `dc skip` |
| `.dc pause` | `dc pause` / `dungeon clear pause` |
| `.dc pull` | `dc pull` / `dungeon clear pull` |
| `.dc status` | `dc status` |
| `.dc bosses` | `dc bosses` |

## Pull modes

How the tank takes trash packs on the way to each boss. There are three modes,
selected from the addon's pull control (or the `dc pull` toggle / setting):

| Mode | Behaviour |
|---|---|
| **Leeroy** *(default)* | Walk straight into each pack and fight on top of it. Fast, no choreography. |
| **Advanced** | Pull every pack back to a held camp before fighting it. Careful, but slow. |
| **Dynamic** | Decide per pack: Leeroy a lone pack, Advanced-pull a clustered or oversized one. |

> ### ❗ Start with Leeroy
>
> **Leeroy is the default and is the right choice for almost every dungeon.** The
> tank's threat, the party's DPS, and bot self-healing handle ordinary trash
> packs fine when fought in place, and Leeroy keeps a clear moving at full speed.
>
> **Advanced and Dynamic make a clear dramatically slower** — every pull adds a
> camp-setup, a tag, and a drag-back round trip. Only reach for **Advanced** in
> the specific spots where Leeroy is *failing* — packs so large or so tightly
> stacked that fighting them in place wipes the party. **Dynamic** is the
> compromise: it Leeroys by default and only pays the Advanced cost on the packs
> that actually need it, so it's slower than pure Leeroy but faster than forcing
> Advanced everywhere.

### Leeroy (default)

The tank walks into each pack and fights it where it stands, party following
normally. No camps, no tagging, no drag-back — just clear and move on. This is
the fastest mode and what you should use unless a particular pull is wiping you.

### Advanced (`dc pull`)

Instead of walking into a pack and fighting on top of it, the tank:

1. marks a **camp** a good distance back along the already-cleared route
   (`DungeonClear.PullSetback`), pushed further if needed so the fight won't aggro
   a neighbouring pack (`DungeonClear.PullCampSafeRadius`, capped by
   `DungeonClear.PullMaxDrag`) — dungeon mobs have no leash, so the camp is placed
   for room, not to avoid an evade;
2. sends the DPS and healers to the camp and holds them there — in pull mode the
   party **never follows the tank**, it holds at the camp and leapfrogs
   camp-to-camp as each pull marks a new one, so it can't pile onto a pull;
3. once the party is set, **tags** the pack — a ranged class pull (Heroic Throw,
   Avenger's Shield, Death Grip, Faerie Fire) when it has one, otherwise it steps
   in — and immediately **drags** the pack back to the waiting party;
4. **releases** the party to fight the moment the tank reaches camp.

If a held, passive member is hurt (a patrol clipped the camp) the pull aborts and
the whole party is released to defend (`DungeonClear.PullSafetyHpPct`). Advanced
pull is correct but **much slower** than Leeroy because of the camp/tag/drag round
trip on every pack — use it only where Leeroy can't hold.

### Dynamic

Dynamic keeps Leeroy as the baseline and upgrades to an Advanced pull only when a
pack warrants it, deciding fresh for each pack the tank approaches:

- a **lone** pack — nothing else within `DungeonClear.PullDynamicChainRadius`
  (line-of-sight, not behind a closed door) and at or under
  `DungeonClear.PullDynamicLargePackThreshold` mobs — is **Leeroy**-pulled;
- a **clustered** room (another pack inside the chain radius) or an oversized lone
  pack is handled with the careful **Advanced** pull-to-camp.

Each pack gets one stable verdict, and the addon's status readout shows what
Dynamic chose. It is slower than pure Leeroy but spends the Advanced cost only on
the packs that need it.

Pull mode is per-run and resets with the clear. The `.dc` slash command always
works. **Chat keywords and follow-tank need the
`dungeon clear` strategy applied** — the login hook applies it to bots present
at login, but the only path that reaches a self-bot created mid-session is the
playerbots config. Add to your deployed `playerbots.conf`:

```
AiPlayerbot.NonCombatStrategies          = "+dungeon clear"
AiPlayerbot.RandomBotNonCombatStrategies = "+dungeon clear"
```

Non-tank party bots follow the tank only while it has dungeon clear enabled,
then revert to the player automatically.

### You can't play *as* the tank with dungeon clear on

Dungeon clear drives the **tank bot's** AI, so the tank must be a bot — if you
personally control the tank, the AI and you fight over the same character and
movement breaks. The exception is **self-bot mode**: turn your own character
into a self-bot (`.playerbots bot self`) and let the AI drive, and dungeon clear
works on it normally (self-bots need the `NonCombatStrategies` config above). If
you want hands on the keyboard, roll the tank as a normal player bot and play a
follower instead.

## Companion addon

[**mod-dungeon-clear-addon**](https://github.com/jrad7/mod-dungeon-clear-addon)
is a client-side WoW addon (interface 30300, patch 3.3.5a) that replaces typing
`dc`/`.dc` commands with a movable in-game panel. Everything it does is also
reachable via chat keywords and `.dc`.

**Install:** copy the `DungeonClear` folder (`DungeonClear.toc` +
`DungeonClear.lua`) into your client's `Interface/AddOns/` and enable
**DungeonClear** on the character-select list. No extra server-side install.

**Features:**

- `/dc` toggles the window; `/dc <sub> [param]` forwards a raw subcommand (e.g.
  `/dc on`, `/dc skip`) to the tank.
- **Action row:** On / Off / Skip / Pause·Resume. Pause holds the tank in place
  without ending the clear.
- **Status panel:** live mode (OFF / ON / PAUSED), current state (Advancing,
  Clearing Trash, Boss Fight, Looting, Resting, Door Blocked, …), next boss
  target, and a stall warning.
- **Boss list:** every boss with status (Alive / Dead / Skipped / Missing) and a
  per-boss **Go** button (auto-enables dungeon clear). Filtered to the bot's
  nearest wing on split maps.
- **Tiny mode:** collapses the panel to a single-line readout; window position,
  state, and visibility persist via saved variables.

## How it integrates

mod-playerbots exposes no extension API, so this module:

1. Appends its four `DungeonClear*Context` factories into the engine's shared
   context registries on the first world tick (see `src/AiObjectContextAccess.h`
   and `src/DungeonClearModule.cpp`).
2. Registers a `.dc` command and a login hook for the `dungeon clear` strategy.

It touches **no** playerbots file. The trade-off: it couples to playerbots'
internal class shape, so an upstream rename of those registries surfaces as a
**compile error** here, never a silent runtime failure.

## mod-playerbots settings

Between pulls the tank waits for the party to rest. By default eating/drinking is
mod-playerbots' job, which restores health up to `AiPlayerbot.AlmostFullHealth`
(default `85`) and mana up to `AiPlayerbot.HighMana` (default `65`), then stops.
The rest gate tracks those two settings automatically, so the tank pulls as soon
as the party finishes resting; **no config change is needed.**

To rest to a different level **without touching the playerbots config** (which a
group on a shared server can't see), set `DungeonClear.RestHealthPct` and/or
`DungeonClear.RestManaPct` — overridable live from the companion addon's Settings
panel, per run. `0` inherits the playerbots value. A non-zero value both raises
the rest gate **and** drives the bots to eat/drink up to it (the
`DungeonClearNeeds{Eat,Drink}` triggers top up past the stock stop; the
`DungeonClearMultiplier` caps below it), so a group can rest harder — or pull
faster — than the server-wide default.

A few other playerbots ranges also affect runs: `AiPlayerbot.LootDistance`,
`ReactDistance` (pull range), `SightDistance` (target scan), and `FollowDistance`.

## License

AGPL-3.0-or-later (inherited from mod-playerbots). See `LICENSE`.
