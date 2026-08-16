# gamescope on NVIDIA (Blackwell / RTX 5090, driver 610)

Status and notes for running gamescope on the NVIDIA proprietary driver, from
testing on an RTX 5090 (Blackwell, sm120), driver 610.43.02, kernel 6.19-rc,
under COSMIC (Wayland).

## What works today

- **Builds cleanly** against the vendored wlroots 0.19.3 (no NVIDIA-specific
  build flags needed).
- **Nested mode works** (gamescope as a Wayland client of another compositor —
  e.g. COSMIC). Verified: the 5090 is selected as the render device, a flip-mode
  swapchain is created, `VK_EXT_swapchain_maintenance1` is used, runs at the
  output's refresh:

  ```
  Selected GPU 1: NVIDIA GeForce RTX 5090, type: DiscreteGpu
  [Gamescope WSI] Forcing on VK_EXT_swapchain_maintenance1.
  [Gamescope WSI] Created swapchain ... imageCount: 3 ... flip: true
  ```

- **Explicit-sync quirk is already handled**: the NVIDIA 555+ drivers advertise
  `DRM_CAP_SYNCOBJ` but reject `IN_FENCE_FD` with `EPERM`; gamescope detects this
  and disables `IN_FENCE_FD` on the first failing commit (see
  `Backends/DRMBackend.cpp`, "NVIDIA 555 series" comment). This fork goes
  further and disables `IN_FENCE_FD` proactively once the NVIDIA proprietary
  driver is detected, rather than waiting for the first `EPERM`.

## The real NVIDIA gaps

1. **Standalone DRM backend** (gamescope as the session compositor directly on
   KMS, not nested). This is where NVIDIA historically diverges from AMD:
   atomic plane assignment via libliftoff, modeset, and explicit sync all run
   against `nvidia-drm` instead of `amdgpu`. Needs on-hardware validation.

2. **HDR colour management.** gamescope's DRM HDR pipeline offloads the colour
   transform to **AMD-only** plane properties — `AMD_PLANE_CTM`,
   `AMD_PLANE_SHAPER_LUT/TF`, `AMD_PLANE_LUT3D`, `AMD_PLANE_BLEND_TF` (see
   `DRMBackend.cpp`). NVIDIA exposes none of these. The HDR10 *signalling*
   (`HDR_OUTPUT_METADATA` + `Colorspace`) NVIDIA *does* support, and gamescope's
   GPU-agnostic **Vulkan composite** colour path (`color_helpers.cpp`,
   `rendervulkan.cpp`) can perform the tone-mapping in-shader. The work is to
   confirm gamescope cleanly falls back to the Vulkan colour path on NVIDIA when
   the AMD plane props are absent, and that HDR output is correct — rather than
   silently producing a wrong/clipped image.

## Fixed: SIGSEGV on shutdown, misread as a SPIR-V compiler bug

On driver 610 (tested 610.57.04, RTX 5090), gamescope SIGSEGVs whenever the
primary child exits early -- e.g. `gamescope -- vkcube`, where the client dies
with `Failed to get Wayland objects`. The backtrace lands inside NVIDIA's
shader compiler:

```
#22 libnvidia-glvkspirv.so.610.57.04 + 0xa21db
#23 _nv002nvvm (libnvidia-glvkspirv.so.610.57.04 + 0xa2a06)
...
#32 CVulkanDevice::compilePipeline(...)
#33 CVulkanDevice::compileAllPipelines(std::stop_token)
```

This reads like a driver bug in the SPIR-V->NVVM compiler, but it is not.
It is a lifetime bug in gamescope: the pre-compile thread from
`CVulkanDevice::BInit()` is never stopped or joined during shutdown, so the
process tears down while that thread is still executing inside
`libnvidia-gpucomp`.

Evidence it is not a bad shader:

- Given a long-lived child (`gamescope -- sleep 30`), **all 110 pre-compiled
  pipelines compile successfully** and gamescope exits 0. That includes this
  fork's `EWA_LANCZOS`, `BILATERAL_DENOISER` and `HDEBAND`.
- Individual compiles are fast (0.1-0.4 ms typical, ~130 ms worst case).
- The crash point moves between runs, which a malformed shader would not do.

Fixed by joining the pre-compile thread before teardown. Measured with the
shader cache purged and `__GL_SHADER_DISK_CACHE=0`: 9/10 runs SIGSEGV before,
0/10 after.

### Note on the shader disk cache

A *warm* NVIDIA shader cache hides this bug completely (0/10 crashes), because
cached pipelines never re-enter the compiler. Purging `~/.cache/nvidia` or
setting `__GL_SHADER_DISK_CACHE=0` is therefore required to reproduce it, and
any measurement taken without doing so is meaningless.

## Testing

```sh
# nested smoke test (safe, runs inside your existing Wayland session):
gamescope -W 1280 -H 720 -b -- vkcube

# if the SPIR-V compiler SEGV above is hit, this env var disables the
# WSI layer's present-wait usage, which works around a separate NVIDIA crash:
GAMESCOPE_WSI_HIDE_PRESENT_WAIT_EXT=1 gamescope -- <game>
```

Run the standalone DRM backend (`--backend drm`) only from a bare TTY, never
from inside a running desktop session.
