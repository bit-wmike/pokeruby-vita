/*
 * "HQ" display style: an edge-smoothing filter on the GPU.
 *
 * For every screen pixel the shader looks at the GBA pixel under it and at
 * the two neighbours towards the screen pixel's corner of it (across and
 * down). Where those two are alike, but the pixel itself and the opposite
 * neighbours differ - the corner of a diagonal edge - the corner is blended
 * towards them, the more the closer the screen pixel is to the corner. Flat
 * areas and straight edges stay sharp; stairs of diagonals get smooth (like
 * Scale2x/hq2x on emulators, at any scale: 2x wide view, 3x menus).
 *
 * The shaders are compiled at start-up with the system's Cg compiler
 * (ur0:data/libshacccg.suprx, the file many Vita ports need). Without it
 * the style is not available and the picture stays pixel exact.
 */
#include <stdlib.h>
#include <string.h>

#include <psp2/gxm.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/shacccg.h>
#include <vita2d.h>

#include "vita_hq.h"

void Platform_Log(const char *fmt, ...);

static const char sVertexSrc[] =
    "void main(float3 aPosition, float2 aTexcoord, uniform float4x4 wvp,\n"
    "          out float4 vPosition : POSITION, out float2 vTexcoord : TEXCOORD0)\n"
    "{\n"
    "    vPosition = mul(float4(aPosition, 1.0f), wvp);\n"
    "    vTexcoord = aTexcoord;\n"
    "}\n";

static const char sFragmentSrc[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex : TEXUNIT0,\n"
    "            uniform float2 texSize) : COLOR\n"
    "{\n"
    "    float2 ps = 1.0f / texSize;\n"
    "    float2 p = vTexcoord * texSize;\n"
    "    float2 t = floor(p);\n"
    "    float2 f = p - t;\n"
    "    float2 d = float2(f.x >= 0.5f ? 1.0f : -1.0f, f.y >= 0.5f ? 1.0f : -1.0f);\n"
    "    float2 c0 = (t + 0.5f) * ps;\n"
    "    float3 C = tex2D(tex, c0).rgb;\n"
    "    float3 A = tex2D(tex, c0 + float2(d.x, 0.0f) * ps).rgb;\n"
    "    float3 B = tex2D(tex, c0 + float2(0.0f, d.y) * ps).rgb;\n"
    "    float3 Ao = tex2D(tex, c0 - float2(d.x, 0.0f) * ps).rgb;\n"
    "    float3 Bo = tex2D(tex, c0 - float2(0.0f, d.y) * ps).rgb;\n"
    "    float3 one = float3(1.0f, 1.0f, 1.0f);\n"
    "    float thr = 0.10f;\n"
    "    float e = (1.0f - step(thr, dot(abs(A - B), one))) * step(thr, dot(abs(B - Ao), one))\n"
    "            * step(thr, dot(abs(A - Bo), one)) * step(thr, dot(abs(C - A), one));\n"
    "    float2 g = abs(f - 0.5f) * 2.0f;\n"
    "    float w = saturate((g.x + g.y - 0.5f) * 1.6f) * e;\n"
    "    return float4(lerp(C, (A + B) * 0.5f, w), 1.0f);\n"
    "}\n";

static int sState;   /* 0 not tried, 1 ready, -1 not available */
static SceGxmShaderPatcherId sVid, sFid;
static SceGxmVertexProgram *sVp;
static SceGxmFragmentProgram *sFp;
static const SceGxmProgramParameter *sWvp, *sTexSize;
static SceShaccCgSourceFile sSource;

static SceShaccCgSourceFile *OpenFile(const char *fileName, const SceShaccCgSourceLocation *includedFrom,
                                      const SceShaccCgCompileOptions *compileOptions, const char **errorString)
{
    (void)fileName; (void)includedFrom; (void)compileOptions; (void)errorString;
    return &sSource;
}

static void *Compile(const char *text, int fragment)
{
    SceShaccCgCallbackList cb;
    SceShaccCgCompileOptions opt;
    const SceShaccCgCompileOutput *out;
    void *bin = NULL;
    int i;

    sSource.fileName = "<hq>";
    sSource.text = text;
    sSource.size = (SceUInt32)strlen(text);
    sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
    cb.openFile = OpenFile;
    sceShaccCgInitializeCompileOptions(&opt);
    opt.mainSourceFile = sSource.fileName;
    opt.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    opt.entryFunctionName = "main";
    opt.useFx = 1;
    opt.optimizationLevel = 3;
    opt.warningLevel = 0;
    out = sceShaccCgCompileProgram(&opt, &cb, 0);
    if (!out) {
        Platform_Log("[hq] compiler returned nothing\n");
        return NULL;
    }
    for (i = 0; i < out->diagnosticCount; i++)
        Platform_Log("[hq] %s shader: %s\n", fragment ? "fragment" : "vertex", out->diagnostics[i].message);
    if (out->programData && out->programSize) {
        bin = malloc(out->programSize);
        if (bin)
            memcpy(bin, out->programData, out->programSize);
    }
    sceShaccCgDestroyCompileOutput(out);
    return bin;
}

int VitaHq_Available(void)
{
    void *vbin, *fbin;
    SceGxmShaderPatcher *patcher;
    const SceGxmProgram *vp, *fp;
    const SceGxmProgramParameter *pos, *tc;
    SceGxmVertexAttribute attr[2];
    SceGxmVertexStream stream;
    int r;

    if (sState)
        return sState > 0;
    sState = -1;
    r = sceKernelLoadStartModule("ur0:data/libshacccg.suprx", 0, NULL, 0, NULL, NULL);
    if (r < 0) {
        Platform_Log("[hq] ur0:data/libshacccg.suprx not loaded (0x%08X): HQ not available\n", r);
        return 0;
    }
    sceShaccCgSetDefaultAllocator(malloc, free);
    vbin = Compile(sVertexSrc, 0);
    fbin = Compile(sFragmentSrc, 1);
    if (!vbin || !fbin || sceGxmProgramCheck(vbin) < 0 || sceGxmProgramCheck(fbin) < 0) {
        Platform_Log("[hq] shaders not compiled: HQ not available\n");
        return 0;
    }
    patcher = vita2d_get_shader_patcher();
    if (sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)vbin, &sVid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)fbin, &sFid) < 0) {
        Platform_Log("[hq] shaders not registered\n");
        return 0;
    }
    vp = sceGxmShaderPatcherGetProgramFromId(sVid);
    fp = sceGxmShaderPatcherGetProgramFromId(sFid);
    pos = sceGxmProgramFindParameterByName(vp, "aPosition");
    tc = sceGxmProgramFindParameterByName(vp, "aTexcoord");
    sWvp = sceGxmProgramFindParameterByName(vp, "wvp");
    sTexSize = sceGxmProgramFindParameterByName(fp, "texSize");
    if (!pos || !tc || !sWvp || !sTexSize) {
        Platform_Log("[hq] shader parameters missing\n");
        return 0;
    }
    memset(attr, 0, sizeof(attr));
    attr[0].streamIndex = 0;
    attr[0].offset = 0;
    attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[0].componentCount = 3;
    attr[0].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(pos);
    attr[1].streamIndex = 0;
    attr[1].offset = 12;
    attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[1].componentCount = 2;
    attr[1].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(tc);
    stream.stride = sizeof(vita2d_texture_vertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, sVid, attr, 2, &stream, 1, &sVp) < 0 ||
        sceGxmShaderPatcherCreateFragmentProgram(patcher, sFid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, NULL, vp, &sFp) < 0) {
        Platform_Log("[hq] programs not created\n");
        return 0;
    }
    Platform_Log("[hq] ready\n");
    sState = 1;
    return 1;
}

void VitaHq_Draw(vita2d_texture *t, float x, float y, float sx, float sy, float w, float h, float scale)
{
    SceGxmContext *ctx = vita2d_get_context();
    vita2d_texture_vertex *v = vita2d_pool_memalign(4 * sizeof(vita2d_texture_vertex), sizeof(vita2d_texture_vertex));
    const float tw = (float)vita2d_texture_get_width(t), th = (float)vita2d_texture_get_height(t);
    const float u0 = sx / tw, v0 = sy / th, u1 = (sx + w) / tw, v1 = (sy + h) / th;
    const float x1 = x + w * scale, y1 = y + h * scale;
    /* the screen as vita2d sets it up: 960x544, y down */
    static const float wvp[16] = {
        2.0f / 960.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f / 544.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f,
    };
    float ts[2] = {tw, th};
    void *buf;

    if (!v)
        return;
    v[0].x = x;  v[0].y = y;  v[0].z = 0.5f; v[0].u = u0; v[0].v = v0;
    v[1].x = x1; v[1].y = y;  v[1].z = 0.5f; v[1].u = u1; v[1].v = v0;
    v[2].x = x;  v[2].y = y1; v[2].z = 0.5f; v[2].u = u0; v[2].v = v1;
    v[3].x = x1; v[3].y = y1; v[3].z = 0.5f; v[3].u = u1; v[3].v = v1;
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmSetVertexProgram(ctx, sVp);
    sceGxmSetFragmentProgram(ctx, sFp);
    sceGxmReserveVertexDefaultUniformBuffer(ctx, &buf);
    sceGxmSetUniformDataF(buf, sWvp, 0, 16, wvp);
    sceGxmReserveFragmentDefaultUniformBuffer(ctx, &buf);
    sceGxmSetUniformDataF(buf, sTexSize, 0, 2, ts);
    sceGxmSetFragmentTexture(ctx, 0, &t->gxm_tex);
    sceGxmSetVertexStream(ctx, 0, v);
    sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, vita2d_get_linear_indices(), 4);
}
