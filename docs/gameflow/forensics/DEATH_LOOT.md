# Death loot: what a dead character drops, and picking it up

Retail `Revenant.exe` (SHA-256 `28bec273…`), read from the baseline listing
(`recon/retail_asm/baseline`) and the class decompiles. **C** = read in the
asm; **D** = decompile only; **I** = inferred.

When a character dies, its inventory bursts out of the body: gold drops at
its feet, everything else is thrown out to a walkable spot a little beyond
the body, tumbling through the air with a trail, landing with a bounce and a
brief flash. The player picks it up by clicking or with the Get key.

## 1. When — `TCharacter::Pulse` `0x004c1bb0` (`0x004c215e`–`0x004c21e4`)

```
if Health() <= 0 and !(charflags & 8 /*Unkillable*/):
    if objclass != PLAYER (or, a player, only in multiplayer host cases):
        if NumInventoryItems() (0x470040: +0x68 after following linked inventories) > 0:
            (multiplayer: a network notice first)
            DropLoot()                                   // 0x004d6570
    ... the death action (doing != DEAD: start it) ...
```
**C**. It runs on every pulse while the character is dead and still holds
something; DropLoot empties the inventory, so in practice once, on the first
dead pulse, while the death animation starts. A single-player Locke never
drops.

## 2. DropLoot — `0x004d6570`

```
if NumInventoryItems() <= 0: return
swag = FindObjInventory("Swag Bag")        (slot 0xa8)
if swag: MoveInventory(swag, this, -1)     (slot 0x64: out of any bag, to the top level)
(multiplayer, a player: a "pouch" gathers the inventory; not single player)
play "gold" (0x49c430 find, 0x49b650 mount, 0x49b990 play: volume 0x7f)
for each item of the inventory, top level (iterator 0x46dfb0), CanTrade() (slot 0x5c:
        Value != -1; a "Swag Bag" container says no):
    if the item's type name is "gold" (stricmp):
        SetPos(body + (30, 30, 0), the body's level)       (slot 8)
        RemoveFromInventory(); AddToMap()                    (slots 0x60, 0x8c)
    else:
        from = body position; cur = from
        dir = random(0, 256) * 2π/256; step = (cos·16, sin·16)
        n = 0; done = false
        repeat:
            cur.xy += step
            GetWalkHeightRadius(cur, level, 16) -> height, maxdelta, hole   (0x4530a0)
            cur.z = height
            if |maxdelta| <= 32 and !hole: drop = round(cur)          (accept this step)
            elif n < Radius()/16: restart from the body, a new random dir, n = 0
            else: done = true                                          (keep the last accepted)
            n += 1
        until n > 2·(Radius()/16) or done
        SetPos(drop, -1); level = the body's
        RemoveFromInventory(); AddToMap()
        if WantsAnimator() (slot 0x30): CreateAnimator() (slot 0x28)
        start = from; unless the item's class is "Armor": start.z += 60
        if GetAnimator() (slot 0x24): animator.Toss(start, drop, 0, -1, 0)   (0x445ef0, §4)
        PlayItemSound()                                                     (0x473a10, §3)
    count -= 1
RenumberInventorySlots()                    (0x46ff80: slots < 0x100 packed from 0)
if this == Player: the inventory pane refresh (0x5391a0)
```
**C**. `Radius()` is slot `0x258`, the `RAD` object stat. The step walks out
up to `2·(Radius/16)` steps of 16 — about twice the body's radius — as long
as each step is walkable; a wall inside the body's own radius sends it off in
a new direction. Draws: one `random(0,256)` per non-gold item, one more per
restart.

Retail quirks: a dead character holding only untradeable items keeps
`NumInventoryItems() > 0`, so DropLoot (and the "gold" sound) runs on every
pulse; and if every direction is blocked within the radius the restart loop
never ends. **I** (from the code; no shipped case seen).

## 3. The item's class sound — `0x00473a10`

| class | sound |
|---|---|
| MONEY | `gold` |
| RANGEDWEAPON | `bow` |
| WEAPON | `weapon` |
| TALISMAN | `talis1` / `talis2` / `talis3` (`random(0,2)`, table `0x5d47f0`) |
| FOOD | `food` |
| POTION | `potion` |
| SCROLL | `scroll` |
| ARMOR | `jewel` for EqSlot 1; `jewel2` for EqSlot 5 or 6; else `armor` |
| KEY | `viles` when the name contains "Vial", else `keys` |

Volume `0x7f` (full), unpositioned. **C**. Callers: DropLoot (each thrown
item, not the gold), Pickup `0x004cfef0`, equipping.

## 4. The toss — animator `0x00445ef0` (start) and `0x00445b20` (each tick)

The object animator owns a 0x150-byte toss record at `+0x38` (null when
still). **C** for the fields below.

| offset | field |
|---|---|
| `+0x00/08/10` | velocity x, y, z (double) |
| `+0x18/20/28` | spin about x, y, z (rad per tick) |
| `+0x30/38/40` | position (double), then 4 older positions every 0x18 (`+0x48 … +0x90`) |
| `+0xa8/b0/b8` | angles x, y, z, then 4 older sets (`+0xc0 … +0x108`) |
| `+0x120/124/128` | start (int) |
| `+0x12c/130/134` | target (int); `+0x134` is the floor |
| `+0x138` | gravity, 1.5 |
| `+0x140` | settle countdown (−1 until settled) |
| `+0x144`, `+0x148` | the call's 3rd and 5th arguments |

**Start** `Toss(from, to, a3, countdown, still)`:
- the target's z is raised by 2 for MONEY and WEAPON items, else by
  `2 + h`, where `h` is the imagery height of the current state (slot `0x6c`,
  `-3` outside 1..32, halved when above the state's `+0x2e`);
- all five history entries are `from` with angles (π/2, π/2, π/2); the
  object's facing becomes the one angle z gives (`(−64 − round(az·255/−2π)) &
  0xff` = 0);
- `t = (random(0,150) + 75) · 0.01 · 16` ticks (12–36);
- the object is placed at the raised target (slot 8);
- unless `still`: `v.xy = (to − from).xy / (t·2.5)`,
  `v.z = t·1.5·0.125·0.5 + 80/t`, spin z = `0.4·(random(0,50) + 50)·0.01`
  (0.2–0.4), the other spins 0. With `still` every velocity and spin is 0.

**Each tick** (animator Pulse, slot `0x2c`):
```
the landing pulse (+0x34, +0x35): see §5
if toss and countdown < 0:
    shift the history one step (4 ← 3 ← … ← 0)
    if pos.z > floor:
        pos += v; angles += spin (each wrapped into [0, 2π)); v.z -= gravity
    if pos.z < floor:                                    (it went through the ground)
        pos.z = floor + 0.1; v.z *= -0.5; spin.z *= 0.5
        if |pos.x − to.x| < 16 and |pos.y − to.y| < 16 and v.z <= 3·gravity:
            v = 0; spin = 0; if countdown == -1: countdown = 5
if toss and countdown > 0:
    countdown -= 1
    if countdown == 0:
        facing from angle z; SetPos(round(pos)); free the record
        +0x34 = 0, +0x35 = 1                             (start the landing pulse)
```
The flight is simulated per 24 Hz tick.

## 5. Which animator, and the Equip animator's resting pose

T3DImagery's NewAnimator (`0x0040cf50`, vtable slot `0x34`) picks a builder
from the registry (`0x005e851c`, count `0x005e872c`): one registered under
the object's **type name** first; else, for class **weapon**, **armor**,
**lightsource**, or **ammo** whose type is **arrow**, the builder named
**"Equip"** (`0x005e8720`, vtable `0x005a3784`); else one registered under
the class name; else the default 3D animator (`0x005e8508`, vtable
`0x005a370c`). **C**.

| | default 3D animator `0x5a370c` | Equip animator `0x5a3784` |
|---|---|---|
| Pulse (slot `0x2c`) | `0x0040e2e0`: no toss step | `0x00410de0` = `jmp 0x00445b20`, the base TObjectAnimator Pulse with the toss |
| Render (slot `0x34`) | `0x0040e8d0`: no toss | `0x0040f5b0`: the toss draw, the landing flash, the resting pose |

So only Equip-animator items fly visibly. A default-animator item (potions,
food, scrolls, keys, talismans, containers, …) gets `Toss` called, which
places it at its raised target, but nothing steps or draws the flight: it
simply appears where it lands. Gold isn't tossed at all.

**The resting pose** (`0x0040fdc8`–`0x0040fe96`), every frame there is no
toss record (or the item is Armor), for every part: `flags |= 0x6c`
(SCL1 | ROT2 | POS3), position 0, scale 1, rotation (π/2, π/2, π/2) —
replacing the part's own animated transform. Armor instead: rotation
(0, 0, −π/4), scale 1.5 (and `+0x348 = 2`, a blend value with no BLEND flag
set). **C**. A weapon modelled upright for the hand (the Short Sword's blade
runs up its Z, 35 long) lies flat this way; its authored part offset
(−20.7, 0.7, 0) is dropped.

## 6. Drawing — Equip render `0x0040f5b0`, landing flash `0x00419ab6`

While a toss record exists and the item's class isn't Armor, the mesh is
drawn five times: four ghosts at history entries 4, 3, 2, 1 and the item
at entry 0. Each part's transform is overridden (`flags |= 0x100006c`:
SCL1 | ROT2 | POS3 | BLEND): scale 1, rotation = that entry's angles,
position = that entry's position less the object's (the object stands at
the target). A ghost's material has its colour zeroed and alpha `0.2·k`
(k = 1…4, oldest faintest), with part blend `0x10`; the fifth draw is the
item, opaque. **C** for the transforms and the order; **I** for the exact
material fields (`0x410f60` part, `+0xc` alpha). Armor is drawn normally,
already at its target.

The landing pulse counts `+0x34` up 0→6 (`+0x35 = 1`), then down to 0
(`+0x35 = 0xff`), one step a tick. While it is above 0, no toss is live and
the editor is off, the 3D scene draws a flash over the object
(`T3DScene 0x00414d70`, intensity `pulse/6·255`, flags `0x90`). **C** for the
gate and intensity; **I** for what `0x414d70` draws.

## 7. Picking it up

- **By mouse**: the hand cursor over an inventory item; a click walks to it
  and picks it up (MAP_INPUT.md; Pickup `0x004cfef0`, which plays the item's
  class sound).
- **The Get key** (`G`, command 15, `0x004d0430`): alive only. The objects
  within 80 of the point 40 ahead of the player (FindObjectsInRange
  `0x452060`, at most 10) that aren't the player, are inventory items or
  offer a cursor (slot `0xc0`(0) ≠ −1, or slot `0x134`), and aren't
  invisible (flags `0x80`): the nearest (slot 4, Distance). None: the main
  player hears "GETNOTHING" ("There is nothing to get"). Else the main player
  sees "FULLGET" with the name, or "BASEGET %s" ("Get <name>"), then Pickup
  (which adds "<name> Picked up."). **C**.

## 8. The port

| retail | port |
|---|---|
| Pulse `0x004c21e4` | `TCharacter::Pulse`, the dead branch |
| DropLoot `0x004d6570` | `TCharacter::DropLoot` (`[loot]` lines in the log) |
| can trade, slot `0x5c` (`0x46fad0`, containers `0x4dd3a0`) | `TObjectInstance::CanTrade`, `TContainer::CanTrade` |
| class sound `0x00473a10` | `TObjectInstance::PlayItemSound` (DropLoot, Pickup) |
| toss start / tick `0x445ef0` / `0x445b20` | `TObjectAnimator::Toss` / `TObjectAnimator::Pulse` (`SToss`) |
| NewAnimator `0x40cf50` | `SelectAnimatorBuilder` (3dimage.cpp) |
| Equip animator `0x5a3784` | `T3DEquipAnimator` (equipanimator.cpp): the resting pose, the toss draw |
| Get `0x004d0430` | `TCharacter::TryGet`, `GAMECMD_GET`; control entries Get and Combat Get |

Deviations:
- The flight is stepped per tick, as retail's, and drawn between the last
  two ticks at the frame's fraction (smooth at any frame rate).
- DropLoot tests each step itself; retail tested the spot it last accepted,
  a step behind, so a thing could rest a step into a wall or over a ledge.
  Same result on open ground.
- The heading restarts give up after 32 tries and leave the thing at the
  body (retail looped without end when every heading was walled in close).
- DropLoot runs only while the body holds something tradeable (retail ran,
  and played "gold", every pulse while it held anything at all).

Verified (2026-10-11, headless, `postopen` save, `--exec` with `prompt
alreadydead; prompt dummies`, an Araknid given 50 gold, Short Sword, Battle
Axe, Torch, Red Cloth Shirt and Apple, then `Araknid.stat health = 0`):
the gold drops at its feet and the rest one step (its radius 14 → one 16
step) around it; the sword, axe and torch fly up from the body, spinning,
and come down within about half a second; the shirt (Armor) and the apple
(2D) appear where they land. Seven Get presses next to the body pick up all
six ("Get Torch", "Torch Picked up." … "50 Gold Picked up.") and then
"There is nothing to get". The Short Sword on the floor now lies flat
(it stood on its edge with the authored pose).

Ground rendering (2026-11-09): an unsupported particle controller no
longer suppresses an inventory item's authored base meshes. Coin.I3D can
show its resting pile while its referenced particle prototypes remain
hidden. Unresolved prototypes and unsupported effect owners still fail
closed; no additional particle controller is enabled.

Flat 2D items such as Fish.I2D have no depth bitmap. Their uploaded local
depth is correctly zero. The ordinary tile pass uses decoded color alpha
for coverage, rather than rejecting zero depth a second time. Authored
transparent depth pixels already have zero alpha at upload. Metal's
projected heightfield intersection keeps its existing validity rules.

Not yet ported (phase two): the four trailing ghosts (§6) and the landing
flash (the emissive redraw and the scene flash `0x414d70`). To check
against retail: the resting pose and the flight in the dosbox-x lab.
