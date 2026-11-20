# Custom Points System (native code)

Source of the native plugin in the Swofty Launcher addon **Custom Points System** (`twilight.points-system`) for F1 25.
The addon's database part sets a fixed points table; this plugin adds the **random table each session** options:
at every session start it picks a table (history tables, everything, or a fresh chaos table), optionally a random
fastest lap point and sprint table, writes them into the loaded game database and shows the table in a toast at the race start.

- `src/points_random.cpp`: the whole plugin. Finds the game's database insert/lookup functions by signature, keeps each
  record's class when it writes (the class of the `points` lists is worked out from the build's vtables at runtime).
- `build.sh`: builds `points_random.dll` with LLVM clang for `x86_64-pc-windows-msvc`. Pass the path of the
  `templates/addon/native/include` folder of the [Swofty Launcher](https://github.com/Swofty-Developments/SwoftyLauncher) repo.

```bash
./build.sh /path/to/SwoftyLauncher/templates/addon/native/include
```

Settings (from the addon manifest, `binds: native`): `random_mode` off | history | all | chaos, `random_fl`, `random_sprint`.

By Twilight Sparkle. MIT licence.
