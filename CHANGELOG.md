# Changelog

## Unreleased

- **Up to twice the frame rate when the CPU is the limit** (no frame limit, or a slower CPU): with the render thread
  the game finished its GPU queue every frame - it does when its measured GPU time is under 4 ms, and our counters
  always were - so the game, the render thread and the GPU ran one after another (the render thread even made it
  slower than without it). That call now takes the game's own path for a slower GPU. Pallet Town, 4K with no
  effects, unlimited: 145 -> 307 fps; the menu 140 -> 300; walking 8.5 -> 5.0 ms a frame. 30 / 60 / 120 fps and frame
  generation pace exactly as before. `LARECOMP_FRAME_FINISH=1` brings the old wait back. Also: fences recorded inside
  command lists were never set (the condition was read as the sync; unused by Let's Go), and the game's indirect calls
  are ~1.5 % cheaper.
- **Cheaper GI**: the indirect light traces against a depth and normals at its own (reduced) size, the depth as 16 bits
  of log distance, instead of scattered reads over the full-size depth; the normals are rebuilt only at the GI's pixels
  (they were a full-size pass). 4K, RTX 5070 Ti, Pallet Town: GI high 1.24 -> 0.95 ms a frame, GI low 0.48 -> 0.36; the
  image the same (its debug view differs from before no more than two runs of the old build differ from each other).
  Sharing the SSAO's normals with it was tried and dropped: a full-size normal pass for both cost 0.125 ms and saved the
  SSAO nothing, and its seam handling breaks the GI's light into dark speckles on foliage.
- `LARECOMP_GPUPROF_AT`: each of our passes has a row of its own (the "SSAO" row was the motion vectors and the "GI" row
  included the SSAO), the GI is split into trace / blur / composite, and the frame's last pass (FSR 3.1, the present) is
  shown.
- **Shorter loads: the shaders' translation is cached on disk** (`cache/glsl.pak`, next to the SPIR-V cache that already
  skipped the compile after it). Entering the world translated ~370 programs every time: ~1.0 s of CPU on the shader
  workers, now 0.1 s from the second visit on (0.37 s the first time: programs share shaders), and fewer draws wait
  for their shader (Pallet Town, 1512 -> 1228 skipped while loading). Kept per build (a new version starts it again)
  and checked on every read: a garbled or cut file is translated again where it went bad, never used.
  `LARECOMP_GLSL_CACHE=0` turns it off.
- **F12 Advanced: the effects' parameters by hand**, applied live: SSAO radius, intensity, samples and accumulation;
  contact shadows on / off (with any SSAO level), their length, strength, steps and thickness; GI intensity, reach,
  thickness, rays, steps, resolution and accumulation. The sliders start from the quality level's values; moving one
  turns *Custom parameters* on, and from then on the levels only turn SSAO and GI on and off. Saved in `larecomp.ini`
  (`fx_custom = 1` and the values); *Level values* (or *Reset*) goes back to the levels'.
- **F12 menu: Ctrl (or the controller's Y / X, the top face button) makes it small** - half the size, in the bottom
  right corner, without darkening the game - so the effects can be tuned while seeing most of the image; again brings
  it back. Not shown among the button prompts.
- **SSAO retuned, and cheaper**: a wider reach at a lighter intensity (radius 100 -> 240, intensity 2 -> 1.2) with
  fewer samples (high 16 -> 8, medium 12 -> 6, low 8 -> 4); Ultra's contact shadows reach farther in fewer steps
  (60 -> 100 units, 8 -> 6 steps, strength 38 -> 35 %). 4K, Pallet Town: SSAO high 2.40 -> 1.46 ms, medium 1.94 ->
  1.21, low 1.61 -> 0.97; contact shadows 0.54 -> 0.53.
- **Title screen: no contact shadows** (SSAO Ultra): on its close-up of Pikachu they drew the shadows of its own arms
  onto its chest as jagged dark triangles. They are made for the world's scale (feet, grass, small props).

## 0.6.0 (2026-10-22)

Iván Serrano's ([@Vancete](https://github.com/Vancete), PRs #17 and #19): the in-house frame generation, SSAO Ultra,
the controller layout and the menus in the game's own style.

- **Faster where the scene is busy** (issue #20: Viridian City). The render thread no longer waits for the GPU before the
  game's small buffer copies, copies vertex data on two helper threads, and keeps vertex data in VRAM from one frame to
  the next while the game doesn't write it (the OS tracks the heap's written pages). Unlimited frame rate, i5-14600KF +
  RTX 3080, 1080p, the same build with and without: Viridian City standing 163 -> 208 fps, walking 141 -> 195 (Eevee),
  116 -> 167 (Pikachu); Viridian Forest 128 -> 165. Locked at 60 fps it held 60 before as well on this machine; the gain
  is the headroom a slower CPU needs. The kept vertex data is on when the primary display adapter is an NVIDIA card (the
  only one it was tested on); `LARECOMP_WRITE_WATCH=1` turns it on elsewhere, and the log says which.
- **The F12 menu and the start screen look like the game's own menus** (its Options screen): a lime background with
  big pale gears, slowly sliding stripes and halftone dots; each setting a white pill with dark text, the one under the
  cursor yellow with a bobbing arrow at its left and `< >` around its value; the setting's description in a box with a
  rainbow frame; the tabs as pills between L and R; the button prompts on a white strip at the bottom. All of it drawn
  by the runtime from shapes and gradients (nothing of the game's). As in the game, left / right change the
  highlighted setting at once (A still opens the list), and the on / off debug views are rows with their value. The
  "close the game?" question and the name entry take the same look.
- **F12 menu: A / B follow the controller layout**: with the Xbox layout, A (bottom) confirms and B (right) closes, as
  in the game; it was the Nintendo positions whatever the layout.
- **The menus on one grid**: the setting rows, the buttons and the description box span the same width (their left
  and right edges lined up with the title and the tabs), their text on one column; the values in the rows' right half
  on every page; 12 px between rows and 24 between groups, in place of separator lines. The sliders are a slim track
  with the value after it (it was written over the knob), and left / right step them too. On the start screen the
  toggles (fullscreen, wild battles...) and the key bindings are rows like the rest, each mod a row with an on / off
  switch and its arrows at the right end, and the buttons pills on the same grid.
- **Fixes the start screen opening in full screen** with `fullscreen = 1`: it took its own size, then went back to full
  screen at once (the setting is now applied to the game's window only).

- **Frame generation: grass coming into view no longer shows ahead of the ground** (Route 2, walking down out of the
  gate after going in and out: clumps at the top left not drawn right in the in-between images). A copy of a mesh with
  no partner in the frame before (it comes into view this frame: its closest copy there is 700 - 2500 units away, the
  others' partners taken) kept all of frame N's constants - the camera folded into them too, while the images around it
  have the camera part of the way from N-1: with the camera scrolling, those clumps were drawn ahead of everything else.
  Now each of its words changes from N as the nearest copy of it that has a partner changes at that word: still where
  both are still (where it stands, its pose), moving with the camera and the light where those are folded in.
  `LARECOMP_INTERP_SIBLING=0` compares; the walk used for comparisons the same with and without it (flicker 0.99 % of the
  blocks, detail and halfway placement equal). (Its words now go into the present's blend plan too: the second and third
  in-between images had gone back to N's camera.)
- **Frame generation: turning things keep their shape** - the tall grass rustling round a wild Pokemon (Route 2): each
  clump has eight bones, and they swing tens of degrees a frame; blended term by term, a matrix half-way between two
  orientations is not a rotation any more and shrinks and shears what it moves (by cos(angle / 2): 30 degrees, 3 %; 90,
  29 %) - blades cut short and bent in the in-between images. Any three rows of a constant buffer that hold a rotation
  with a uniform scale in both frames, turned by 3 to 150 degrees, are now blended along the arc (Rodrigues' rotation
  about the axis between them, the scale and the translation along the line), in the present's blend plan as well.
  `LARECOMP_INTERP_SLERP=0` compares.

- **Frame generation: the interface no longer flickers in battles** (and wherever text or boxes sit over a moving
  scene): with FSR 3.1, or with the in-between images at a reduced size, an in-between image is resolved against frame
  N's image along the motion vectors of the scene under each pixel - the interface's too, so static text took N's text
  shifted by the scene's motion and showed doubled in the images between two frames, clean in the real ones (FSR 3.1
  has the interface mask): a flicker at the game's rate. The real frame's interface mask is now made with frame
  generation on whatever the anti-aliasing, and where it says interface an in-between image takes N's processed image
  at the same place - its interface is N's anyway (2D draws keep N's constants). Viridian Forest, a trainer's battle
  text: in-between images against their frame 13.1 -> 0.27 (the text identical in every image). In the world the mask
  covers the interface alone (0.4 % of the image in a battle with only the ball counter shown).
  `LARECOMP_INTERP_UI_KEEP=0` compares.
- **FSR 3.1: the interface stands still** (with or without frame generation; most visible at 4K): FSR 3.1 rebuilt the
  interface as part of the jittered scene - it is drawn into the same image, without the jitter - and each frame's
  text and boxes landed where that frame's jitter put them, a pixel off from the frame before (at 4K Quality, 1.5x),
  shaking at the game's rate under in-between images that keep frame N's. The interface is now composited after the
  upscale: where the mask says interface, the image FSR 3.1 was given, Catmull-Rom from the render size. 4K, battle
  menu: the HP boxes from one real frame to the next 5.8 -> 0.10 (the opponent's 4.4 -> 0.02), as sharp as before
  (detail 8.8 -> 9.3). `LARECOMP_FSR3_UI_COMPOSITE=0` compares.

- **Frame generation: the in-between images cost ~45 % less CPU** (30 fps x 4, Pallet Town: 3.25 -> 1.77 ms a replay;
  the draws' CPU time 756 -> 490 ms every 2 s). The replays run frame N's command lists again, and redid what does not
  change from one image to the next:
  - Each constant buffer's blend - kept at N, switched half-way, along the line or the curve - is decided at the
    present's first in-between image and only evaluated at the others' t (`c0 + t (c1 + t c2)` for the words that move;
    the rest is N's as it is). Checked against the full blend (`LARECOMP_INTERP_PLAN=2`): 1.9 million buffers, the same
    to 1.8e-7, no buffer kept by one and blended by the other.
  - A replay's draw binds the index and vertex buffers frame N's same draw had (its place in the frame: the lists are
    the same) instead of scanning and copying them again - while their ring memory was not taken again (99.9 %). The
    vertex data is then N's, not what the game may be writing for the next frame meanwhile.
  - An in-between image drawn into the window right after it is made is no longer copied first (~66 MB of traffic at
    4K and two full barriers an image); the per-frame tables are cleared by the slots used, not whole (2 x 256 KB a
    frame, 512 KB a replay).
  - Our own full-screen passes wait for what they read with a barrier for the fragment stages instead of a full one,
    and the second barrier after most of them is gone (no difference measured where the CPU sets the pace).

- **Frame generation "Auto" lowers the in-between images' resolution before the multiplier**: when the GPU can't keep
  up, the in-between images are rendered at 75 %, then 50 % of the real frames' size on each axis (half a second late
  each), and only then the multiplier goes down a step (a second late), at full size again - 60 fps at 240 Hz: 4x at
  100 / 75 / 50 %, then 2x at 100 / 75 / 50 %, each step cheaper than the one before (4K: 3.65 / 2.52 / 1.63 ms an
  in-between image, detail within 1 - 3 % of the real frames). Back up as before. Measured at 60 fps, 240 Hz: 4x 100 ->
  75 -> 50 % -> 2x, and up again to 4x at 50 % with room (`[fg] auto: 4x, in-between images at 75 % ...`).
- **SSAO "Ultra"** (graphics page, after High; `ssao_level = 4`): High plus contact shadows - short rays toward the
  sun in the scene's depth, for the shadows the game's 2048² shadow map is too coarse to show: under the feet, in the
  grass, under a lamp's cap. The sun is the shadow map's own light (its pass's light matrix, by a vote over its draws).
  8 steps along a ray, blurred along the depth like the SSAO, faded out in the distance; 0.54 ms at 4K.
- **Sharper textures with TAA and FSR 3.1**: the game's textures in 3D draws take a negative mip bias while a temporal
  resolve brings the detail back - AMD's `log2(render / display) - 1` with FSR 3.1 (native: -1), -0.5 with TAA - not
  its render targets (bloom, the depth pyramid read at explicit levels) nor the interface. Detail +2.5 % at 720p.
- **Debanding**: the last post pass smooths the 8-bit steps of smooth gradients (sky, fog, the menus' blurred backdrop)
  and adds a fixed one-step dither; texture detail and edges left as they were (`LARECOMP_DEBAND=0` compares).
- FSR 3.1 is given the real frame time (16.7 ms at 60 fps; it was always 33.3); the lead before the first in-between
  image is capped at 16 ms on both presentation paths (one had 12).

- **Anti-aliasing: FSR 3.1 is its last choice** (Off, SMAA, TAA, FSR 3.1), its mode and sharpening shown under it when
  picked; the upscaler row is gone. With the others a lower internal resolution reaches the window bilinearly (FSR 1
  stays only as FSR 3.1's fallback when its DLL is missing; an older `upscaler = fsr1` now loads as bilinear).
- **Frame generation research: the in-between images at a lower resolution** (`LARECOMP_FG_SCALE=<f>`, docs/DEPURACAO.md;
  the game always renders them at full size - it was a setting for a while, left out).
  An in-between image renders the scene again, and at 4K that was most of its cost. Its replay can draw into smaller
  twins of the game's scaled render targets (the scene, its depth, bloom, the composite: `runtime/nvn/vulkan/textures.c`
  `vk_tex_lo_begin`; the game's code, viewports and our passes only see smaller targets), the result rebuilt on
  frame N's full image: with FSR 3.1 the resolve against its output that was already there, otherwise the same resolve
  against N's own image plus RCAS. Measured at 4K (FSR 3.1 native, SSAO, GI high), GPU per in-between image: 3.65 ms
  full, 2.52 at 75 %, 1.63 at 50 % (SMAA: 3.08 / 2.61 / 1.82). Detail within 1 - 3 % of the real frames in every mode
  (the RCAS after it set by the size), halfway placement the same (93 % of the moving blocks), flicker 1.60 / 1.66 /
  1.85 % of the blocks with FSR 3.1 (SMAA 1.40 / 1.62 / 1.81 %).
  Also: an in-between image with FSR 3.1 no longer copies the depth or builds the interface mask it didn't use.
- **Frame generation "Auto"** (first after Off): as many images per world frame as the display's refresh shows evenly
  - 60 fps at 240 Hz: 4x, one image every refresh; 30 fps: 4x (8x would also fit, 4x is the most); 60 fps at 144 Hz,
  where none is even: the most at or below the refresh. When the GPU can't keep up (the frames a second late in a row)
  it goes down a step and tries again after 10 s, the wait doubling up to 2 min if it had to come down again
  (`[fg] auto: ...` in the log). Measured at 240 Hz (`LARECOMP_PRESENT_LOG`): 60 fps in a window, 4x throughout, 97.5 %
  of the images one refresh apart; 30 fps at 4K full screen, 4x throughout, 86 % two refreshes apart; 60 fps at 4K full
  screen, where 4x needs ~16.8 ms of GPU a frame (a fixed 4x drops the world to 50 fps and the images go uneven), 2x.
  With a variable-refresh display, `LARECOMP_FG_VRR=1` drops the refresh grid (docs/DEPURACAO.md).
- **Frame generation: no more shadows of bushes and trees where there are none** (walking right from Viridian's sign,
  starting to walk right in front of Pewter's gym, and elsewhere): an in-between image drew a shadow caster at another
  copy's place. Each draw of the frame before is now the partner of one draw of N only, the closest to it (once per
  present, for all its in-between images): a copy coming into view takes another's place in the order of the draws, and
  wanted that copy's partner - which the copy, the same in both frames, matches exactly; it now keeps frame N's
  constants. Pewter: a tree drawn three times in the shadow map, then four, the new one 900 - 1250 units from any of
  them; the dark area where its shadow showed, 7510 samples -> 0 (`LARECOMP_INTERP_ONE_TO_ONE=0` compares); a walk there
  otherwise the same (flicker 1.21 / 1.23 % of the blocks, 91 % of the moving blocks halfway; 1 - 2 draws a frame lose
  their partner). Viridian's case before it: The same mesh drawn twice in the
  shadow map (x 1420 and 2670), the second coming into its view in frame N: the only candidate in N-1 was the first,
  1250 units away, and the jump test for a draw paired by content (half the two values and 100 units) let it through.
  A partner found by content that moved a word by more than a quarter of the two values and 50 units is now another copy:
  the draw keeps frame N's constants. Found with the shadow map dumped per image (`LARECOMP_INTERP_RT_DUMP`) and research
  switches to keep draws at N by target or identity (`LARECOMP_INTERP_KEEP_RT`, `_KEEP_SIG`). Measured on that walk
  (30 fps x 4): flicker in the images where it showed 2.5 - 2.7 -> 1.5 % of the blocks, the walk 1.37 -> 1.34 % (max
  2.7 -> 2.1); the walk in the grass 0.78 -> 0.73 %. Also: a value alternating between two states a frame at a time (back
  to N-2's) keeps N instead of being blended.
- **Frame generation: the interface is never blended** - with frame generation on, every 2D draw (no depth test: the
  interface, in the world and in menus) keeps frame N's constants in the in-between images, as the game draws it; the
  60 fps mode's menus without frame generation still slide theirs. The per-object cut check now counts 3D draws only:
  an interface element fading or scaling changes by large steps, and could cancel a frame's in-between images. What
  stays at N can be looked for with a new research log (`LARECOMP_INTERP_KEPT_LOG`): walking in Pallet Town and Viridian
  every 3D draw is paired and blended. Left as research, off: blending the vertex data the CPU writes
  (`LARECOMP_INTERP_VTX`) - Viridian's flowers already came halfway, and it lost tall grass clumps.
- **Frame generation: curved motion through three frames** - the in-between images followed a straight line from N-1 to
  N, and at 30 fps x 4 the speed changed at each real frame. A value that moved in both of the last two steps, by a
  smooth change (its acceleration at most a quarter of the steps), now follows the quadratic through N-2, N-1 and N
  (the draws' constants, the camera, the scene block; N-1's partner in N-2 is the one its own replay paired - a third
  frame is kept). Checked against the truth (a walk with turns at 120 fps; every 4th / 2nd frame taken as 30 / 60 and the
  frames between predicted, the camera): at 30 x 4 mean error 0.203 -> 0.196 cm, 99th percentile 2.82 -> 2.55; at 60 x 2
  mean 0.103 -> 0.090, never worse (with any acceleration it was worse at 60 in its worst 5 %: a step in the camera's
  follow is not a curve). In the game: the turn at 30 x 4 flicker 3.68 -> 3.61 % of the blocks, the walk and the menus
  the same. `LARECOMP_INTERP_CURVE=0` compares.

- **Controller layout** (launcher, Game page): Nintendo (A on the right button, B on the bottom, as before) or Xbox (A
  on the bottom, B on the right, X on the left, Y on the top - the letters printed on an Xbox controller).
  `larecomp.ini`: `pad_layout = nintendo|xbox`. The layout sets the controller's default bindings; changing it moves
  A / B and X / Y where they are still at the old defaults (a remap of the Controls page stays), and the button
  prompts follow the bindings.
- **SSAO at half its intensity on the title screen and in cutscenes** (when it is on), the setting's own in the world
  and in battles; Pikachu and Eevee 1.0.0.
  - The title screen (the logo with Pikachu or Eevee): before the world's first update this boot - the world's update
    is now hooked at any frame rate, not only at 60 / 120 (`runtime/hle/fps60.c` `game_title`; logged: `[scene] the
    world's first update`). Measured on the title screen: occlusion 17.4 -> 9.0; in the world the same (5.31 / 5.31).
  - Cutscenes: the event scripts take the camera with the native `EvCameraStart` and give it back with `EvCameraEnd`
    (a talk with someone doesn't); between the two (`runtime/hle/cutscene.c`, `LARECOMP_CUTSCENE_LOG=1`). The natives
    were found by name in the image's table of script natives, in both games (Eevee `0x7000180a20` / `a70`, Pikachu
    `0x70001809f0` / `a40`); the hooks install in both. Not yet seen in a cutscene: none was reached in the tests
    (the new game's intro waits at an interactive step).
  - `LARECOMP_IMAGE_DUMP=<file>` writes the main module as loaded (research: reading the image without the NSO tools).
- **FSR 3.1 sharpening slider** under the FSR 3.1 mode (graphics page, start screen and F12): its RCAS, AMD's robust
  contrast-adaptive sharpening, 0-100 %, 50 % by default (`fsr3_sharpness` in `larecomp.ini`); the frame generation's
  in-between images follow it. It was the TAA sharpness slider's value; that slider (Image and sound) is now TAA's
  alone. Measured: image detail 6.4 at 0 %, 12.3 at 100 %.
- **SSAO: fewer thin lines along the seams of the ground at 4K** (Viridian's dirt road, the path patches): since the
  occlusion takes the whole scene pass, the patches drawn over the terrain are in its depth, and their seams have
  cracks and slivers one or two pixels wide; at 4K (a pixel ~0.5 units of ground) such a pixel's normal came out
  nearly level and the ground around it occluded it. A pixel whose two neighbours of a direction (one or two away)
  are both nearer or both farther now takes their surface. Measured on the road: the darkest line 28 -> 9 levels;
  the scene's occlusion otherwise the same (mean 5.28 / 5.28). Left: short lines where a patch has a real sloped edge
  10-50 units high (geometry; a larger bias would cost 40 % of the real occlusion).
- **Frame generation in full screen at 4K: even, and no images lost**. Measured at the screen (`LARECOMP_PRESENT_LOG`,
  4K full screen, 240 Hz, 60 fps x 2, FSR 3.1 native, SSAO, GI high): before, 1 image in 6 reached the screen in the
  same refresh as the one before it (one of the two lost) and others came in pairs 15 / 0 ms apart; now none is lost
  and 97 % of the intervals are 8.3 ms (the rest of the 90 % measured is the measurement's own wobble: an interval
  off by 2 ms with its neighbour off the other way). Windowed, as before. Three causes:
  - The images were drawn into the window at their time on the one GPU queue, behind the next frame's work (at 4K the
    GPU runs ~10 ms behind the presents). Now each is drawn into a window image right after its replay and only
    presented at its time, from a second queue (`LARECOMP_FG_DRAW_AT_TIME`, `LARECOMP_ONE_QUEUE` to compare).
  - The render thread presented them between jobs, and waits for the GPU inside some of them: each present's last
    image went ~8 ms late, beside the next present's first. Now a thread of their own presents them (the swapchain
    under a lock; acquiring retries without holding it).
  - Off the display's refresh, an image due near a blank went at that one or the next by microseconds. Now on the
    refresh grid (Windows' compositor timing), a quarter of a refresh after the blank before its time.
  The lead before the first image now goes up to 16 ms (at 4K the GPU finishes a present's images 9-12.5 ms after
  it, `LARECOMP_FG_GPULAG`).
- **The in-between images are as sharp as the real frames with FSR 3.1**: they were 13-15 % less detailed (no RCAS,
  and the reprojected history sampled bilinearly), and the detail pulsed at the frame rate - smoothness lost even
  with the motion right. Now the history is sampled Catmull-Rom and RCAS sharpens them, 0.6 stops weaker than FSR's
  (it sharpens a history that is sharp already): within 3 % of the real frames.
- GPU profiler marks for our own passes (`ours: GI`, `FSR 3.1`, `SSAO`, `post`, `keep copy`, `present`, `replay`):
  at 4K a real frame costs ~7-8 ms (GI high 3.1, FSR 3.1 native 1.6-2.7, the game's scene 1.3), an in-between image
  ~2.2 ms (the scene 1.3-1.5, the rest reprojected).
- **Frame generation: no more flickering tall grass, and repeated objects in general**: walking past tall grass, clumps
  went missing, squashed or shifted in the in-between images (holes showing the ground, every few frames). Found with
  a flicker detector (the in-between image against the real frames around it, motion-compensated) and research
  switches (`LARECOMP_INTERP_TFIX`, `_DROP`, `_FAR_LOG`, `_SIG_LOG`: docs/DEPURACAO.md). Three causes, all fixed in
  general rather than for the grass:
  - **A draw is paired with the previous frame by all its constant buffers together**, not by the first one. The 63
    grass clumps share the camera buffer and differ in the next (their offsets); deciding at the first one tied all
    63, and a clump took another's offsets (252 -> 49 units, where the wind moves them 1 or 2). Of the previous
    frame's draws of the same program and arguments - any vertex buffer address or object, since the game reuses
    buffers between copies - the closest that doesn't jump; its own identity first.
  - **Occurrences are counted per draw**, not per buffer slot: one of the grass draws leaves a slot unbound, and that
    slot's count was one behind the others for every draw after it.
  - **A value that held still and then changes switches half-way** instead of being blended: a sprite's frame in an
    atlas, an expression, a blink. Motion changes every frame and is still blended.
  Walking left past the tall grass at 30 fps x 3 (FSR 3.1, SSAO, GI): the frames that flickered went from 5-9 % of
  the image to 1 % (the level of the others: the detector's noise at edges). Walking at 2x: 97 % of the moving
  blocks halfway. 6000 turbo frames at 4x without a fault; 4K at 4x still 60 / 240 fps.
- **Frame generation interpolates the characters too**: the in-between images showed the player (and other walking
  characters) almost where frame N had them, so they kept moving at the game's rate while the camera and the scenery
  were smooth. Two checks meant to catch cuts and swapped models fired on ordinary motion:
  - A word "jumped" when it changed by more than half its size plus 4 units, and then the whole buffer stayed at N. A
    skeleton's translations near 0 (the player in the middle of the screen, where the camera's coordinates are small)
    change sign every frame: a quarter of the player's skeleton jumped. Now 100 units (`LARECOMP_INTERP_JUMP_ABS`);
    rotations never jumped and still don't.
  - An object was not blended when more than 20 % of its changed words changed by a lot; a walking skeleton at 30 fps
    has 20-25 %, so it was frozen at N one frame in two, and when nothing else moved the whole in-between image was
    dropped (a 33 ms step several times a second at 2x). Now 40 % (swapped models and cuts: 50-95 %).
  Walking at 30 fps x 2: the player's in-between image is halfway (difference to N-1 / N 7.8 / 7.9; before 10.8 /
  3.2), 95 % of the moving 16 x 16 blocks of the image halfway, and no frame without its in-between image in 400
  (before ~30); at the screen 603 of 622 intervals at 16.7 ms. Captures and the battle transition checked: no torn
  models. `LARECOMP_INTERP_REJ_LOG` says which frames and objects are kept at N and why.
- **Frame generation is evenly paced**: the images came in pairs, so 2x looked hardly smoother than without it. Each
  present's first in-between image went out as soon as it was rendered and the real frame at half the period: 6.7 /
  10 ms apart instead of 8.3 / 8.3. Worse, the render thread's timed wait (winpthreads) woke up to ~7 ms late, and the
  scheduled image went out together with the next present: 15 / 2 ms. Now every image of a present is scheduled a
  fixed lead after it (the replays' time on the GPU, learnt from whether each image was ready on time; ~4 ms), on a
  timeline that follows the game's presents smoothly. The render thread waits in 1 ms slices of SDL's high-resolution
  timer, and the kept images come in two sets, so a present's images can wait while the next one's are rendered.
  Measured at the screen (`LARECOMP_PRESENT_LOG`, `VK_KHR_present_wait`, 240 Hz, 60 fps): 2x 708 of 718 intervals
  at 8.3 ms (before: none), also when rendering at 4K with FSR 3.1, SSAO and GI. 4x: 2015 of 2520 at one refresh
  (the others: two images in one refresh, one of them dropped). 3x is 180 images a second, which a 240 Hz screen
  can't show evenly. `LARECOMP_VK_PRESENT=fifo` gives an even 4x (1418 of 1438) at ~58 fps of game.
- **Frame generation on any GPU: Off / 2x / 3x / 4x** (F12 menu, `frame_gen = 0 / 2 / 3 / 4`; replaces FSR 3.1's,
  which needed FSR 3.1 anti-aliasing and had only 2x on Vulkan): k - 1 in-between images per frame, drawn by running
  the frame's draws again with their constants (camera, objects, bones) blended between the last frame's and this
  one's - the 60 fps mode's in-between image for menus, generalised. The multiplier is of the world's rate (60 fps x 4 =
  240): a menu or a battle at the game's 30 gets twice the images, and the window keeps the same rate. The in-between
  images are rendered and kept at once, the first shown at once and the others on schedule by the render thread between
  the next frame's jobs, so the game is not held up (with the images presented from the game thread, 60 x 4 slowed it to
  ~33 fps); the game then runs one present ahead instead of two (with two it rewrote the command lists being replayed).
  Each draw is paired with the previous frame's occurrence with the closest constants, not the one at its place in the
  draw order - that order shifts when a copy of a repeated mesh (flower bushes, grass) is culled, and the in-between
  images mixed bushes with their neighbours (the defect that took interpolation out of the world before). Measured at
  real speed (Pallet Town, RTX 5070 Ti, 240 Hz): 30 fps x 2 / 3 / 4 = 60 / 90 / 120 images a second, 60 x 4 = 240 (also
  with FSR 3.1, SSAO and GI high) and 240 in the menus, the game at its own speed; ~2.6-3 ms per in-between image;
  9000 turbo frames (26000 replays) without a fault. Not yet: the interface and particles stay at the game's rate; one
  frame of latency more. `LARECOMP_INTERP_MULT=<k>` fixes k (research).
  - **The in-between images are side images**: they read N's temporal state and leave it as it was, and the real
    frames chain N-1, N, N+1 by their own count. Counted as frames of their own (rendered after N, but before it in
    time) they ran every history back and forth: the GI's and SSAO's accumulation shimmered in blotches, TAA and FSR
    3.1 got the past after the future, and the jitter sequence skipped phases (half of FSR's eight at 2x). Now: their
    SSAO / GI are N's, moved by their motion vectors (relative to N) instead of computed again; TAA reads N's history and
    writes apart; with FSR 3.1 they are resolved against N's FSR output like TAA (FSR's history is the real frames'
    alone); the camera and the per-object vectors are relative to N; the jitter has a sequence for each. Static camera,
    4K, FSR 3.1: the step real -> in-between -> real went from 0.49 / 0.20 (out and back) to 0.35 / 0.35, as real ->
    real (0.31).
  - **k from the game's state, not the measured period**: a frame the GPU could not finish in time came later and got
    more images (4 instead of 2 in the world at 60), and was slower still - at 4K the game sank to ~30 fps with 2x.
    Now k = frame_gen x the world's rate / the frame's rate as the 60 fps mode means it (fps60_period_ns).
  - The kept images are copied raw only when nothing processed them (a 4K copy per image saved).
  - 4K, RTX 5070 Ti, the world at 60 with SSAO medium and GI high: FSR 3.1 native 2x 60 fps / 120 images (before: ~30
    / 120), native 3x ~48 / ~145, Quality 3x 60 / 180, Quality 4x ~55 / ~220.
- **FSR 3.1 frame generation and AMD's swapchain removed**: the swapchain is the driver's again, the device has no extra
  queues, and `ffx_swapchain` is gone (FSR 3.1 upscaling stays).

## 0.5.0 (2026-10-19)

A good part of this release is Iván Serrano's ([@Vancete](https://github.com/Vancete), PRs #15 and #16): 120 fps,
the walking fix at 60 / 120 fps, FSR 3.1 frame generation, the F12 menu's quality levels and skipping the controller
screen.

- **A launcher, and the F12 menu redone alike**: the same look (the F12 menu's colours and fonts, scaled to the
  window) and the same settings pages, from one place (`runtime/ui/settings_ui.mm`). The start screen is a launcher's
  window - no system frame (dragged by its top; its own minimize and close), taller than wide - with a sidebar: the
  project's icon, the pages (Game, Graphics, Image and sound, Controls, Mods) and a big Play; it now has the graphics
  settings too. Play gives the game its window back (frame, 1280 x 720, full screen when set). The F12 menu has tabs:
  Graphics, Image and sound, Advanced. L / R on a controller change the page / tab. The "close the game?" question's
  buttons line up with its text.
- **One frame rate setting**: the start screen's "60 fps" box is gone; the Graphics page's frame rate limit (30 / 60 /
  120) is the same row as the F12 menu's.
- **Translation is now Mods**: any mod that replaces the game's files (Atmosphere LayeredFS: translations, textures,
  models) installs and is ticked on the Mods page, several at once: where two replace the same file, the higher one
  in the list wins (arrows move them; a new one comes first). **Open the mods folder** shows where to copy one by
  hand, and the list follows when you come back. Mods of the game's code (exefs, IPS, cheats) can't apply to the
  recompiled game, and the tab says so. `larecomp.ini`: `mod = <name>|<name>...` (one name, as before, still works).
- The walking fix at 60 / 120 fps hooks the movement and turning functions by the per-build address table (both
  games' addresses checked with tools/addrmap.py) instead of looking for their code near Pikachu's; the 60 fps
  research tools moved to `runtime/hle/fps60_research.c`.

- **Fixes SSAO and GI flickering as the camera moves** (rocks, bushes and characters going in and out of the effect):
  they went onto the scene at its first blended draw after 16 opaque ones, and Let's Go draws blended ones among its
  opaque ones - whatever came after got neither, and that line moved with the view (34 to 68 of about 130 opaque
  draws walking in Pewter's gym). They now run when the 3D scene ends, with every opaque draw in (and before a fade
  drawn over it, such as the title screen's from white). In battles they
  also flickered because every few frames the scene's camera slot held a sky camera at the origin, and the depth was
  read with it: such a camera is now skipped, and a frame without the scene's camera keeps the previous one. Checked
  on Eevee and Pikachu.
- **FSR 3.1 is in the upscaler row again** (Bilinear / FSR 1 / FSR 3.1), and only there: with it, the anti-aliasing
  row says "Included in FSR 3.1" (it anti-aliases on its own); another upscaler brings back the anti-aliasing it
  replaced.
- **Skip controller selection** (launcher, `skip_controller = 1` in `larecomp.ini`; off by default; Eevee and Pikachu
  1.0.0): the game starts at the Game Freak logo with the controller already chosen, without "Press a button on the
  controller you want to use" and its confirmation - no button is pressed for it. The start sequence's controller
  step (f_700004eb50, the same address in both games) asks in mode 1 and only checks the players' controllers in mode
  0; it is turned to 0 before its first run, and finds the runtime's one controller (handheld) as player 0's. Only the
  first time per boot: when a controller is lost while playing, the game still asks. Checked on both games: no
  controller screen, the title takes A and the world the stick; with the option off the screen is there as before.
- **Fixes the ghosting on the player's left eye with FSR 3.1, TAA and frame generation**: per-object motion vectors pair
  a draw with its previous frame's by a key over its arguments, and that key read one word past them - two of the
  call's float registers, leftovers that alternated from frame to frame for the eye. It never found its previous
  constants, took the camera's vector (none for a walking character) and was smeared behind the face. Three other
  draw groups were missed every frame the same way (an NPC's part, the hedge by the fence, a ground decal). The key
  now takes the draw's own arguments only (7 for indexed draws, 5 for arrays); the 60 fps in-between image's draw
  signature too. Pallet Town, 100 frames: 512 misses -> 7.
- **Fixes flowers and grass smeared by frame generation (and FSR 3.1 / TAA) while the camera moves**: a mesh drawn many
  times without a game object of its own (the flower bushes, the grass) was paired with the previous frame by the order
  of its draws, and that order shifts by one whenever a copy is culled or comes into view: each bush then took a
  neighbour's previous constants and a vector towards it - the generated frames showed flower beds in blocks and
  pieces of them floating beside. Of the previous frame's copies of the same draw, the one with the closest constants
  is now taken. Pallet Town, walking with the camera moving, the frame where a bush leaves the view: reprojection error
  on the flower bed 4.99 -> 1.76 (pixels off by more than 40: 2187 -> 375); jump rejections over the run 196 -> 84;
  no change in speed.
- **Fixes walking speed at 60 and 120 fps**: the player walked at ~85 % of the speed at 60 fps and ~53 % at 120. The
  player moves by the walking animation's root motion (time-based) plus, once per world update, an offset of its own
  (f_700021b6f0 of Pikachu 1.0.0: -2 cm while running, sized for a 33.3 ms update) - 60 cm/s off the animation's
  450.8 at 30 fps, 120 at 60, 240 at 120. The same function turns the player toward the stick with a slerp of t = 0.5
  per update, so turning was 2x / 4x as fast. Now, in the world at 60 / 120 fps, that offset is scaled by the update's
  share of 33.3 ms and the slerp's t becomes 1 - (1 - t)^share. Measured in game time from the camera
  (`tools/walk_speed.sh`): 391 cm/s at 30, 60 and 120 fps (before: 391 / 331 / 211). The functions are found by their
  code near Pikachu 1.0.0's address (Eevee's should be a few instructions away - not tested). Found with new research
  aids: `LARECOMP_CAM_LOG`, `LARECOMP_STEP_SCALE`, `LARECOMP_FARG_SCALE`, `LARECOMP_FIND_MOVER`, memwatch's
  `LARECOMP_WATCH_LIKE` / `_AT` / `_MAX` (docs/DEPURACAO.md, docs/PESQUISA-60FPS.md).
- **Frame rate limit in the F12 menu**: 30 fps (original) / 60 fps / 120 fps (experimental), switched while playing.
  120 is the 60 fps mode with the world's step at 8.33 ms; menus stay at 30 with their in-between image (60), battles
  and captures at 30. The mode's hooks are now added when it is first turned on (on the game thread; a hook's function
  is written before its address, for threads looking it up meanwhile). The start screen's 60 fps box keeps 120 when on.
  `larecomp.ini`: `fps = 120`.
  - The player walked slower the higher the rate (85 % / 53 %): fixed, see "Fixes walking speed" above.
- **Fixes the extreme ghosting of frame generation on vertical motion**: the present turns the game image upside down
  (its origin is the lower left), and frame generation works on the window image - but it was given the depth and the
  motion vectors the right way up. Sideways the two agree; up and down every vector pointed the wrong way, and the
  generated frame doubled the whole scene vertically while walking up or down. Frame generation now gets them flipped
  (the vectors' y and the jitter's negated). Checked on the generated frames alone, captured from the desktop while
  walking down through Pallet Town (`tools/fg_walk.sh`). The F12 menu's frame generation is Off / 2x only now.
- The post passes' pipeline cache had 16 entries for about 30 passes: with FSR 3.1, frame generation, SSAO and GI on,
  some pipelines were made again every frame. 64 now.
- **F12 menu reorganised**:
  - **Anti-aliasing**: Off / SMAA / TAA / FSR 3.1. FSR 3.1 is the last choice instead of an upscaler of its own (with
    its mode below it); with any other choice the upscaler of a lower render resolution is picked below it: FSR 1
    (the default, as before) or bilinear. FSR 1 is also the fallback when FSR 3.1's DLL is missing. `larecomp.ini`:
    `aa = fsr3`; an older `upscaler = fsr3` reads as it.
  - **Frame generation**: Off / 2x, with FSR 3.1 anti-aliasing, switched live (AMD's swapchain is made or dropped at the
    next frame; its queues are now created with the device whenever the DLL is there). FSR 3.1 generates one frame
    between two (several is FSR 4's, DirectX 12 only). `frame_gen = 2`.
  - **SSAO and GI**: quality levels Off (default) / Low / Medium / High instead of a box, and their parameters are gone
    from the advanced page (its debug views stay, under Debug). High is what they had before. SSAO: 16 / 12 / 8 samples
    (the shader takes the count now). GI: 4 rays x 8 steps at half the scene's size / 3 x 6 at half / 2 x 6 at a third.
    The strength, reach and accumulation are the same at every level. 720p, RTX 5070 Ti, both on: +0.21 / +0.19 / +0.17
    ms. `ssao_level`, `gi_level` (0-3); an older `ssao = 1` / `gi = 1` reads as high.
- **FSR 3.1 frame generation** (`frame_gen = 1` in `larecomp.ini`, off by default; with the FSR 3.1 upscaler; read when
  the game starts): AMD FidelityFX generates a frame between each two of the game's, on the swapchain below. At
  `fps = 60` the window gets 120 images a second while playing, and 60 in menus and cutscenes (where the game runs at
  30); the game's own in-between image of the 60 fps mode is off then. Interface excluded: the window image without the
  interface (FSR 3's output where the interface mask is clear, the scene from before the interface where it is not) is
  handed over each frame as FidelityFX's "HUD-less" image, so the interface is taken from the real frame and not warped.
  The depth, the motion vectors and the jitter are FSR 3.1's. Where FSR 3.1 does not run (2D screens without the 3D
  scene), no frame is generated. AMD's presenter thread keeps only the latest frame; two dropped in a row stalled the
  GPU for good (1 run in 3 of the scripted turbo test, after a slow frame), so a present now waits until the presenter
  has made one since the frame before (at most 100 ms; 18 turbo runs without a stall since).
- **Fixes FSR 3.1's interface mask while playing**: the mask (the pixels the interface draws, which FSR 3.1 treats
  apart) was taken only from draws into the presented image, but while playing the game draws its interface at the
  render size into the image its last pass grades into the presented one - and the 2x interface images and the
  high-resolution text read as the scene's (scaled) images. Now the copy is made of that image, and only a scaled render
  target counts as the scene's.
- **AMD FidelityFX swapchain, the first step towards frame generation** (`ffx_swapchain = 1` in `larecomp.ini`, off by
  default; read when the game starts): the window's swapchain is FidelityFX's (SDK 1.1.4, the same signed
  `amd_fidelityfx_vk.dll` as FSR 3.1). The game draws into FidelityFX's images and FidelityFX copies them into the real
  swapchain on a present queue of its own. No frame is generated yet. The device gets three more queues for it
  (present, image acquire, async compute; RTX 5070 Ti: families 0, 1 and 2). Without a free queue or the DLL, the
  driver's swapchain is used as before. Tested in Pallet Town at 30 and 60 fps with FSR 3.1, TAA, SSAO and GI, and in
  full screen at 4K (the swapchain made again): same images, same frame pacing.

## 0.4.2 (2026-10-19)

- **Wild battles no longer crash when you switch to your partner** (#6), and the 0.4.1 fix is now exact. The crash
  happens whenever the partner (the Pikachu or Eevee you started with) enters the battle while it is the one walking
  with you (early in the game nobody else does): the battle brings in the field model of the Pokémon walking with you,
  and the battle this mode uses has none for it. In that case touching a wild Pokémon now starts the game's own
  catch, as without the mode. With another Pokémon walking with you, wild battles work, the partner included -
  0.4.1 had sent some of those to the catch too.

## 0.4.1 (2026-10-19)

- **Wild battles no longer crash early in the game** (#6): with the partner in the first party slot no Pokémon walks
  alongside the player, and the battle's opening was made for one (the game stopped right after "A wild ... appeared!").
  Until that case is supported, those encounters go to the game's own capture. Checked on both games with a Route 1
  save (3 encounters each, all to the capture); with another Pokémon in the first slot, the battles run as before.

## 0.4.0 (2026-10-19)

- **Fixes the world drawn wrong after a catch** (0.3.2): black trees with yellow stripes, bushes with a sign's texture
  or a flat colour, and no dirt path until the area changed. After a catch the game loads shaders again at addresses
  that held other shaders, and the renderer handed back the old translations; shaders are now known by their content
  too. Checked with automatic catch loops: 7 catches on Eevee and 9 on Pikachu.
- **2x interface checked before it is packed**: waifu2x once (in seven runs) exited normally with garbage for every
  image, and the menus' icons came out as noise. Each 2x image, taken back down to 1x, must now match the original
  (good ones differ by up to 7 of 255, that garbage by 66 or more); with more than 10% off, no pack is written and the
  start screen says to try again.
- **Fixes many 2x interface images showing wrong** (partly or wholly transparent): waifu2x-ncnn-vulkan 20250915
  loses the alpha of an RGBA image outside its last tiles (a fully opaque 512x288 image came out 25/32 transparent).
  Images are now upscaled as RGB only - with transparency, their colour and their alpha (as a grey image) separately,
  put back together when packed. Opaque images now go into BC1 (the pack: 667 -> 548 MB). Make them again once.
- **Better background blur and depth of field** (`blur_quality = 1`, on by default): the menus' blurred backdrop and the
  title screen's depth of field come from one post-processing chain of the game's - the HDR scene taken down to 1/2,
  1/3 and 1/6 of its size with a single bilinear tap, blurred there with 5- and 9-tap separable kernels, mixed back by
  each pixel's blur size with bilinear reads (blocky, diamond-shaped steps at 1440p and 4K). Seven of its shaders, known
  by the hash of their code, now read their textures better: a 13-tap downsample (Jimenez), four rotated-grid taps
  per blur tap, and a cubic B-spline wherever a smaller level is magnified (the full-size scene is read as before). The
  game's own weights, focus and blend are untouched. The bloom shares the downsample and one blur shader. Cost at
  1440p, RTX 5070 Ti: +0.27 ms while a menu or the depth of field is on screen (the composite 0.05 -> 0.22 ms), a few
  hundredths of a millisecond for the bloom otherwise.
- **Sharper interface (2x)**, optional: a start-screen button makes the game's menu and interface images again at
  twice their resolution from the player's own game files - nothing of the game ships with the app. Every texture of
  the interface archives (`bin/app/*.arc`, 3081 different images, 316 more in formats left out) is decoded, upscaled by
  waifu2x-ncnn-vulkan (CUnet, light denoising; downloaded once from GitHub, 35 MB, checked by its SHA-256) and
  compressed again (BC1 / BC3 / BC4 / BC5) into `cache/ui2x.pak` (~670 MB). In the game a texture is matched by the
  key of its content and drawn from its 2x version at the same place. RTX 5070 Ti: about a minute (14 s reading the
  images, 17 s upscaling, 25 s compressing). A box on the start screen turns them off (`ui2x = 0`). The Real-ESRGAN
  build tried first loses the device on RTX 50 GPUs (2022 ncnn); waifu2x's 2025 build runs there.
- **Fixes SSAO, GI, TAA and FSR 3.1 flickering in cutscenes and menus at 60 fps**: there the game runs at 30 and an
  in-between image is drawn by replaying the frame; that image shared the frame's number, so the scene effects (which
  run once per frame) skipped it and every other image on screen went out without occlusion, indirect light or
  anti-aliasing. The in-between image is now a frame of its own for them, in the order the images are rendered: its own
  jitter and history, its camera blended between the two frames as its draws are (motion vectors), and frame N's
  post-processing (FSR 3.1, SMAA, the colour pass) done before the replay overwrites its inputs and shown after it.
  Professor Oak's cutscene in Pallet Town, FSR 3.1 Native AA + SSAO + GI: the brightness jump between consecutive
  images 0.100 -> 0.007, the in-between and final images now as alike as with the effects off (0.2 vs 0.13).
- **Sharper text at high resolutions** (always on, nothing to set): the game rasterizes its text with the system font into
  a 2048x1024 glyph cache at sizes 22-36, which went blurry when stretched to 1440p or 4K. Every glyph the game caches is
  now rasterized again by the same engine (nn::fontll) at N x its size into an N x copy of the cache, which the renderer
  samples instead of the original (same coordinates, so placement and layout are the game's). N is one step per 720
  lines of the window when the game starts (720p: off, 1440p: 2x, 4K: 3x). Cost: one extra glyph rasterization per new glyph and only the changed part of
  the cache uploaded (one full upload when the cache is made). `runtime/hle/font_hires.c`.
- **In-game settings on F12**: volume, brightness, contrast and saturation, applied live and saved to `larecomp.ini`
  (`brightness`, `contrast`, `saturation`). From a controller: − and + together, then *Settings*.
  `LARECOMP_MENU_KEY=<key name>` picks another key (Steam takes F12 for screenshots). The colour pass runs on the
  whole image, the game's interface included, and only when a value is not 100%.
- **Smoother shadows** (F12, *Shadow filter*): the game samples its 2048² shadow map with nearest filtering, so each
  of its 11 taps is a hard in / out and shadow edges come out stair-stepped. *Bilinear* makes each tap a 2x2
  depth-compare filter; *Smooth* (the default) a 3x3 tent of four bilinear taps; *Game's* keeps the original. Same
  shadow-map resolution. Measured at 1280x720 on an RTX 5070 Ti: the scene pass from 0.16 to 0.62 ms (*Smooth*).
  `shadow_filter = game | bilinear | smooth` in `larecomp.ini`.
- **Camera motion vectors** (groundwork for temporal effects): the game's view and projection are read from its scene
  constant block each frame; between the scene's opaque and blended draws each pixel's depth is taken to the previous
  frame (previous view-projection x inverse of this one, in double) into an RG16F image. Static scenery only: the
  characters get the camera's motion, not their own. F12 *Show motion vectors (debug)*; checked with
  `tools/mv_check.py` (reprojecting the previous frame cuts the error by 80-90% while walking).
- **F12 menu**: brightness, contrast and saturation moved to *Advanced...* (an *Image* section); the first page keeps the
  volume, the effects' switches, the shadow filter, anti-aliasing and the upscaler. With **FSR 3.1** chosen, its mode
  sets the render resolution - AMD's presets: Native AA (1.0x, FSR as anti-aliasing), Quality (1.5x, the default),
  Balanced (1.7x), Performance (2.0x), Ultra Performance (3.0x) - instead of the start screen's resolution
  (`fsr3_preset = native | quality | balanced | performance | ultra`); TAA's sharpness slider also sets FSR 3.1's. A
  page taller than the screen (the advanced one at 720p) scrolls. The start screen's box is just "Anti-aliasing" (SMAA
  when on, unless TAA was chosen in F12).
- **Per-object motion vectors** (F12 *Advanced...*, on by default whenever motion vectors are used): the scene pass's
  depth-tested draws run a variant of the game's vertex shader that also computes the previous frame's position from
  that draw's previous constant buffers (the shader body emitted twice by the translator, `cb<k>` read as `pcb<k>`),
  and their fragment shaders write the velocity to an extra RG16F-class target. A draw is matched to the previous
  frame's by program, vertex buffer, draw parameters, game object and occurrence; a draw missing any previous block, or
  whose block jumps (another object paired, a cut), marks its velocity unknown and the camera's is used there.
  Measured walking (`tools/mv_check.py`, `MV_BOX` on the player): reprojection error on the player 22-24 -> 12
  (camera-only vectors moved the player like the background); whole image 1.87 -> 1.67.
- **TAA** (F12, *Anti-aliasing*: Off / SMAA / TAA; sharpness under *Advanced...*): the scene's draws on its depth are
  jittered (Halton 2, 3 in the vertex shader, after the position the velocity uses), the complete HDR scene is resolved
  just before the game's bloom (Catmull-Rom history, YCoCg variance clipping, the nearest velocity of the 3x3,
  depth-checked disocclusion, tone-mapped blending), RCAS at the present. The interface is not touched (SMAA blurred
  it). Frame-to-frame instability walking 10-17% lower than without; no trail behind the player.
- **FSR 3.1** (F12, *Upscaler*: Bilinear / FSR 1 / FSR 3.1), when the render resolution is below the window's: AMD
  FidelityFX SDK 1.1.4 through its C API and AMD's signed `amd_fidelityfx_vk.dll` (MIT; shipped next to the exe,
  loaded at run time - without it, FSR 1). The newest FSR with a Vulkan backend: the FSR SDK 2.x (FSR 4) is DirectX 12
  only. The scene is jittered with FSR's own sequence; FSR gets the game image, the scene depth, the motion vectors
  (camera + per object) and a mask of the interface (the pixels that changed after the frame's first interface draw).
  From 640x360 to 1280x720 it is visibly cleaner than FSR 1 (difference to native 5.14 vs 5.40). The backend asks for
  debug-utils and AMD marker functions this device lacks: they get no-ops; `...KHR` names fall back to Vulkan 1.1 core.
  Not yet checked: the interface mask over a moving scene (battles).
- **Indirect light (GI)**, experimental, off by default (F12 *Indirect light (GI)*; its parameters under *Advanced...*):
  one bounce of the visible light in screen space, after the design of FastGI (a Unity URP feature; the technique,
  rewritten for this renderer - no code taken from it). Between the scene's opaque and blended draws: normals rebuilt
  from the depth (the game has no normal buffer), rays traced at half / a third / a quarter of the resolution against
  the depth with the game's own projection (cosine-distributed, quadratic steps, the first crossing refined by
  bisection and validated: on the ray, facing it and the camera), the scene's colour where they hit (luminance
  clamped), two bilateral blurs, accumulation along the camera motion vectors (depth-validated, neighbourhood-clamped),
  an edge-aware upsample added onto the scene. Grass greens the curbs, fences and the foot of walls, the red roofs
  their eaves. Defaults: intensity 200%, radius 300, thickness 25 (the game's units are ~cm), 4 rays x 8 steps, half
  resolution. Measured at 1280x720 on an RTX 5070 Ti: +0.36 ms. Walking, the accumulated light has no trail behind the
  player (brighter / darker differences against single-frame GI symmetric) and much less grain.
- **F12 menu split**: the effects are switched on the first page (SSAO, GI, the shadow filter); their sliders and the
  debug views moved to *Advanced...*. SSAO defaults are now intensity 200%, radius 100.
- **SSAO accumulated over frames** (F12 *Accumulate over frames*, on by default with SSAO): the sample spiral turns
  every frame and the result is blended with the previous one fetched along the camera motion vectors (90% history),
  rejected where the stored distance is not the one the pixel had (uncovered ground, characters moving on their own)
  and clamped to the current 3x3 range. Checked walking and stopping against single-frame SSAO
  (`tools/ssao_temporal_check.py`): a first version with a 2% clamp margin left the player's contact shadow as a trail
  that never faded (2-4% of pixels darker); with the exact clamp the differences are symmetric noise (~1% darker, ~1%
  lighter) and nothing remains after stopping; high-frequency noise down 8-10%.
- The F12 debug views (the occlusion alone, the motion vectors) are shown at the present, in place of the game image:
  written into the scene they came out white or washed out by the game's fog, bloom and tone mapping.
- **Ambient occlusion (SSAO)**, experimental and off by default (F12: on / off, intensity, radius, a view of the
  occlusion alone): computed from the game's depth between its opaque and blended draws, with the game's own projection.

## 0.3.2 (2026-10-18)

- **Fixes the launcher forgetting your settings** (0.3.1): it saved `larecomp.ini` with some lines run together, so
  the next start went back to the defaults for 60 fps, the resolution, anti-aliasing, the translation, movie audio,
  the fps counter, the d-pad and wild battles. Fixed, and a file saved by 0.3.1 is read correctly: tested with one,
  every setting came back.
- **Game folders with accents or other non-Latin letters work** (for example a Windows user folder named with an
  accent). Tested with a folder named in Cyrillic and Portuguese letters: 0.3.1 could not open the game there, 0.3.2
  runs it.
- **Less memory reserved**: the game's threads no longer commit 256 MB of stack each up front. Pewter City, Eevee:
  committed memory 7.2-7.4 GB -> 4.7 GB; the memory actually in use is unchanged. Helps on PCs with little RAM or a
  small page file.
- **Less GPU time**: fragment and compute shaders unpack half-precision values natively. RTX 3080, Pewter City at 2x
  resolution: 4.0 -> 3.75 ms of GPU per frame (the frame rate at the game's 30 fps does not change on this machine).
  Frames look the same.
- Fixes from LAS Recomp, the project this port is built on: render passes no longer hand empty color slots to the
  driver (they crash Intel UHD 630 drivers there; not tested here on Intel), a GPU fence race, duplicate pipelines,
  and shader instruction details. A rare crash when two game threads touch new memory at the same moment is fixed
  too (found in another port of the same runtime; not seen in Let's Go).
- After a crash, no extra "Visual C++ Runtime Library" box. A controller Windows does not recognise can be added with
  your own SDL mapping line in `gamecontrollerdb.txt` next to the exe.
- Tested with Eevee and Pikachu 1.0.0 on the regression routes (world, menus, area change); saving from the menu checked with Eevee.
- Upgrading: unzip over your install (no new setup).
- Tested with the zip built for this release, unzipped into a new folder named with accented letters, the game files of 0.3.x installs of both games, started without MSYS2 on the PATH (no recompiling needed): Eevee and Pikachu load a save and reach the world; the start screen rewrote a settings file saved by 0.3.1 with every setting kept.

## 0.3.1 (2026-10-18)

- **Fixes the world breaking after a capture** (found playing 0.3.0 with 60 fps on): after entering and leaving a
  capture, fences, signs and ferns could show flat or dark until you changed area. The cause was older than 0.3.0 and
  only luck kept it hidden: when the game rebuilt a texture in place (it streams them), the texture slot the game had
  registered kept pointing at the freed copy, so what it showed depended on what the memory allocator put there next.
  The render thread and the video-memory models of 0.3.0 changed the allocation pattern, which made it show. Slots
  now follow the texture when the game rebuilds it.
- Tested here with a new unattended loop that walks in the grass of Viridian Forest and runs from each encounter
  (Eevee and Pikachu 1.0.0, both 0.3.0 options on): the world looked the same before and after every encounter. The
  0.3.0 performance options stay **on by default**.
- Upgrading: unzip over your install (no new setup).

## 0.3.0 (2026-10-18)

A performance release. Everything below was measured on one machine (Core i5-14600KF, RTX 3080, Windows 11), with
runs taken in turn (A, B, A, B...) because the same frame varies 10-18 ms of GPU time between runs. **Not measured on
other graphics cards** (AMD, Intel, integrated): feedback is welcome in an issue.

- **The graphics work runs on its own thread (on by default).** The game's calls into the Vulkan backend used to run
  on the game's own thread, in the chain that also runs its logic. Unlimited frame rate on the Pewter City route:
  Eevee 126.9 -> 185.5 fps (frame time ratio 1.46, 95% interval 1.40-1.53), Pikachu 132.1 -> 191.4 fps (1.45, 1.31-1.54);
  the 99th percentile frame time drops from 13.3 to 9.9 ms. At the game's own 30 fps cap you will not see more frames:
  the gain is CPU headroom. The paced 60 fps mode is
  unchanged (16.63 ms per frame with and without). Rendered frames match the old path within the run-to-run noise;
  the Vulkan validation layers report no errors on the regression routes of both games. `render_thread = 0` in
  `larecomp.ini` (or `LARECOMP_RENDER_THREAD=0`) turns it off.
- **Models are read from video memory on dedicated GPUs.** The vertex and index data was read by the GPU in place from
  the game's memory (system memory, over PCIe) for every vertex invocation. It is copied to VRAM now: GPU time of the
  Pewter City frame 10.2 -> 1.2 ms. Integrated GPUs keep the old path (not tested). `vertex_read = direct` in `larecomp.ini` (or
  `LARECOMP_VERTEX_READ=direct`) goes back to it.
- **Mutex waits no longer stall the game threads.** A contended lock spun briefly before sleeping on a condition
  variable (~1000 contended locks per frame per thread, ~12 us each on Windows). The paced 60 fps mode now holds 60.0
  fps (it was 58.8-59.3); unlimited frame rate went from about 60 to 65-74 fps at the time.
- **`larecomp.ini` edited by hand on Windows works**: lines ending in CR+LF kept the CR in the value, so
  `resolution = dynamic`, `upscaler = bilinear` and the percentage modes fell back silently.
- **Dynamic resolution** (`resolution = dynamic`, ini only) judged a steady 30 fps as too slow and dropped to 0.60
  scale; it budgets by the scene's pace now.
- For developers: a much faster regression battery (6 routes of both games in ~100 s, was ~30 min), an interleaved
  A/B performance test with a confidence interval, per-draw GPU timing and a frame time CSV (docs/DEPURACAO.md,
  docs/PESQUISA-GPU.md).
- Upgrading from 0.2.x: unzip over your install; no new setup (the game library is not rebuilt).
- Tested with the zip built for this release, unzipped over 0.2.x installs of both games and started without MSYS2 on the PATH (no recompiling needed): Eevee loads a save and reaches the world (Route 2); Pikachu boots with the render thread on and plays the opening movie.

## 0.2.2 (2026-10-17)

From the second patch in [issue #1](https://github.com/raulzrrs/letsgo-recomp/issues/1), thanks again to
@porstobotgovna:

- **No more crashes on the communication screens.** The network services (`nifm`, `ssl`, `bsd`, `ldn`) didn't exist:
  the game got an empty session and closed when opening local wireless, Mystery Gift or internet co-op. They now
  answer like a console that is offline. Tested on both games (Eevee and Pikachu 1.0.0): **local trade** searches for
  a player and B goes back; **Mystery Gift** stays on "Connecting..." (that screen has no way out offline: the game
  waits with no timeout and ignores B, as on a console without Nintendo's servers); the **Pokémon GO settings**
  search and B goes back to the menu.
- The other Bluetooth (`btm:u`) commands answer with no data, like a console with nothing paired.
- Tested with the zip downloaded from this release, unzipped over 0.2.1 installs of both games (no recompiling
  needed): each one boots a save and reaches the local trade search.

## 0.2.1 (2026-10-17)

From [issue #1](https://github.com/raulzrrs/letsgo-recomp/issues/1), with thanks to @porstobotgovna, who played
*Let's Go, Pikachu!* 1.0.2 on an AMD RX 7800 XT with Windows 10 and sent a patch:

- **Supports *Let's Go, Pikachu!*** (and updated games): the launcher accepts either game by its program id, shows
  which one is set up, keeps Pikachu's saves in `save-pikachu\` and installs translations for the set-up game.
  Tested here with Pikachu 1.0.0: setup (13 to 17 minutes), a new game through the title movie, Professor Oak and the
  name screen, and played (from a copy of an Eevee save) with 60 fps, wild battles and the PT-BR translation. Each
  folder holds one set-up game: to have both, unzip the release twice. **60 fps and wild battles work on Pikachu 1.0.0 too:** their hooks and the generator's instruction patches
  are addresses in the game's executable, now in a table per build (the same functions in Pikachu, found by comparing
  the code instruction by instruction, `tools/addrmap.py`). Other builds (the updates) get neither (greyed out on the
  start screen). The game is told its real version (1.0.0 / 1.0.2) instead of always 1.0.0.
- **Fixes a crash at boot (`c0000005` accessing `0x38`) on some Windows 10 machines**, whose Yu Gothic Regular font
  breaks the game's font engine: Yu Gothic Medium and MS Gothic come first now. `LARECOMP_FONT=<file>` overrides it.
- **Pokémon GO Park settings:** the game waited on a Bluetooth pairing event that was never created and aborted; the
  event exists now, and a wait on an empty handle slot waits on the others.
- **Fixes wild battles with 60 fps on (0.2.0, both games):** the battle counted as a menu and got in-between frames;
  it runs at 30 fps like the game's own battles now.
- Flat dumps (the code files next to the RomFS's `bin/`) are set up too.
- The overlay (close the game?, name entry) speaks English, Portuguese or Russian, from the system language
  (`LARECOMP_LANG=en|pt|ru`).
- Crash reports list the last calls to the host, and the previous run's log is kept as `lasrecomp-prev.log`.
- Tested with the zip downloaded from this release, unzipped twice without development tools on PATH: Eevee 1.0.0
  and Pikachu 1.0.0 set up from `exefs` + `romfs.bin` / `romfs` (13 minutes each), an Eevee save in Viridian City with
  the PT-BR translation and 60 fps in the world, and a new Pikachu game with the translation installed from its `.rar`
  (Professor Oak's lines in Portuguese).

## 0.2.0 (2026-10-17)

Two experimental options on the start screen, both off by default:

- **60 fps (experimental).** The overworld runs at 60 fps natively: the game's own time step is halved there, so
  walking, the camera and the world's animations are smooth at the right speed. Menus and the Pokédex keep the game's
  30 updates per second and are shown at 60 with an in-between frame. Battles and captures stay at 30 fps, as the game
  made them (their timelines count frames; an in-between frame there was tried and left out). Tested by playing:
  world, menus and Pokédex at 60 without speed-ups; battles and captures at normal speed.
- **Wild battles (experimental).** Touching a wild Pokémon starts a classic battle against it (its species, level,
  gender, IVs and level-up moves) and then the capture, like the game does with Snorlax and the legendary birds.
  Played on Route 1. Not done yet: split experience, nature and shiny of the touched Pokémon, regional forms, Pokémon
  with an aura; other areas not tested.

Also:

- **Controller stick dead zone**, adjustable on the start screen ("Keyboard and mouse"): a stick at rest no longer
  moves the Poké Ball aim.
- Known issue, also in 0.1.2: the colored aura of special wild Pokémon sometimes disappears for a single frame
  ([docs/PROGRESSO.md](docs/PROGRESSO.md), item 6).
- Tested with the zip downloaded from this release, in a new folder without development tools on PATH: setup from a
  game folder with `exefs` and `romfs` (13 minutes to recompile on a 20-thread CPU), a new game through the controller
  screen, the title movie, Professor Oak and the character and name screens, and the Brazilian Portuguese fan
  translation installed from its downloaded `.rar` (Professor Oak's lines in Portuguese). With both options on (another
  clean folder): the world at 60 fps and a classic battle started by touching a wild Pidgey.

## 0.1.2 (2026-10-16)

- **Fixes a crash at the title screen on a new game** (no save yet) in 0.1.1: the movie player looked for the
  game's movies in an extracted `romfs` folder, which the release's setup doesn't create (it builds the `romfs.bin`
  image). Movies are now read from inside the image.
- Tested from a clean folder with the downloaded zip: setup, a new game through the controller and language screens
  into the introduction, and the Brazilian Portuguese translation installed from the `.rar`.

## 0.1.1 (2026-10-16)

- **Fixes the game closing right after the controller screen** ("svcBreak") in the 0.1.0 release: its FFmpeg was
  built without the Opus decoder the game's music and sound effects use. The release build now checks every decoder
  the game needs, and a missing one is reported by name instead of crashing.
- The one-time setup's progress bar no longer reaches 100% long before the end (it counted the four game modules as
  two).
- The README states exactly which game files and version are supported, with the sources.

## 0.1.0 (2026-10-15)

First public release.

- Native Windows port of *Pokémon: Let's Go, Eevee!* v1.0.0 by static recompilation (built on
  [LAS Recomp](https://github.com/peiikai/LASRecomp)): the game's four executable modules are translated from ARM64
  to C and compiled to x86-64; no ARM64 interpreter or JIT at run time.
- Release zip with `LetsGoRecomp.exe`: pick your extracted game folder in the launcher and the game code is
  recompiled on your PC (bundled generator and Zig compiler; nothing else to install).
- Stable 30 fps, 720p to 4K internal resolution (with supersampling), SMAA, 16x anisotropic filtering, any window
  size, fullscreen; parallel shader translation with disk caches.
- Keyboard and mouse (rebindable; the mouse aims Poké Balls and is the touch screen), any SDL 3 controller, virtual
  finger with the right stick for Play with Eevee.
- Atmosphere-style LayeredFS mods; one-click install of fan translations from the downloaded .zip / .rar / .7z.
- Launcher in English and Brazilian Portuguese.
