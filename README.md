## About this gamescope fork: gamescope-lanczos-downscaling

Gamescope is the the micro-compositor formerly known as steamcompmgr. This fork adds a high-quality Linux-side "supersampling" path intended for use cases like running a 4K (or higher) internal render target on a 1080p / 1440p display. I made this for myself and partially for Ross Scott from Accursed Farms. Supersampling is the way to go!

Compared to the FSR/NIS/Bicubic upscalers that upstream gamescope ships, these are **downscalers** and post-process passes — they are intended to run on frames whose internal resolution is *larger* than the output. And yes, you can't downscale using FSR on normal gamescope. Normal gamescope is wired to only use FSR to upscale. If you want to use FSR as a downscaler, consider patching: https://github.com/ValveSoftware/gamescope/pull/702. 

DISCLAIMER: **This work was partially AI-Assisted.** Meaning, I feed an AI **ACTUAL human work** as the basis for the work in this fork. **The AI did not hallucinate this code out of nowhere, it only implemented the human code I gave it.** Everything has it's proper atribution and source. See credits below for the people that actually deserve the credit for coming up with all this. There is a big difference between AI GENERATED and AI ASSISTED. I honestly believe that the work done in this fork consists in an ethical use of AI. Please support the people that actually figured this out in the credits below.

## New features:

* **`-F lanczos`** — EWA Lanczos4-sharpest (polar `jinc * jinc`) downscaler with anti-ringing, adapted from [libplacebo](https://code.videolan.org/videolan/libplacebo)'s `pl_filter_ewa_lanczos4sharpest`. Handles integer and fractional downscale ratios (e.g. 4K→1080p, 1440p→1080p, 8K→1080p). The shader also handles the `F < 1` upscale case via the same `cfg.blur *= max(1/ratio, 1)` scaling libplacebo uses, so the same kernel is reused in both directions.
* **`-F lanczos:bilateral`** — Optional joint / cross-bilateral post-pass that runs immediately after the lanczos downscale. Uses the tight range-sigma formulation from Shiandow / igv's KrigBilateral (`SIGMA_R = 0.05`, `RADIUS = 2`): suppresses sub-5% block-coding noise, quantisation artefacts and residual banding while leaving any edge with ≥5% contrast completely untouched. When a high-resolution guide (the pre-downscale frame) is bound on slot 1, it runs as a true joint-bilateral, pulling range weights from the hi-res luma rather than the already-downscaled one.
* **`-F lanczos:hdeband`** — Optional homogeneous-run debander, ported from an3223's `hdeband.glsl` mpv hook. Scans 8 directions per pixel looking for near-constant "runs" and averages them with a Gaussian intensity weight — specifically targets the slow luma gradients that re-band after a sharp downscale on HDR-ish content.
* Passes can be chained with `:` — e.g. `-F lanczos:bilateral,hdeband` runs both post-passes after the lanczos downscale, in that order.

## Fork-specific fixes:

* **KDE / XDP-screen-sharing virtual-output sizing.** When running on KDE under the Wayland backend, the compositor could call `libdecor_frame_configure` with an XDP (screen-share) virtual output's logical size instead of the physical monitor, shrinking gamescope below the user's requested `-W` / `-H`. The fork clamps the configured size to the user's `-W` / `-H` preferences in that handler so the Lanczos path doesn't silently downscale against a smaller-than-expected output. (`src/Backends/WaylandBackend.cpp`.)
* `useLanczosLayer0` now forces the full composite path in the Wayland backend the same way the existing `useFSRLayer0` / `useNISLayer0` flags do, so the Lanczos kernel actually runs instead of being bypassed by the direct-scanout path.

The internal plumbing (new command-buffer submission split for the lanczos kernel on NVIDIA, `clearState()` fix for stale LUT pointers, new shader entries in `meson.build`, etc.) lives in `src/rendervulkan.cpp` / `src/rendervulkan.hpp` and the three new `.comp` files under `src/shaders/`.

### Credits for the fork

The downscale path was built on top of work by:

* [**Ruan E. Formigoni**](https://github.com/ValveSoftware/gamescope/pull/740) — for the upstream Gamescope PR #740 (bicubic downscaling) provided the base of this fork's `-F lanczos` implementation.
* [**Niklas Haas and the libplacebo contributors**](https://github.com/haasn/libplacebo) — for the original EWA Lanczos implementation.
* [**Shiandow**](https://github.com/Shiandow/MPDN_Extensions) and [**igv**](https://gist.github.com/igv/a015fc885d5c22e6891820ad89555637) — for the original KrigBilateral.glsl the bilateral denoiser is derived from.
* [**an3223**](https://github.com/AN3223/dotfiles/tree/master) — for the original `hdeband.glsl` the debander is ported from.

And also:

* **Valve and the Valve Developers** — For creating Gamescope, this amazing thing.

## Licensing

The gamescope project as a whole is distributed under the **BSD 2-Clause** license (see `LICENSE`).

The three shader files added by this fork are derivative works of LGPL-licensed upstream code and are therefore licensed under the LGPL (the gamescope *whole* remains BSD-2-Clause; these files specifically carry a different licence):

| File | Upstream | License |
| --- | --- | --- |
| `src/shaders/cs_ewa_lanczos.comp` | libplacebo (Niklas Haas) | **LGPL-2.1-or-later** |
| `src/shaders/cs_bilateral_denoiser.comp` | KrigBilateral.glsl (Shiandow / igv) | **LGPL-3.0-or-later** |
| `src/shaders/cs_hdeband.comp` | hdeband.glsl (an3223) | **LGPL-2.1-or-later** |

Each of those files carries an SPDX header and the attribution / standard LGPL boilerplate at the top. The full license texts are bundled in this repository:

* `COPYING.LESSER.v2.1` — full GNU LGPL v2.1 text (applies to `cs_ewa_lanczos.comp` and `cs_hdeband.comp`)
* `COPYING.LESSER.v3` — full GNU LGPL v3.0 text (applies to `cs_bilateral_denoiser.comp`)
* `COPYING.GPL.v3` — full GNU GPL v3.0 text (referenced by the LGPLv3)

Rationale: the Lanczos / bilateral / hdeband math itself is public-domain signal-processing, but the specific *expression* of that math in those three shaders was adapted from existing LGPL-licensed code (libplacebo's `filters.c`, Shiandow / igv's `KrigBilateral.glsl`, and an3223's `hdeband.glsl`). Translating code between languages or APIs does not strip its copyright — so the translated files carry the upstream LGPL licence, while the rest of gamescope stays BSD-2-Clause.

## Building

```
git submodule update --init
meson setup build/
ninja -C build/
build/src/gamescope -- <game>
```

Install with:

```
meson install -C build/ --skip-subprojects
```

## Keyboard shortcuts

* **Super + F** : Toggle fullscreen
* **Super + N** : Toggle nearest neighbour filtering
* **Super + U** : Toggle FSR upscaling
* **Super + Y** : Toggle NIS upscaling
* **Super + I** : Increase FSR sharpness by 1
* **Super + O** : Decrease FSR sharpness by 1
* **Super + S** : Take screenshot (currently goes to `/tmp/gamescope_$DATE.png`)
* **Super + G** : Toggle keyboard grab

## Examples

On any X11 or Wayland desktop, you can set the Steam launch arguments of your game as follows:

```sh
# Upscale a 720p game to 1440p with integer scaling
gamescope -h 720 -H 1440 -S integer -- %command%

# Limit a vsynced game to 30 FPS
gamescope -r 30 -- %command%

# Run the game at 1080p, but scale output to a fullscreen 3440×1440 pillarboxed ultrawide window
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%

# Render internally at 4K, output to a 1080p display, with EWA Lanczos4 downscaling
gamescope -w 3840 -h 2160 -W 1920 -H 1080 -F lanczos -f -- %command%

# Same, plus a bilateral denoise pass on the downscale (cleans up compression noise
# without softening edges)
gamescope -w 3840 -h 2160 -W 1920 -H 1080 -F lanczos:bilateral -f -- %command%

# Full chain: downscale + bilateral + debander (good for HDR-ish content that
# re-bands after the downscale)
gamescope -w 3840 -h 2160 -W 1920 -H 1080 -F lanczos:bilateral,hdeband -f -- %command%
```

## Options

Same as normal gamescope. See `gamescope --help` for a full list of options.

* `-W`, `-H`: set the resolution used by gamescope. Resizing the gamescope window will update these settings. Ignored in embedded mode. If `-H` is specified but `-W` isn't, a 16:9 aspect ratio is assumed. Defaults to 1280×720.
* `-w`, `-h`: set the resolution used by the game. If `-h` is specified but `-w` isn't, a 16:9 aspect ratio is assumed. Defaults to the values specified in `-W` and `-H`.
* `-r`: set a frame-rate limit for the game. Specified in frames per second. Defaults to unlimited.
* `-o`: set a frame-rate limit for the game when unfocused. Specified in frames per second. Defaults to unlimited.
* `-F fsr`: use AMD FidelityFX™ Super Resolution 1.0 for upscaling.
* `-F nis`: use NVIDIA Image Scaling v1.0.3 for upscaling.
* `-F lanczos`: use EWA Lanczos4-sharpest (polar `jinc*jinc`) for downscaling, with built-in anti-ringing. Handles integer and fractional ratios (e.g. 4K→1080p, 1440p→1080p, 8K→1080p) and is intended as a Linux equivalent to Windows "supersampling" (DSR / DLDSR / VSR) modes.
* `-F lanczos:bilateral`: as above, plus a joint / cross-bilateral denoise post-pass (edge-preserving sub-5%-luma smoother, adapted from Shiandow / igv's KrigBilateral).
* `-F lanczos:hdeband`: as above, plus a homogeneous-run debander (ported from an3223's `hdeband.glsl`).
* `-F lanczos:bilateral,hdeband`: run both post-passes, in order.
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `-b`: create a border-less window.
* `-f`: create a full-screen window.

## Reshade support

Gamescope supports a subset of Reshade effects/shaders using the `--reshade-effect [path]` and `--reshade-technique-idx [idx]` command line parameters.

This provides an easy way to do shader effects (ie. CRT shader, film grain, debugging HDR with histograms, etc) on top of whatever is being displayed in Gamescope without having to hook into the underlying process.

Uniform/shader options can be modified programmatically via the `gamescope-reshade` wayland interface. Otherwise, they will just use their initializer values.

Using Reshade effects will increase latency as there will be work performed on the general gfx + compute queue as opposed to only using the realtime async compute queue which can run in tandem with the game's gfx work.

Using Reshade effects is **highly discouraged** for doing simple transformations which can be achieved with LUTs/CTMs which are possible to do in the DC (Display Core) on AMDGPU at scanout time, or with the current regular async compute composite path.
The looks system where you can specify your own 3D LUTs would be a better alternative for such transformations.

Pull requests for improving Reshade compatibility support are appreciated.
