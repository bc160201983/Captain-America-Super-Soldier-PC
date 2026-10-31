// cass - Captain America: Super Soldier, ReXGlue recompiled project

#pragma once

#include <float.h>
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/ui/window.h>

#include "cass_crash_guard.h"
#include "cass_fsr.h"
#include "cass_iso.h"
#include "cass_pacing.h"
#include "cass_paths.h"
#include "cass_pso_library.h"
#include "cass_test_input.h"

// The Xbox 360 never raises floating point exceptions; the game's code relies on that.
// Swallow any host FP exception and keep going with all exceptions masked.
inline LONG WINAPI CassFpExceptionHandler(PEXCEPTION_POINTERS info) {
  switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_STACK_CHECK:
      info->ContextRecord->MxCsr |= 0x1F80;   // mask all SSE FP exceptions
      info->ContextRecord->MxCsr &= ~0x003F;  // clear pending flags
      _clearfp();
      return EXCEPTION_CONTINUE_EXECUTION;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
}

class CassApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<CassApp>(new CassApp(ctx, "cass", PPCImageConfig));
  }

  // GPU flags live in the xenos plugin DLL, which may register them after OnPreSetup.
  // Set the flag now if it exists, and also through its REX_* variable, which the
  // plugin reads when it registers. A value the player set (config file or command
  // line) wins.
  static void SetGpuFlag(const char* name, const char* value) {
    using rex::cvar::Source;
    const Source src = rex::cvar::GetFlagSource(name);
    if (src == Source::kCommandLine || src == Source::kConfig) return;
    if (rex::cvar::GetFlagInfo(name)) rex::cvar::SetFlagByName(name, value);
    std::string env = "REX_";
    for (const char* p = name; *p; ++p) env += static_cast<char>(toupper(static_cast<unsigned char>(*p)));
    if (!getenv(env.c_str())) _putenv_s(env.c_str(), value);
  }

  // Started without --game_data_root (double-click, desktop shortcut): use the location the
  // launcher saved as cass_game_dir in cass.toml, else <exe>/game, else <exe>/../../../game, else a
  // lone game .iso next to the exe, in <exe>/../../.. or in <exe>/../../../.. . Runs before cass.toml is loaded, so
  // cass_paths.cpp reads that one key itself. The location (also --game_data_root) may be an .iso:
  // game_data_root then becomes a stub folder and OnLoadXexImage mounts the image (cass_iso.h).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    if (paths.game_data_root.empty()) {
      paths.game_data_root = rex::glue::ResolveGameDir(paths.config_path);
    }
    if (!paths.game_data_root.empty() && !std::filesystem::is_directory(paths.game_data_root, ec) &&
        std::filesystem::is_regular_file(paths.game_data_root, ec)) {
      paths.game_data_root = rex::glue::PrepareIsoMode(paths.game_data_root, paths.cache_root);
    }
  }

  // ISO mode: swap the stub folder behind game:\ and d:\ for the disc image itself.
  void OnLoadXexImage(std::string& xex_image) override { rex::glue::MountIsoImage(runtime()); }

  // Earliest hook with a logger: crashes during window / D3D12 setup are reported too.
  void OnPostInitLogging() override { rex::glue::InstallCrashGuard(); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    rex::glue::InstallCrashGuard();  // idempotent: re-applies this thread's terminate handler
    config.gpu_plugin = "xenos";
    // Settings the original 0.4 port needed to reach the title screen.
    SetGpuFlag("gpu_allow_invalid_fetch_constants", "true");
    SetGpuFlag("d3d12_submit_on_primary_buffer_end", "true");
    _clearfp();
    _controlfp_s(nullptr, _MCW_EM, _MCW_EM);
    AddVectoredExceptionHandler(1, CassFpExceptionHandler);

    // Before the runtime creates its D3D12 device (cass.toml and cache_root are loaded by now).
    rex::glue::InstallPsoLibrary(cache_root());
    // FSR first: pacing chains onto the presenter hook FSR installs.
    rex::glue::InstallFsr();
    rex::glue::InstallPacing();  // frame limiter, present dedup, power-throttling opt-out, FPS overlay
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Runs on the UI thread after the window exists; replaces "cass <SDK build>" in the title bar.
    if (window()) window()->SetTitle("Captain America: Super Soldier  -  Port by BilalReloaded");
    SetWindowIconFromResource();
    rex::glue::SetFsrImGuiDrawer(drawer);  // FSR stands aside while an ImGui window is on screen
  }

  // Title-bar and taskbar icon: the exe's icon resource 1 (src/cass.rc), set on every top-level
  // window of the UI thread (the SDL game window; its handle is private to the SDK).
  static void SetWindowIconFromResource() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto load = [inst](int metric_x, int metric_y) {
      return static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(metric_x),
                                           GetSystemMetrics(metric_y), LR_DEFAULTCOLOR | LR_SHARED));
    };
    // ("small" is a macro in the Windows headers.)
    static HICON icon_big = load(SM_CXICON, SM_CYICON), icon_small = load(SM_CXSMICON, SM_CYSMICON);
    if (!icon_big) return;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM) -> BOOL {
          if (!GetParent(hwnd) && IsWindowVisible(hwnd)) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(icon_small ? icon_small : icon_big));
          }
          return TRUE;
        },
        0);
  }

  // Called first in ReXApp::OnDestroy, while the D3D12 device and presenter are still alive.
  void OnShutdown() override {
    rex::glue::ShutdownTestInput();  // stop the script thread first
    rex::glue::FlushPsoLibrary();
    rex::glue::ShutdownFsr();
  }

  // Scripted controller input for automated tests (CASS_TEST_INPUT / cass_test_input; does nothing
  // unless set). The window exists by now and the guest has not read the pad yet.
  void OnPreLaunchModule() override {
    rex::glue::InstallTestInput(window() ? window()->GetNativeWindowHandle() : nullptr);
  }
};
