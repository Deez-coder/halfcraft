# HalfCraft

Play real Minecraft inside Half Sword. Minecraft's picture is drawn into Half Sword's world, and you play as Steve
among Half Sword's fighters.
- Your Minecraft weapons hit the fighters, and the fighters' blows hit you.
- Minecraft's mobs fight them too.
- Blocks you place are solid for them.
- Half Sword's arenas become Minecraft ground, and fighters ride your boats and minecarts.

**Just want to play?** Download the zip from the [Releases](../../releases) page and follow the README inside. You
don't need anything in this repository.

Made for **Half Sword Early Access 0.6.1.5** and **Minecraft 26.3 (Fabric)**.

## How it works

| Part | What it does |
|---|---|
| `mc/` | A Fabric mod. Minecraft drives: it runs Steve, the mobs and the blocks, and talks to Half Sword over a local WebSocket (127.0.0.1:25599). It shares its finished frames through shared memory. |
| `hs/` | A ReShade add-on (`HSPassthrough.addon64`) for Half Sword. It hooks Unreal Engine (MinHook on ProcessEvent/ProcessInternal), keeps Half Sword's camera on Minecraft's, and composites Minecraft's frames with depth (`shaders/MCPassthrough.fx`). It mirrors Steve, the mobs, blocks and boats into Half Sword, sends Half Sword's ground shape back as collision, and passes your keyboard and mouse to Minecraft. |
| `tools/` | Two scripts that fix the generated Half Sword SDK so it compiles. |

`PLANS.md` holds the design notes and the roadmap.

## Building

### The Minecraft mod (`mc/`)

1. Install Java 25 (JDK).
2. From the `mc` folder, run:

   ```bash
   gradlew build
   ```

The mod ends up in `mc/build/libs/passthrough-0.1.0.jar`. Put it in your mods folder together with Fabric API.

### The Half Sword add-on (`hs/`)

You need Visual Studio 2022 or newer with the "Desktop development with C++" workload, and Python 3.

The add-on is compiled against a Half Sword SDK generated from your own copy of the game. The SDK isn't in this
repository.

1. **Generate the SDK.** Build and inject [Dumper-7](https://github.com/Encryqed/Dumper-7) into a running Half Sword
   (EA 0.6.1.5). Copy the contents of its `CppSDK` output folder into `hs/third_party/sdk/`, so that
   `hs/third_party/sdk/SDK.hpp` exists.
2. **Fix the enum sizes.** Run `python tools/fix_enums.py`. Dumper-7 gets a few enum sizes wrong for this game, which
   shifts the layout of every class that uses them.
3. **Run `hs\build.bat` once.** It finds Visual Studio. The first build may stop on SDK layout errors; that's
   expected.
4. **Clear the layout checks.** Run `python tools/fix_asserts.py`. It switches off the layout checks that fail under
   MSVC, for structs the add-on doesn't use.
5. **Build.** Run `hs\build.bat` again to get `hs/build/HSPassthrough.addon64`.

To install, put the add-on and `hs/shaders/MCPassthrough.fx` where the release README says, or run
`hs/install.ps1`.

A different Half Sword version means a fresh SDK. A few fixed offsets in `hs/src/bridge.cpp` (marked with the class
member they read) may need updating too.

## Credits

- Made by **holdup.lethimcook** (Discord).
- Built together with Claude (Anthropic's AI), in Claude Code.
- Uses:
  - [Fabric](https://fabricmc.net/) and Fabric API
  - [ReShade](https://reshade.me/) by crosire
  - [MinHook](https://github.com/TsudaKageyu/minhook)
  - [Dumper-7](https://github.com/Encryqed/Dumper-7)
  - [Java-WebSocket](https://github.com/TooTallNate/Java-WebSocket)
- The idea for the ground collision comes from [SkyCraft](https://github.com/chasmlol/SkyCraft) (Minecraft in Skyrim)
  by chasmlol (MIT). None of its code is included.
- Half Sword is by Half Sword Games and Minecraft is by Mojang. This is a fan mod, not affiliated with either.

## License

MIT: see [LICENSE](LICENSE). Bundled third-party code keeps its own license (listed in LICENSE).
