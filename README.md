passthrough mod for minecraft to be inside half sword.

(first i declare that this mod was made almost entirely with ai, it was made in a few days using claude opus and the universal modder plugin)

i do NOT promise to maintain this. i made this for fun.

i added as much as i can. this includes but is not limited to:
- all blocks function, so ram willys with pistons as much as you want (as far as i tested)
- mobs fight willys and willys fight mobs
- clean pvp: swords, axes, the mace, bows, shields, potions, fire and knockback all work on willys
- placeable blocks that willys collide with and walk around
- every arena works, slopes and all (you can fix weird barrier spots per map with /hs barriers)
- willys get in boats and minecarts and ride along kneeling
- save your inventory as a kit for every fight with /hs kit
- /hs help in minecraft chat shows everything else

works with half sword early access 0.6.1.5 and minecraft 26.3 (fabric).
if half sword updates it will probably break until i update it too.
both games run at once, so you need a decent pc.

## how to install

download the zip from the [releases](../../releases) page and follow the README inside.

## how to build it yourself

minecraft mod (mc folder): install java 25, then run `gradlew build` in the mc folder.

half sword add-on (hs folder): needs visual studio (c++) and python.
1. generate the half sword sdk with [dumper-7](https://github.com/Encryqed/Dumper-7) and copy its CppSDK folder's contents into `hs/third_party/sdk`
2. run `python tools/fix_enums.py`
3. run `hs\build.bat` once (it may fail the first time)
4. run `python tools/fix_asserts.py`
5. run `hs\build.bat` again

## credits

- made by holdup.lethimcook (discord)
- built with claude (anthropic)
- uses fabric, reshade, minhook, dumper-7 and java-websocket
- ground collision idea from [skycraft](https://github.com/chasmlol/SkyCraft) by chasmlol
- not affiliated with half sword games or mojang

## license

MIT, see the LICENSE file.
