// GE pixel pipeline for the SDL3 GPU renderer (rt_gpu.c). One source for every backend:
// tools/build_shaders.py compiles it to SPIR-V (Vulkan) and DXIL (D3D12) into runtime/gpu_shaders.h.
// Resource bindings follow SDL_gpu.h: vertex uniforms set 1 / (b0, space1); fragment sampler
// set 2 / (t0, s0, space2); fragment uniforms set 3 / (b0, space3).
//
// Vertices arrive already transformed and lit by the CPU (rt_gesoft.c), in PSP screen space:
// x, y in pixels of the render target, z 0..65535, w = clip w (1 in through mode). As on the
// PSP, colours and fog are interpolated linearly in screen space and texture coordinates
// with perspective correction.

[[vk::binding(0, 1)]] cbuffer VertexUniforms : register(b0, space1) {
    float4 u_screen;  // 2/width, 2/height
};

struct VSIn {
    float4 pos : TEXCOORD0;
    float3 uvq : TEXCOORD1;
    float4 c0 : TEXCOORD2;
    float4 c1 : TEXCOORD3;
    float fog : TEXCOORD4;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 uvq : TEXCOORD0;
    noperspective float4 c0 : COLOR0;
    noperspective float4 c1 : COLOR1;
    noperspective float fog : TEXCOORD1;
};

VSOut vs_main(VSIn i) {
    VSOut o;
    float w = i.pos.w;
    float x = i.pos.x * u_screen.x - 1.0;
    float y = 1.0 - i.pos.y * u_screen.y;
    o.pos = float4(x * w, y * w, (i.pos.z / 65535.0) * w, w);
    o.uvq = i.uvq;
    o.c0 = i.c0;
    o.c1 = i.c1;
    o.fog = i.fog;
    return o;
}

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> u_tex : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState u_smp : register(s0, space2);

[[vk::binding(0, 3)]] cbuffer FragmentUniforms : register(b0, space3) {
    uint4 u_mode;      // x: flags, y: texture function, z: alpha-test function, w: alpha ref | mask << 8
    float4 u_env;      // texture environment colour
    float4 u_fogcol;
    float4 u_fixa;     // source factor applied here when both blend factors are fixed colours
    float4 u_zrange;   // x, y: min/max depth (0..1)
};

#define F_TEX 1u
#define F_RGBA 2u
#define F_DOUBLE 4u
#define F_PROJ 8u
#define F_CLEAR 16u
#define F_FOG 32u
#define F_ZRANGE 64u
#define F_SRCFIX 128u
#define F_TEXOPAQUE 256u  // texture from a 565 render target: no alpha

bool alpha_test(uint func, uint a, uint ref) {
    switch (func) {
    case 0: return false;
    case 2: return a == ref;
    case 3: return a != ref;
    case 4: return a < ref;
    case 5: return a <= ref;
    case 6: return a > ref;
    case 7: return a >= ref;
    default: return true;
    }
}

float4 fs_main(VSOut i) : SV_Target {
    uint flags = u_mode.x;
    if ((flags & F_ZRANGE) != 0 && (i.pos.z < u_zrange.x || i.pos.z > u_zrange.y)) discard;
    float4 c = i.c0;
    if ((flags & F_CLEAR) != 0) return c;
    if ((flags & F_TEX) != 0) {
        float2 uv = (flags & F_PROJ) != 0 ? i.uvq.xy / i.uvq.z : i.uvq.xy;
        float4 t = u_tex.Sample(u_smp, uv);
        if ((flags & F_TEXOPAQUE) != 0) t.a = 1.0;
        float k = (flags & F_DOUBLE) != 0 ? 2.0 : 1.0;
        bool rgba = (flags & F_RGBA) != 0;
        float3 rgb;
        float a = c.a;
        switch (u_mode.y) {
        case 0: rgb = c.rgb * t.rgb * k; if (rgba) a = c.a * t.a; break;                 // modulate
        case 1: rgb = (rgba ? lerp(c.rgb, t.rgb, t.a) : t.rgb) * k; break;                // decal
        case 2: rgb = lerp(c.rgb, u_env.rgb, t.rgb) * k; if (rgba) a = c.a * t.a; break;  // blend
        case 3: rgb = t.rgb * k; if (rgba) a = t.a; break;                                // replace
        default: rgb = (c.rgb + t.rgb) * k; if (rgba) a = c.a * t.a; break;              // add
        }
        c = float4(rgb, a);
    }
    c.rgb += i.c1.rgb;
    c = saturate(c);
    uint a8 = (uint)round(c.a * 255.0);
    if (!alpha_test(u_mode.z, a8 & (u_mode.w >> 8), u_mode.w & 255u)) discard;
    if ((flags & F_FOG) != 0) c.rgb = lerp(u_fogcol.rgb, c.rgb, saturate(i.fog));
    if ((flags & F_SRCFIX) != 0) c.rgb *= u_fixa.rgb;
    return c;
}
