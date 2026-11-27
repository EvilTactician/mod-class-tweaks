# Class Tweaks 1.0

<div align="center">

![Interface Version](https://img.shields.io/badge/Interface-30300-blue)
![WoW Version](https://img.shields.io/badge/WoW-3.3.5a-orange)
[![Version](https://img.shields.io/badge/Version-1.0.0-green)](https://github.com/EvilTactician/mod-class-tweaks)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

**AzerothCore module (WotLK 3.3.5a) with class quality-of-life and balance tweaks. No client patch needed. Only real players are affected; playerbots keep stock behaviour. Every feature can be switched off or tuned in `mod_class_tweaks.conf`.**

</div>

Built for AzerothCore with mod-playerbots. Playerbot sessions are detected through
`WorldSession::IsHeadless()` (or `IsBot()` on older cores); if neither exists the build fails
rather than silently treating bots as players.

## Rogue

- **Combo points follow you** between targets (based on
  [buildthehomelab/wow-mod-forever-rogue](https://github.com/buildthehomelab/wow-mod-forever-rogue), rewritten
  event-driven). Points move to the next hostile target you select or use a builder on; points from a target
  that died wait a configurable time.
- **Quick lockpick** (based on [buildthehomelab/wow-mod-quick-lockpick](https://github.com/buildthehomelab/wow-mod-quick-lockpick)).
  Right-click a locked box to Pick Lock it; it opens as soon as it unlocks (also with skeleton keys, any class).
- **Poison duration**: configurable (stock 60 minutes by default).
- **Poison auto-apply**: when a poison runs out on the main or off hand, the same poison (same rank) is taken
  from your bags and put back on that weapon. In dungeons and raids, optionally everywhere. Per character via
  `.classtweaks poison auto on|off` and `.classtweaks poison outside on|off`, or from
  [my DragonUI fork](https://github.com/EvilTactician/wow-addon-DragonUI) (see Addon support below).
- **Poison attack power scaling**: the attack power part of Instant, Deadly and Wound Poison damage × multiplier.
- **Find Weakness poison bypass**: with Find Weakness rank 3 (Assassination), poisons and Envenom go through a
  creature's own Nature immunity. Other rogues, bots, players and temporary immunities are unaffected.
- **Rupture**: damage multiplier.

## Priest

- **Smite and Holy Fire**: damage multiplier against creatures (not in Shadowform; PvP untouched).
- **Holy AoE healing** (priests with the Circle of Healing talent): critical Circle of Healing and Prayer of
  Healing heals also trigger Holy Concentration, and their spell power part is multiplied.

## Install

1. Put the `mod-class-tweaks` folder in `azerothcore/modules/` (the folder name must stay `mod-class-tweaks`).
2. Rebuild the worldserver. The module's SQL (`data/sql/db-world/`) is applied automatically on startup.
3. Copy `conf/mod_class_tweaks.conf.dist` to your config folder as `mod_class_tweaks.conf`.
4. For per-character poison choices to be kept between sessions, set `EnablePlayerSettings = 1` in
   `worldserver.conf`.

The worldserver log shows on startup:

```
mod-class-tweaks 1.0.0: N rogue poison enchants found (N poison items, N proc spells).
mod-class-tweaks: Find Weakness poison bypass ready (N spells).
mod-class-tweaks: N Holy Concentration ranks found.
```

## Addon support

[My DragonUI fork](https://github.com/EvilTactician/wow-addon-DragonUI) supports the poison auto-apply options:
rogues get checkboxes for auto-apply in dungeons and raids and outside them. They only appear when the server runs
this module; everything also works without the addon through the chat command.

## Addon protocol (optional)

Any addon can show the poison auto-apply options. Messages use the addon prefix `ClassTweaks`, sent as an addon
whisper to the player themselves; the server answers the same way.

| Direction | Message | Meaning |
|---|---|---|
| client → server | `HELLO 1` | Ask whether the module is there |
| client → server | `SET AUTOAPPLY 0\|1` | Auto-apply in dungeons and raids |
| client → server | `SET OUTSIDE 0\|1` | Also auto-apply outside dungeons and raids |
| server → client | `STATE 1 <autoapply> <outside> <outsideAllowed>` | Current settings (after `HELLO` and every change) |

No answer to `HELLO` means the module isn't installed, the character isn't a rogue, or auto-apply is off on
the realm; the addon should then show nothing.

## Files

- `src/ClassTweaksScripts.cpp` — config, hooks (each registered only for what it uses), chat command
- `src/RogueComboPoints.cpp`, `src/RogueQuickLockpick.cpp`, `src/RoguePoisons.cpp`, `src/RogueBalance.cpp`
- `src/PriestBalance.cpp`
- `src/ClassTweaks.h` — shared state and the playerbot check
- `data/sql/db-world/mod_class_tweaks_spell_scripts.sql` — binds the Holy AoE spell script

## Credits

The combo point and quick lockpick features are based on buildthehomelab's MIT-licensed modules
[wow-mod-forever-rogue](https://github.com/buildthehomelab/wow-mod-forever-rogue) and
[wow-mod-quick-lockpick](https://github.com/buildthehomelab/wow-mod-quick-lockpick).
All other features are my original work.
