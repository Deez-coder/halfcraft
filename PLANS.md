# Next up (written 2026-10-09, nothing built yet)

Current good build: `builds\2026-10-09_2116_release-nav-gaps` (installed and in the share zip).

## 1. Pistons: not a bug (resolved 2026-10-10)

The lever was placed on the piston itself. An extending piston base isn't a full block any more, so vanilla pops anything attached to it, the power cuts and the piston retracts. Power pistons from a neighbouring block.

## 2. Fighters riding boats and minecarts (kneeling)

**DONE 2026-10-10 (builds/2026-10-10_1420_release-boats-carts), confirmed by the user.** Seat = where MC draws the vehicle; whole body carried by velocity each frame. Fighters board on touch; a hidden rising plank in Half Sword lifts and carries the kneeling body. Original plan below.

1. **Quick in-game tests first (Lua, nothing installed):**
   - Does `Kneel_Event()` or `Is_Crouched` visibly lower a fighter and hold?
   - Can a live fighter be held at a fixed spot (movement off, placed every tick) without his AI or physics breaking?
   - What does a boat do to a fighter's proxy villager today? Vanilla boats grab villagers, and the mod yanks the proxy back every tick.
2. **Minecraft side:** while a proxy is a passenger, send `{"t":"ride", handle, seat x,y,z, yaw}`, plus a stop message. Stop the proxy-follows-fighter code from fighting the vehicle.
3. **Half Sword side:** hold that fighter like Steve's stand-in: movement off, placed at the seat every tick, `Kneel_Event()`. Release with `Un_Kneel_Event()` when he gets out, the vehicle breaks, or he's knocked out or dies.
4. **Extra:** minecarts on rails carry fighters around the track. Optionally make boats and carts solid in Half Sword (a moving box, like blocks).

## 3. Ground on maps: DONE (2026-10-10, builds/2026-10-10_1333_release-full-maps)

SkyCraft-style hybrid ground: invisible collision shapes plus barriers as support. Per-map barrier tweaks via /hs barriers. The notes below are kept for reference.

## 3. Ground on maps that aren't flat ("map blocks not generating properly")

**Context (user, 2026-10-09):** the current ground system only really works on the flat Yard arena (Map_Arena_Yard). It was built around it: one ground level set at the start, plus whole-block barrier columns. Other maps break. Test the rework on those maps.

**How it works now:**
- Half Sword traces straight down once per 1 m column, starting 2.5 m above Steve's feet.
- Minecraft fills that column with barriers up to the hit height, rounded to a whole block.
- **What that breaks:**
  - slopes and stairs become 1 m steps, and Minecraft only steps up 0.6 blocks without jumping;
  - ground snaps up or down by up to half a block, so Steve floats or sinks;
  - bridges, overhangs, arches and rooms with a floor above can't be represented;
  - walls under 2.5 m above Steve count as solid columns, while higher ceilings are ignored.

**Options, recommended last:**

- **A. Better barriers (smallest change).** Add our own invisible block with 16 height steps, like snow layers, instead of full barriers. Slopes become 1/16-block steps that Minecraft walks up, and floating goes away. Still one surface per column, so no overhangs. Pistons can push it or not, our choice.
- **B. Half Sword moves Steve (what you suggested).** Minecraft sends Steve's input, Half Sword moves the stand-in with its own movement on the real map collision, and Minecraft teleports Steve to the result.
  - **Upside:** exact on any map.
  - **Downside:** Minecraft's player movement runs on the client, so this means fighting its prediction, which risks lag and rubber-banding. Minecraft's own blocks and mobs would also need to work through Half Sword. Biggest rework.
- **C. Collision with no blocks at all (recommended to try first).**
  1. Remove the barriers.
  2. The mod adds Half Sword's map shape directly into Minecraft's collision check (one mixin where entities collide with the world), as invisible shapes at any size, not whole blocks.
  3. Half Sword sends the nearby map shape: several heights per 25 cm cell (so floors above floors work), or the actual boxes of nearby walls and props.
  - Steve, mobs, items and arrows all collide with Half Sword's real shape on any map.
  - Pistons stop being blocked (fixes suspect 1 of item 1).
  - Minecraft stays in charge of movement, so no lag.
  - **Risk:** Minecraft builds some things on solid blocks: mob spawning, placing blocks on the ground, redstone on the floor, mobs' pathfinding. Those need a fallback, either a thin barrier layer only where needed or a mixin.
- **First step either way:** load a non-flat map and record what goes wrong, using the existing logs plus the in-game Lua tools (`lab/lua/blocks_up.lua` lists mirrored blocks above ground).

### Reference: how SkyCraft (Minecraft in Skyrim, MIT, github.com/chasmlol/SkyCraft) solved it = option C

**Minecraft side**
- Void world, no barriers.
- `BlockCollisionsMixin` uses `@WrapOperation` on `BlockCollisions.computeNext`, around `CollisionContext.getCollisionShape(BlockState, CollisionGetter, BlockPos)`. It ORs in `SkyCollision.shapeAt(pos)`, a VoxelShape made of 8×8×8 sub-cubes per block (`BitSetDiscreteVoxelShape(8,8,8)` → `CubeVoxelShape`).
- Vanilla movement, step-up, onGround and fall damage then just work.

**Data from the game side**
- **Streaming:** per 8×8×8-block region, one record per block (xyz + eight 64-bit layer masks), with an epoch so a full clear can't mix in stale data.
- **Steep surfaces:** anything steeper than about 50° (|n.y| < 0.643) is snapped to whole-block footprints, so Minecraft won't step up cliffs.
- **Coverage:** about 5 regions around the player, 3 below and 2 above. Regions next to the player are re-sent every 1 s.
- **Cost limits:** 2.5 ms of work per frame, triangle work on a worker thread.

**Where Skyrim's shape comes from**
- **Final version:** walks the physics engine's actual shapes (boxes, capsules, convex, triangle meshes) into triangles, then voxelizes them.
- **First version (MVP):** ray-cast a grid with several hits per column. Use this for Half Sword first.
  - Trace a 1/8-block grid near Steve, coarser further out, spread over frames.
  - Re-trace downward from just below each hit to catch the floors under it.
  - Later option: read Unreal's own collision shapes (BodySetup boxes, convex hulls, landscape heightfields).

**Extras they have**
- `TriCollider`: the player collides with exact triangles for smooth slopes.
- `SkyRay` / `EntityPickMixin`: clicking and placing blocks on non-block ground.
- `SkyDig`: digging the game's terrain.
- `SkyWater`: the game's water acts as Minecraft water.

**What we need that they don't**
- **Mob pathfinding:** our Minecraft mobs walk on Half Sword ground, but Minecraft pathfinding reads block states, not collision. That needs a mixin in the walk-node path-type check, so Half Sword voxels count as solid or walkable.
- **Mob spawning and redstone:** both also read blocks. Choose per feature: a mixin, or a thin real block layer only where needed.
- **Block placing:** clicking Half Sword ground to place a block needs their pick-ray trick.

**Credit:** if we copy code, keep their MIT notice.
