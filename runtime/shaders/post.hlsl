// Full-screen passes of the SDL3 GPU renderer (rt_gpu.c), compiled with ge.hlsl by
// tools/build_shaders.py into runtime/gpu_shaders.h:
//   - copy: a render target's resolved colour back into its multisampled colour (after a copy or
//     an upload changed the resolved texture), with point sampling;
//   - box: an integer-ratio box filter, the internal resolution down to the PSP's 480x272 when the
//     CPU (screenshots, recordings, comparisons) reads what the GPU drew;
//   - fxaa: edge anti-aliasing on the luma of the picture, in the spirit of FXAA;
//   - cas: contrast-adaptive sharpening (the idea of AMD FidelityFX CAS: sharpen less where the
//     neighbourhood already has contrast);
//   - present: the picture into the window (bilinear, or point sampling, or an area filter when the
//     internal resolution is larger than the window), as SDR or as HDR (scRGB or HDR10/ST 2084)
//     with inverse tone mapping of the SDR picture.
// Bindings follow SDL_gpu.h: fragment sampler set 2 / (t0, s0, space2), fragment uniforms set 3 / (b0, space3).

struct VSOut {
    float4 pos : SV_Position;
    float2 t : TEXCOORD0;  // 0..1 across the viewport
};

// one triangle covering the viewport
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.t = p;
    return o;
}

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> u_tex : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState u_smp : register(s0, space2);

[[vk::binding(0, 3)]] cbuffer PostUniforms : register(b0, space3) {
    float4 u_src;    // source rectangle in texels: x0, y0, x1, y1
    float4 u_size;   // source texture: width, height, 1/width, 1/height
    uint4 u_mode;    // x: pass, y: box ratio / present taps, z: output (0 SDR, 1 scRGB, 2 HDR10)
    float4 u_p0;     // cas: sharpness; present: paper white (nits), peak (nits), knee
    float4 u_p1;     // present: source texels per window pixel
};

#define P_COPY 0u
#define P_BOX 1u
#define P_FXAA 2u
#define P_CAS 3u
#define P_PRESENT 4u

float3 tap(float2 texel) { return u_tex.SampleLevel(u_smp, texel * u_size.zw, 0).rgb; }
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 box(float2 t) {
    uint n = u_mode.y;
    float2 o = u_src.xy + floor(t * (u_src.zw - u_src.xy) / n) * n;
    float3 s = 0;
    for (uint j = 0; j < n; j++)
        for (uint i = 0; i < n; i++) s += tap(o + float2(i, j) + 0.5);
    return s / (n * n);
}

// edge search along the direction of the strongest luma change, then a blend across the edge
float3 fxaa(float2 p) {
    float3 c = tap(p);
    float lc = luma(c), ln = luma(tap(p + float2(0, -1))), ls = luma(tap(p + float2(0, 1)));
    float lw = luma(tap(p + float2(-1, 0))), le = luma(tap(p + float2(1, 0)));
    float lmin = min(lc, min(min(ln, ls), min(lw, le))), lmax = max(lc, max(max(ln, ls), max(lw, le)));
    float range = lmax - lmin;
    if (range < max(0.0312, lmax * 0.125)) return c;
    float lnw = luma(tap(p + float2(-1, -1))), lne = luma(tap(p + float2(1, -1)));
    float lsw = luma(tap(p + float2(-1, 1))), lse = luma(tap(p + float2(1, 1)));
    float horiz = abs(lnw + lne - 2 * ln) + 2 * abs(lw + le - 2 * lc) + abs(lsw + lse - 2 * ls);
    float vert = abs(lnw + lsw - 2 * lw) + 2 * abs(ln + ls - 2 * lc) + abs(lne + lse - 2 * le);
    bool is_h = horiz >= vert;  // the edge runs horizontally: blend vertically
    float l1 = is_h ? ln : lw, l2 = is_h ? ls : le;
    float g1 = abs(l1 - lc), g2 = abs(l2 - lc);
    float step_ = g1 >= g2 ? -1.0 : 1.0;
    float lside = g1 >= g2 ? l1 : l2, grad = max(g1, g2) * 0.25, lavg = 0.5 * (lc + lside);
    float2 n = is_h ? float2(0, step_ * 0.5) : float2(step_ * 0.5, 0), d = is_h ? float2(1, 0) : float2(0, 1);
    float2 pa = p + n - d, pb = p + n + d;
    float ea = luma(tap(pa)) - lavg, eb = luma(tap(pb)) - lavg;
    [unroll] for (int k = 0; k < 8; k++) {
        bool da = abs(ea) >= grad, db = abs(eb) >= grad;
        if (da && db) break;
        float s = k < 3 ? 1.0 : 2.0;
        if (!da) { pa -= d * s; ea = luma(tap(pa)) - lavg; }
        if (!db) { pb += d * s; eb = luma(tap(pb)) - lavg; }
    }
    float dista = is_h ? p.x - pa.x : p.y - pa.y, distb = is_h ? pb.x - p.x : pb.y - p.y;
    bool near_a = dista < distb;
    float e = near_a ? ea : eb;
    float off = ((lc - lavg < 0) != (e < 0)) ? 0.5 - min(dista, distb) / (dista + distb) : 0.0;
    float sub = saturate(abs((2 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12 - lc) / range);
    sub = sub * sub * (3 - 2 * sub);
    off = max(off, sub * sub * 0.75);
    return tap(p + n * 2 * off);
}

float3 cas(float2 p) {
    float3 a = tap(p + float2(-1, -1)), b = tap(p + float2(0, -1)), c = tap(p + float2(1, -1));
    float3 d = tap(p + float2(-1, 0)), e = tap(p), f = tap(p + float2(1, 0));
    float3 g = tap(p + float2(-1, 1)), h = tap(p + float2(0, 1)), i = tap(p + float2(1, 1));
    float3 mn = min(min(min(d, e), min(f, b)), h), mx = max(max(max(d, e), max(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));
    mx += max(mx, max(max(a, c), max(g, i)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));
    float3 w = -amp / lerp(8.0, 5.0, saturate(u_p0.x));
    return saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
}

float3 srgb_to_linear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }

// SDR white goes to paper white; luminance above the knee is lifted smoothly toward the peak
float3 inverse_tonemap(float3 c) {
    float3 lin = srgb_to_linear(saturate(c));
    float y = dot(lin, float3(0.2126, 0.7152, 0.0722)), knee = u_p0.z;
    float t = saturate((y - knee) / (1.0 - knee));
    float gain = 1.0 + (u_p0.y / u_p0.x - 1.0) * t * t;
    return lin * gain * u_p0.x;  // nits
}

float3 pq(float3 nits) {
    float3 l = pow(saturate(nits / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * l) / (1.0 + 18.6875 * l), 78.84375);
}

float4 present(float2 t) {
    float2 p = u_src.xy + t * (u_src.zw - u_src.xy);
    float3 c = 0;
    uint n = u_mode.y;  // taps per axis: more than one when the picture is made smaller (u_p1: texels per pixel)
    for (uint j = 0; j < n; j++)
        for (uint i = 0; i < n; i++) c += tap(p + u_p1.xy * (float2(i + 0.5, j + 0.5) / n - 0.5));
    c /= n * n;
    if (u_mode.z == 0u) return float4(c, 1.0);
    float3 nits = inverse_tonemap(c);
    if (u_mode.z == 1u) return float4(nits / 80.0, 1.0);  // scRGB: 1.0 = 80 nits, BT.709 primaries
    float3x3 to2020 = float3x3(0.6274040, 0.3292820, 0.0433136,
                               0.0690970, 0.9195400, 0.0113612,
                               0.0163916, 0.0880132, 0.8955950);
    return float4(pq(mul(to2020, nits)), 1.0);
}

float4 fs_main(VSOut i) : SV_Target {
    float2 t = i.t;
    float2 p = u_src.xy + t * (u_src.zw - u_src.xy);  // texel position
    switch (u_mode.x) {
    case P_BOX: return float4(box(t), 1.0);
    case P_FXAA: return float4(fxaa(p), 1.0);
    case P_CAS: return float4(cas(p), 1.0);
    case P_PRESENT: return present(t);
    default: return u_tex.SampleLevel(u_smp, p * u_size.zw, 0);  // copy, with alpha
    }
}
