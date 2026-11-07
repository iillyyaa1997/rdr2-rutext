# RuText for Red Dead Redemption 2

> **Superseded by [Telegraph — Mod Auto Translator](https://github.com/iillyyaa1997/rdr2-telegraph).** Telegraph does everything RuText does, reads RuText dictionaries as they are, and translates the rest automatically (Google without a key, or DeepL / DeepSeek / an OpenAI-compatible model with your key). RuText stays here for those who want dictionaries only.
>
> **Заменён на [Telegraph](https://github.com/iillyyaa1997/rdr2-telegraph):** то же самое плюс автоматический перевод того, чего нет в словарях. Словари RuText подходят без изменений.

Shows the hard-coded English text of ScriptHook mods in your language. Many mods have menus, prompts and messages compiled into the `.asi`. RuText swaps that text for a translation from plain-text dictionaries at the moment the mod sends it to the game. The mod files stay untouched.

Download: [Nexus Mods](https://www.nexusmods.com/reddeadredemption2/mods/11518) or [Releases](https://github.com/iillyyaa1997/rdr2-rutext/releases).

Hideout memory for Bandit Hideouts, which was part of RuText 1.4.0, is now its own mod: [rdr2-hideout-memory](https://github.com/iillyyaa1997/rdr2-hideout-memory).

## Requirements

- RDR2 1.0.1491.50 (Steam), Story Mode
- [ScriptHookRDR2 V2](https://www.nexusmods.com/reddeadredemption2/mods/1472) and an ASI loader (`dinput8.dll` or `version.dll`)

## Install

Copy `RuText.asi` into the game folder and the `RuText` folder (with `RuText.ini`) next to it. Put dictionaries into `<game>\RuText\`.
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

## How it works

ScriptHook mods call natives through three exports of `ScriptHookRDR2.dll`: `nativeInit`, `nativePush64`, `nativeCall`. RuText rewrites those import-table entries in every loaded `.asi`, as soon as it loads, and for `CREATE_VAR_STRING(…, "LITERAL_STRING", text)` and `DATABINDING_ADD_DATA_STRING` swaps the text pointer.

## Build

`./build.sh` cross-compiles `out/RuText.asi` with mingw-w64 in a throwaway Docker container.

## License

MIT, see [LICENSE](LICENSE).

---

## По-русски

Переводит зашитый в ScriptHook-моды английский текст (меню, подсказки, сообщения) по словарям в `<game>\RuText\`. Сами моды не меняются. Непереведённое, что появилось на экране, пишется в `RuText\untranslated\`. Нужны ScriptHookRDR2 V2 и ASI-загрузчик. Словари для чужих модов здесь не лежат: их авторы просят спрашивать разрешения. Память логов Bandit Hideouts теперь отдельный мод — rdr2-hideout-memory.
