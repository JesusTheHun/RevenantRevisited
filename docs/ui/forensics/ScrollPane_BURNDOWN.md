# TScrollPane — Reconstruction Burndown
Spec: ScrollPane_SPEC.md   Status: harness verified; production reader integrated

## Production integration

The in-game reader in [scroll.cpp](../../../src/scroll.cpp) composes actual
object text through TButtonPane, FONT.DEF atlases, and the canonical word
wrapper. Short known text tags resolve through the active language table.
The deferred opening uses PushModal/EndModal, with owned artwork and text,
cleanup after input dispatch, and a centered classic layout on resize.
Scroll and book readers share this lifecycle and rendering implementation.

Nine asset-free regression tests cover the production declarations and method
bodies. The Level 1 magic scroll was checked in-game on Windows D3D11 for
opening, localized instructions, closing, reopening, and shutdown while open.
Book layout has fixture coverage; full retail visual parity remains unverified.

The checklist below records the separate UIScrollPane harness, including its
approximate font, shadow, and synthetic-content choices.

## Assets to load            (spec §2)
- [x] scroll.dat:scroll   570×374 → parchment chrome               (§2)
- [x] scroll.dat:downup   28×23  → down-button released sprite      (§2)
- [x] scroll.dat:downdown 28×23  → down-button pressed sprite       (§2)
- [x] scroll.dat:upup     28×23  → up-button released sprite        (§2)
- [x] scroll.dat:updown   28×23  → up-button pressed sprite         (§2)
- [x] scroll.dat:exitup   28×31  → exit-button released sprite      (§2)
- [x] scroll.dat:exitdown 28×31  → exit-button pressed sprite       (§2)
- [x] SpellIcons.dat:Might 40×40 → spell icon for spell-scroll variant (UNCONFIRMED ref)

## Surfaces / render target  (spec §3)
- [x] Compose pane into fixed 640×480 RT; blit in HUD pass          (§3)

## Static elements           (spec §4)
- [x] scroll chrome at screen (35, 26) 570×374                      (§4)
- [x] body text cell at screen (100, 94) 430×230                    (§4)
- [x] "down" button hit/draw at screen (487, 356) 28×31             (§4)
- [x] "up" button hit/draw at screen (519, 349) 28×31               (§4)
- [x] "exit" button hit/draw at screen (553, 347) 28×31             (§4)
- [x] spell icon overlay at (kIconX, kIconY) for spell-scroll variant (harness approx)

## Draw order                (spec §5)
- [x] step 1: guard IsOpen() check                                   (§5)
- [x] step 2: stamp parchment chrome at (35,26)                      (§5)
- [x] step 3: if text != null, draw body text at (100,94) w=430 h=230(§5)
- [x] step 4: down/up/exit button visibility (based on line/numlines) (§5)
- [x] step 5: draw button sprites at registered positions            (§5)

## Algorithms                (spec §6)
- [x] chip origin: x=(640-570)/2=35, y=(480-374)/4=26              (§6)
- [x] paging: line += delta; clamp [0, numlines] (snapshot clamp)   (§6)
- [x] numlines per page = bodyheight / lineHeight                    (§6)
- [x] button visibility: down hidden when line+numLinesPerPage>=numlines (§6)
- [x] button visibility: up hidden when line < 1                     (§6)
- [x] auto-page harness cycles item-scroll ↔ spell-scroll every 3s  (harness)

## Effects / shadows         (spec §7)
- [x] parchment chrome: chroma-key + DM_BACKGROUND (opaque body)     (§7)
- [x] body text: 3-pass SE black shadow via DrawTextShadowedToTarget  (§7)
- [x] buttons: flat sprites, no shadow                               (§7)
- [x] no pink-halo (DrawTextShadowedToTarget uses real alpha atlas)   (§7 §11)

## Text                      (spec §8)
- [x] body text cell (100, 94, 430, 230) left-align, top-align       (§8)
- [x] font: Arimo-Regular 12px (retail "Scroll" BMFONT approximated) (§8 UNCONFIRMED-E)
- [x] color: sepia (#4a2810 approx) — UNCONFIRMED-E                  (§8)
- [x] multi-line wrap at cellW=430 via pre-wrapped newlines (harness) (§8)
- [x] start-line offset = `line` field for paging                    (§8)

## Animation                 (spec §9)
- [x] no animation — static between inputs                           (§9)
- [x] dirty/redraw: redraw only when dirty                           (§9)

## Input / dispatch          (spec §10)
- [x] harness: UIScrollPane::Scroll() API for line advancement       (§10)
- [x] button sprites show pressed/released based on state            (§10)

## New port primitives needed (spec §13)
- [x] none — all exist: DrawBitmapToTarget, DrawTextShadowedToTarget (§13)

## Errors to close            (spec §0)
- [x] mbr_0x190 is SCROLLWIDTH=430, NOT a font pointer              (§0 #1)
- [x] mbr_0x194 is body-text height=230, NOT colour                 (§0 #2)
- [x] NUMLINES from bodyheight/lineHeight, not passed directly       (§0 #3)

## Spec gaps found during build
- [ ] UNCONFIRMED-E: Scroll font face/size/color — used Arimo 12px sepia approx
- [ ] UNCONFIRMED-F: sidebar vs centre placement — used centred layout per spec
- [ ] UNCONFIRMED-D: body text shadow — reproduced with shadow (§7 "likely yes")
- [ ] Icon position in spell-scroll variant not specified in spec §4 — placed
      at kTextX-kIconW-4, kTextY (left of text cell, harness approximation)
