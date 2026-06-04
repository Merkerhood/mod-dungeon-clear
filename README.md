# mod-dungeon-clear

Autonomous dungeon-clearing mode for **mod-playerbots** tank bots, packaged as a
drop-in AzerothCore module. A tank bot drives the party from boss to boss —
clearing blocking trash, pathing around the layout, pausing for loot, and
handling doors and stalls along the way. You sit back, deal damage, and let the
tank run the dungeon; no need to lead pulls or remember the route.

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
| `.dc status` | `dc status` |
| `.dc bosses` | `dc bosses` |

The `.dc` slash command always works. **Chat keywords and follow-tank need the
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

## License

AGPL-3.0-or-later (inherited from mod-playerbots). See `LICENSE`.
