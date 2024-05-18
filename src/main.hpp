#pragma once

#include <getopt.h>

#include <atomic>
#include <vector>

extern const char *gamescope_optstring;
extern const struct option *gamescope_options;

extern std::atomic< bool > g_bRun;

extern int g_nNestedWidth;
extern int g_nNestedHeight;
extern int g_nNestedRefresh; // mHz
extern int g_nNestedUnfocusedRefresh; // mHz
extern int g_nNestedDisplayIndex;

extern uint32_t g_nOutputWidth;
extern uint32_t g_nOutputHeight;
extern bool g_bForceRelativeMouse;
extern int g_nOutputRefresh; // mHz
extern bool g_bOutputHDREnabled;
extern bool g_bForceInternal;

extern bool g_bForceCompositionRotation;
extern uint32_t g_uOutputRotation;

extern bool g_bFullscreen;

extern bool g_bGrabbed;

extern float g_mouseSensitivity;
extern const char *g_sOutputName;
extern std::vector<uint32_t> g_customRefreshRates;

enum class GamescopeUpscaleFilter : uint32_t
{
    LINEAR = 0,
    NEAREST,
    FSR,
    NIS,
    PIXEL,
    LANCZOS,    // EWA Lanczos4 Sharpest downscaler (+ optional bilateral denoiser / hdeband passes)

    FROM_VIEW = 0xF, // internal
};

// Optional post-processing passes the LANCZOS filter can chain after the
// downscale. Enabled via --filter=lanczos:bilateral etc.
struct GamescopeLanczosOptions
{
    bool bBilateralDenoiser = false;
    bool bHdeband           = false;
};
extern GamescopeLanczosOptions g_lanczosOptions;

static constexpr bool DoesHardwareSupportUpscaleFilter( GamescopeUpscaleFilter eFilter )
{
    // Could do nearest someday... AMDGPU DC supports custom tap placement to an extent.

    return eFilter == GamescopeUpscaleFilter::LINEAR;
}

enum class GamescopeUpscaleScaler : uint32_t
{
    AUTO,
    INTEGER,
    FIT,
    FILL,
    STRETCH,
};

extern GamescopeUpscaleFilter g_upscaleFilter;
extern GamescopeUpscaleScaler g_upscaleScaler;
extern GamescopeUpscaleFilter g_wantedUpscaleFilter;
extern GamescopeUpscaleScaler g_wantedUpscaleScaler;
extern int g_upscaleFilterSharpness;

extern bool g_bBorderlessOutputWindow;

extern bool g_bExposeWayland;

extern bool g_bRt;

extern int g_nXWaylandCount;
extern bool g_bNoTouchPointerEmulation;

extern uint32_t g_preferVendorID;
extern uint32_t g_preferDeviceID;

