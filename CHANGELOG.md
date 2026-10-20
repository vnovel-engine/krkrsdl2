# Changelog

Changes against upstream [krkrsdl2/krkrsdl2](https://github.com/krkrsdl2/krkrsdl2).

| | |
|---|---|
| Upstream | https://github.com/krkrsdl2/krkrsdl2 (git remote `upstream`) |
| Fork | https://gitlab.com/matchaxnb/krkrsdl2 (git remote `origin`) |
| Branch | `main` |
| Fork point | `124bdf8` — "cleanup: 0/nullptr/NULL comparison and control flow cleanups" |

The fork point is the common ancestor with `upstream/HEAD`; `git merge-base
HEAD upstream/HEAD` reproduces it. It is consumed by the
[vnovel](https://gitlab.com/matchaxnb/vnovel) libretro core as its KiriKiri
engine. `external/krkrz` and `external/FAudio` are forks in their own right;
see their `LINEAGE.md`.


## Unreleased

### Visual-effects shims: `filter`, `slideopen`, and the three missing `extrans` handlers

Fate/stay night's effect reel (`~/VN/fate-fx`) exercises every visual effect the
game's Windows DLL plugins and its own effect scripts provide. Three surfaces
were missing from this fork, and each aborted the scene at the tag that reached
it, because a KAG tag handler that raises stops the scenario there:

- **`filter.dll`** published twelve TJS globals (`Stretch`, `initHaze`,
  `doHaze`, `endHaze`, `Contrast`, `Noise`, `InitLens`, `ReleaseLens`, `Lens`,
  `Blur`, `Smudge`, `update`) and the override table answered its link with a
  plain no-op, leaving every one of them undefined. `ConditionPlugin.ks` /
  `HazePlugin.ks` / `NoisePlugin.ks` / `DashPlugin.ks` call them **by name**,
  so the first `Smudge` or `Stretch` in a scene raised
  `Member "Stretch" does not exist` and the scene died there. Now a built-in
  shim (`src/shims/filter.cpp`), implemented from the decompiled plugin and the
  engine's own pixel primitives.
- **`slideopen.dll`** (`initSlideOpen` / `drawSlideOpen` / `finishSlideOpen`)
  was likewise a no-op; the game's `SlideOpenPlugin.ks` drives it for every
  open/close transition. Now `src/shims/slideopen.cpp`.
- **`extrans`** registered only `mosaic` and `wave` — the two Fate itself
  requests — and stated in a comment that the other five "can never run here".
  That is not true of the engine: the shipped plugin registers **seven**
  handlers (`mosaic`, `wave`, `ripple`, `turn`, `rotatezoom`, `rotatevanish`,
  `rotateswap` — upstream `RegisterRotateTransHandlerProvider` spawns three
  providers, rotatetrans.cpp:487-493), and a later TYPE-MOON title on this
  engine can request any of them. All seven are now ported faithfully from the
  upstream `src/plugins/win32/extrans` source (including `turn`'s 64×64
  generated table, transcribed byte-for-byte).

Two bugs were found and fixed while verifying, both worth recording:

- **A shim's `BuiltinShim::description` must be plain ASCII.** It is handed to
  `ttstr(const char *)`, which decodes in the ANSI codepage; a UTF-8 em-dash in
  the string threw `Cannot convert given narrow string to wide string` from
  inside `TVPInstallBuiltinShim`, so the shim's `V2Link` never ran and the
  plugin link failed — with the failure surfacing at the *game's* line, not
  here. `BuiltinShims.h` now states the constraint at the field.
- **Ported pixel code must treat the pitch as signed.** `ripple`'s border-fold
  transform declared its per-pixel offset `tjs_uint`; with this engine's
  bottom-up bitmap the pitch is negative (an 800×600 layer reports −3200), so
  `x*4 + y*pitch` wrapped to a huge positive offset and the transform
  segfaulted. Windows' DIB pitch is positive, so the upstream source's unsigned
  type was harmlessly correct there. Now `tjs_int`; the two other offset sites
  in the file were already signed and were checked.

The reel now runs all **51** effects end to end with no failure and no skip, and
loops. `src/shims/EXTRANS-HANDLERS.md` records the handler-name and option-key
cross-check between the upstream source and the shipped binary, including the
correction that the binary registers seven handlers rather than five.

#### `doHaze`'s vertical taper, and two phase errors

`initHaze` reads six keys the shim was ignoring — `upper`, `center`, `lower` and
`upperpow`, `centerpow`, `lowerpow` — and Fate's own scenarios pass all six in 7
of their 46 `@haze` calls (`upper=90 lower=490 center=250 upperpow=0.3
lowerpow=0.2 centerpow=0.7`). They select which rows are displaced at all and
scale the displacement per row, so leaving them out made the wave uniform over
the whole frame. `FUN_100051d0`'s three branches are now ported as they are
written: uniform when `upper` and `lower` are both negative, one ramp
`upper → lower` when `center` is negative, two ramps otherwise, with the
per-row slope an integer division as in the original (`0x1000584b`,
`0x1000591f`).

Two phase errors fell out while checking the taper against the disassembly:

- **The table index advanced 651.9× too slowly** — `2048/PI` exactly, the
  missing factor. The wrapper converts `speed` to `2*PI/speed` before calling
  (`HazePlugin.ks:127..128`) and `InitHaze` stores that value verbatim
  (`0x10004df0..0x10004e2a`; its own no-key default at `0x10004e32` is
  `2*PI/4000`), while `doHaze` multiplies it by **`2048/PI`** (`0x1000f618`,
  multiplied at `0x100054e4`) to get the index. The shim took the wrapper's
  `2*PI/speed` as its per-ms index advance, so the index moved at
  `2*PI/speed` where the DLL moves at `4096/speed`: with the default
  `speed=4000` a wave that should take 2000 ms took 21.7 minutes, which is
  indistinguishable from no motion within a scene.
- **`cycle` was divided twice.** The wrapper already reduces it to `4096/cycle`
  (`HazePlugin.ks:129..130`), which is what the DLL stores
  (`0x10004e8c..0x10004e97`).

The displacement also rounds the way the DLL does: to nearest on the untapered
path (the `0.5` added at `0x10005751`) and truncated on the tapered one, which
is already fixed-point (`sar eax,0x14` at `0x10005952`).

`lwaves` remains unimplemented. `InitHaze` does read it (`0x10005149`), but the
displacement is built from `waves` alone, so a second wave set has no consumer
in `doHaze` — recorded here rather than guessed at.

### Changed

- `MenuItem.visible` is item-level only and is no longer forwarded to the host
  (`TVPSetMenuVisibleCallback` is gone, with `TVPNotifyMenuVisible`). Upstream's
  tree-toggled-bar semantics are about a Win32 window's menu bar and this target
  has no window frame to attach one to, so there was nothing for the request to
  do; forwarding it to the core's own menu overlay conflated two unrelated
  things. The value is kept and read back as before.
- The text-plane seam's header now states what the published pointer is: the
  *last* row of a bottom-up bitmap, so a host must walk the signed stride rather
  than copy `pitch × height` bytes forward from it. The core's copy did the
  latter and read past the end of the allocation; the rule was implicit in the
  pitch's sign and is now written down where a host reads it.
- The compatibility table gained The House in Fata Morgana; its row and the
  reasoning are in the era section below, which is where the table is described.

## 2026-10-16 — text plane, native menu, shims

### Text plane

- The engine publishes the plane from `tTVPNativeBaseBitmap::InternalDrawText`,
  the one funnel every glyph on every bitmap is blitted through, with the final
  clipped destination rectangle. That is why no "which layer is the text layer"
  rule is needed here: the plane *is* whatever surface text is drawn into, and
  the host is told per bitmap and per glyph. No cost when nothing is listening —
  the gate is one test and the callback is handed the live surface, not a copy.
- The plane carries a **negative** pitch, as the engine's bitmaps are
  bottom-up. That is the row direction, not an error; the host contract says so
  explicitly. Every guard that tested `pitch <= 0` or `pitch < width*4` rejected
  real publications (`-3232` for an 808x48 surface) and is now magnitude-based.

### Native menu

- `MenuItem` and the read-only `Window.menu` are restored as native classes —
  the Win32-like contract the scripts already expect — rather than TJS
  polyfills. The removed `MenuItem.tjs` polyfill declared `class MenuItem` and
  overwrote the native one, so a script's `class KAGMenuItem extends MenuItem`
  extended the script class and `Member "kag" does not exist` aborted start-up.
- Menu visibility is forwarded to the host through the presentation seam
  (`TVPSetMenuVisibleCallback`), beside the present and text-plane callbacks.
- `menubase` and its `vramhdl`/`fontmng` dependencies are vendored unmodified
  from NP2kai (MIT, (c) 2017 AZO), with the host surface in `include/`.

### Visual-effects shims

- `drawer` (line, anti-aliased line, anti-aliased triangle), `firespark` and
  `extrans` are built in as CPU shims, reached by name from the override table.
- `shim_abi.h` mangles each shim's entry points per name (`shim_<name>_V2Link`),
  because every shim defines C `V2Link` and N of them in one binary collide. The
  standalone PE build keeps the plain names.
- `extrans` replaces an erroneous no-op: `mosaic` (17 scenario call sites) and
  `wave` (8) are requested **by name** and the engine's transition registry holds
  only crossfade/universal/scroll, so `StartTransition` threw
  `TVPCannotFindTransHander`, nothing caught it, and the `@prick`/`@wave` scenes
  aborted. A class stub cannot satisfy a name lookup.
- All three keep the **sign** of the bitmap pitch. `drawer` had stripped it and
  walked rows the wrong way (row 0 wrote outside the image); `firespark`
  rebased-then-flipped in both `BindLayer` and `RefreshLayer`, which mirrored the
  effect and made the pointer differ per call, so it re-sampled `clearColor` from
  a wrong address each frame.

### Saves and compatibility

- The `structconst`/`structcrlf` save wire-format switches are bound as one
  format and read at serialise time; all four `#ifdef TJS_TEXT_OUT_CRLF` sites
  are gone.
- The override directory is reachable and correct: the walker's file keys are
  `tjs_string` (they were widened through `std::wstring` and reinterpreted, so
  every key collapsed to one stray character), and registration logs the
  resolved directory, file count and every key.

### Removed

- `SDL_libretro_surface` and the accessors that read it; frames are consumed
  through the presentation seam.
- The `MenuItem.tjs` polyfill (see Native menu).


## The port

Runs KiriKiri Z games (The House in Fata Morgana, Fate/stay night) on Linux
from the libretro core, reading `.xp3` archives in place:

- CMake detects and links libvorbisfile; Vorbis decoder added.
- `PluginImpl`: `wuvorbis` / `adjustmoni` / `extrans` plugin links are simulated
  as no-op successes (`krkrsteam` stays fatal so the game's own catch sets
  `Steam = void`).
- `StorageImpl`: `Storages.dirlist` implemented (font scanning).
- `SysInitImpl`: auto-discovers `data.xp3` next to `ExePath()`.
- `SystemImpl`: message boxes logged to stderr; honours `KRKRSDL2_PERSONAL_PATH`.

## Engine compatibility (the era mask)

Two titles are in the table, and they sit at opposite ends of it — which is the
point of the mechanism:

| Game | `data.xp3` crc32 | Engine | Era |
|---|---|---|---|
| Fate/stay night (English) | `64960AF9` | 2.19.15.783 | `2.19` — every switch on |
| The House in Fata Morgana | `71604817` | 2.31.2013.411 | `modern` — none needed |

Fata Morgana's engine is newer than every threshold in `CompatProfile`, so its
row is the empty set stated explicitly: the title is recognised and logged by
name rather than falling through to the default. Measured against the Wine
reference of the same release, stepped through the same script input; at the
title screen the two frames differ by 0.044 % of their pixels (a software
cursor), and nothing about the era is what differs between them.

`CompatProfile` selects the set of behaviours a game was written against — the
class-scope `var`, post-`!`, `holdAlpha`, unary-`*` and layer-compositing
switches — as a bitmask, because a published title can carry one backported
change without the rest.

- **Source order**: the per-run spec (`KRKRSDL2_COMPAT`, which the core exports
  from its `vnovel_kirikiri_compat` option), then a per-game user override
  (`KRKRSDL2_COMPAT_<id>`), then the per-game table, then `TVP_COMPAT_DEFAULT_LEVEL`.
  Every run logs which source supplied the mask, so "which era did this game
  get" is answerable from the log rather than by re-reading the environment.
- **The table** is keyed by the game's identity: the crc32 of its `data.xp3`,
  computed by the core during `retro_load_game` and passed in `KRKRSDL2_GAME_ID`.
  That is the same fingerprint `tools/hash-game.py` records in `dat/vnovel.dat`
  and `vnovel.rdb` is indexed on, so one game has one id across the tools, the
  database and the engine. `TVP_COMPAT_DEFAULT_LEVEL` is now the last resort
  (the engine's own defaults), not one title's era.
- Flags stay individually settable and a level is only a named preset; the
  parser, the mask and the level thresholds are unchanged.
- **`composite219`** selects the pre-2006 layer-compositing algorithm
  (`CompositeLevel.{h,cpp}`) and is set by the `2.19` preset. The two eras are
  different algorithms, not branches of one, so the level is bound once from the
  resolved mask (`TVPApplyCompatMask` → `TVPBindCompositeLevel`) before the
  scripting engine starts, and the hand-off in `tTJSNI_BaseLayer::DrawCompleted`
  is dispatched through a member-function pointer chosen from it. The modern
  form, when a layer has no image of its own, fills the destination's uncovered
  parts with transparency through `CopySelf` and tracks them in a `DrawnRegion`;
  that fill is the 2006 eraser that overwrites children's output (the observed
  loss of the message text on Fate's disclaimer). Legacy219 drops the eraser and
  keeps the re-routing of a child's bitmap to the current draw target. Each run
  logs `(info) layer compositing level: Legacy219|Modern`.

## Engine module build

- `KRKRSDL2_LIBRETRO` (default OFF) selects the engine-module build: module
  name `vnovel_engine_krkrsdl2` (no `lib` prefix, no `tvpwin` suffix), the PE
  export list, and the `KRKRSDL2_LIBRETRO` define that switches the SDL2
  backend to the captured-audio / silent-device path. OFF keeps upstream
  standalone naming.
- The Windows build works at all now: POSIX-isms in the sdl2 layer are guarded
  (`dirent` `d_type`, `io.h`, `direct.h`, `SDL_loadso`) and the DLL is produced.

## Rendering, input and timing

- **Modal windows**: libretro has one framebuffer and no
  compositor, but TVP creates a real SDL window+surface per TVP window, so a
  YesNoDialog published its own 317x117 surface (stretched across the viewport,
  unreachable buttons). A live modal's surface is now alpha-blended into the
  primary window's frame at its position via a shadow buffer; non-live windows
  are skipped so a closed dialog cannot leave a stale overlay.
- **Input, polled state**: `SDL_PushEvent`
  updates only the event queue, so anything *polling* input
  (`System.getKeyState` → `TVPGetKeyMouseAsyncState` / `TVPGetJoyPadAsyncState`)
  saw the key as up — which broke KAG text advance (`VK_RETURN`) and menu focus
  (`VK_PAD*`, Fata Morgana's `createPadKeyMap`). Injected state is now recorded
  in tables written by the same handlers that post the TJS events. The engine's
  own `SDL_GameControllerOpen` is also disabled under libretro, so a RetroPad
  press no longer reaches KAG twice and RetroArch remapping applies.
- **Input, stale button flags**: the shift/button flags posted with a mouse
  event were snapshotted at the top of the branch, *before* the button
  transition was recorded, so a button-up still carried the "button held" bit.
  The layer manager consults exactly that bit to decide whether to release
  mouse capture (`PrimaryMouseUp` → `TVPIsAnyMouseButtonPressedInShiftStateFlags`),
  so `ReleaseCapture()` never ran and `CaptureOwner` stayed set from the first
  click: every later click was routed to that first layer instead of the layer
  under the cursor. Clicking a link did nothing while hovering it worked, since
  hover always walks `GetMostFrontChildAt`. The flag snapshot is now refreshed
  after the transition is recorded. Full case study:
  `private/INPUT-ARCHITECTURE.md` in the core repo.
- **Forced resolution**: `krkr_set_resolution` reports
  the requested size via `tTVPScreen` instead of forcing the SDL frame surface.
  Forcing the surface while the KiriKiri primary layer stayed at the script's
  size made the 1:1 completion blit run off the heap (corrupting a layer's
  Manager pointer) or silently drop the frame.
- **Clock and fast-forward**: FF dilation was
  summed from per-iteration deltas and lost all precision once the loop spun
  fast, freezing `krkrsdl2_tick_extra_ms` so typewriter and wall-clock-timed
  events ran at normal speed while frames raced. It now reads the core's single
  installed clock (`krkr_set_clock`), so every KiriKiri timer, transition and
  script wait scales and the engine keeps no dilation state.
- **Pause**: RetroArch has no core-visible pause callback and just
  stops calling `retro_run()`, so the core stamps a heartbeat each frame and the
  loop idles once it goes stale.
- **Quit and restart**: a user quit hung the frontend —
  the pause gate returned before the terminate check, and `Run()` blocked in
  `SDL_WaitEvent(NULL)`. The frontend now posts a quit *request* that the engine
  loop acknowledges on its own thread; `krkrsdl2_prepare_restart` re-arms the
  subsystems so the libretro Reset can boot the engine a second time in-process.
- **Audio suspend**: `krkr_set_audio_suspended` idles the main loop
  and, through the FAudio mix callback, stops the engine advancing while the
  frontend reports audio suspended.
- **Pacing**: the main loop re-ran as fast as the CPU
  allowed whenever anything animated (the `fpslimit` plugin is a no-op stub),
  costing ~115 % CPU at a static screen. The remainder of the frame budget is
  slept after each iteration (`KRKRSDL2_FPS`, default 60; 0 disables; guarded
  out on Emscripten).

## Fonts

- **Bundled fallback**: the engine hardcoded its default face to
  "Noto Sans CJK JP" and looked it up by exact name, so on a host without it
  (the Pi: DejaVu + Droid Sans Fallback) the first text render threw and nothing
  drew. Mona is now bundled and used as the fallback; MS face names are aliased.
- **Discovery backends**: font discovery moves to
  `FontDiscovery.cpp`, leaving matching/aliasing to `FreeType.cpp`. On Linux/BSD
  it uses fontconfig (`FcInit` + `FcInitLoadConfigAndFonts`, config made
  current, then `FcConfigGetFonts`), so distribution and per-user directories,
  aliases and substitutions are honoured without a maintained path list. The
  non-fontconfig walk covers the per-user locations each platform actually uses
  (`%LOCALAPPDATA%\Microsoft\Windows\Fonts`, `%WINDIR%\Fonts`, `~/Library/Fonts`,
  …). The default face is configurable instead of hardcoded.

## Plugin/resource overrides and encrypted XP3

Games ship Windows PE plugins (`*.dll`, `*.tpm`) and some XOR-encrypt their
archives; neither PE can be `dlopen`'d. One flat table, `data/overrides.txt`,
maps `<fileName> <fileHash> <replacementFile> <needDecryption>`:

- Names are matched as Unicode (UTF-16) codepoints, not bytes, so Shift-JIS/CJK
  file names compare correctly and case-insensitively without a hand-rolled
  ASCII fold. A record matches only when every criterion it names is satisfied.
- `replacementFile` is `dlopen`'d and its `V2Link` called like the plugin it
  replaces; a load failure (wrong architecture, missing object) is a *warning*,
  because the game ships a Windows PE and a mis-built shim otherwise looks like
  success. The replacement must be built for the target architecture.

## Crash fixes

- `global.AdjustMoni` was never defined after the plugin was link-stubbed, so
  YesNoDialog.tjs threw whenever a save/exit dialog appeared; a stub that
  centres the dialog over its parent is provided.
- `TVPLoadSystemFonts()` stored `c_str()` pointers to temporary `std::string`s
  for the `~/.fonts` / `~/.local/share/fonts` dirs, dangling before the loop
  ran; the strings are kept alive.
- `PublishLibretroSurface()` freed the old shadow surface while
  `SDL_libretro_surface` still pointed at it, so the core read freed memory from
  another thread (garbage w/h, SIGSEGV launching Fata Morgana). The replacement
  is created and published before the old one is freed.

## License

Inherited from upstream — see `LICENSE`.
