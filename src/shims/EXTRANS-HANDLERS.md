# extrans.dll — handler name & option-key cross-check

Factual cross-check of the upstream extrans source against the shipped
`extrans.dll`. Every claim cites a source `file:line` or a binary byte offset.

- Upstream source: `/home/matcha/dev/kirikiri2/kirikiri2/src/plugins/win32/extrans/`
  (CP932-encoded; the `file:line` numbers below are stable regardless of
  encoding when read as bytes via `:raw`).
- Shipped binary: `/home/matcha/VN/fate-fx-wine/extrans.dll`
  (MD5 `459a07bf51df5ea68dee91ab3c233404`, 237568 bytes, image base
  `0x10000000`). Section map (PE): `.text` rva=0x1000 raw=0x1000,
  `.rdata` rva=0x11000 raw=0x11000 — `.text` maps 1:1 so a file offset in
  `.text` equals its RVA.
- Decompilation: `~/dev/fate-decompiled/ghidra-output/extrans.dll/extrans.dll.decomp.c`
- Address table: `~/dev/fate-decompiled/exports/extrans.dll.md`

## Registration count — 7 handlers, not 5

Upstream `Main.cpp:32-36` `V2Link` calls five `Register*TransHandlerProvider`
functions. One of them, `RegisterRotateTransHandlerProvider`
(`rotatetrans.cpp:483-493`), internally registers **three** providers:

```c
// rotatetrans.cpp:487-493
RotateZoomTransHandlerProvider   = new tTVPRotateZoomTransHandlerProvider();
TVPAddTransHandlerProvider(RotateZoomTransHandlerProvider);
RotateVanishTransHandlerProvider = new tTVPRotateVanishTransHandlerProvider();
TVPAddTransHandlerProvider(RotateVanishTransHandlerProvider);
RotateSwapTransHandlerProvider   = new tTVPRotateSwapTransHandlerProvider();
TVPAddTransHandlerProvider(RotateSwapTransHandlerProvider);
```

So upstream registers **7** handlers: mosaic, wave, turn, ripple, rotatezoom,
rotatevanish, rotateswap.

The shipped binary matches this: `V2Link` (decomp `extrans.dll.decomp.c:13-19`,
VA `0x10001000`) makes five `FUN_*` registration calls, and
`FUN_10004f90` is the rotate registrar that, like its source counterpart,
creates three provider objects. Decisive evidence — each handler NAME string in
`.rdata` is referenced exactly once from `.text` (a 4-byte LE store of the
string's absolute VA into the `GetName` out-param), and all **seven** names are
so referenced:

| Name | `.rdata` file off | absolute VA | `.text` ref (file=VA) |
|---|---|---|---|
| `mosaic` | `0x0330a4` | `0x100330a4` | `0x100020da` |
| `ripple` | `0x0331f4` | `0x100331f4` | `0x1000397a` |
| `rotatezoom` | `0x033560` | `0x10033560` | `0x100050aa` |
| `rotatevanish` | `0x0335b8` | `0x100335b8` | `0x1000586a` |
| `rotateswap` | `0x0335d4` | `0x100335d4` | `0x10005cea` |
| `turn` | `0x033638` | `0x10033638` | `0x10006efa` |
| `wave` | `0x033714` | `0x10033714` | `0x1000768a` |

> **Disagreement with `exports/extrans.dll.md`.** That note (lines 85-94,
> 135) states the binary registers **five** names: `mosaic, ripple,
> rotatezoom, turn, wave`. This is incorrect. The binary's `.rdata` contains
> all seven name literals and each is referenced once from code; the five
> count came from counting `V2Link` calls (five) rather than providers
> (seven, since the rotate registrar spawns three). The same note (line 92)
> also mislists `rotatevanish`/`rotateswap` as *option keys* of `rotatezoom`;
> they are in fact sibling handler NAME strings (providers in their own
> right, `rotatetrans.cpp:252` and `:424`).

## The `)name` artifact (Ghidra normalization)

Ghidra's name table (and the first-pass byte scan that produced the `.md`)
shows two handler-name literals as starting with `)`: `)time` and `)turn`.
This is an off-by-one alignment artifact, **not** a real name.

The handler name strings are UTF-16LE. Immediately *before* each of the two
affected strings lies an ASCII (single-byte, not UTF-16) RTTI/mangled name
that happens to end in `)` followed by a NUL. A naive scan that starts a
UTF-16LE run one byte early picks up that trailing `) 00` as the first
character:

- `time` key: at file offset `0x3315a` the byte is `0x29` (`)`), the last
  char of the ASCII string `tTJSVariant::Type()` (NUL at `0x3315b`-1). The
  real UTF-16LE `t\x00 i\x00 m\x00 e\x00` begins at **`0x3315c`**. Ghidra
  names this `u_time_1003315c` (decomp refs at lines 2128, 2445) — note the
  `1003315c` address, which is the *correct* start, confirming the `)` is
  not part of the string.
- `turn` name: at file offset `0x33636` the byte is `0x29` (`)`), the last
  char of ASCII `...onError(const char *)`. The real UTF-16LE
  `t\x00 u\x00 r\x00 n\x00` begins at **`0x33638`** (absolute VA
  `0x10033638`).

Raw bytes:
```
0x3315a: 29 00 74 00 69 00 6d 00 65 00 00 00   ->  ')','t','i','m','e',NUL  (real "time" @ 0x3315c)
0x33636: 29 00 74 00 75 00 72 00 6e 00 00 00   ->  ')','t','u','r','n',NUL  (real "turn" @ 0x33638)
```

The other five name strings are cleanly aligned and have no such artifact.

## Per-handler detail

For each handler: (1) upstream NAME literal with `file:line`, (2) the same
string as found in the shipped binary `.rdata` (file offset + exact UTF-16LE
bytes), (3) option keys from upstream `GetValue` calls with `file:line`, and
the same keys' offsets in the binary, (4) extra upstream handlers.

### mosaic
1. **NAME (source):** `TJS_W("mosaic")` — `mosaic.cpp:408`.
2. **NAME (binary):** file offset `0x0330a4`, VA `0x100330a4`.
   Bytes: `6d 00 6f 00 73 00 61 00 69 00 63 00 00 00` = `"mosaic"`.
3. **Option keys (source):**
   - `time`   — `mosaic.cpp:439` (`options->GetValue(TJS_W("time"), &tmp)`, required)
   - `maxsize`— `mosaic.cpp:445` (optional)
   **Keys (binary):** `time` @ `0x03315c`; `maxsize` @ `0x0330dc`
   (`6d 00 61 00 78 00 73 00 69 00 7a 00 65 00`). Set matches source exactly.

### wave
1. **NAME (source):** `TJS_W("wave")` — `wave.cpp:297`.
2. **NAME (binary):** file offset `0x033714`, VA `0x10033714`.
   Bytes: `77 00 61 00 76 00 65 00 00 00` = `"wave"`.
3. **Option keys (source):**
   - `time`     — `wave.cpp:332` (required)
   - `maxh`     — `wave.cpp:338`
   - `maxomega` — `wave.cpp:341`
   - `bgcolor1`— `wave.cpp:344`
   - `bgcolor2`— `wave.cpp:347`
   - `wavetype` — `wave.cpp:350`
   **Keys (binary):** `wavetype` @ `0x033720`; `bgcolor2` @ `0x033734`;
   `bgcolor1` @ `0x033748`; `maxomega` @ `0x03375c`; `maxh` @ `0x033770`;
   plus shared `time` @ `0x03315c`. Set matches source exactly.

### ripple
1. **NAME (source):** `TJS_W("ripple")` — `ripple.cpp:1448`.
2. **NAME (binary):** file offset `0x0331f4`, VA `0x100331f4`.
   Bytes: `72 00 69 00 70 00 70 00 6c 00 65 00 00 00` = `"ripple"`.
3. **Option keys (source):**
   - `time`      — `ripple.cpp:1486` (required)
   - `centerx`   — `ripple.cpp:1492`
   - `centery`   — `ripple.cpp:1495`
   - `rwidth`    — `ripple.cpp:1503` (must be 16/32/64/128, `ripple.cpp:1506`)
   - `roundness` — `ripple.cpp:1511`
   - `speed`     — `ripple.cpp:1517`
   - `maxdrift`  — `ripple.cpp:1520`
   **Keys (binary):** `maxdrift` @ `0x0332e0`; `speed` @ `0x0332f4`;
   `roundness` @ `0x033380`; `rwidth` @ `0x0333dc`; `centery` @ `0x033484`;
   `centerx` @ `0x033494`; plus shared `time` @ `0x03315c`. Set matches source
   exactly. The binary also embeds the runtime validation strings
   (`maxdrift must be lesser than both image width and height` @ `0x033208`,
   `maxdrift cannot be nagative or larger than 127` @ `0x033280`,
   `roundness cannot be nagative or equal to 0` @ `0x033300`,
   `rwidth must be 16, 32, 64 or 128` @ `0x033398`,
   `centerx and centery cannot be out of the image` @ `0x0333f0`) — these
   confirm the key set and constraints match source verbatim.

### turn
1. **NAME (source):** `TJS_W("turn")` — `turn.cpp:526`.
2. **NAME (binary):** file offset `0x033638`, VA `0x10033638` (the `)` at
   `0x033636` is an artifact — see "The `)name` artifact" above).
   Bytes at `0x33638`: `74 00 75 00 72 00 6e 00 00 00` = `"turn"`.
3. **Option keys (source):**
   - `time`    — `turn.cpp:557` (required)
   - `bgcolor` — `turn.cpp:563`
   **Keys (binary):** `bgcolor` @ `0x0335ec`
   (`62 00 67 00 63 00 6f 00 6c 00 6f 00 72 00`); plus shared `time` @
   `0x03315c`. Set matches source exactly. (`bgcolor` is shared in `.rdata`
   by the rotate handlers that also read it — see below — but there is only
   one `bgcolor` literal at `0x0335ec`.)

### rotatezoom
1. **NAME (source):** `TJS_W("rotatezoom")` — `rotatetrans.cpp:152`.
2. **NAME (binary):** file offset `0x033560`, VA `0x10033560`.
   Bytes: `72 00 6f 00 74 00 61 00 74 00 65 00 7a 00 6f 00 6f 00 6d 00 00 00`
   = `"rotatezoom"`.
3. **Option keys (source):**
   - `time`       — `rotatetrans.cpp:188` (required)
   - `factor`     — `rotatetrans.cpp:194`
   - `accel`      — `rotatetrans.cpp:197`
   - `twist`      — `rotatetrans.cpp:200`
   - `twistaccel` — `rotatetrans.cpp:203`
   - `centerx`    — `rotatetrans.cpp:206`
   - `centery`    — `rotatetrans.cpp:209`
   **Keys (binary):** `twistaccel` @ `0x033578`; `twist` @ `0x033590`;
   `accel` @ `0x03359c`; `factor` @ `0x0335a8`; `centerx` @ `0x033494`;
   `centery` @ `0x033484`; plus shared `time` @ `0x03315c`. Set matches source
   exactly. (`centerx`/`centery` are the same `.rdata` literals ripple uses;
   `factor`/`accel`/`twist`/`twistaccel` are exclusive to the rotate family.)

## Extra upstream handlers (not in the "five", but present in source AND binary)

### rotatevanish
1. **NAME (source):** `TJS_W("rotatevanish")` — `rotatetrans.cpp:252`
   (provider `tTVPRotateVanishTransHandlerProvider`, class def
   `rotatetrans.cpp:222`, static instance `rotatetrans.cpp:316`).
2. **NAME (binary):** file offset `0x0335b8`, VA `0x100335b8`. Referenced once
   from `.text` @ `0x1000586a` (a `GetName` store), so it **is** registered in
   the shipped binary.
   Bytes: `72 00 6f 00 74 00 61 00 74 00 65 00 76 00 61 00 6e 00 69 00 73 00
   68 00 00 00` = `"rotatevanish"`.
3. **Option keys (source):** `time` (`rotatetrans.cpp:287`, required), `accel`
   (`:293`), `twist` (`:296`), `twistaccel` (`:299`), `centerx` (`:302`),
   `centery` (`:305`). Note: no `factor` (unlike rotatezoom).

### rotateswap
1. **NAME (source):** `TJS_W("rotateswap")` — `rotatetrans.cpp:424`
   (provider `tTVPRotateSwapTransHandlerProvider`, class def
   `rotatetrans.cpp:394`, handler class `tTVPRotateSwapTransHandler`
   `rotatetrans.cpp:318`, static instance `rotatetrans.cpp:475`).
2. **NAME (binary):** file offset `0x0335d4`, VA `0x100335d4`. Referenced once
   from `.text` @ `0x10005cea` — registered in the shipped binary.
   Bytes: `72 00 6f 00 74 00 61 00 74 00 65 00 73 00 77 00 61 00 70 00 00 00`
   = `"rotateswap"`.
3. **Option keys (source):** `time` (`rotatetrans.cpp:456`, required),
   `bgcolor` (`:462`), `twist` (`:465`).

Both `rotatevanish` and `rotateswap` are registered by the same
`RegisterRotateTransHandlerProvider` (`rotatetrans.cpp:483-493`) that also
registers `rotatezoom`; the binary mirrors this via `FUN_10004f90`.

## Summary table

| Handler | Source NAME `file:line` | Binary name off (VA) | Source option keys (`file:line`) | Binary key offsets |
|---|---|---|---|---|
| `mosaic` | `mosaic.cpp:408` | `0x0330a4` (`0x100330a4`) | `time` `:439`, `maxsize` `:445` | time `0x3315c`, maxsize `0x330dc` |
| `wave` | `wave.cpp:297` | `0x033714` (`0x10033714`) | `time` `:332`, `maxh` `:338`, `maxomega` `:341`, `bgcolor1` `:344`, `bgcolor2` `:347`, `wavetype` `:350` | wavetype `0x33720`, bgcolor2 `0x33734`, bgcolor1 `0x33748`, maxomega `0x3375c`, maxh `0x33770`, time `0x3315c` |
| `ripple` | `ripple.cpp:1448` | `0x0331f4` (`0x100331f4`) | `time` `:1486`, `centerx` `:1492`, `centery` `:1495`, `rwidth` `:1503`, `roundness` `:1511`, `speed` `:1517`, `maxdrift` `:1520` | maxdrift `0x332e0`, speed `0x332f4`, roundness `0x33380`, rwidth `0x333dc`, centery `0x33484`, centerx `0x33494`, time `0x3315c` |
| `turn` | `turn.cpp:526` | `0x033638` (`0x10033638`) | `time` `:557`, `bgcolor` `:563` | bgcolor `0x335ec`, time `0x3315c` |
| `rotatezoom` | `rotatetrans.cpp:152` | `0x033560` (`0x10033560`) | `time` `:188`, `factor` `:194`, `accel` `:197`, `twist` `:200`, `twistaccel` `:203`, `centerx` `:206`, `centery` `:209` | twistaccel `0x33578`, twist `0x33590`, accel `0x3359c`, factor `0x335a8`, centerx `0x33494`, centery `0x33484`, time `0x3315c` |
| `rotatevanish` *(extra)* | `rotatetrans.cpp:252` | `0x0335b8` (`0x100335b8`) | `time` `:287`, `accel` `:293`, `twist` `:296`, `twistaccel` `:299`, `centerx` `:302`, `centery` `:305` | (shares accel/twist/twistaccel/centerx/centery/time literals above) |
| `rotateswap` *(extra)* | `rotatetrans.cpp:424` | `0x0335d4` (`0x100335d4`) | `time` `:456`, `bgcolor` `:462`, `twist` `:465` | (shares bgcolor/twist/time literals above) |

**Source ↔ binary agreement:** for every handler, the NAME string and the
option-key set are identical between upstream source and the shipped binary.
No key is present in the binary that source does not read, and no key is read
by source that is absent from the binary. The binary registers all **seven**
handlers (the five commonly listed plus `rotatevanish` and `rotateswap`).
