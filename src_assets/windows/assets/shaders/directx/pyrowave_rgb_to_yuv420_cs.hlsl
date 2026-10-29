// Canonical gamma-encoded RGB from BGRA/RGBA UNORM SRV, never an sRGB view.
// No filtering, gamma transform, scaling or rotation. Positive even sizes required.
// Centered 2x2 RGB mean matches native CHROMA_SITING_CENTER=0, not legacy left siting.
Texture2D<float4> source : register(t0);
RWTexture2D<float> plane_y : register(u0);
RWTexture2D<float> plane_u : register(u1);
RWTexture2D<float> plane_v : register(u2);
cbuffer range_params : register(b0) {
  uint full_range;
  uint3 padding;
};

// Explicit nearest code rounding (half up), clamp before R8 UNORM storage.
float quantize(float code) {
  return floor(clamp(code, 0.0, 255.0) + 0.5) / 255.0;
}

float luma(float3 rgb) {
  return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main_cs(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  plane_y.GetDimensions(width, height);
  uint2 p = id.xy * 2u;
  if (p.x >= width || p.y >= height) return;
  float3 a = source.Load(int3(p, 0)).rgb;
  float3 b = source.Load(int3(p + uint2(1, 0), 0)).rgb;
  float3 c = source.Load(int3(p + uint2(0, 1), 0)).rgb;
  float3 d = source.Load(int3(p + uint2(1, 1), 0)).rgb;
  // Limited: Y=16+219Y, UV=128+224C; full: Y=255Y, UV=128+255C.
  float y_scale = full_range != 0u ? 255.0 : 219.0;
  float y_offset = full_range != 0u ? 0.0 : 16.0;
  float uv_scale = full_range != 0u ? 255.0 : 224.0;
  plane_y[p] = quantize(y_offset + y_scale * luma(a));
  plane_y[p + uint2(1, 0)] = quantize(y_offset + y_scale * luma(b));
  plane_y[p + uint2(0, 1)] = quantize(y_offset + y_scale * luma(c));
  plane_y[p + uint2(1, 1)] = quantize(y_offset + y_scale * luma(d));
  float3 mean = (a + b + c + d) * 0.25;
  float y = luma(mean);
  plane_u[id.xy] = quantize(128.0 + uv_scale * (mean.b - y) / 1.8556);
  plane_v[id.xy] = quantize(128.0 + uv_scale * (mean.r - y) / 1.5748);
}
