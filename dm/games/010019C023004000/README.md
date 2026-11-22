# Until Then (`010019C023004000`)

Update 1.0.7 (`v458752`) over the base release, SDK 19.3.9. A Godot game on its Vulkan renderer (`RenderingDevice`, the mobile forward renderer, canvas and particle shaders), run through the host's Vulkan layer. Loads `main` and `sdk`; `subsdk0` (NintendoSDK_OpenGL) and `subsdk1` (NintendoSDK_Vulkan) are replaced by the layer. The profile started as the NSP script's provisional one and was brought up on the Mac and an iPhone 15 (A16).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | the game makes a 1280x720 layer; docked, the Vulkan surface was 1920x1080 and the picture filled only the top-left. Handheld gives 720p and touch |
| `vk_capture.tsv` | profile | the keys of the 3,359 SPIR-V modules the game uses; see [Shaders](#shaders) |
| `modules main sdk` | `game.tsv` | `subsdk0` and `subsdk1` are not loaded |
| 4 GB block: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false and `main` sets a heap of the pool minus 64 MB (a 3,328 MB heap region) |

## Shaders

- **Built at run time.** Godot builds its pipelines while the game runs, so the first NSP-script build captured SPIR-V by booting the game on the Mac (`DM_VK_CAPTURE`). Each new scene found more shaders: 864 after two long plays, and an iPhone run still hit missing effects.
- **The RomFS has all of them.** `shader_cache/<family>/<hash>/<hash>.vulkan.cache` (GDSC v3, GSBD) holds every variant as zstd blocks of SMOL-V, which decode to SPIR-V; the keys match the host's (first 8 bytes of the SHA-256). 3,359 distinct modules in 24 families: scene forward mobile 2,279, canvas 926, particles 27, the rest small. All 864 captured and the 10 missing ones from the iPhone run were in it. See [Godot's shader cache holds the SPIR-V of every variant](../../../docs/reference/shaders.md#godots-shader-cache-holds-the-spir-v-of-every-variant).
- **Packing today is by hand:** the modules were decoded to `vk_capture/<key>.spv` with a scratch script and merged into the version's capture folder; `vk_capture.tsv` lists the 3,359 keys. A packer step that does this from the RomFS is not written (open).
- **Seven libraries.** One link of 3,359 shaders used 28 GB; `vk_shaders.py` links 500 to a library (`DM_VK_LIB_SHADERS`) and the layer loads them all: [One library for thousands of Vulkan shaders exhausts memory at the link](../../../docs/reference/shaders.md#one-library-for-thousands-of-vulkan-shaders-exhausts-memory-at-the-link). The step took 5:32 with each `metallib` near 1 GB.

## iPhone 15 (A16)

- **No BC textures.** The first iPhone run aborted one frame after the first present: the game's first BC3 image reached `MTLTextureDescriptor` with pixel format 134. The Vulkan layer now stores BC images as ASTC 4x4 or decompressed ([rendering](../../../docs/reference/rendering.md#gpus-without-bc-textures-are-refused-at-start), `t_vk_bc`); `DM_TEST_NO_BC=1` reproduces the path on a Mac.
- **Current:** a run from a new game, about 102,300 frames (over 20 minutes), no crash. `7 shader libraries, 3359 functions` at start, no missing-shader line, every frame `skipped 0`. `phys_footprint` peaked at 1.02 GB. Not measured: frame time and heat.

## Open

- **Subpasses:** three `[RED]` lines, once each: a render pass with two subpasses, only the first is used, and a pipeline uses subpass 1 (the next subpass should draw into the first one's attachments). No visible fault is attributed to it yet.
- **Audio:** `[RED] AudioOut sample rate 44100: audout takes 48 kHz only` (the open fails with `0x699`); sound is not verified. The audio session also failed once after the app was backgrounded and recovered.
- **Swapchain size:** `surf_ext()` ignores the layer's size; `op_mode handheld` hides it. A fix would use `dm_vi_layer_size()`.
- **Vulkan GL** still lacks the no-BC conversion.
- **Packer step** for the Godot shader cache (above).
- **Imported saves** from the Mac (`savegame.res`, `settings.res`) were not confirmed to load on the phone; the last run started a new game.
- **Mac validation:** the Mac host plays it (the shaders were captured there), but no timing run is recorded.
