// dear imgui: Renderer Backend for DirectX9
// This needs to be used along with a Platform Backend (e.g. Win32)

// Implemented features:
//  [X] Renderer: User texture binding. Use 'LPDIRECT3DTEXTURE9' as texture identifier. Read the FAQ about ImTextureID/ImTextureRef!
//  [X] Renderer: Large meshes support (64k+ vertices) even with 16-bit indices (ImGuiBackendFlags_RendererHasVtxOffset).
//  [X] Renderer: Texture updates support for dynamic font atlas (ImGuiBackendFlags_RendererHasTextures).
//  [X] Renderer: IMGUI_USE_BGRA_PACKED_COLOR support, as this is the optimal color encoding for DirectX9.

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// CHANGELOG
// (minor and older changes stripped away, please see git history for details)
//  2026-04-23: Added support for standard draw callbacks (in platform_io): DrawCallback_ResetRenderState, DrawCallback_SetSamplerLinear, DrawCallback_SetSamplerNearest. (#9378)
//  2026-03-19: Fixed issue in ImGui_ImplDX9_UpdateTexture() if ImTextureID_Invalid is defined to be != 0, which became the default since 2026-03-12. (#9295, #9310)
//  2025-09-18: Call platform_io.ClearRendererHandlers() on shutdown.
//  2025-06-11: DirectX9: Added support for ImGuiBackendFlags_RendererHasTextures, for dynamic font atlas.
//  2024-10-07: DirectX9: Changed default texture sampler to Clamp instead of Repeat/Wrap.
//  2024-02-12: DirectX9: Using RGBA format when supported by the driver to avoid CPU side conversion. (#6575)
//  2022-10-11: Using 'nullptr' instead of 'NULL' as per our switch to C++11.
//  2021-06-29: Reorganized backend to pull data from a single structure to facilitate usage with multiple-contexts (all g_XXXX access changed to bd->XXXX).
//  2021-06-25: DirectX9: Explicitly disable texture state stages after >= 1.
//  2021-05-19: DirectX9: Replaced direct access to ImDrawCmd::TextureId with a call to ImDrawCmd::GetTexID(). (will become a requirement)
//  2021-04-23: DirectX9: Explicitly setting up more graphics states to increase compatibility with unusual non-default states.
//  2021-03-18: DirectX9: Calling IDirect3DStateBlock9::Capture() after CreateStateBlock() as a workaround for state restoring issues (see #3857).
//  2021-03-03: DirectX9: Added support for IMGUI_USE_BGRA_PACKED_COLOR in user's imconfig file.
//  2021-02-18: DirectX9: Change blending equation to preserve alpha in output buffer.
//  2019-05-29: DirectX9: Added support for large mesh (64K+ vertices), enable ImGuiBackendFlags_RendererHasVtxOffset flag.
//  2019-04-30: DirectX9: Added support for special ImDrawCallback_ResetRenderState callback to reset render state.
//  2019-03-29: Misc: Fixed erroneous assert in ImGui_ImplDX9_InvalidateDeviceObjects().
//  2019-01-16: Misc: Disabled fog before drawing UI's. Fixes issue #2288.
//  2018-11-30: Misc: Setting up io.BackendRendererName so it can be displayed in the About Window.
//  2018-06-08: Misc: Extracted imgui_impl_dx9.cpp/.h away from the old combined DX9+Win32 example.
//  2018-06-08: DirectX9: Use draw_data->DisplayPos and draw_data->DisplaySize to setup projection matrix and clipping rectangle.
//  2018-05-07: Render: Saving/restoring Transform because they don't seem to be included in the StateBlock. Setting shading mode to Gouraud.
//  2018-02-16: Misc: Obsoleted the io.RenderDrawListsFn callback and exposed ImGui_ImplDX9_RenderDrawData() in the .h file so you can call it yourself.
//  2018-02-06: Misc: Removed call to ImGui::Shutdown() which is not available from 1.60 WIP, user needs to call CreateContext/DestroyContext themselves.

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_dx9.h"

// DirectX
#include <d3d9.h>

// Clang/GCC warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wold-style-cast"         // warning: use of old-style cast                            // yes, they are more terse.
#pragma clang diagnostic ignored "-Wsign-conversion"        // warning: implicit conversion changes signedness
#endif

// DirectX data
struct ImGui_ImplDX9_Data
{
    LPDIRECT3DDEVICE9           pd3dDevice;
    LPDIRECT3DVERTEXBUFFER9     pVB;
    LPDIRECT3DINDEXBUFFER9      pIB;
    int                         VertexBufferSize;
    int                         IndexBufferSize;
    bool                        HasRgbaSupport;
    bool                        UseStateBlock;
    bool                        AutoSwitchRenderTarget;

    ImGui_ImplDX9_Data()        { memset((void*)this, 0, sizeof(*this)); VertexBufferSize = 5000; IndexBufferSize = 10000; }
};

#ifndef IMGUI_IMPL_DX9_DEFAULT_USE_STATEBLOCK
#define IMGUI_IMPL_DX9_DEFAULT_USE_STATEBLOCK false
#endif

#ifndef IMGUI_IMPL_DX9_DEFAULT_AUTO_SWITCH_RENDER_TARGET
#define IMGUI_IMPL_DX9_DEFAULT_AUTO_SWITCH_RENDER_TARGET false
#endif

static bool g_UseStateBlock = IMGUI_IMPL_DX9_DEFAULT_USE_STATEBLOCK;
static bool g_AutoSwitchRenderTarget = IMGUI_IMPL_DX9_DEFAULT_AUTO_SWITCH_RENDER_TARGET;

struct CUSTOMVERTEX
{
    float    pos[3];
    D3DCOLOR col;
    float    uv[2];
};
#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1)

#ifdef IMGUI_USE_BGRA_PACKED_COLOR
#define IMGUI_COL_TO_DX9_ARGB(_COL)     (_COL)
#else
#define IMGUI_COL_TO_DX9_ARGB(_COL)     (((_COL) & 0xFF00FF00) | (((_COL) & 0xFF0000) >> 16) | (((_COL) & 0xFF) << 16))
#endif

// Backend data stored in io.BackendRendererUserData to allow support for multiple Dear ImGui contexts
// It is STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context + multiple windows) instead of multiple Dear ImGui contexts.
static ImGui_ImplDX9_Data* ImGui_ImplDX9_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplDX9_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

// Functions
static void ImGui_ImplDX9_SetupRenderState(ImDrawData* draw_data)
{
    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();

    // Setup viewport
    D3DVIEWPORT9 vp;
    vp.X = vp.Y = 0;
    vp.Width = (DWORD)draw_data->DisplaySize.x;
    vp.Height = (DWORD)draw_data->DisplaySize.y;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;

    LPDIRECT3DDEVICE9 device = bd->pd3dDevice;
    device->SetViewport(&vp);

    // Setup render state: fixed-pipeline, alpha-blending, no face culling, no depth testing, shade mode (for gradient), bilinear sampling.
    device->SetPixelShader(nullptr);
    device->SetVertexShader(nullptr);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPING, TRUE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // Setup orthographic projection matrix
    // Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
    // Being agnostic of whether <d3dx9.h> or <DirectXMath.h> can be used, we aren't relying on D3DXMatrixIdentity()/D3DXMatrixOrthoOffCenterLH() or DirectX::XMMatrixIdentity()/DirectX::XMMatrixOrthographicOffCenterLH()
    {
        float L = draw_data->DisplayPos.x + 0.5f;
        float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x + 0.5f;
        float T = draw_data->DisplayPos.y + 0.5f;
        float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y + 0.5f;
        D3DMATRIX mat_identity = { { { 1.0f, 0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f } } };
        D3DMATRIX mat_projection =
        { { {
            2.0f/(R-L),   0.0f,         0.0f,  0.0f,
            0.0f,         2.0f/(T-B),   0.0f,  0.0f,
            0.0f,         0.0f,         0.5f,  0.0f,
            (L+R)/(L-R),  (T+B)/(B-T),  0.5f,  1.0f
        } } };
        device->SetTransform(D3DTS_WORLD, &mat_identity);
        device->SetTransform(D3DTS_VIEW, &mat_identity);
        device->SetTransform(D3DTS_PROJECTION, &mat_projection);
    }
}

// Draw callbacks
static void ImGui_ImplDX9_DrawCallback_ResetRenderState(const ImDrawList*, const ImDrawCmd*)    {} // Intentionally empty. Used as an identifier for rendering loop to call its code. Simpler to implement this way.
static void ImGui_ImplDX9_DrawCallback_SetSamplerLinear(const ImDrawList*, const ImDrawCmd*)    { ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData(); bd->pd3dDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); bd->pd3dDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); }
static void ImGui_ImplDX9_DrawCallback_SetSamplerNearest(const ImDrawList*, const ImDrawCmd*)   { ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData(); bd->pd3dDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); bd->pd3dDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); }

struct ImGui_ImplDX9_RenderState
{
    LPDIRECT3DDEVICE9               Device = nullptr;
    IDirect3DVertexShader9*         VertexShader = nullptr;
    IDirect3DPixelShader9*          PixelShader = nullptr;
    IDirect3DVertexDeclaration9*    VertexDeclaration = nullptr;
    DWORD                           FVF = 0;
    IDirect3DVertexBuffer9*         StreamSource = nullptr;
    UINT                        StreamOffset = 0;
    UINT                        StreamStride = 0;
    IDirect3DIndexBuffer9*      Indices = nullptr;
    IDirect3DBaseTexture9*      Texture = nullptr;
    D3DVIEWPORT9                Viewport = {};
    RECT                        ScissorRect = {};
    D3DMATRIX                   World = {};
    D3DMATRIX                   View = {};
    D3DMATRIX                   Projection = {};

    DWORD                       SamplerMinFilter = 0;
    DWORD                       SamplerMagFilter = 0;
    DWORD                       SamplerAddressU = 0;
    DWORD                       SamplerAddressV = 0;

    DWORD                       TSS0_ColorOp = 0;
    DWORD                       TSS0_ColorArg1 = 0;
    DWORD                       TSS0_ColorArg2 = 0;
    DWORD                       TSS0_AlphaOp = 0;
    DWORD                       TSS0_AlphaArg1 = 0;
    DWORD                       TSS0_AlphaArg2 = 0;
    DWORD                       TSS1_ColorOp = 0;
    DWORD                       TSS1_AlphaOp = 0;

    DWORD                       RS_FillMode = 0;
    DWORD                       RS_ShadeMode = 0;
    DWORD                       RS_ZWriteEnable = 0;
    DWORD                       RS_AlphaTestEnable = 0;
    DWORD                       RS_CullMode = 0;
    DWORD                       RS_ZEnable = 0;
    DWORD                       RS_AlphaBlendEnable = 0;
    DWORD                       RS_BlendOp = 0;
    DWORD                       RS_SrcBlend = 0;
    DWORD                       RS_DestBlend = 0;
    DWORD                       RS_SeparateAlphaBlendEnable = 0;
    DWORD                       RS_SrcBlendAlpha = 0;
    DWORD                       RS_DestBlendAlpha = 0;
    DWORD                       RS_ScissorTestEnable = 0;
    DWORD                       RS_FogEnable = 0;
    DWORD                       RS_RangeFogEnable = 0;
    DWORD                       RS_SpecularEnable = 0;
    DWORD                       RS_StencilEnable = 0;
    DWORD                       RS_Clipping = 0;
    DWORD                       RS_Lighting = 0;

    ~ImGui_ImplDX9_RenderState() { Release(); }

    void Save(LPDIRECT3DDEVICE9 device)
    {
        Device = device;
        device->GetVertexShader(&VertexShader);
        device->GetPixelShader(&PixelShader);
        device->GetVertexDeclaration(&VertexDeclaration);
        device->GetFVF(&FVF);
        device->GetStreamSource(0, &StreamSource, &StreamOffset, &StreamStride);
        device->GetIndices(&Indices);
        device->GetTexture(0, &Texture);
        device->GetViewport(&Viewport);
        device->GetScissorRect(&ScissorRect);
        device->GetTransform(D3DTS_WORLD, &World);
        device->GetTransform(D3DTS_VIEW, &View);
        device->GetTransform(D3DTS_PROJECTION, &Projection);

        device->GetSamplerState(0, D3DSAMP_MINFILTER, &SamplerMinFilter);
        device->GetSamplerState(0, D3DSAMP_MAGFILTER, &SamplerMagFilter);
        device->GetSamplerState(0, D3DSAMP_ADDRESSU, &SamplerAddressU);
        device->GetSamplerState(0, D3DSAMP_ADDRESSV, &SamplerAddressV);

        device->GetTextureStageState(0, D3DTSS_COLOROP, &TSS0_ColorOp);
        device->GetTextureStageState(0, D3DTSS_COLORARG1, &TSS0_ColorArg1);
        device->GetTextureStageState(0, D3DTSS_COLORARG2, &TSS0_ColorArg2);
        device->GetTextureStageState(0, D3DTSS_ALPHAOP, &TSS0_AlphaOp);
        device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &TSS0_AlphaArg1);
        device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &TSS0_AlphaArg2);
        device->GetTextureStageState(1, D3DTSS_COLOROP, &TSS1_ColorOp);
        device->GetTextureStageState(1, D3DTSS_ALPHAOP, &TSS1_AlphaOp);

        device->GetRenderState(D3DRS_FILLMODE, &RS_FillMode);
        device->GetRenderState(D3DRS_SHADEMODE, &RS_ShadeMode);
        device->GetRenderState(D3DRS_ZWRITEENABLE, &RS_ZWriteEnable);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &RS_AlphaTestEnable);
        device->GetRenderState(D3DRS_CULLMODE, &RS_CullMode);
        device->GetRenderState(D3DRS_ZENABLE, &RS_ZEnable);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &RS_AlphaBlendEnable);
        device->GetRenderState(D3DRS_BLENDOP, &RS_BlendOp);
        device->GetRenderState(D3DRS_SRCBLEND, &RS_SrcBlend);
        device->GetRenderState(D3DRS_DESTBLEND, &RS_DestBlend);
        device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &RS_SeparateAlphaBlendEnable);
        device->GetRenderState(D3DRS_SRCBLENDALPHA, &RS_SrcBlendAlpha);
        device->GetRenderState(D3DRS_DESTBLENDALPHA, &RS_DestBlendAlpha);
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &RS_ScissorTestEnable);
        device->GetRenderState(D3DRS_FOGENABLE, &RS_FogEnable);
        device->GetRenderState(D3DRS_RANGEFOGENABLE, &RS_RangeFogEnable);
        device->GetRenderState(D3DRS_SPECULARENABLE, &RS_SpecularEnable);
        device->GetRenderState(D3DRS_STENCILENABLE, &RS_StencilEnable);
        device->GetRenderState(D3DRS_CLIPPING, &RS_Clipping);
        device->GetRenderState(D3DRS_LIGHTING, &RS_Lighting);
    }

    void Restore()
    {
        if (!Device)
            return;

        Device->SetTransform(D3DTS_WORLD, &World);
        Device->SetTransform(D3DTS_VIEW, &View);
        Device->SetTransform(D3DTS_PROJECTION, &Projection);

        Device->SetRenderState(D3DRS_FILLMODE, RS_FillMode);
        Device->SetRenderState(D3DRS_SHADEMODE, RS_ShadeMode);
        Device->SetRenderState(D3DRS_ZWRITEENABLE, RS_ZWriteEnable);
        Device->SetRenderState(D3DRS_ALPHATESTENABLE, RS_AlphaTestEnable);
        Device->SetRenderState(D3DRS_CULLMODE, RS_CullMode);
        Device->SetRenderState(D3DRS_ZENABLE, RS_ZEnable);
        Device->SetRenderState(D3DRS_ALPHABLENDENABLE, RS_AlphaBlendEnable);
        Device->SetRenderState(D3DRS_BLENDOP, RS_BlendOp);
        Device->SetRenderState(D3DRS_SRCBLEND, RS_SrcBlend);
        Device->SetRenderState(D3DRS_DESTBLEND, RS_DestBlend);
        Device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, RS_SeparateAlphaBlendEnable);
        Device->SetRenderState(D3DRS_SRCBLENDALPHA, RS_SrcBlendAlpha);
        Device->SetRenderState(D3DRS_DESTBLENDALPHA, RS_DestBlendAlpha);
        Device->SetRenderState(D3DRS_SCISSORTESTENABLE, RS_ScissorTestEnable);
        Device->SetRenderState(D3DRS_FOGENABLE, RS_FogEnable);
        Device->SetRenderState(D3DRS_RANGEFOGENABLE, RS_RangeFogEnable);
        Device->SetRenderState(D3DRS_SPECULARENABLE, RS_SpecularEnable);
        Device->SetRenderState(D3DRS_STENCILENABLE, RS_StencilEnable);
        Device->SetRenderState(D3DRS_CLIPPING, RS_Clipping);
        Device->SetRenderState(D3DRS_LIGHTING, RS_Lighting);

        Device->SetTextureStageState(0, D3DTSS_COLOROP, TSS0_ColorOp);
        Device->SetTextureStageState(0, D3DTSS_COLORARG1, TSS0_ColorArg1);
        Device->SetTextureStageState(0, D3DTSS_COLORARG2, TSS0_ColorArg2);
        Device->SetTextureStageState(0, D3DTSS_ALPHAOP, TSS0_AlphaOp);
        Device->SetTextureStageState(0, D3DTSS_ALPHAARG1, TSS0_AlphaArg1);
        Device->SetTextureStageState(0, D3DTSS_ALPHAARG2, TSS0_AlphaArg2);
        Device->SetTextureStageState(1, D3DTSS_COLOROP, TSS1_ColorOp);
        Device->SetTextureStageState(1, D3DTSS_ALPHAOP, TSS1_AlphaOp);

        Device->SetSamplerState(0, D3DSAMP_MINFILTER, SamplerMinFilter);
        Device->SetSamplerState(0, D3DSAMP_MAGFILTER, SamplerMagFilter);
        Device->SetSamplerState(0, D3DSAMP_ADDRESSU, SamplerAddressU);
        Device->SetSamplerState(0, D3DSAMP_ADDRESSV, SamplerAddressV);

        Device->SetScissorRect(&ScissorRect);
        Device->SetViewport(&Viewport);
        Device->SetTexture(0, Texture);
        Device->SetIndices(Indices);
        Device->SetStreamSource(0, StreamSource, StreamOffset, StreamStride);
        if (VertexDeclaration)
            Device->SetVertexDeclaration(VertexDeclaration);
        if (FVF)
            Device->SetFVF(FVF);
        Device->SetPixelShader(PixelShader);
        Device->SetVertexShader(VertexShader);

        Release();
    }

    void Release()
    {
        if (VertexShader)       { VertexShader->Release(); VertexShader = nullptr; }
        if (PixelShader)        { PixelShader->Release();  PixelShader = nullptr; }
        if (VertexDeclaration)  { VertexDeclaration->Release(); VertexDeclaration = nullptr; }
        if (StreamSource)       { StreamSource->Release(); StreamSource = nullptr; }
        if (Indices)            { Indices->Release();      Indices = nullptr; }
        if (Texture)            { Texture->Release();      Texture = nullptr; }
        Device = nullptr;
    }
};

// Render function.
void ImGui_ImplDX9_RenderDrawData(ImDrawData* draw_data)
{
    // Avoid rendering when minimized
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();
    LPDIRECT3DDEVICE9 device = bd->pd3dDevice;

    // Catch up with texture updates. Most of the times, the list will have 1 element with an OK status, aka nothing to do.
    // (This almost always points to ImGui::GetPlatformIO().Textures[] but is part of ImDrawData to allow overriding or disabling texture updates).
    if (draw_data->Textures != nullptr)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK)
                ImGui_ImplDX9_UpdateTexture(tex);

    // Create and grow buffers if needed
    if (!bd->pVB || bd->VertexBufferSize < draw_data->TotalVtxCount)
    {
        if (bd->pVB) { bd->pVB->Release(); bd->pVB = nullptr; }
        bd->VertexBufferSize = draw_data->TotalVtxCount + 5000;
        if (device->CreateVertexBuffer((UINT)bd->VertexBufferSize * sizeof(CUSTOMVERTEX), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT, &bd->pVB, nullptr) < 0)
            return;
    }
    if (!bd->pIB || bd->IndexBufferSize < draw_data->TotalIdxCount)
    {
        if (bd->pIB) { bd->pIB->Release(); bd->pIB = nullptr; }
        bd->IndexBufferSize = draw_data->TotalIdxCount + 10000;
        if (device->CreateIndexBuffer((UINT)bd->IndexBufferSize * sizeof(ImDrawIdx), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, D3DPOOL_DEFAULT, &bd->pIB, nullptr) < 0)
            return;
    }

    // Backup the DX9 state
    IDirect3DStateBlock9* state_block = nullptr;
    D3DMATRIX last_world, last_view, last_projection;
    ImGui_ImplDX9_RenderState manual_state;
    if (bd->UseStateBlock)
    {
        if (device->CreateStateBlock(D3DSBT_ALL, &state_block) < 0)
            return;
        if (state_block->Capture() < 0)
        {
            state_block->Release();
            return;
        }

        // Backup the DX9 transform (DX9 documentation suggests that it is included in the StateBlock but it doesn't appear to)
        device->GetTransform(D3DTS_WORLD, &last_world);
        device->GetTransform(D3DTS_VIEW, &last_view);
        device->GetTransform(D3DTS_PROJECTION, &last_projection);
    }
    else
    {
        manual_state.Save(device);
    }

    // Allocate buffers
    CUSTOMVERTEX* vtx_dst;
    ImDrawIdx* idx_dst;
    if (bd->pVB->Lock(0, (UINT)(draw_data->TotalVtxCount * sizeof(CUSTOMVERTEX)), (void**)&vtx_dst, D3DLOCK_DISCARD) < 0)
    {
        if (state_block)
            state_block->Release();
        return;
    }
    if (bd->pIB->Lock(0, (UINT)(draw_data->TotalIdxCount * sizeof(ImDrawIdx)), (void**)&idx_dst, D3DLOCK_DISCARD) < 0)
    {
        bd->pVB->Unlock();
        if (state_block)
            state_block->Release();
        return;
    }

    // Copy and convert all vertices into a single contiguous buffer, convert colors to DX9 default format.
    // FIXME-OPT: This is a minor waste of resource, the ideal is to use imconfig.h and
    //  1) to avoid repacking colors:   #define IMGUI_USE_BGRA_PACKED_COLOR
    //  2) to avoid repacking vertices: #define IMGUI_OVERRIDE_DRAWVERT_STRUCT_LAYOUT struct ImDrawVert { ImVec2 pos; float z; ImU32 col; ImVec2 uv; }
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        const ImDrawVert* vtx_src = draw_list->VtxBuffer.Data;
        for (int i = 0; i < draw_list->VtxBuffer.Size; i++)
        {
            vtx_dst->pos[0] = vtx_src->pos.x;
            vtx_dst->pos[1] = vtx_src->pos.y;
            vtx_dst->pos[2] = 0.0f;
            vtx_dst->col = IMGUI_COL_TO_DX9_ARGB(vtx_src->col);
            vtx_dst->uv[0] = vtx_src->uv.x;
            vtx_dst->uv[1] = vtx_src->uv.y;
            vtx_dst++;
            vtx_src++;
        }
        memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        idx_dst += draw_list->IdxBuffer.Size;
    }
    bd->pVB->Unlock();
    bd->pIB->Unlock();
    device->SetStreamSource(0, bd->pVB, 0, sizeof(CUSTOMVERTEX));
    device->SetIndices(bd->pIB);
    device->SetFVF(D3DFVF_CUSTOMVERTEX);

    // Auto-switch RenderTarget to BackBuffer if needed
    LPDIRECT3DSURFACE9 pCurrentRT = nullptr;
    LPDIRECT3DSURFACE9 pBackBuffer = nullptr;
    LPDIRECT3DSURFACE9 pOriginalRT = nullptr;
    LPDIRECT3DSURFACE9 pOriginalDepthStencil = nullptr;
    if (bd->AutoSwitchRenderTarget)
    {
        if (device->GetRenderTarget(0, &pCurrentRT) == D3D_OK &&
            device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer) == D3D_OK)
        {
            if (pCurrentRT != pBackBuffer)
            {
                pOriginalRT = pCurrentRT;
                pCurrentRT = nullptr;
                device->GetDepthStencilSurface(&pOriginalDepthStencil);
                device->SetDepthStencilSurface(nullptr);
                device->SetRenderTarget(0, pBackBuffer);
            }
        }
    }

    // Setup desired DX state
    ImGui_ImplDX9_SetupRenderState(draw_data);

    // Render command lists
    // (Because we merged all buffers into a single one, we maintain our own offset into them)
    int global_vtx_offset = 0;
    int global_idx_offset = 0;
    ImVec2 clip_off = draw_data->DisplayPos;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback != nullptr)
            {
                // User callback, registered via ImDrawList::AddCallback()
                if (pcmd->UserCallback == ImGui_ImplDX9_DrawCallback_ResetRenderState)
                    ImGui_ImplDX9_SetupRenderState(draw_data);
                else
                    pcmd->UserCallback(draw_list, pcmd);
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min(pcmd->ClipRect.x - clip_off.x, pcmd->ClipRect.y - clip_off.y);
                ImVec2 clip_max(pcmd->ClipRect.z - clip_off.x, pcmd->ClipRect.w - clip_off.y);
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                // Apply scissor/clipping rectangle
                const RECT r = { (LONG)clip_min.x, (LONG)clip_min.y, (LONG)clip_max.x, (LONG)clip_max.y };
                device->SetScissorRect(&r);

                // Bind texture, Draw
                const LPDIRECT3DTEXTURE9 texture = (LPDIRECT3DTEXTURE9)pcmd->GetTexID();
                device->SetTexture(0, texture);
                device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, pcmd->VtxOffset + global_vtx_offset, 0, (UINT)draw_list->VtxBuffer.Size, pcmd->IdxOffset + global_idx_offset, pcmd->ElemCount / 3);
            }
        }
        global_idx_offset += draw_list->IdxBuffer.Size;
        global_vtx_offset += draw_list->VtxBuffer.Size;
    }

    // Restore RenderTarget and DepthStencil if switched
    if (pOriginalRT)
    {
        device->SetRenderTarget(0, pOriginalRT);
        device->SetDepthStencilSurface(pOriginalDepthStencil);
        pOriginalRT->Release();
        pOriginalRT = nullptr;
    }
    if (pOriginalDepthStencil) { pOriginalDepthStencil->Release(); pOriginalDepthStencil = nullptr; }
    if (pBackBuffer)           { pBackBuffer->Release();           pBackBuffer = nullptr; }
    if (pCurrentRT)            { pCurrentRT->Release();            pCurrentRT = nullptr; }

    if (bd->UseStateBlock)
    {
        // Restore the DX9 transform
        device->SetTransform(D3DTS_WORLD, &last_world);
        device->SetTransform(D3DTS_VIEW, &last_view);
        device->SetTransform(D3DTS_PROJECTION, &last_projection);

        // Restore the DX9 state
        state_block->Apply();
        state_block->Release();
    }
    else
    {
        manual_state.Restore();
    }
}

static bool ImGui_ImplDX9_CheckFormatSupport(LPDIRECT3DDEVICE9 pDevice, D3DFORMAT format)
{
    LPDIRECT3D9 pd3d = nullptr;
    if (pDevice->GetDirect3D(&pd3d) != D3D_OK)
        return false;
    D3DDEVICE_CREATION_PARAMETERS param = {};
    D3DDISPLAYMODE mode = {};
    if (pDevice->GetCreationParameters(&param) != D3D_OK || pDevice->GetDisplayMode(0, &mode) != D3D_OK)
    {
        pd3d->Release();
        return false;
    }
    // Font texture should support linear filter, color blend and write to render-target
    bool support = (pd3d->CheckDeviceFormat(param.AdapterOrdinal, param.DeviceType, mode.Format, D3DUSAGE_DYNAMIC | D3DUSAGE_QUERY_FILTER | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, format)) == D3D_OK;
    pd3d->Release();
    return support;
}

// Convert RGBA32 to BGRA32 (because RGBA32 is not well supported by DX9 devices)
static void ImGui_ImplDX9_CopyTextureRegion(bool tex_use_colors, const ImU32* src, int src_pitch, ImU32* dst, int dst_pitch, int w, int h)
{
#ifndef IMGUI_USE_BGRA_PACKED_COLOR
    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();
    const bool convert_rgba_to_bgra = (!bd->HasRgbaSupport && tex_use_colors);
#else
    const bool convert_rgba_to_bgra = false;
    IM_UNUSED(tex_use_colors);
#endif
    for (int y = 0; y < h; y++)
    {
        const ImU32* src_p = (const ImU32*)(const void*)((const unsigned char*)src + src_pitch * y);
        ImU32* dst_p = (ImU32*)(void*)((unsigned char*)dst + dst_pitch * y);
        if (convert_rgba_to_bgra)
            for (int x = w; x > 0; x--, src_p++, dst_p++) // Convert copy
                *dst_p = IMGUI_COL_TO_DX9_ARGB(*src_p);
        else
            memcpy(dst_p, src_p, w * 4); // Raw copy
    }
}

void ImGui_ImplDX9_UpdateTexture(ImTextureData* tex)
{
    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();

    if (tex->Status == ImTextureStatus_WantCreate)
    {
        // Create and upload new texture to graphics system
        //IMGUI_DEBUG_LOG("UpdateTexture #%03d: WantCreate %dx%d\n", tex->UniqueID, tex->Width, tex->Height);
        IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        LPDIRECT3DTEXTURE9 dx_tex = nullptr;
        HRESULT hr = bd->pd3dDevice->CreateTexture(tex->Width, tex->Height, 1, D3DUSAGE_DYNAMIC, bd->HasRgbaSupport ? D3DFMT_A8B8G8R8 : D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dx_tex, nullptr);
        if (hr < 0)
        {
            IM_ASSERT(hr >= 0 && "Backend failed to create texture!");
            return;
        }

        D3DLOCKED_RECT locked_rect;
        if (dx_tex->LockRect(0, &locked_rect, nullptr, 0) == D3D_OK)
        {
            ImGui_ImplDX9_CopyTextureRegion(tex->UseColors, (ImU32*)tex->GetPixels(), tex->Width * 4, (ImU32*)locked_rect.pBits, (ImU32)locked_rect.Pitch, tex->Width, tex->Height);
            dx_tex->UnlockRect(0);
        }

        // Store identifiers
        tex->SetTexID((ImTextureID)(intptr_t)dx_tex);
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantUpdates)
    {
        // Update selected blocks. We only ever write to textures regions which have never been used before!
        // This backend choose to use tex->Updates[] but you can use tex->UpdateRect to upload a single region.
        LPDIRECT3DTEXTURE9 backend_tex = (LPDIRECT3DTEXTURE9)(intptr_t)tex->TexID;
        RECT update_rect = { (LONG)tex->UpdateRect.x, (LONG)tex->UpdateRect.y, (LONG)(tex->UpdateRect.x + tex->UpdateRect.w), (LONG)(tex->UpdateRect.y + tex->UpdateRect.h) };
        D3DLOCKED_RECT locked_rect;
        if (backend_tex->LockRect(0, &locked_rect, &update_rect, 0) == D3D_OK)
            for (ImTextureRect& r : tex->Updates)
                ImGui_ImplDX9_CopyTextureRegion(tex->UseColors, (ImU32*)tex->GetPixelsAt(r.x, r.y), tex->Width * 4,
                    (ImU32*)locked_rect.pBits + (r.x - update_rect.left) + (r.y - update_rect.top) * (locked_rect.Pitch / 4), (int)locked_rect.Pitch, r.w, r.h);
        backend_tex->UnlockRect(0);
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantDestroy)
    {
        if (tex->TexID != ImTextureID_Invalid)
            if (LPDIRECT3DTEXTURE9 backend_tex = (LPDIRECT3DTEXTURE9)tex->TexID)
            {
                IM_ASSERT(tex->TexID == (ImTextureID)(intptr_t)backend_tex);
                backend_tex->Release();

                // Clear identifiers and mark as destroyed (in order to allow e.g. calling InvalidateDeviceObjects while running)
                tex->SetTexID(ImTextureID_Invalid);
            }
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

bool ImGui_ImplDX9_CreateDeviceObjects()
{
    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();
    if (!bd || !bd->pd3dDevice)
        return false;
    return true;
}

void ImGui_ImplDX9_InvalidateDeviceObjects()
{
    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();
    if (!bd || !bd->pd3dDevice)
        return;

    // Destroy all textures
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1)
        {
            tex->SetStatus(ImTextureStatus_WantDestroy);
            ImGui_ImplDX9_UpdateTexture(tex);
        }
    if (bd->pVB) { bd->pVB->Release(); bd->pVB = nullptr; }
    if (bd->pIB) { bd->pIB->Release(); bd->pIB = nullptr; }
}

void ImGui_ImplDX9_NewFrame()
{
    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplDX9_Init()?");
    IM_UNUSED(bd);
}


bool ImGui_ImplDX9_Init(IDirect3DDevice9* device)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");

    // Setup backend capabilities flags
    ImGui_ImplDX9_Data* bd = IM_NEW(ImGui_ImplDX9_Data)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_dx9";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Renderer_TextureMaxWidth = platform_io.Renderer_TextureMaxHeight = 4096;
    platform_io.DrawCallback_ResetRenderState = ImGui_ImplDX9_DrawCallback_ResetRenderState;
    platform_io.DrawCallback_SetSamplerLinear = ImGui_ImplDX9_DrawCallback_SetSamplerLinear;
    platform_io.DrawCallback_SetSamplerNearest = ImGui_ImplDX9_DrawCallback_SetSamplerNearest;

    bd->pd3dDevice = device;
    bd->pd3dDevice->AddRef();
    bd->HasRgbaSupport = ImGui_ImplDX9_CheckFormatSupport(bd->pd3dDevice, D3DFMT_A8B8G8R8);
    bd->UseStateBlock = g_UseStateBlock;
    bd->AutoSwitchRenderTarget = g_AutoSwitchRenderTarget;

    return true;
}

void ImGui_ImplDX9_UseStateBlock(bool use_stateblock)
{
    g_UseStateBlock = use_stateblock;
    if (ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData())
        bd->UseStateBlock = use_stateblock;
}

void ImGui_ImplDX9_EnableAutoRenderTargetSwitch(bool enable)
{
    g_AutoSwitchRenderTarget = enable;
    if (ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData())
        bd->AutoSwitchRenderTarget = enable;
}

void ImGui_ImplDX9_Shutdown()
{
    ImGui_ImplDX9_Data* bd = ImGui_ImplDX9_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (bd->pd3dDevice) { bd->pd3dDevice->Release(); }

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    platform_io.ClearRendererHandlers();
    IM_DELETE(bd);
}

//-----------------------------------------------------------------------------

#endif // #ifndef IMGUI_DISABLE
