# Until Then (`010019C023004000`)

Updates 1.0.7 (`v458752`) and 1.0.9 (`v589824`, current) over the base release, SDK 19.3.9; both share the `sdk` build. A Godot game on its Vulkan renderer (`RenderingDevice`, the mobile forward renderer, canvas and particle shaders), run through the host's Vulkan layer. Loads `main` and `sdk`; `subsdk0` (NintendoSDK_OpenGL) and `subsdk1` (NintendoSDK_Vulkan) are replaced by the layer. The profile started as the NSP script's provisional one and was brought up on the Mac and an iPhone 15 (A16).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | the game makes a 1280x720 layer; docked, the Vulkan surface was 1920x1080 and the picture filled only the top-left. Handheld gives 720p and touch |
| `modules main sdk` | `game.tsv` | `subsdk0` and `subsdk1` are not loaded |
| 4 GB block: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false and `main` sets a heap of the pool minus 64 MB (a 3,328 MB heap region) |

## Shaders

- **Built at run time, cached in the RomFS.** Godot builds its pipelines while the game runs, so a capture by playing (`DM_VK_CAPTURE`) holds only what was played: 864 shaders after two long plays, and an iPhone run still hit missing effects. The RomFS holds Godot's own cache, though: `shader_cache/<family>/<hash>/<hash>.vulkan.cache` (GDSC v3, GSBD) has every variant as zstd blocks of SMOL-V, which decode to SPIR-V with the same keys as the host's (first 8 bytes of the SHA-256). 3,359 distinct modules in 24 families: scene forward mobile 2,279, canvas 926, particles 27, the rest small. All 864 captured and the 10 missing ones from the iPhone run were in it. See [Godot's shader cache holds the SPIR-V of every variant](../../../docs/reference/shaders.md#godots-shader-cache-holds-the-spir-v-of-every-variant).
- **The packer reads the cache.** `vk_shaders.py` takes the cache files as a shader input (`dm/tools/smolv.py` decodes the SMOL-V), so the NSP script needs no capture run and the profile has no `vk_capture.tsv`. The first build of 1.0.7 used a capture folder made from the cache by a scratch script; the profile then listed its 3,359 keys.
- **Seven libraries.** One link of 3,359 shaders used 28 GB; `vk_shaders.py` links 500 to a library (`DM_VK_LIB_SHADERS`) and the layer loads them all: [One library for thousands of Vulkan shaders exhausts memory at the link](../../../docs/reference/shaders.md#one-library-for-thousands-of-vulkan-shaders-exhausts-memory-at-the-link). The step took 5:32 with each `metallib` near 1 GB.

## iPhone 15 (A16)

- **No BC textures.** The first iPhone run aborted one frame after the first present: the game's first BC3 image reached `MTLTextureDescriptor` with pixel format 134. The Vulkan layer now stores BC images as ASTC 4x4 or decompressed ([rendering](../../../docs/reference/rendering.md#gpus-without-bc-textures-are-refused-at-start), `t_vk_bc`); `DM_TEST_NO_BC=1` reproduces the path on a Mac.
- **Current:** a run from a new game, about 102,300 frames (over 20 minutes), no crash. `7 shader libraries, 3359 functions` at start, no missing-shader line, every frame `skipped 0`. `phys_footprint` peaked at 1.02 GB. Sound works. Not measured: frame time and heat.
- **1.0.9 (current):** a run with another player's save from chapter 6 (about 10,200 frames, no crash): `7 shader libraries, 3364 functions` (the cache holds 3,364 modules, five more than 1.0.7's), no missing-shader line, peak footprint 0.93 GB, audio underruns only in the first 2,000 callbacks (70 and 67). After Start the game compiles about 70,000 pipelines (64 at frame 1,200, 25,300 at 3,000, 70,677 at 6,600; no pipeline cache in this build, `Unsupported internal pipelines cache`), which is the wait before a scene appears; the log carries 70,677 `!new_pipelines_cache_size` errors, one per compile.

## Saves

A save from another player's emulator (Godot `user://` files: `savegame.res`, `savegame.bak.res`, the per-chapter `store/` folders; his `settings.res` has PC graphics options and `input.res` keyboard bindings, which are left out) loads on the iPhone and continues through chapter 6 on the base game: `Loaded store diffs from 6.1a`, story scenes 6/1b to 6/3, about 21,000 frames, no error. Save files go in the app's `Documents/Data/Application/<id>/Documents/save` (the files app, with the game closed). The game rewrites `savegame.res` when a DLC scene is opened, so after a failed DLC scene Continue points at it and the game quits at once; restoring the original file fixes that.

## DLC: Afterimages (current: incomplete)

The DLC (`010019C023005002`, v0, 0.62 GB: one data NCA of 651 MB; the nsz check `[VERIFIED]` shows the dump is whole) mounts (`aoc2`, 75,217 entries) but its scenes fail: opening Homecoming (`scenes/_dlc_rdvt/ch1sc1a`) logs `No loader found for resource: .../ch1sc1a/assets/logo.png`, and chapter 2 (`Ch2Sc1a`) lacks `SmoothOpaqueSimple.gdshader` and the bike and motorcycle textures under `chapter_global/envi/two_wheelers`. The game then logs `Tried to change scene ... failed miserably` and `Until then...` and quits by itself. The files, and their imported copies (`.godot/imported/<name>-<md5 of the res path>.ctex`), are in neither the DLC image nor the base RomFS, on 1.0.7 or 1.0.9. 1.0.9 adds 41 files and changes 43; one is `scenes/_dlc_rdvt/ch2sc9b/scripts/Precache.gd`, none is a missing asset. Another player's emulator runs Homecoming. Not known: whether a DLC update above v0 carries them, or whether the Switch release has the same gap.

## Open

- **Subpasses:** three `[RED]` lines, once each: a render pass with two subpasses, only the first is used, and a pipeline uses subpass 1 (the next subpass should draw into the first one's attachments). No visible fault is attributed to it yet.
- **Audio (resolved, not a fault):** the one `[RED] AudioOut sample rate 44100` line is the game's probe: it opens a 44.1 kHz output, which fails with `0x699`, closes it, and opens the default (48 kHz) output at once (`audio #1`; `audio #2` is a second stream from `nro0`). Sound plays correctly on the iPhone (player's report, no glitches); the log shows 0 underruns. The audio session also failed once after the app was backgrounded and recovered.
- **Swapchain size:** `surf_ext()` ignores the layer's size; `op_mode handheld` hides it. A fix would use `dm_vi_layer_size()`.
- **Vulkan GL** still lacks the no-BC conversion.
- **Pipeline compile after Start:** about 70,000 pipelines are built each time before a scene shows (a minute or two on the iPhone). A prewarm list or a pipeline cache would remove it; not examined.
- **DLC:** see above; needs a DLC version above v0, or confirmation that the Switch release lacks the files.
- **Mac validation:** the Mac host plays it (1.0.7's shaders were captured there), but no timing run is recorded.
