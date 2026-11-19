// One profile per plugin. Every shipped DLL is a single engine, so a build selects exactly one
// profile and the shared filter code (frame_gen.cpp) fills in the names from it.
//
// A profile picks four things: the plugin's VapourSynth namespace and description, the engine and
// its session class, and the folder beside the DLL that holds that engine's runtime package. The
// four engines therefore ship side by side in one directory tree without knowing about each other.

#pragma once

#include "fg_types.h"

#if defined(FG_PLUGIN_VDLSSG)

#define FG_NS "vdlssg"
#define FG_DESC "DLSS Frame Generation frame interpolation"
#define FG_ENGINE fg::Engine::Dlss
#define FG_SESSION_HEADER "ngx_session.h"
#define FG_SESSION_NS vdlssg
#define FG_SESSION_CLASS NgxSession
// The extracted DLSS 4.5 engine pack (version.dll proxy + dlssg_sm86\).
#define FG_PROXY_DIR L"engine45"

#elif defined(FG_PLUGIN_VXESSFG)

#define FG_NS "vxessfg"
#define FG_DESC "XeSS-FG frame interpolation"
#define FG_ENGINE fg::Engine::Xess
#define FG_SESSION_HEADER "ngx_session.h"
#define FG_SESSION_NS vdlssg
#define FG_SESSION_CLASS NgxSession
// The same proxy package with Engine=XeSS in dlssg_sm86.ini: XeSS-FG model 15 runs on the proxy's
// own CUDA kernels, so libxess_fg.dll is not needed.
#define FG_PROXY_DIR L"engine45_xess"

#elif defined(FG_PLUGIN_VVFG)

#define FG_NS "vvfg"
#define FG_DESC "NVIDIA Video Frame Generation frame interpolation"
#define FG_ENGINE fg::Engine::Vfg
#define FG_SESSION_HEADER "vfg_session.h"
#define FG_SESSION_NS vfg
#define FG_SESSION_CLASS VfgSession
// Video Effects SDK 1.3 runtime (NVCVImage.dll, NVVideoEffects.dll, nvVFXVideoFrameGeneration.dll,
// cudart64_12.dll). The model is sm_89-only and gets unlocked in-process on Ampere.
#define FG_PROXY_DIR L"engine_nvvfx"

#elif defined(FG_PLUGIN_VFSR4)

#define FG_NS "vfsr4"
#define FG_DESC "AMD FSR frame generation frame interpolation"
#define FG_ENGINE fg::Engine::Fsr4
#define FG_SESSION_HEADER "fsr4_session.h"
#define FG_SESSION_NS fsr4
#define FG_SESSION_CLASS Fsr4Session
// The SmoothMyVideo FSR bridge, AMD's FSR runtime, and the patched vkd3d-proton it runs on.
#define FG_PROXY_DIR L"engine_fsrfg"

#else
#error "select a plugin profile: FG_PLUGIN_VDLSSG, FG_PLUGIN_VXESSFG, FG_PLUGIN_VVFG or FG_PLUGIN_VFSR4"
#endif
