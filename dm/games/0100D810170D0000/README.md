# Nine Sols (`0100D810170D0000`)

Update 1.3 (`v196608`) over the base release. Unity 2022.3.33f1 IL2CPP with NVN, Wwise and FMOD. Loads `main`, `subsdk0` (NintendoSDK_movie) and `sdk` (18.3.2). The profile started as the NSP script's provisional one, guessed from the survey, and was brought up on the Mac and an iPhone 15.

## Memory

The NPDM declares 8 MiB of system resource, and `main` sets a heap. The profile keeps the 5 GB block of every iPhone game with virtual address memory (4.375 GB alias region, 256 MB stacks) and the hardware application pool.

With the Mac's 64 GB alias region, `nn::mem::StandardAllocator::Initialize` (returning to `main+0xbe08c0`) reserves 3.14 GB at alias +0.06 GB and maps well under 1 GB. Unity's own 16, 4 and 2 MB chunks are placed above it and reach +5.25 GB in a long Mac run. With the iPhone's 4.375 GB limit they run out of address space and an allocation returns null: `sig=11 addr=0 pc=main+0x795b30`.

- **iPhone 15 (current):** the first build crashed that way at about 160 s (frame 9,354), right after the intro movie ended; the log's last `[mem]` line was `alias region mapped 1.00 GB`.
- **Mac (current):** `DM_ALIAS_SIZE=0x118000000` reproduces it; the last `MapPhysicalMemory` calls sit at `0x43fc00000`, under the region's top at `0x440000000`.
- **Fix:** `stdalloc_cap 0x60000000` (1.5 GiB), the cap Persona 5 Tactica ([note](../010087701B092000/README.md#memory)) uses for the same Unity layout. The check Mac run (iPhone alias limit, 16 minutes, frame 59,400) reached +3.76 GB with no crash, and the iPhone run (981 s, frame 57,600) +4.00 GB of 4.38 GB.

The NSP script's address region check measured only the first 15 s (+4.06 GB, within the region) and did not see the growth after the intro movie. Margin on the phone is about 0.4 GB; the cap is not tuned beyond that. The mechanism is the one in [the Mac's alias region is the Switch's 64 GB](../../../docs/reference/memory.md#the-macs-alias-region-is-the-switchs-64-gb).

## Files and shaders

- RomFS 5.01 GB, 1,017 files in the survey: UnityFS bundles (two in the survey's header census, 0.54 GB; Addressables bundles under `Data/StreamingAssets/aa/Switch`), serialized `.assets`, Wwise `.bnk` and `.wem`, one FMOD bank.
- The shader split finds 16,996 shaders, 11 of them GLSLC outputs in `main` (the Unity player's own). All 16,996 built for macOS and iOS with the chain `b9626bbdb2a0` (Xcode 27.2's Metal compiler).
- The game sets depth mode to zero in the heap check's boot, so the profile has `depth_mode zero`.
- No game-specific rewriter patches: `patches.py` does not exist for this build (`main=976ab66e…`).

## Mac validation

A 16-minute Mac run with the iPhone alias limit reached the title, the intro movie and into the game: no crash, no `[RED]` line, frame time 16.7 ms p50 (60 fps), one 812 ms stall around the intro movie. Memory at frame 58,200: resident 2,250 MB (mostly file-backed guest memory), GPU allocated 817 MB. Not covered: audio (the check run had no `DM_AUDIO=1`), later chapters and boss fights, other languages.

## iPhone 15 (current)

An iPhone 15 (A16) run in LiveContainer: 981 s, frame 57,600, no crash, no `[RED]` line; a DualShock 4 and an EasySMX pad were recognised.

- **Frame rate:** 16.6 ms p50 in nearly every 300-frame window (60 fps); one window at frame 39,900 averaged 17.4 ms p50 with 33 dropped frames.
- **Memory:** `phys_footprint` peaked at 577 MiB, headroom to the system limit never below 3.5 GB. This game does not come near the iPhone's limit.
- **GPU and heat:** GPU time was about 3.4 ms in the menus and about 14 ms (85% of the 16.7 ms budget) in the game scenes; CPU energy 0.3 to 0.64 W. Thermal state 0 for about 4 minutes, 1 until about 8 minutes, then 2 until the end; brightness fell from 0.26 to 0.21. The player felt the phone warm near the end. The swap chain is 1920x1080; whether the app ran docked or handheld, and whether handheld runs cooler, is not measured.
- **Hitches:** a 7,062 ms frame at 28 s (frame 1,138, GPU 6 ms, submit 0.5 ms), just before the first movie decoder starts: a stall in the guest, cause not examined. Frames of 873, 628, 397 and 424 ms followed at 146 s, 729 s and 981 s. 21 of the 155 long frames compiled new pipelines (no pipeline archive in this build: `--gpu none`).

## Frame rate

The game runs at 60 fps unmodified; the profile has no `fps_target` or `fps_cap`.

## Running

Point [`scripts/build_ipa_from_nsp.py`](../../../scripts/build_ipa_from_nsp.py) at the NSP folder and the local keys file. Use `--platform mac` for the Mac host and its run command. Add `DM_ALIAS_SIZE=0x118000000` to the run command to reproduce the iPhone's address space on the Mac, and `DM_AUDIO=1` for sound. The default iOS output is the unsigned `NineSols.ipa` (2.1 GB compressed, 6.7 GB with RomFS and the iOS shader image).

## Open

- **Prewarm list:** `pso_manifest.tsv` is empty. The iPhone run recorded about 100 pipelines in its `pso_user.tsv`; a list recorded on the Mac would remove the first-use compile hitches.
- **Address-space margin:** about 0.4 GB on the phone after the cap; a longer session or a later chapter may need `stdalloc_cap` lowered or a larger block (the repo's proven phone limit is 5 GB; 6 GB reserves succeed without the extended entitlement, not tested with Metal here).
- **7 s stall** at the first movie (frame 1,138): cause not examined.
- **Heat:** whether handheld mode runs cooler is not measured.
- **Newer builds:** the web reports newer Switch updates and an engine update on PC (unconfirmed); only v196608 was tested. A newer build would pack from a derived profile without a build-keyed check.
- **Audio** is not verified on the Mac; the iPhone log showed the audio session starting and no underruns.
