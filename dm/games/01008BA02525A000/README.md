# Dispatch (`01008BA02525A000`)

Update v589824 (display version 1.0.17878; the base v0 is packed with it), SDK 20.5.15, Unreal Engine (version not read from the binary; the paks are version 11) with FMOD, on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The 19 paks keep the shaders in shader code archives (3 paks selected, 4 archives, 5,560 shaders decoded by the game's own booted decoder, as for OCTOPATH TRAVELER 0); the shader split finds 5,470 and all 5,470 build for the Mac. The 2.8 MB DLC is a licence: the host reports it as owned and mounts it as `aoc0` (`aoc/3.img`, 6 entries).

On the Mac (current): a 150 s headless boot with no input (4,200 frames, no `[RED]`), then about 10 minutes of windowed play by the user from the title into the game (about 19,000 frames, a save written, one `[RED]`: the first audio unit start failed and the retry worked). The picture, sound and controls were looked at by the user; no per-scene check was made.

## What it needs

The survey's provisional profile held as generated. No host change, rewriter cut or SDK cut was needed for the Mac boot and play.

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | every module `main` imports from; `subsdk0` is the SDK's movie library |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource and `main` sets a heap: a 5 GB reserve with a large alias region |
| `shader_groups ShaderArchive-Global-=global ShaderArchive-=game` | `game.tsv` | the decoded shader code archives: the engine's global shaders and the game's material shaders |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 in the NSP script's check boot |
| `romfs_marker Dispatch/Content/Paks/pakchunk17-Switch.pak`, `zip_store .bank:.pak:.ttf:.metallib` | `game.tsv` | the packed RomFS check and the stored types for the IPA |

The game sets present interval 1 -> 2 itself, so it runs at 30 fps (frame time p50 33.4 ms, p95 about 37-39 ms, GPU p50 6-9 ms on the Mac). No `fps_target` is set: the Unreal fixed frame rate is left as the game sets it.

## Shaders

5,470 shaders from [`shader_map.tsv`](shader_map.tsv). [`pso_manifest.tsv`](pso_manifest.tsv) holds what two Mac sessions built on the spot (1,614 `S`, 86 `R`, 10 `C` rows, recorded on v589824 with the boot's own list loaded in the second session), so a first launch prewarms them: with an empty list the first play had pipeline stalls of up to 1.6 s (frame 4 at 1.3 s, frame 429 at 1.6 s with 28 pipelines). The list with the manifest loaded has not been re-measured yet.

## Memory

On the Mac: resident 1,710 MB and GPU allocated 1,132 MB at frame 300; resident 2,263 MB and GPU 1,376 MB at frame 19,200; process footprint up to 2.7 GB. Still rising slowly at the end of the session (not measured longer).

The NSP script's address-region check (first 15 s in `nnMain`) reaches alias +62.06 GB in this Mac-only build's 64 GB alias region, far past the iPhone profile's 4.38 GB layout. No iPhone build was made: an iPhone 15 run is open, and the memory above is a Mac number.

## Mac routes

The build and work folders are `<NSP folder>/01008BA02525A000_build` and `<NSP folder>/01008BA02525A000_survey`. Run the NSP script with `--platform mac`; it prints the run command (`DM_WINDOW=1 DM_AUDIO=1` for a window with sound).

## Found on this game

Nothing yet; no general finding came from this game.

## Open

- iPhone 15: not built or run. The alias region the game asks for (+62 GB on the Mac), a footprint of 2.3-2.7 GB on the Mac and a 7 GB RomFS make it unproven; the iOS shader build is also not done.
- Frames 6,000-8,700 of the play session show p50 67 ms. The log shows the window covered and display-link stalls of 16-120 s in that stretch (frame times then measure nothing), the GPU at 8.7 ms and no pipeline work, so it is most likely the covered window; a scene that is genuinely slow there is not ruled out.
- About 10 minutes were played: later chapters, effects that use shaders those sessions did not draw, the other languages (ja, fr, de, es-419, es, ru, ko, zh, pt-BR), saves across a restart and the base v0 package were not checked. The audio unit start failure at boot (status 2003329396, retry works) is not looked at.
- The Unreal version is not read from the binary.
