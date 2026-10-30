// Adapted from gowj (Gears of War: Judgment port) src/fsr/gowj_fsr.hlsl (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
// Reference source of the precompiled src/fsr/g_*.h shaders (same bytecode as the reference port).
// cass - AMD FidelityFX Super Resolution 1.0 (EASU + RCAS) as pixel shaders.
// FSR source: cass/third_party/fsr1 (MIT, Copyright (c) 2021 Advanced Micro Devices).
// Compiled at build time with fxc (precompiled into src/fsr/g_*.h; fxc /T ps_5_0 or vs_5_0 /E <entry> /Vn g_<name>), entry points:
//   VSMain  - full-screen triangle
//   PSEasu  - edge-adaptive upsampling of the guest output to the target size
//   PSRcas  - robust contrast-adaptive sharpening of the upsampled image

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

cbuffer FsrConstants : register(b0) {
  uint4 Const0;
  uint4 Const1;
  uint4 Const2;
  uint4 Const3;
};

Texture2D<AF4> Source : register(t0);
SamplerState LinearClamp : register(s0);

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return Source.GatherRed(LinearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return Source.GatherGreen(LinearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return Source.GatherBlue(LinearClamp, p); }

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return Source.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
  float2 uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSEasu(float4 pos : SV_Position) : SV_Target {
  AF3 c;
  FsrEasuF(c, AU2(pos.xy), Const0, Const1, Const2, Const3);
  return float4(c, 1.0);
}

float4 PSRcas(float4 pos : SV_Position) : SV_Target {
  AF3 c;
  FsrRcasF(c.r, c.g, c.b, AU2(pos.xy), Const0);
  return float4(c, 1.0);
}
