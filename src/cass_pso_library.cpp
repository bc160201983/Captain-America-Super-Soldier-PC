// Adapted from gowj (Gears of War: Judgment PC port) src/gowj_pso_library.cpp (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - persistent D3D12 pipeline library.
//
// Hooks ID3D12Device::CreateGraphicsPipelineState on the shared device vtable and answers
// creates from a pipeline library stored in <runtime cache dir>\pso_library.bin.
// cass_pso_library=false (cvar) or CASS_PSOLIB=0 (env) disables it. Anything unexpected
// (no library support, a stale/corrupt file, a driver or GPU change, a fault while loading)
// falls back to a normal create.
//
// On-disk format (cass addition): a 32-byte header {magic, version, payload size, payload
// hash, adapter vendor/device id} followed by the driver's serialized library. A truncated or
// damaged file is discarded before it ever reaches the driver; a different adapter discards it
// up front; a driver update is caught by CreatePipelineLibrary itself
// (D3D12_ERROR_DRIVER_VERSION_MISMATCH / D3D12_ERROR_ADAPTER_NOT_FOUND) and the library starts
// empty and is rewritten.

#include "cass_pso_library.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(cass_pso_library, true, "gpu",
                    "Keep compiled D3D12 pipelines in a library file under the cache directory "
                    "so later runs load them instead of recompiling (removes repeat-visit "
                    "compile stutter). Rebuilt automatically after a driver or GPU change.");

namespace {

// ---- hashing ------------------------------------------------------------------------
// The key is a hash of the whole description (shader bytes plus fixed-function state,
// field by field so padding cannot leak in); the library validates the match itself, so a
// collision only costs a normal create.
struct Fnv {
  uint64_t a = 14695981039346656037ull, b = 0x9E3779B97F4A7C15ull;
  void Add(const void* p, size_t n) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
      a = (a ^ q[i]) * 1099511628211ull;
      b = (b + q[i] + 0x7F4A7C15ull) * 0xFF51AFD7ED558CCDull;
      b ^= b >> 29;
    }
  }
  void U32(uint32_t v) { Add(&v, 4); }
};

// Fast word-wise checksum for the file payload (hundreds of MB; byte-wise FNV is too slow).
uint64_t PayloadHash(const char* p, size_t n) {
  uint64_t h = 0x243F6A8885A308D3ull ^ n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001B3ull;
    h ^= h >> 32;
  }
  for (; i < n; ++i) h = (h ^ uint8_t(p[i])) * 0x100000001B3ull;
  h ^= h >> 29;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 32;
  return h;
}

void HashBytecode(Fnv& h, const D3D12_SHADER_BYTECODE& bc) {
  h.U32(uint32_t(bc.BytecodeLength));
  if (bc.pShaderBytecode && bc.BytecodeLength) h.Add(bc.pShaderBytecode, bc.BytecodeLength);
}

bool HashDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, wchar_t* name /*[40]*/) {
  if (d.StreamOutput.NumEntries || d.CachedPSO.CachedBlobSizeInBytes) return false;
  Fnv h;
  h.U32(0x504C4231);  // layout version salt
  // The root signature is not hashed (it is an object, not a description); the library
  // checks it on load and a mismatch only costs a normal create.
  HashBytecode(h, d.VS);
  HashBytecode(h, d.PS);
  HashBytecode(h, d.DS);
  HashBytecode(h, d.HS);
  HashBytecode(h, d.GS);
  h.U32(d.BlendState.AlphaToCoverageEnable);
  h.U32(d.BlendState.IndependentBlendEnable);
  for (const auto& r : d.BlendState.RenderTarget) {
    h.U32(r.BlendEnable); h.U32(r.LogicOpEnable);
    h.U32(r.SrcBlend); h.U32(r.DestBlend); h.U32(r.BlendOp);
    h.U32(r.SrcBlendAlpha); h.U32(r.DestBlendAlpha); h.U32(r.BlendOpAlpha);
    h.U32(r.LogicOp); h.U32(r.RenderTargetWriteMask);
  }
  h.U32(d.SampleMask);
  const auto& ra = d.RasterizerState;
  h.U32(ra.FillMode); h.U32(ra.CullMode); h.U32(ra.FrontCounterClockwise);
  h.U32(uint32_t(ra.DepthBias)); h.Add(&ra.DepthBiasClamp, 4); h.Add(&ra.SlopeScaledDepthBias, 4);
  h.U32(ra.DepthClipEnable); h.U32(ra.MultisampleEnable); h.U32(ra.AntialiasedLineEnable);
  h.U32(ra.ForcedSampleCount); h.U32(ra.ConservativeRaster);
  const auto& ds = d.DepthStencilState;
  h.U32(ds.DepthEnable); h.U32(ds.DepthWriteMask); h.U32(ds.DepthFunc);
  h.U32(ds.StencilEnable); h.U32(ds.StencilReadMask); h.U32(ds.StencilWriteMask);
  for (const auto* f : {&ds.FrontFace, &ds.BackFace}) {
    h.U32(f->StencilFailOp); h.U32(f->StencilDepthFailOp); h.U32(f->StencilPassOp);
    h.U32(f->StencilFunc);
  }
  h.U32(d.InputLayout.NumElements);
  for (UINT i = 0; i < d.InputLayout.NumElements && d.InputLayout.pInputElementDescs; ++i) {
    const auto& e = d.InputLayout.pInputElementDescs[i];
    if (e.SemanticName) h.Add(e.SemanticName, strlen(e.SemanticName) + 1);
    h.U32(e.SemanticIndex); h.U32(e.Format); h.U32(e.InputSlot);
    h.U32(e.AlignedByteOffset); h.U32(e.InputSlotClass); h.U32(e.InstanceDataStepRate);
  }
  h.U32(d.IBStripCutValue); h.U32(d.PrimitiveTopologyType); h.U32(d.NumRenderTargets);
  for (auto f : d.RTVFormats) h.U32(f);
  h.U32(d.DSVFormat); h.U32(d.SampleDesc.Count); h.U32(d.SampleDesc.Quality);
  h.U32(d.NodeMask); h.U32(d.Flags);
  swprintf(name, 40, L"%016llx%016llx", (unsigned long long)h.a, (unsigned long long)h.b);
  return true;
}

// ---- file header ----------------------------------------------------------------------
constexpr uint32_t kMagic = 0x4C535043;  // "CPSL"
constexpr uint32_t kVersion = 1;
struct FileHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t payload_size;
  uint64_t payload_hash;
  uint32_t adapter_key_lo;  // VendorId << 16 | (DeviceId & 0xFFFF)
  uint32_t adapter_key_hi;  // SubSysId
};
static_assert(sizeof(FileHeader) == 32, "header layout");

// ---- state ------------------------------------------------------------------------------
ID3D12PipelineLibrary* g_lib = nullptr;
std::vector<char> g_lib_blob;  // must stay alive as long as g_lib (D3D12 requirement)
std::atomic<bool> g_lib_dirty{false};
std::atomic<uint64_t> g_lib_hits{0}, g_lib_misses{0}, g_lib_stored{0};
std::filesystem::path g_lib_path;
std::filesystem::path g_cache_dir;
uint64_t g_adapter_key = 0;
std::atomic<bool> g_lib_enabled{true};
std::mutex g_save_mutex;
bool g_shutdown = false;  // guarded by g_save_mutex; set once the final save is done

// D3D12 documents ID3D12PipelineLibrary as internally synchronized except for several threads
// loading the same name at once, which callers must serialize. Loads are striped by name hash.
constexpr size_t kLoadStripes = 64;
std::mutex g_load_mutex[kLoadStripes];

// UTF-8 for log lines. path::string() converts to the ANSI code page and throws on characters
// it cannot represent (e.g. a non-ASCII user name in the cache path).
std::string PathUtf8(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (!n || n >= MAX_PATH) return {};
  return std::filesystem::path(buf).parent_path();
}

std::filesystem::path LibPath() {
  std::filesystem::path dir = g_cache_dir;
  if (dir.empty()) {
    const auto exe = ExeDir();
    if (exe.empty()) return {};
    dir = exe / "cache";
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);  // ignore errors: open just fails later
  return dir / "pso_library.bin";
}

// Best-effort adapter identity from the device LUID through DXGI (dxgi.dll is loaded
// dynamically so the executable needs no extra import library). 0 = unknown, which never
// invalidates by itself.
uint64_t AdapterKey(ID3D12Device* dev) {
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (!dxgi) return 0;
  auto create = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
      reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
  uint64_t key = 0;
  IDXGIFactory4* factory = nullptr;
  if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        key = (uint64_t(desc.SubSysId) << 32) | (uint64_t(desc.VendorId & 0xFFFF) << 16) |
              (desc.DeviceId & 0xFFFF);
      }
      adapter->Release();
    }
    factory->Release();
  }
  // dxgi.dll stays loaded (d3d12 already depends on it); no FreeLibrary.
  return key;
}

bool SaveNowImpl() {
  if (g_shutdown || !g_lib || g_lib_path.empty() ||
      !g_lib_enabled.load(std::memory_order_relaxed) || !g_lib_dirty.exchange(false)) {
    return false;
  }
  const SIZE_T n = g_lib->GetSerializedSize();
  if (!n || n > 0xFFFFFFFFull - sizeof(FileHeader)) return false;
  std::vector<char> buf(sizeof(FileHeader) + n);
  if (FAILED(g_lib->Serialize(buf.data() + sizeof(FileHeader), n))) {
    g_lib_dirty = true;  // try again next round
    return false;
  }
  FileHeader hdr{};
  hdr.magic = kMagic;
  hdr.version = kVersion;
  hdr.payload_size = n;
  hdr.payload_hash = PayloadHash(buf.data() + sizeof(FileHeader), n);
  hdr.adapter_key_lo = uint32_t(g_adapter_key);
  hdr.adapter_key_hi = uint32_t(g_adapter_key >> 32);
  std::memcpy(buf.data(), &hdr, sizeof(hdr));

  std::filesystem::path tmp = g_lib_path;
  tmp += L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD w = 0;
  bool ok = WriteFile(f, buf.data(), DWORD(buf.size()), &w, nullptr) && w == buf.size();
  ok = FlushFileBuffers(f) && ok;
  CloseHandle(f);
  if (ok) {
    ok = MoveFileExW(tmp.c_str(), g_lib_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  }
  if (!ok) DeleteFileW(tmp.c_str());
  REXLOG_INFO("PSOLIB saved {} bytes ok={} hits={} misses={} stored={}", buf.size(), ok,
              g_lib_hits.load(), g_lib_misses.load(), g_lib_stored.load());
  return ok;
}

// Runs on the writer thread and at shutdown: never let an exception (bad_alloc for a large
// library) escape.
bool SaveNow() {
  std::lock_guard<std::mutex> lock(g_save_mutex);
  try {
    return SaveNowImpl();
  } catch (...) {
    g_lib_dirty = true;
    return false;
  }
}

void LibWriter() {
  for (;;) {
    Sleep(30000);  // serializing can be 100+ MB; keep it off the hot path
    SaveNow();
    std::lock_guard<std::mutex> lock(g_save_mutex);
    if (g_shutdown) return;
  }
}

// No C++ objects with destructors here: __try needs a plain frame.
HRESULT SafeLoad(ID3D12PipelineLibrary* lib, const wchar_t* name,
                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID r, void** out,
                 bool* faulted) {
  __try {
    return lib->LoadGraphicsPipeline(name, desc, r, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = true;
    return E_FAIL;
  }
}

HRESULT SafeCreateLibrary(ID3D12Device1* d1, const void* blob, SIZE_T size,
                          ID3D12PipelineLibrary** out, bool* faulted) {
  __try {
    return d1->CreatePipelineLibrary(blob, size, __uuidof(ID3D12PipelineLibrary),
                                     reinterpret_cast<void**>(out));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = true;
    *out = nullptr;
    return E_FAIL;
  }
}

// Reads <path>, checks the header, and leaves only the driver payload in `blob`.
// Returns a short reason when the file was present but unusable (nullptr otherwise).
const char* ReadLibraryFile(const std::filesystem::path& path, std::vector<char>& blob) {
  blob.clear();
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return nullptr;  // first run
  const char* why = nullptr;
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(f, &sz) || sz.QuadPart < LONGLONG(sizeof(FileHeader)) ||
      sz.QuadPart >= (1ll << 32)) {
    why = "bad size";
  } else {
    std::vector<char> raw(size_t(sz.QuadPart));
    DWORD r = 0;
    if (!ReadFile(f, raw.data(), DWORD(raw.size()), &r, nullptr) || r != raw.size()) {
      why = "read failed";
    } else {
      FileHeader hdr;
      std::memcpy(&hdr, raw.data(), sizeof(hdr));
      const size_t n = raw.size() - sizeof(FileHeader);
      const uint64_t key = (uint64_t(hdr.adapter_key_hi) << 32) | hdr.adapter_key_lo;
      if (hdr.magic != kMagic || hdr.version != kVersion) {
        why = "unknown format";
      } else if (hdr.payload_size != n) {
        why = "truncated";
      } else if (key && g_adapter_key && key != g_adapter_key) {
        why = "different GPU";
      } else if (hdr.payload_hash != PayloadHash(raw.data() + sizeof(FileHeader), n)) {
        why = "checksum mismatch";
      } else {
        raw.erase(raw.begin(), raw.begin() + sizeof(FileHeader));
        blob.swap(raw);
      }
    }
  }
  CloseHandle(f);
  return why;
}

void InitLib(ID3D12Device* dev) {
  {
    ID3D12Device1* d1 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d1)))) {
      REXLOG_WARN("PSOLIB: device has no ID3D12Device1, library disabled");
      return;
    }
    D3D12_FEATURE_DATA_SHADER_CACHE sc{};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &sc, sizeof(sc))) ||
        !(sc.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
      REXLOG_WARN("PSOLIB: driver reports no pipeline-library support, library disabled");
      d1->Release();
      return;
    }
    g_lib_path = LibPath();
    if (g_lib_path.empty()) {
      REXLOG_WARN("PSOLIB: no cache directory, library disabled");
      d1->Release();
      return;
    }
    g_adapter_key = AdapterKey(dev);
    std::vector<char>& blob = g_lib_blob;  // CreatePipelineLibrary keeps pointing into it
    if (const char* why = ReadLibraryFile(g_lib_path, blob)) {
      REXLOG_WARN("PSOLIB: stored library discarded ({}), starting empty", why);
    }
    bool faulted = false;
    HRESULT hr = SafeCreateLibrary(d1, blob.empty() ? nullptr : blob.data(), blob.size(),
                                   &g_lib, &faulted);
    const bool loaded = SUCCEEDED(hr) && !blob.empty();
    if (FAILED(hr) && !blob.empty()) {
      // Stale blob: D3D12_ERROR_DRIVER_VERSION_MISMATCH / D3D12_ERROR_ADAPTER_NOT_FOUND after
      // a driver update or GPU swap, or E_INVALIDARG for damaged data. Start empty; the next
      // save overwrites the file.
      REXLOG_WARN("PSOLIB: stored library rejected hr={:08x}{}, starting empty",
                  static_cast<uint32_t>(hr), faulted ? " (fault)" : "");
      g_lib_blob.clear();
      g_lib_blob.shrink_to_fit();
      faulted = false;
      hr = SafeCreateLibrary(d1, nullptr, 0, &g_lib, &faulted);
      g_lib_dirty = true;  // replace the stale file even if nothing new gets stored soon
    }
    d1->Release();
    REXLOG_INFO("PSOLIB ready hr={:08x} loaded_existing={} file_bytes={} path={}",
                static_cast<uint32_t>(hr), loaded, g_lib_blob.size(), PathUtf8(g_lib_path));
    if (SUCCEEDED(hr) && g_lib) {
      std::thread(LibWriter).detach();
    } else {
      g_lib = nullptr;
      g_lib_dirty = false;
    }
  }
}

// Called from inside a D3D12 device method: an exception must never escape (bad_alloc on a
// large file, filesystem errors, thread creation failure). Any failure disables the library.
ID3D12PipelineLibrary* GetLib(ID3D12Device* dev) {
  static std::once_flag once;
  std::call_once(once, [dev] {
    try {
      InitLib(dev);
    } catch (...) {
      if (g_lib) g_lib->Release();
      g_lib = nullptr;
      g_lib_dirty = false;
      g_lib_enabled = false;
    }
  });
  return g_lib_enabled.load(std::memory_order_relaxed) ? g_lib : nullptr;
}

using PsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                          REFIID, void**);
PsoFn g_pso = nullptr;

HRESULT STDMETHODCALLTYPE HookPso(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                  REFIID r, void** out) {
  HRESULT hr = E_FAIL;
  bool from_lib = false;
  wchar_t name[40];
  ID3D12PipelineLibrary* lib =
      (g_lib_enabled.load(std::memory_order_relaxed) && desc && out &&
       r == __uuidof(ID3D12PipelineState))
          ? GetLib(d)
          : nullptr;
  const bool keyed = lib && HashDesc(*desc, name);
  if (keyed) {
    bool faulted = false;
    {
      // Same name -> same stripe; name is 32 hex digits, the last ones pick the stripe.
      const size_t stripe = size_t(wcstoul(name + 28, nullptr, 16)) % kLoadStripes;
      std::lock_guard<std::mutex> lock(g_load_mutex[stripe]);
      hr = SafeLoad(lib, name, desc, r, out, &faulted);
    }
    if (faulted) {
      // Never touch the library again this session, never save it, and drop the stored file
      // so the next run starts clean instead of faulting again.
      g_lib_enabled = false;
      DeleteFileW(g_lib_path.c_str());
      REXLOG_WARN("PSOLIB: LoadGraphicsPipeline faulted, library disabled and file removed");
    }
    from_lib = SUCCEEDED(hr) && !faulted;
    (from_lib ? g_lib_hits : g_lib_misses).fetch_add(1, std::memory_order_relaxed);
  }
  if (!from_lib) {
    if (out) *out = nullptr;
    hr = g_pso(d, desc, r, out);
    if (keyed && g_lib_enabled.load(std::memory_order_relaxed) && SUCCEEDED(hr) && *out &&
        SUCCEEDED(lib->StorePipeline(name, static_cast<ID3D12PipelineState*>(*out)))) {
      g_lib_stored.fetch_add(1, std::memory_order_relaxed);
      g_lib_dirty = true;
    }
  }
  return hr;
}

bool PatchSlot(void** vtable, size_t index, void* hook, void** orig) {
  DWORD old = 0;
  if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
  *orig = vtable[index];
  vtable[index] = hook;
  VirtualProtect(&vtable[index], sizeof(void*), old, &old);
  FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void*));
  return true;
}

}  // namespace

namespace rex::glue {

void InstallPsoLibrary(const std::filesystem::path& cache_dir) {
  static bool done = false;
  if (done) return;
  done = true;
  if (!REXCVAR_GET(cass_pso_library)) {
    g_lib_enabled = false;
    REXLOG_INFO("PSOLIB: disabled by cass_pso_library=false");
    return;
  }
  char buf[8]{};
  if (GetEnvironmentVariableA("CASS_PSOLIB", buf, sizeof(buf)) && buf[0] == '0') {
    g_lib_enabled = false;
    REXLOG_INFO("PSOLIB: disabled by CASS_PSOLIB=0");
    return;
  }
  g_cache_dir = cache_dir;
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  auto create = d3d12 ? reinterpret_cast<HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID,
                                                          void**)>(
                            reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                      : nullptr;
  ID3D12Device* device = nullptr;
  if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    REXLOG_WARN("PSOLIB: throwaway device failed, library not installed");
    return;
  }
  // The device vtable is shared by every device of this implementation, so patching it through
  // a throwaway device also covers the one the runtime creates. Slot 10 is
  // CreateGraphicsPipelineState (IUnknown 3 + ID3D12Object 4 + GetNodeCount,
  // CreateCommandQueue, CreateCommandAllocator).
  void** vt = *reinterpret_cast<void***>(device);
  const bool ok = PatchSlot(vt, 10, reinterpret_cast<void*>(&HookPso),
                            reinterpret_cast<void**>(&g_pso));
  device->Release();
  REXLOG_INFO("PSOLIB hook installed={} cache_dir={}", ok, PathUtf8(cache_dir));
}

void FlushPsoLibrary() {
  // Final save, then stop: after OnShutdown the runtime tears the device down, so neither the
  // writer thread nor the hook may use the library afterwards. The library object itself is
  // intentionally leaked (process exit).
  SaveNow();
  std::lock_guard<std::mutex> lock(g_save_mutex);
  g_shutdown = true;
  g_lib_enabled = false;
}

}  // namespace rex::glue
