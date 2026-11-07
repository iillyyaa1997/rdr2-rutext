# RuText for Red Dead Redemption 2

An ASI plugin for RDR2 Story Mode with two jobs:

1. **Translate the hard-coded text of ScriptHook mods.** Many mods have menus, prompts and messages in English that are compiled into the `.asi`. RuText swaps that text for a translation from plain-text dictionaries at the moment the mod sends it to the game. The mod files stay untouched.
2. **Hideout memory for [Bandit Hideouts](https://www.nexusmods.com/reddeadredemption2/mods/2213).** Hideout skulls stay off the map until you have been near the hideout, then they stay on the map for good, the way the map fills in as you explore. A cleared hideout still disappears until it respawns, as Bandit Hideouts does it.

## Requirements

- RDR2 1.0.1491.50 (Steam), Story Mode
- [ScriptHookRDR2 V2](https://www.nexusmods.com/reddeadredemption2/mods/1472) and an ASI loader (`dinput8.dll` or `version.dll`)

## Install

Copy `RuText.asi` into the game folder and `RuText.ini` into `<game>\RuText\`. Put dictionaries into `<game>\RuText\`.
With Vortex: install the archive and set the mod type to the one that deploys into the game folder (`reddeadredemption2asi` in the RDR2 extension).

## Dictionaries

One UTF-8 file per mod, named after the mod's `.asi`: `PedDamageOverhaul.txt` is used for `PedDamageOverhaul.asi`, `_common.txt` for every mod. See [`examples/ExampleMod.txt`](examples/ExampleMod.txt).

```
Open Menu => Открыть меню                       exact line
You paid $*. They rode off. => Ты заплатил $*.  pattern, * captures text
~Valentine => Валентайн                          replaced inside longer lines
```

Files are reloaded while the game runs. English text that reached the screen without a translation is written to `<game>\RuText\untranslated\<mod>.txt`, ready to be filled in.

This repository ships no dictionaries for other people's mods: their authors ask for permission before their files are changed or translated.

## Hideout memory

Needs `ALWAYS_SHOW_HIDEOUT_BLIP=1` in `Bandit_Hideouts.ini` (Bandit Hideouts then creates a skull for every hideout; RuText hides the ones you have not visited). Settings in `RuText.ini`, section `[HideoutMemory]`:

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | |
| `Radius` | 150 | metres: how close you have to come for a hideout to count as visited |
| `Diag` | 0 | 1 logs every native the hideout module calls |

Visited hideouts are kept in `<game>\RuText\HideoutMemory.dat` (one `x y z` per line). Delete it to start over.

## How it works

ScriptHook mods call natives through three exports of `ScriptHookRDR2.dll`: `nativeInit`, `nativePush64`, `nativeCall`. RuText rewrites those import-table entries in every loaded `.asi`. For `CREATE_VAR_STRING(…, "LITERAL_STRING", text)` and `DATABINDING_ADD_DATA_STRING` it swaps the text pointer. For the hideout module it watches `BLIP_ADD_FOR_RADIUS` / `SET_BLIP_SPRITE` and toggles `BLIP_MODIFIER_HIDDEN` on the skulls.

## Build

`./build.sh` cross-compiles `out/RuText.asi` with mingw-w64 in a throwaway Docker container.

## License

MIT, see [LICENSE](../LICENSE)

---

## По-русски

ASI-плагин для сюжетного режима RDR2:

1. **Переводит зашитый в моды английский текст** (меню, подсказки, сообщения) по словарям в `<game>\RuText\`. Сами моды не меняются. Непереведённое, что появилось на экране, пишется в `RuText\untranslated\`.
2. **Память логов для Bandit Hideouts:** значок логова появляется на карте, когда вы подъехали ближе 150 м, и остаётся навсегда. Нужен `ALWAYS_SHOW_HIDEOUT_BLIP=1` в `Bandit_Hideouts.ini`.

Нужны ScriptHookRDR2 V2 и ASI-загрузчик. `RuText.asi` — в папку игры, `RuText.ini` — в `<game>\RuText\`. Словари для чужих модов здесь не лежат: их авторы просят спрашивать разрешения.
