// Adapted from gowj (Gears of War: Judgment port) src/gowj_fsr.cpp and the RefreshGuestOutputImpl hook in src/gowj_perf_watch.cpp (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - final-image pipeline: SMAA 1x -> AMD FSR 1.0 EASU -> FSR RCAS.
//
// The stock rexruntime.dll is built without REX_HAS_FIDELITYFX_SDK, so its presenter
// only stretches the guest output to the window bilinearly (present_effect offers
// "bilinear" only). This redraws the final image ourselves, without touching
// runtime internals:
//  * Input: D3D12Presenter::RefreshGuestOutputImpl is hooked in the exported
//    presenter vtable; the refresher is wrapped so the texture that receives this
//    frame's guest output (D3D12GuestOutputRefreshContext::resource_uav_capable,
//    R10G10B10A2, left in PIXEL_SHADER_RESOURCE) reaches CassFsrNoteGuestOutput.
//  * Output: the runtime's real swap chain is patched at creation (dxgi.dll import
//    hooks on CreateDXGIFactory1/2 + factory vtable hooks on CreateSwapChain*). The
//    CreateSwapChain* "device" argument is the D3D12 command queue the chain
//    presents on - captured there, because a swap chain cannot return its queue.
//    Right before each Present, on that same queue:
//       SMAA edges -> weights -> neighborhood blend   (guest size)
//       EASU (output larger than guest) or bilinear   (window size, aspect kept
//                                                      when present_letterbox)
//       RCAS -> back buffer
//  * Skipped while one of the runtime's ImGui windows is on screen, so those
//    windows (painted into the back buffer) stay visible.
// Shaders: src/fsr/cass_fsr.hlsl and cass_smaa.hlsl, precompiled to src/fsr/g_*.h.

#include "cass_fsr.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/imgui_drawer.h>

#define A_CPU 1
#include "../third_party/fsr1/ffx_a.h"
#include "../third_party/fsr1/ffx_fsr1.h"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

#include "fsr/g_fsr_easu_ps.h"
#include "fsr/g_fsr_rcas_ps.h"
#include "fsr/g_fsr_vs.h"
#include "fsr/g_smaa_BilinearPS.h"
#include "fsr/g_smaa_BlendPS.h"
#include "fsr/g_smaa_BlendVS.h"
#include "fsr/g_smaa_EdgePS.h"
#include "fsr/g_smaa_EdgeVS.h"
#include "fsr/g_smaa_PlainVS.h"
#include "fsr/g_smaa_WeightPS.h"
#include "fsr/g_smaa_WeightVS.h"

REXCVAR_DEFINE_BOOL(cass_fsr, true, "gfx",
                    "Final-image pipeline (SMAA + FSR 1.0 EASU/RCAS). EASU upscales only when "
                    "the window is larger than the internal image; RCAS sharpening always runs. "
                    "False = the runtime's own bilinear stretch, untouched. Set "
                    "swap_post_effect = \"none\" while this is on.");
REXCVAR_DEFINE_BOOL(cass_smaa, true, "gfx",
                    "SMAA 1x anti-aliasing on the guest output before upscaling (needs cass_fsr).");
REXCVAR_DEFINE_BOOL(cass_fsr_easu, true, "gfx",
                    "FSR EASU edge-adaptive upscaling when the window is larger than the internal "
                    "image. False = bilinear.");
REXCVAR_DEFINE_DOUBLE(cass_fsr_sharpness, 0.5, "gfx",
                      "FSR RCAS sharpness reduction in stops: 0 = sharpest, 2 = softest.");
// The stock D3D12 presenter always presents with sync interval 0. This makes every present
// wait for the monitor's refresh (no tearing, even frame spacing) - a host-side version of the
// Gears 3 port's 0021-d3d12-host-vsync SDK patch.
REXCVAR_DEFINE_BOOL(cass_vsync, true, "gfx",
                    "Wait for the monitor's refresh on every present (no tearing). Turn off on a "
                    "G-Sync/FreeSync display together with a frame limit for lowest latency.");

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kFrames = 3;
constexpr UINT kSrvPerFrame = 16;
constexpr UINT kRtvPerFrame = 8;
constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R10G10B10A2_UNORM;

std::mutex g_guest_mu;
ComPtr<ID3D12Resource> g_guest;  // latest refreshed guest output
uint32_t g_guest_w = 0, g_guest_h = 0;

std::mutex g_queue_mu;
ComPtr<ID3D12CommandQueue> g_chain_queue;  // queue the runtime created its swap chain on

std::atomic<rex::ui::ImGuiDrawer*> g_imgui{nullptr};

struct Tex {
  ComPtr<ID3D12Resource> res;
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

struct State {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> easu, bilinear, rcas, edge, weight, blend;
  DXGI_FORMAT back_format = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap;
  UINT srv_inc = 0, rtv_inc = 0;
  ComPtr<ID3D12CommandAllocator> alloc[kFrames];
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  UINT64 frame_fence[kFrames] = {};
  UINT frame = 0;
  Tex inter, edges, weights, smaa_out, area, search;
  bool failed = false;
};
State g;
std::mutex g_state_mu;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
thread_local bool t_in_present = false;  // Present may forward to Present1 internally
std::atomic<bool> g_shutdown{false};      // ShutdownFsr ran: pass and guest capture off

// Original function of every vtable slot we patched, keyed by the slot's address,
// so objects of different classes (e.g. a debug-layer factory next to a plain
// one, or two swap-chain implementations) each chain to their own original
// instead of sharing one global pointer that the second patch would overwrite.
struct SlotOrig {
  std::atomic<void**> slot{nullptr};
  void* orig = nullptr;
};
constexpr size_t kMaxSlots = 32;
SlotOrig g_slots[kMaxSlots];
std::mutex g_patch_mu;

void* FindOrig(void** slot) {
  for (auto& e : g_slots) {
    void** s = e.slot.load(std::memory_order_acquire);
    if (!s) break;
    if (s == slot) return e.orig;
  }
  return nullptr;
}

// Original implementation of `obj`'s vtable slot `index` (null if never patched).
template <typename Fn>
Fn OrigOf(const void* obj, size_t index) {
  void** vtable = *reinterpret_cast<void** const*>(obj);
  return reinterpret_cast<Fn>(FindOrig(&vtable[index]));
}

bool Fail(const char* what, HRESULT hr) {
  REXLOG_WARN("FSR {} failed hr={:08x} - final-image pipeline disabled for this session", what,
              static_cast<uint32_t>(hr));
  g.failed = true;
  return false;
}

// One log line per distinct reason the pipeline did not run, so a silent no-op
// cannot happen.
void Skip(int reason, const char* why) {
  static std::atomic<uint32_t> seen{0};
  if (!(seen.fetch_or(1u << reason) & (1u << reason))) {
    REXLOG_WARN("FSR pipeline skipped: {}", why);
  }
}

void WaitAll() {
  if (!g.fence) return;
  for (UINT64 v : g.frame_fence) {
    if (v && g.fence->GetCompletedValue() < v) {
      g.fence->SetEventOnCompletion(v, g.fence_event);
      WaitForSingleObject(g.fence_event, 2000);
    }
  }
}

ComPtr<ID3D12PipelineState> Pso(const void* vs, size_t vs_size, const void* ps, size_t ps_size,
                                DXGI_FORMAT fmt) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g.root.Get();
  pd.VS = {vs, vs_size};
  pd.PS = {ps, ps_size};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = fmt;
  pd.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) Fail("pipeline", hr);
  return pso;
}

bool BuildRoot() {
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 16};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // SMAA VS reads metrics
  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.MaxLOD = D3D12_FLOAT32_MAX;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs{2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                       D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
  auto serialize = reinterpret_cast<SerializeFn>(
      GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12SerializeRootSignature"));
  ComPtr<ID3DBlob> blob, err;
  HRESULT hr = serialize ? serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err) : E_FAIL;
  if (FAILED(hr)) return Fail("root signature serialize", hr);
  hr = g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     IID_PPV_ARGS(&g.root));
  if (FAILED(hr)) return Fail("CreateRootSignature", hr);
  g.easu = Pso(g_fsr_vs, sizeof(g_fsr_vs), g_fsr_easu_ps, sizeof(g_fsr_easu_ps), kColor);
  g.bilinear = Pso(g_smaa_PlainVS, sizeof(g_smaa_PlainVS), g_smaa_BilinearPS,
                   sizeof(g_smaa_BilinearPS), kColor);
  g.edge = Pso(g_smaa_EdgeVS, sizeof(g_smaa_EdgeVS), g_smaa_EdgePS, sizeof(g_smaa_EdgePS),
               DXGI_FORMAT_R8G8_UNORM);
  g.weight = Pso(g_smaa_WeightVS, sizeof(g_smaa_WeightVS), g_smaa_WeightPS,
                 sizeof(g_smaa_WeightPS), DXGI_FORMAT_R8G8B8A8_UNORM);
  g.blend = Pso(g_smaa_BlendVS, sizeof(g_smaa_BlendVS), g_smaa_BlendPS, sizeof(g_smaa_BlendPS),
                kColor);
  return !g.failed;
}

bool MakeTex(Tex& t, UINT w, UINT h, DXGI_FORMAT fmt, bool rt) {
  if (t.res && t.w == w && t.h == h && t.fmt == fmt) return true;
  WaitAll();  // earlier frames may still read the old texture
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = fmt;
  rd.SampleDesc.Count = 1;
  rd.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
  t.res.Reset();
  t.w = t.h = 0;
  const HRESULT hr = g.device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd,
      rt ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&t.res));
  if (FAILED(hr)) return Fail("texture", hr);
  t.w = w;
  t.h = h;
  t.fmt = fmt;
  return true;
}

// SMAA's precomputed area/search lookup textures, uploaded once.
bool UploadLookups() {
  if (!MakeTex(g.area, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, false) ||
      !MakeTex(g.search, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, false)) {
    return false;
  }
  struct Src {
    Tex* t;
    const unsigned char* bytes;
    UINT pitch;
  } srcs[2] = {{&g.area, areaTexBytes, AREATEX_PITCH},
               {&g.search, searchTexBytes, SEARCHTEX_PITCH}};
  ComPtr<ID3D12Resource> upload[2];
  g.alloc[0]->Reset();
  g.list->Reset(g.alloc[0].Get(), nullptr);
  for (int i = 0; i < 2; ++i) {
    D3D12_RESOURCE_DESC td = srcs[i].t->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&upload[i]));
    if (FAILED(hr)) {
      g.list->Close();
      return Fail("upload buffer", hr);
    }
    uint8_t* p = nullptr;
    if (FAILED(hr = upload[i]->Map(0, nullptr, reinterpret_cast<void**>(&p)))) {
      g.list->Close();
      return Fail("upload map", hr);
    }
    for (UINT y = 0; y < srcs[i].t->h; ++y) {
      std::memcpy(p + fp.Offset + y * fp.Footprint.RowPitch, srcs[i].bytes + y * srcs[i].pitch,
                  srcs[i].pitch);
    }
    upload[i]->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{srcs[i].t->res.Get(),
                                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{upload[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = srcs[i].t->res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->ResourceBarrier(1, &b);
  }
  g.list->Close();
  ID3D12CommandList* lists[] = {g.list.Get()};
  g.queue->ExecuteCommandLists(1, lists);
  g.queue->Signal(g.fence.Get(), ++g.fence_value);
  g.fence->SetEventOnCompletion(g.fence_value, g.fence_event);
  WaitForSingleObject(g.fence_event, 5000);  // upload buffers die at scope end
  return true;
}

bool EnsureDevice() {
  ComPtr<ID3D12CommandQueue> queue;
  {
    std::lock_guard lock(g_queue_mu);
    queue = g_chain_queue;
  }
  if (!queue) {
    Skip(0, "no command queue captured from the runtime's swap-chain creation");
    return false;
  }
  if (g.queue.Get() == queue.Get() && g.device) return true;
  // New queue (first use, or the runtime recreated its device): start over.
  WaitAll();
  if (g.fence_event) CloseHandle(g.fence_event);
  g = State{};
  g.queue = queue;
  HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g.device));
  if (FAILED(hr)) return Fail("queue GetDevice", hr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kFrames * kSrvPerFrame,
                                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srv_heap))))
    return Fail("SRV heap", hr);
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrames * kRtvPerFrame, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        0};
  if (FAILED(hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv_heap))))
    return Fail("RTV heap", hr);
  g.srv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  g.rtv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (auto& a : g.alloc) {
    if (FAILED(hr = g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&a))))
      return Fail("allocator", hr);
  }
  if (FAILED(hr = g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g.alloc[0].Get(), nullptr, IID_PPV_ARGS(&g.list))))
    return Fail("command list", hr);
  g.list->Close();
  if (FAILED(hr = g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
    return Fail("fence", hr);
  g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g.fence_event) return Fail("fence event", HRESULT_FROM_WIN32(GetLastError()));
  if (!BuildRoot() || !UploadLookups()) return false;
  REXLOG_INFO("FSR initialized on the runtime's queue (SMAA lookups uploaded)");
  return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

struct Rect {
  UINT x = 0, y = 0, w = 0, h = 0;
};

struct Frame {
  UINT f;
  UINT next_srv = 0, next_rtv = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE Srv(UINT i) const {
    auto h = g.srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(f * kSrvPerFrame + i) * g.srv_inc;
    return h;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT i) const {
    auto h = g.srv_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(f * kSrvPerFrame + i) * g.srv_inc;
    return h;
  }
  // A 3-SRV table; unused slots get a null descriptor.
  D3D12_GPU_DESCRIPTOR_HANDLE Table(ID3D12Resource* a, ID3D12Resource* b = nullptr,
                                    ID3D12Resource* c = nullptr) {
    const UINT base = next_srv;
    next_srv += 3;
    ID3D12Resource* rs[3] = {a, b, c};
    for (UINT i = 0; i < 3; ++i) {
      D3D12_SHADER_RESOURCE_VIEW_DESC null_desc{};
      null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      null_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      null_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      null_desc.Texture2D.MipLevels = 1;
      g.device->CreateShaderResourceView(rs[i], rs[i] ? nullptr : &null_desc, Srv(base + i));
    }
    return SrvGpu(base);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(ID3D12Resource* r) {
    auto h = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(f * kRtvPerFrame + next_rtv++) * g.rtv_inc;
    g.device->CreateRenderTargetView(r, nullptr, h);
    return h;
  }
};

// Draws a full-screen triangle into the `vp` rectangle of `target` (which the
// caller has made a render target). `clear` zeroes the whole target first (SMAA
// discards where it has nothing; letterbox bars stay black).
void Pass(Frame& fr, ID3D12PipelineState* pso, ID3D12Resource* target, Rect vp,
          D3D12_GPU_DESCRIPTOR_HANDLE table, const uint32_t* k, bool clear) {
  auto* l = g.list.Get();
  const auto rtv = fr.Rtv(target);
  if (clear) {
    const float zero[4] = {0, 0, 0, 0};
    l->ClearRenderTargetView(rtv, zero, 0, nullptr);
  }
  l->SetPipelineState(pso);
  l->SetGraphicsRootSignature(g.root.Get());
  ID3D12DescriptorHeap* heaps[] = {g.srv_heap.Get()};
  l->SetDescriptorHeaps(1, heaps);
  l->SetGraphicsRootDescriptorTable(0, table);
  l->SetGraphicsRoot32BitConstants(1, 16, k, 0);
  D3D12_VIEWPORT v{float(vp.x), float(vp.y), float(vp.w), float(vp.h), 0, 1};
  D3D12_RECT sr{LONG(vp.x), LONG(vp.y), LONG(vp.x + vp.w), LONG(vp.y + vp.h)};
  l->RSSetViewports(1, &v);
  l->RSSetScissorRects(1, &sr);
  l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  l->DrawInstanced(3, 1, 0, 0);
}

// Render-target pass on one of our own textures (kept in PIXEL_SHADER_RESOURCE).
void OwnPass(Frame& fr, ID3D12PipelineState* pso, Tex& t, Rect vp,
             D3D12_GPU_DESCRIPTOR_HANDLE table, const uint32_t* k, bool clear) {
  auto b = Transition(t.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  g.list->ResourceBarrier(1, &b);
  Pass(fr, pso, t.res.Get(), vp, table, k, clear);
  b = Transition(t.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  g.list->ResourceBarrier(1, &b);
}

// True while one of the runtime's ImGui windows is drawn into the back buffer.
bool OverlayOnScreen() {
  rex::ui::ImGuiDrawer* d = g_imgui.load(std::memory_order_acquire);
  if (!d || !d->HasDialogs()) return false;
  return d->GetIO().MetricsRenderWindows > 0;
}

void Apply(IDXGISwapChain* sc) {
  if (!REXCVAR_GET(cass_fsr)) return;
  if (g_shutdown.load(std::memory_order_acquire)) return;
  if (OverlayOnScreen()) return;  // leave the runtime's ImGui visible
  std::lock_guard lock(g_state_mu);
  if (g_shutdown.load(std::memory_order_relaxed)) return;
  if (g.failed || !EnsureDevice() || g.failed) return;
  ComPtr<IDXGISwapChain3> sc3;
  if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return Skip(1, "no IDXGISwapChain3");
  DXGI_SWAP_CHAIN_DESC1 desc{};
  sc3->GetDesc1(&desc);
  if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
      desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
    return Skip(2, "unsupported back-buffer format");
  }
  const UINT ow = desc.Width, oh = desc.Height;
  if (!ow || !oh) return;  // minimized
  ComPtr<ID3D12Resource> guest;
  uint32_t gw = 0, gh = 0;
  {
    std::lock_guard glock(g_guest_mu);
    guest = g_guest;
    gw = g_guest_w;
    gh = g_guest_h;
  }
  if (!guest || !gw || !gh) return Skip(4, "no guest output captured yet");
  {
    // After a device loss the swap chain (and so our queue/device) can be new
    // while the captured texture still belongs to the old device; a view of it
    // on our device would be invalid.
    ComPtr<ID3D12Device> guest_device;
    if (FAILED(guest->GetDevice(IID_PPV_ARGS(&guest_device))) ||
        guest_device.Get() != g.device.Get()) {
      return Skip(6, "guest output belongs to another D3D12 device (waiting for a refresh)");
    }
  }
  // SMAA and the bilinear pass address the whole texture with 0..1 coordinates,
  // so work at the texture's real size (the presenter sizes it to the output).
  const D3D12_RESOURCE_DESC gd = guest->GetDesc();
  if (gd.Width != gw || gd.Height != gh) {
    Skip(5, "guest output texture size differs from the refresh size - using the texture size");
    gw = static_cast<uint32_t>(gd.Width);
    gh = gd.Height;
  }
  ComPtr<ID3D12Resource> back;
  if (FAILED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))))
    return Skip(3, "GetBuffer failed");
  if (g.back_format != desc.Format) {
    g.rcas = Pso(g_fsr_vs, sizeof(g_fsr_vs), g_fsr_rcas_ps, sizeof(g_fsr_rcas_ps), desc.Format);
    if (g.failed) return;
    g.back_format = desc.Format;
  }

  // Destination rectangle: the whole window, or the guest aspect ratio centred
  // in it when the runtime letterboxes (present_letterbox, default on).
  Rect dst{0, 0, ow, oh};
  if (rex::cvar::GetFlagByName("present_letterbox") != "false") {
    const uint64_t lhs = uint64_t(ow) * gh, rhs = uint64_t(oh) * gw;
    if (lhs > rhs) {  // window wider than the image: pillarbox
      dst.w = std::max<UINT>(1, UINT(rhs / gh));
      dst.x = (ow - dst.w) / 2;
    } else if (lhs < rhs) {  // window taller: letterbox
      dst.h = std::max<UINT>(1, UINT(lhs / gw));
      dst.y = (oh - dst.h) / 2;
    }
  }
  const bool bars = dst.w != ow || dst.h != oh;

  const bool smaa = REXCVAR_GET(cass_smaa);
  // EASU is an upscaler: only when the destination is larger than the image. At
  // equal size (e.g. resolution_scale 2 on 2560x1440) or when downsampling
  // (resolution_scale 3), the bilinear pass copies / filters down and RCAS
  // still sharpens.
  const bool easu = REXCVAR_GET(cass_fsr_easu) && gw < dst.w && gh < dst.h;
  if (!MakeTex(g.inter, ow, oh, kColor, true)) return;
  if (smaa && (!MakeTex(g.edges, gw, gh, DXGI_FORMAT_R8G8_UNORM, true) ||
               !MakeTex(g.weights, gw, gh, DXGI_FORMAT_R8G8B8A8_UNORM, true) ||
               !MakeTex(g.smaa_out, gw, gh, kColor, true))) {
    return;
  }

  Frame fr{g.frame};
  g.frame = (g.frame + 1) % kFrames;
  if (g.frame_fence[fr.f] && g.fence->GetCompletedValue() < g.frame_fence[fr.f]) {
    g.fence->SetEventOnCompletion(g.frame_fence[fr.f], g.fence_event);
    if (WaitForSingleObject(g.fence_event, 1000) != WAIT_OBJECT_0 &&
        g.fence->GetCompletedValue() < g.frame_fence[fr.f]) {
      // Resetting an allocator the GPU still uses would remove the device.
      return Skip(7, "GPU did not finish an earlier FSR frame within 1 s - frame skipped");
    }
  }
  g.alloc[fr.f]->Reset();
  g.list->Reset(g.alloc[fr.f].Get(), nullptr);

  // 1. SMAA 1x on the guest output (guest size).
  ID3D12Resource* src = guest.Get();
  const Rect guest_rect{0, 0, gw, gh};
  uint32_t k[16] = {};
  if (smaa) {
    const float metrics[4] = {1.0f / gw, 1.0f / gh, float(gw), float(gh)};
    std::memcpy(k, metrics, sizeof(metrics));
    OwnPass(fr, g.edge.Get(), g.edges, guest_rect, fr.Table(src), k, true);
    OwnPass(fr, g.weight.Get(), g.weights, guest_rect,
            fr.Table(g.edges.res.Get(), g.area.res.Get(), g.search.res.Get()), k, true);
    OwnPass(fr, g.blend.Get(), g.smaa_out, guest_rect, fr.Table(src, g.weights.res.Get()), k,
            false);
    src = g.smaa_out.res.Get();
  }
  // 2. Resample into the destination rectangle of the window-sized intermediate.
  std::memset(k, 0, sizeof(k));
  if (easu) {
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    FsrEasuCon(reinterpret_cast<AU1*>(&k[0]), reinterpret_cast<AU1*>(&k[4]),
               reinterpret_cast<AU1*>(&k[8]), reinterpret_cast<AU1*>(&k[12]), AF1(gw), AF1(gh),
               AF1(sd.Width), AF1(sd.Height), AF1(dst.w), AF1(dst.h));
    // EASU maps SV_Position (absolute in the target) to the input; shift that
    // mapping by the destination offset so a letterboxed rectangle starts at 0.
    if (dst.x || dst.y) {
      float c0[4];
      std::memcpy(c0, k, sizeof(c0));
      c0[2] -= c0[0] * float(dst.x);
      c0[3] -= c0[1] * float(dst.y);
      std::memcpy(k, c0, sizeof(c0));
    }
    OwnPass(fr, g.easu.Get(), g.inter, dst, fr.Table(src), k, bars);
  } else {
    OwnPass(fr, g.bilinear.Get(), g.inter, dst, fr.Table(src), k, bars);
  }
  // 3. RCAS sharpening of the whole intermediate into the back buffer.
  std::memset(k, 0, sizeof(k));
  const double sharpness = std::clamp(REXCVAR_GET(cass_fsr_sharpness), 0.0, 2.0);
  FsrRcasCon(reinterpret_cast<AU1*>(&k[0]), AF1(static_cast<float>(sharpness)));
  auto b = Transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  g.list->ResourceBarrier(1, &b);
  Pass(fr, g.rcas.Get(), back.Get(), Rect{0, 0, ow, oh}, fr.Table(g.inter.res.Get()), k, false);
  b = Transition(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  g.list->ResourceBarrier(1, &b);
  if (FAILED(g.list->Close())) {
    Fail("command list Close", E_FAIL);
    return;
  }
  ID3D12CommandList* lists[] = {g.list.Get()};
  g.queue->ExecuteCommandLists(1, lists);
  g.queue->Signal(g.fence.Get(), ++g.fence_value);
  g.frame_fence[fr.f] = g.fence_value;

  static uint64_t last_mode = ~0ull;
  const uint64_t mode = (smaa ? 1ull : 0ull) | (easu ? 2ull : 0ull) | (uint64_t(gw) << 2) |
                        (uint64_t(gh) << 18) | (uint64_t(ow) << 34) | (uint64_t(oh) << 49);
  if (last_mode != mode) {
    last_mode = mode;
    REXLOG_INFO("FSR active: {}{} {}x{} -> {}x{} (window {}x{}), RCAS sharpness {:.2f} stops",
                smaa ? "SMAA + " : "", easu ? "EASU" : (gw == dst.w ? "copy" : "bilinear"), gw,
                gh, dst.w, dst.h, ow, oh, sharpness);
  }
}

void NoteHookEntered(const char* which) {
  static std::atomic<int> once{1};
  if (once.fetch_sub(1) > 0) {
    REXLOG_INFO("FSR {} hook reached by a swap chain", which);
  }
}

// cass_vsync: sync interval 1 and no tearing flag (DXGI rejects ALLOW_TEARING with interval > 0).
void ApplyVsync(UINT& sync, UINT& flags) {
  if (!REXCVAR_GET(cass_vsync)) return;
  if (sync == 0) sync = 1;
  flags &= ~DXGI_PRESENT_ALLOW_TEARING;
}

HRESULT STDMETHODCALLTYPE HookPresentChain(IDXGISwapChain* sc, UINT sync, UINT flags) {
  NoteHookEntered("Present");
  ApplyVsync(sync, flags);
  const auto orig = OrigOf<PresentFn>(sc, 8);
  if (!orig) return DXGI_ERROR_INVALID_CALL;  // unreachable: only patched slots lead here
  const bool outer = !t_in_present;
  if (outer && !(flags & DXGI_PRESENT_TEST)) Apply(sc);
  t_in_present = true;
  const HRESULT hr = orig(sc, sync, flags);
  if (outer) t_in_present = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1Chain(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                            const DXGI_PRESENT_PARAMETERS* params) {
  NoteHookEntered("Present1");
  ApplyVsync(sync, flags);
  const auto orig = OrigOf<Present1Fn>(sc, 22);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const bool outer = !t_in_present;
  if (outer && !(flags & DXGI_PRESENT_TEST)) Apply(sc);
  t_in_present = true;
  const HRESULT hr = orig(sc, sync, flags, params);
  if (outer) t_in_present = false;
  return hr;
}

// Points vtable[index] at `hook`, remembering the previous function for that
// slot (FindOrig). `orig_out`, if given, receives it.
bool PatchSlot(void** vtable, size_t index, void* hook, void** orig_out = nullptr) {
  std::lock_guard lock(g_patch_mu);
  void** slot = &vtable[index];
  if (*slot == hook) {  // already ours
    if (orig_out) *orig_out = FindOrig(slot);
    return true;
  }
  SlotOrig* entry = nullptr;
  for (auto& e : g_slots) {
    if (!e.slot.load(std::memory_order_relaxed)) {
      entry = &e;
      break;
    }
  }
  if (!entry) return false;
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  entry->orig = *slot;
  entry->slot.store(slot, std::memory_order_release);  // published before the hook is live
  if (orig_out) *orig_out = entry->orig;
  *slot = hook;
  VirtualProtect(slot, sizeof(void*), old, &old);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
  return true;
}

// The swap chain the runtime actually creates is patched at creation, through
// hooks on the factory it is created with (a probe swap chain's vtable was
// measured in the reference port NOT to be the runtime's).
void PatchSwapChainObject(IUnknown* obj, IUnknown* dev, const char* how) {
  if (!obj) return;
  // For D3D12 the CreateSwapChain* "device" argument IS the command queue the
  // chain presents on - the only reliable place to learn it.
  ComPtr<ID3D12CommandQueue> queue;
  if (dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&queue)))) {
    std::lock_guard lock(g_queue_mu);
    g_chain_queue = queue;
  }
  ComPtr<IDXGISwapChain1> sc;
  if (FAILED(obj->QueryInterface(IID_PPV_ARGS(&sc)))) return;
  void** vtable = *reinterpret_cast<void***>(sc.Get());
  // IDXGISwapChain::Present is slot 8, IDXGISwapChain1::Present1 is slot 22.
  const bool a = PatchSlot(vtable, 8, reinterpret_cast<void*>(&HookPresentChain));
  const bool b = PatchSlot(vtable, 22, reinterpret_cast<void*>(&HookPresent1Chain));
  REXLOG_INFO("FSR attached to swap chain via {} (Present={} Present1={} queue={})", how, a, b,
              queue ? "captured" : "MISSING");
}

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                    const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                    IDXGIOutput*, IDXGISwapChain1**);
using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
                                                           const DXGI_SWAP_CHAIN_DESC1*,
                                                           IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* dev,
                                              DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
  const auto orig = OrigOf<CreateSwapChainFn>(f, 10);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const HRESULT hr = orig(f, dev, d, out);
  if (SUCCEEDED(hr) && out) PatchSwapChainObject(*out, dev, "CreateSwapChain");
  return hr;
}
HRESULT STDMETHODCALLTYPE HookCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w,
                                            const DXGI_SWAP_CHAIN_DESC1* d,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                            IDXGIOutput* o, IDXGISwapChain1** out) {
  const auto orig = OrigOf<CreateForHwndFn>(f, 15);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const HRESULT hr = orig(f, dev, w, d, fs, o, out);
  if (SUCCEEDED(hr) && out) PatchSwapChainObject(*out, dev, "CreateSwapChainForHwnd");
  return hr;
}
HRESULT STDMETHODCALLTYPE HookCreateForComposition(IDXGIFactory2* f, IUnknown* dev,
                                                   const DXGI_SWAP_CHAIN_DESC1* d,
                                                   IDXGIOutput* o, IDXGISwapChain1** out) {
  const auto orig = OrigOf<CreateForCompositionFn>(f, 24);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const HRESULT hr = orig(f, dev, d, o, out);
  if (SUCCEEDED(hr) && out) PatchSwapChainObject(*out, dev, "CreateSwapChainForComposition");
  return hr;
}

void PatchFactoryObject(IUnknown* obj) {
  ComPtr<IDXGIFactory2> f;
  if (!obj || FAILED(obj->QueryInterface(IID_PPV_ARGS(&f)))) return;
  void** vt = *reinterpret_cast<void***>(f.Get());
  // IDXGIFactory::CreateSwapChain 10, IDXGIFactory2::CreateSwapChainForHwnd 15,
  // IDXGIFactory2::CreateSwapChainForComposition 24.
  PatchSlot(vt, 10, reinterpret_cast<void*>(&HookCreateSwapChain));
  PatchSlot(vt, 15, reinterpret_cast<void*>(&HookCreateForHwnd));
  PatchSlot(vt, 24, reinterpret_cast<void*>(&HookCreateForComposition));
}

using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
CreateFactory2Fn g_real_factory2 = nullptr;
CreateFactory1Fn g_real_factory1 = nullptr;

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
  const HRESULT hr = g_real_factory2(flags, riid, out);
  if (SUCCEEDED(hr) && out) PatchFactoryObject(static_cast<IUnknown*>(*out));
  return hr;
}
HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out) {
  const HRESULT hr = g_real_factory1(riid, out);
  if (SUCCEEDED(hr) && out) PatchFactoryObject(static_cast<IUnknown*>(*out));
  return hr;
}

// Rewrites `module`'s import of dxgi.dll!`name` to `hook`; returns whether found.
bool PatchImport(HMODULE module, const char* name, void* hook) {
  if (!module) return false;
  auto* base = reinterpret_cast<uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "dxgi.dll") != 0) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) != 0) continue;
      DWORD old = 0;
      if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
      thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
      VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
      return true;
    }
  }
  return false;
}

// --- Guest-output source: D3D12Presenter::RefreshGuestOutputImpl ------------

// bool D3D12Presenter::RefreshGuestOutputImpl(uint32_t mailbox_index,
//     uint32_t frontbuffer_width, uint32_t frontbuffer_height,
//     std::function<bool(GuestOutputRefreshContext&)> refresher,  (by address)
//     bool& is_8bpc_out_ref)
using RefreshFn = bool (*)(void* self, uint32_t mailbox_index, uint32_t frontbuffer_w,
                           uint32_t frontbuffer_h, void* refresher, bool* is_8bpc_out);
RefreshFn g_refresh_orig = nullptr;

// ABI note: `refresher` is a std::function passed BY VALUE. In the Microsoft x64
// C++ ABI the caller builds that temporary and passes its address, and the
// CALLEE destroys it. So this hook owns `*refresher` and must destroy it, and the
// original destroys the `wrapped` object we hand it - which is therefore built
// in raw storage here, not as a local that would be destroyed a second time.
bool HookRefresh(void* self, uint32_t mailbox_index, uint32_t frontbuffer_w,
                 uint32_t frontbuffer_h, void* refresher, bool* is_8bpc_out) {
  using Refresher = std::function<bool(rex::ui::Presenter::GuestOutputRefreshContext&)>;
  Refresher* inner = static_cast<Refresher*>(refresher);
  alignas(Refresher) unsigned char storage[sizeof(Refresher)];
  Refresher* wrapped = ::new (static_cast<void*>(storage))
      Refresher([inner, frontbuffer_w,
                 frontbuffer_h](rex::ui::Presenter::GuestOutputRefreshContext& c) {
        const bool ok = (*inner)(c);
        if (ok) {
          CassFsrNoteGuestOutput(
              static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(c)
                  .resource_uav_capable(),
              frontbuffer_w, frontbuffer_h);
        }
        return ok;
      });
  const bool result =
      g_refresh_orig(self, mailbox_index, frontbuffer_w, frontbuffer_h, wrapped, is_8bpc_out);
  // `wrapped` was destroyed by the original (callee-destroys); `inner` is ours.
  std::destroy_at(inner);
  return result;
}

void InstallRefreshHook() {
  HMODULE mod = GetModuleHandleA("rexruntime.dll");
  if (!mod) {
    REXLOG_WARN("FSR: rexruntime.dll not loaded - no guest-output hook");
    return;
  }
  auto** vtable =
      reinterpret_cast<void**>(GetProcAddress(mod, "??_7D3D12Presenter@d3d12@ui@rex@@6B@"));
  void* refresh = reinterpret_cast<void*>(
      GetProcAddress(mod,
                     "?RefreshGuestOutputImpl@D3D12Presenter@d3d12@ui@rex@@MEAA_"
                     "NIIIV?$function@$$A6A_NAEAVGuestOutputRefreshContext@"
                     "Presenter@ui@rex@@@Z@std@@AEA_N@Z"));
  if (!vtable || !refresh) {
    REXLOG_WARN("FSR: presenter exports missing (vtable={} refresh={}) - no guest-output hook",
                static_cast<void*>(vtable), refresh);
    return;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(vtable, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    REXLOG_WARN("FSR: presenter vtable unreadable");
    return;
  }
  const auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  constexpr size_t kScanSlots = 96;
  for (size_t i = 0; i < kScanSlots; ++i) {
    if (reinterpret_cast<uintptr_t>(vtable + i + 1) > region_end) break;
    if (vtable[i] == reinterpret_cast<void*>(&HookRefresh)) return;  // already ours
    if (vtable[i] != refresh) continue;
    void* orig = nullptr;
    if (!PatchSlot(vtable, i, reinterpret_cast<void*>(&HookRefresh), &orig)) {
      REXLOG_WARN("FSR: presenter vtable slot {} unprotect failed (gle={})", i, GetLastError());
      return;
    }
    g_refresh_orig = reinterpret_cast<RefreshFn>(orig);
    REXLOG_INFO("FSR guest-output hook on D3D12Presenter vtable[{}]", i);
    return;
  }
  REXLOG_WARN("FSR: RefreshGuestOutputImpl not found in the presenter vtable (another hook "
              "owns it?) - that hook must call CassFsrNoteGuestOutput");
}

}  // namespace

extern "C" void CassFsrNoteGuestOutput(ID3D12Resource* resource, uint32_t width,
                                       uint32_t height) {
  std::lock_guard lock(g_guest_mu);
  if (g_shutdown.load(std::memory_order_acquire)) return;  // checked under the lock ShutdownFsr takes
  if (g_guest.Get() != resource || g_guest_w != width || g_guest_h != height) {
    static std::atomic<int> budget{4};
    if (resource && budget.fetch_sub(1) > 0) {
      const D3D12_RESOURCE_DESC d = resource->GetDesc();
      REXLOG_INFO("FSR guest output {}x{} (texture {}x{} format {})", width, height, d.Width,
                  d.Height, static_cast<int>(d.Format));
    }
  }
  g_guest = resource;
  g_guest_w = width;
  g_guest_h = height;
}

namespace rex::glue {

void SetFsrImGuiDrawer(rex::ui::ImGuiDrawer* drawer) {
  g_imgui.store(drawer, std::memory_order_release);
}

void ShutdownFsr() {
  g_shutdown.store(true, std::memory_order_release);
  g_imgui.store(nullptr, std::memory_order_release);
  {
    std::lock_guard lock(g_state_mu);
    WaitAll();  // our last passes may still read the guest output / write a back buffer
    if (g.fence_event) CloseHandle(g.fence_event);
    g = State{};
  }
  {
    std::lock_guard lock(g_guest_mu);
    g_guest.Reset();
    g_guest_w = g_guest_h = 0;
  }
  std::lock_guard lock(g_queue_mu);
  g_chain_queue.Reset();
}

void InstallFsr() {
  static bool done = false;
  if (done) return;
  done = true;
  HMODULE dxgi = LoadLibraryA("dxgi.dll");
  if (!dxgi) {
    REXLOG_WARN("FSR: dxgi.dll missing, not installed");
    return;
  }
  // Attach to the swap chain the runtime really creates: hook factory creation
  // in each module that may create the presenter's factory.
  g_real_factory2 =
      reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
  g_real_factory1 =
      reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  int imports = 0;
  const HMODULE modules[] = {GetModuleHandleA("rexruntime.dll"),
                             GetModuleHandleA("rexgpu-xenos.dll"), GetModuleHandleA(nullptr)};
  for (HMODULE m : modules) {
    if (g_real_factory2) {
      imports += PatchImport(m, "CreateDXGIFactory2",
                             reinterpret_cast<void*>(&HookCreateDXGIFactory2));
    }
    if (g_real_factory1) {
      imports += PatchImport(m, "CreateDXGIFactory1",
                             reinterpret_cast<void*>(&HookCreateDXGIFactory1));
    }
  }
  REXLOG_INFO("FSR factory import hooks installed: {}", imports);
  // Backstop: DXGI factories share one vtable, so patching a throwaway factory's
  // also covers a factory created through a path the import hooks miss.
  if (g_real_factory2) {
    ComPtr<IDXGIFactory2> factory;
    if (SUCCEEDED(g_real_factory2(0, IID_PPV_ARGS(&factory)))) {
      PatchFactoryObject(factory.Get());
    }
  }
  InstallRefreshHook();
}

}  // namespace rex::glue
