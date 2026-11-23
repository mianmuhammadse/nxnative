# Iconoclasts (`0100BC60099FE000`)

Update 1.15p (v65536; the base v0 is packed with it), SDK 5.3.0, its own C++ engine on NVN. Modules `main` and `sdk`. The shaders are NVN binaries inside `main`'s data and the 106 MB `Assets.dat` at the RomFS root; the NSP script's shader split finds 182 of them and all 182 build for the Mac.

On the Mac (current): a 60 s headless boot with no input (about 3,300 frames, p50 16.67 ms, no `[RED]`), then a windowed play session by the user (title, a save, a level; sound and textures looked right; about 7,200 frames, p50 16.67 ms, footprint 960-964 MiB, no `[RED]`, no memory-pressure event). The one 137 ms frame and the 114 dropped frames in the last window are the window being closed. iPhone 15 (A16, Cartouche, EasySMX M15 as player 1): a 4.75 min run (17,100 frames) and a 19.8 min run (71,400 frames), both current; results under [iPhone 15](#iphone-15).

## What it needs

The survey's provisional profile held as generated. No host change, rewriter cut or SDK cut was needed.

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | every module `main` imports from |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource: no virtual address memory, as on Horizon |
| `mem_reserve 0xe0000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | `main` sets a heap of at most the pool - 64 MB - 582 MB of bss: a 2,816 MB heap region, of which 2.58 GB is taken on the Mac |
| `romfs_marker Assets.dat`, `zip_store .dat:.png:.metallib` | `game.tsv` | the big root file and the stored types for the IPA |

The window origin is lower left (the host's GL-style flip) and the picture is 1920x1080.

## Shaders

182 shaders (route B, translated Switch originals) from [`shader_map.tsv`](shader_map.tsv); the game draws with a handful at a time (4 render pipelines built in the 7,200-frame Mac session, 5 over the phone's 71,400 frames). [`pso_manifest.tsv`](pso_manifest.tsv) holds those 5 (the `S` / `R` rows of the Mac session and the phone's `pso_user.tsv`); it is a short seed, not a route over the whole game.

## Memory

On the Mac: resident 963 MB and Metal allocations 409 MB at frame 6,000; footprint flat at about 960 MiB. On the iPhone 15 the footprint is 543-704 MiB over 20 minutes with no memory-pressure event, so the 582 MB of bss and the 2.58 GB heap region are not a problem there.

## iPhone 15

IPA built from this profile (`--platform ios -p v65536 --show-fps`), launched from Cartouche, played with an EasySMX M15 (player 1) through level play and saves. Both runs: no `[RED]`, no crash.

| Run | Frames | p50 frame time | p95 (worst window) | Dropped | GPU p50 | Footprint | Thermal | Memory pressure |
|---|---|---|---|---|---|---|---|---|
| 4.75 min | 17,100 | 16.63-16.64 ms | 17.3 ms | 27 | 5.2 ms mean (7.6 max window) | 646 -> 543 MiB | 0 | 0 events, 0 trims |
| 19.8 min | 71,400 | 16.64 ms mean (16.66 max) | 17.09 ms | 5 | 5.3 ms mean (6.1 max window) | 687 -> 663 MiB, peak 704 | 0 in all 475 samples | 0 events, 0 trims |

- Frames over 40 ms: frame 300 (48 ms) and 600 (43 ms) at the start, and 212 ms at frame 71,100, which is the app going to the background (`oe focus state` 2 -> 3, then SIGTERM). Audio: 0 underruns in nearly every window; one window of the first run had 2 (7.7 ms zero-padded).
- The long run built one pipeline not in the seed list: a vertex / pixel pair (`786d33fea65f46c7` / `01183cb244d06148`, shader map index 83, `vs_other.bin` / `ps_other.bin`) and its render pipeline. It cost no frame over 40 ms in play (hybrid pipelines are off on iOS: the draw waited for the build). It is now in [`pso_manifest.tsv`](pso_manifest.tsv) (11 rows).
- Touch gamepad alone (no controller connected, Cartouche, about 1 min, 3,600 frames): played through levels with a save commit (frame ~1,700) and the residency set growing to 338 MB; 0 `[RED]`, p50 16.65 ms, p95 17.8 ms, GPU p50 5.35 ms, footprint 653 -> 664 MiB, thermal 0. 69 of its 70 dropped frames are in the last window, the app going to the background. How the layout felt was not judged from the log.
- Controllers: each controller is a player of its own. A controller connected before the game (a DualShock 4 left paired) becomes player 1 and **hides the touch gamepad**; a second pad (the EasySMX) was player 2, which the game ignores for play and answered with the controller-support applet about 1,250 times (the host's applet only closes). With only the EasySMX connected it is player 1 and the game plays normally. The touch gamepad alone (no controller) is untested.

## Mac routes

The build and work folders are `<NSP folder>/0100BC60099FE000_build` and `<NSP folder>/0100BC60099FE000_survey`. Run the NSP script with `--platform mac`; it prints the run command (`DM_WINDOW=1 DM_AUDIO=1` for a window with sound).

## Found on this game

Nothing yet; no general finding came from this game.

## Open

- iPhone 15: a second controller that is not player 1 gets no way to become player 1 (the host's controller-support applet only closes: general host limitation, not specific to this game).
- About 20 min were played on the phone and a short route on the Mac: later levels, bosses and effects that use shaders those sessions did not draw are not covered; the prewarm list holds the 5 pipelines the sessions built.
- Saves, the other languages (ja, fr, de, es) and the base v0 package were not checked.
