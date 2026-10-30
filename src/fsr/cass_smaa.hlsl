// Adapted from gowj (Gears of War: Judgment port) src/fsr/gowj_smaa.hlsl (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
// Reference source of the precompiled src/fsr/g_*.h shaders (same bytecode as the reference port).
// cass - SMAA 1x (Jimenez et al., MIT) on the guest output, before FSR EASU.
// Source: cass/third_party/smaa/SMAA.hlsl. Passes:
//   EdgeVS/EdgePS     - luma edge detection            -> edges  (R8G8)
//   WeightVS/WeightPS - blending weight calculation     -> blend  (R8G8B8A8)
//   BlendVS/BlendPS   - neighborhood blending of color  -> output (color format)
// Plus BilinearPS: a plain linear resample, used when EASU is off.

cbuffer SmaaConstants : register(b0) {
  float4 RtMetrics;  // (1/w, 1/h, w, h) of the guest output
};

#define SMAA_RT_METRICS RtMetrics
#define SMAA_HLSL_4_1 1
#define SMAA_PRESET_ULTRA 1
#include "SMAA.hlsl"

Texture2D Tex0 : register(t0);
Texture2D Tex1 : register(t1);
Texture2D Tex2 : register(t2);

void FullScreen(uint id, out float4 pos, out float2 uv) {
  uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

struct EdgeV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; };
EdgeV EdgeVS(uint id : SV_VertexID) {
  EdgeV v;
  FullScreen(id, v.pos, v.uv);
  float4 o[3];
  SMAAEdgeDetectionVS(v.uv, o);
  v.o0 = o[0]; v.o1 = o[1]; v.o2 = o[2];
  return v;
}
float2 EdgePS(EdgeV v) : SV_Target {
  float4 o[3] = {v.o0, v.o1, v.o2};
  return SMAALumaEdgeDetectionPS(v.uv, o, Tex0);
}

struct WeightV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };
WeightV WeightVS(uint id : SV_VertexID) {
  WeightV v;
  FullScreen(id, v.pos, v.uv);
  float4 o[3];
  SMAABlendingWeightCalculationVS(v.uv, v.pix, o);
  v.o0 = o[0]; v.o1 = o[1]; v.o2 = o[2];
  return v;
}
float4 WeightPS(WeightV v) : SV_Target {
  float4 o[3] = {v.o0, v.o1, v.o2};
  return SMAABlendingWeightCalculationPS(v.uv, v.pix, o, Tex0, Tex1, Tex2, float4(0, 0, 0, 0));
}

struct BlendV { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
BlendV BlendVS(uint id : SV_VertexID) {
  BlendV v;
  FullScreen(id, v.pos, v.uv);
  SMAANeighborhoodBlendingVS(v.uv, v.o);
  return v;
}
float4 BlendPS(BlendV v) : SV_Target {
  return SMAANeighborhoodBlendingPS(v.uv, v.o, Tex0, Tex1);
}

struct PlainV { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
PlainV PlainVS(uint id : SV_VertexID) {
  PlainV v;
  FullScreen(id, v.pos, v.uv);
  return v;
}
float4 BilinearPS(PlainV v) : SV_Target {
  return Tex0.SampleLevel(LinearSampler, v.uv, 0);
}
