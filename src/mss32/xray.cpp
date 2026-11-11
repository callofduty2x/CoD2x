#include "xray.h"

#include <windows.h>
#include <d3d9.h>

#include "shared.h"
#include "hook.h"
#include "drawing.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"

#define sv_cheats               (*((dvar_t**)0x00c5c5cc))
#define g_TeamColor_Allies      (*((dvar_t**)0x017d03bc))
#define g_TeamColor_Axis        (*((dvar_t**)0x01897e1c))

// Renderer (gfx dll)
#define d3d9_device             (*((IDirect3DDevice9**)(gfx_module_addr + 0x001d1bf8)))
#define sceneEntities           (*((GfxEntity**)(gfx_module_addr + 0x008cc310)))   // entities of the scene being built (front end)
#define backEnd_entities        (*((GfxEntity**)(gfx_module_addr + 0x00981bc4)))   // backEnd + 0x3c4, entities of the scene being drawn (RB_RenderDrawSurfList)
#define backEnd_currentEntity   (*((GfxEntity**)(gfx_module_addr + 0x00981c40)))  // NULL outside of RB_RenderDrawSurfList
#define backEnd_viewParms       (*((char**)(gfx_module_addr + 0x00981bc8)))       // backEnd + 0x3c8, view of the drawn scene (+0x48 view matrix, +0x88 projection
                                                                                    // matrix), also used by the sprites among the 2D commands (RB_DrawSpriteCmd)
#define tess_numIndexes         (*((int*)(gfx_module_addr + 0x009817d0)))         // tess + 0x14, quads collected for the current material
#define tess_field_24           (*((int*)(gfx_module_addr + 0x009817e0)))         // tess + 0x24, other primitives collected (RB_EndSurface draws both)
#define s_cmdlist               (*((char**)(gfx_module_addr + 0x00438780)))       // render commands being added: commands at 0, +0x30000 used bytes, +0x30004 extra size
#define rb_renderCommandTable   ((void**)(gfx_module_addr + 0x00190518))           // backend functions by command id (RB_ExecuteRenderCommands)

#define RC_STRETCH_PIC          0x0f    // render command of R_AddCmdDrawStretchPic

// MaterialTechniqueType passed to RB_RenderDrawSurfList
#define TECHNIQUE_LIT           6       // opaque world and models

// GfxEntity.renderfx flags read by the renderer (RB_RenderDrawSurfList in gfx dll, same as RF_DEPTHHACK in Quake 3):
// 0x08 = viewmodel (custom near clip + depth range 0..0.2), 0x10 = depth range 0..0.5
#define RF_DEPTHHACK_HALF       0x10
// CoD2x: players tagged for x-ray, the renderer does not use bits 24..31
#define RF_XRAY             0x01000000
#define RF_XRAY_TEAM_SHIFT  25      // 2 bits with team_t of the player

// Modes
#define XRAY_THROUGH_WALLS  1       // players are drawn over the world
#define XRAY_CHAMS          2       // flat team color where the player is behind walls
#define XRAY_OUTLINE        4       // thin team color outline around the parts behind walls
#define XRAY_GLOW           8       // big team color glow around the parts behind walls (like CS:GO)
#define XRAY_MASK           (XRAY_OUTLINE | XRAY_GLOW)  // modes drawn from the mask texture

// Debug build: everything is allowed. Release: the cvars that show enemies to a playing player are cheat protected.
#if DEBUG
#define XRAY_ALWAYS_ALLOWED true
#define XRAY_CHEAT_FLAG         0
#else
#define XRAY_ALWAYS_ALLOWED false
#define XRAY_CHEAT_FLAG         DVAR_CHEAT
#endif

// Stencil bits set while drawing players, cleared after
#define XRAY_STENCIL_VISIBLE_ALLIES 0x80    // an allies player is visible
#define XRAY_STENCIL_VISIBLE_AXIS   0x40    // an axis (or free-for-all) player is visible
#define XRAY_STENCIL_HIDDEN_ALLIES  0x20    // an allies player is hidden
#define XRAY_STENCIL_HIDDEN_AXIS    0x10    // an axis (or free-for-all) player is hidden
#define XRAY_STENCIL_VISIBLE        (XRAY_STENCIL_VISIBLE_ALLIES | XRAY_STENCIL_VISIBLE_AXIS)
#define XRAY_STENCIL_ALL            0xf0
#define XRAY_STENCIL_HEAD           0x08    // head info: the head point is in front of the world (set by Xray_HeadDepthMark, see xray_headDepthTest)
#define XRAY_MAX_SURFS      4096

struct GfxEntity {
    int32_t reType;
    int32_t renderfx;
    char _08[0x34];
    vec3_t origin;          // 0x3c
    char _48[0x2c];
};
static_assert(sizeof(GfxEntity) == 0x74, "sizeof(GfxEntity)");

struct GfxDrawSurf {
    uint32_t sortKey;
    void* surface;
};

// Arguments of DrawIndexedPrimitive in R_DrawIndexedPrimitive
struct GfxDrawPrimArgs {
    UINT minVertexIndex;
    UINT numVertices;
    UINT primCount;
    INT  baseVertexIndex;
    UINT startIndex;
};

// Styles of cg_xray: 0 = off, 1 = players drawn through walls, 2 = chams, 3 = outline, 4 = glow
// Styles of the states (cg_xraySpectator / Dead): 0 = none, 1 = outline, 2 = glow. Playing players never have x-ray (like CS:GO), only cg_xray.
dvar_t* cg_xray;                    // global override, all players in every state (sv_cheats, demo playback, debug build), 0 = the style of the state
dvar_t* cg_xraySpectator;           // real spectators and demo playback, all players (like spec_show_xray in CS:GO, the server can control it)
dvar_t* cg_xrayDead;                // dead players in the spectator session, only teammates
dvar_t* cg_xrayVisibleSpectator;    // cg_xrayVisibleSpectator / Dead - outline / glow also around the visible parts of players (also players
dvar_t* cg_xrayVisibleDead;         // in sight), 0 = only around the hidden parts
dvar_t* cg_xrayCenterFadeRadius;    // x-ray and head info of hidden players fade near the screen center (where we aim), so they do not cover an enemy there.
dvar_t* cg_xrayCenterFadeAlpha;     // Radius in the 640x480 virtual space (0 = off) and the alpha at the center.
dvar_t* cg_xrayOutlineWidth;        // outline: width of one ring in pixels (2 rings)
dvar_t* cg_xrayGlowIntensity;       // glow: strength of the big glow
dvar_t* cg_xrayGlowCoreIntensity;   // glow: strength of the lighter glow on top
dvar_t* cg_xrayHiddenThreshold;     // a part of a player is hidden only when it's this many units behind the world (feet in the ground are not hidden)
dvar_t* cg_xrayColorAllies;         // color of allies ("r g b" 0..1), empty = g_TeamColor_Allies
dvar_t* cg_xrayColorAxis;           // color of axis and free-for-all players, empty = g_TeamColor_Axis

xrayViewer_e xray_viewer = XRAY_VIEWER_NONE;
static bool xray_showTeam[4];       // team_t of players that are shown
static bool xray_visibleParts = false;  // outline / glow also around the visible parts of players (cg_xrayVisible* of the state)
static float xray_zNear = 4.0f;     // r_znear, for the hidden threshold
static vec3_t xray_viewOrigin;      // camera position, for the hidden threshold
static bool xray_depthBias = false; // the GPU supports D3DRS_DEPTHBIAS (checked when the state block is created)
static bool xray_blendMax = false;  // the GPU supports D3DBLENDOP_MAX (checked when the state block is created)
static vec4_t xray_teamColorVec[4]; // color of team_t
static D3DCOLOR xray_teamColor[4];  // the same as D3D color

static int xray_mode = 0;                       // XRAY_* flags, 0 = off
static bool xray_hasStencil = false;            // the depth buffer of the last lit pass has 8 stencil bits (see Xray_HasStencil)
static bool xray_markPlayers = false;           // the lit pass is being drawn and the players are marked in the stencil
static bool xray_playersDrawn = false;          // a player was marked in this lit pass
static bool xray_maskUsesStencil = false;       // outline mask is made from the stencil marks (screen is not multisampled)
static bool xray_maskCleared = false;           // outline mask was cleared in this lit pass (multisampled screen)

static IDirect3DStateBlock9* xray_state = NULL; // engine values of the states changed by x-ray
static IDirect3DVertexDeclaration9* xray_savedDecl = NULL;  // engine vertex declaration (not in the state block)
static DWORD xray_savedFVF = 0;
static IDirect3DTexture9* xray_mask = NULL;     // player silhouettes for the outline
static IDirect3DTexture9* xray_blur[2] = {};    // the mask downsampled to 1/2 for the small and the big glow
static IDirect3DTexture9* xray_blurTemp[2] = {};// temporary textures for the gaussian blur
static IDirect3DTexture9* xray_fade = NULL;     // center fade: alpha by the distance from the texture center (the center is cg_xrayCenterFadeAlpha, the edge 1)
static float xray_fadeTextureAlpha = -1;        // cg_xrayCenterFadeAlpha the fade texture was made with
static bool xray_fadeQuad = false;              // quads are drawn with the fade texture coordinates in the 2nd stage
static UINT xray_maskWidth = 0;
static UINT xray_maskHeight = 0;



/** Releases the outline / glow textures. */
static void Xray_ReleaseTextures() {
    if (xray_mask) { xray_mask->Release(); xray_mask = NULL; }
    for (int i = 0; i < 2; i++)
        if (xray_blur[i]) { xray_blur[i]->Release(); xray_blur[i] = NULL; }
    for (int i = 0; i < 2; i++)
        if (xray_blurTemp[i]) { xray_blurTemp[i]->Release(); xray_blurTemp[i] = NULL; }
}

/** Releases all D3D objects, must be done before the device is reset or destroyed. */
static void Xray_ReleaseResources() {
    if (xray_state) { xray_state->Release(); xray_state = NULL; }
    if (xray_fade) { xray_fade->Release(); xray_fade = NULL; }
    Xray_ReleaseTextures();
}

/** State block with all states set by x-ray, to save and restore the engine values. */
static bool Xray_CreateStateBlock(IDirect3DDevice9* device) {
    if (xray_state)
        return true;

    D3DVIEWPORT9 viewport;
    device->GetViewport(&viewport);

    // The values below are only placeholders, keep the engine state in case the driver applies them while recording
    IDirect3DStateBlock9* engineState = NULL;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &engineState)))
        return false;

    device->BeginStateBlock();
    D3DRENDERSTATETYPE renderStates[] = {
        D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE,
        D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
        D3DRS_STENCILPASS, D3DRS_STENCILZFAIL, D3DRS_STENCILFAIL, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_COLORWRITEENABLE,
        D3DRS_FOGENABLE, D3DRS_SPECULARENABLE, D3DRS_TEXTUREFACTOR, D3DRS_CULLMODE, D3DRS_FILLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_DITHERENABLE, D3DRS_BLENDOP, D3DRS_DEPTHBIAS,
    };
    for (int i = 0; i < (int)(sizeof(renderStates) / sizeof(renderStates[0])); i++)
        device->SetRenderState(renderStates[i], 0);
    for (int stage = 0; stage < 2; stage++) {
        device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(stage, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        device->SetTextureStageState(stage, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetTextureStageState(stage, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        device->SetTextureStageState(stage, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        device->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    for (int sampler = 0; sampler < 2; sampler++) {
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    device->SetTexture(0, NULL);
    device->SetTexture(1, NULL);
    device->SetPixelShader(NULL);
    device->SetVertexShader(NULL);
    device->SetStreamSource(0, NULL, 0, 0);     // DrawPrimitiveUP resets stream 0
    device->SetIndices(NULL);
    device->SetViewport(&viewport);
    // The vertex declaration is saved separately in Xray_SaveState - a recorded SetFVF would restore FVF 0 instead of the engine declaration

    if (FAILED(device->EndStateBlock(&xray_state)))
        xray_state = NULL;

    D3DCAPS9 caps;
    bool hasCaps = SUCCEEDED(device->GetDeviceCaps(&caps));
    xray_depthBias = hasCaps && (caps.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS);
    xray_blendMax = hasCaps && (caps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP);

    engineState->Apply();
    engineState->Release();
    return xray_state != NULL;
}

/** Saves the engine values of all states changed by x-ray. */
static void Xray_SaveState(IDirect3DDevice9* device) {
    xray_state->Capture();
    device->GetVertexDeclaration(&xray_savedDecl);
    device->GetFVF(&xray_savedFVF);
}

/**
 * Restores the engine values of all states changed by x-ray. The engine caches its states and does not set them again,
 * so everything must be exactly as before (a missing vertex declaration stops drawing of everything that uses it).
 */
static void Xray_RestoreState(IDirect3DDevice9* device) {
    xray_state->Apply();
    if (xray_savedDecl) {
        device->SetVertexDeclaration(xray_savedDecl);
        xray_savedDecl->Release();
        xray_savedDecl = NULL;
    } else {
        device->SetFVF(xray_savedFVF);
    }
}

/** Flat color without textures and shaders, works on both the shader and the fixed function (DX7) renderer. */
static void Xray_SetFlatColor(IDirect3DDevice9* device) {
    device->SetPixelShader(NULL);
    device->SetTexture(0, NULL);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_DITHERENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
}

/** D3DRS_DEPTHBIAS takes the bits of the float. */
static void Xray_SetDepthBias(IDirect3DDevice9* device, float bias) {
    DWORD value;
    memcpy(&value, &bias, sizeof(value));
    device->SetRenderState(D3DRS_DEPTHBIAS, value);
}

/** Texture stage 0 draws the texture multiplied by the texture factor (color and alpha). */
static void Xray_SetTextureModulate(IDirect3DDevice9* device) {
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
}

/**
 * True if the depth buffer has 8 stencil bits. The engine can also run with a depth buffer without stencil (D24X8, D16) or with
 * 1 bit (D15S1) - then the stencil test does nothing and everything tested by it would be drawn over the whole screen.
 */
static bool Xray_HasStencil(IDirect3DDevice9* device) {
    IDirect3DSurface9* depth = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(device->GetDepthStencilSurface(&depth)) || !depth)
        return false;
    bool ok = SUCCEEDED(depth->GetDesc(&desc));
    depth->Release();
    return ok && (desc.Format == D3DFMT_D24S8 || desc.Format == D3DFMT_D24FS8);
}

/** Quad without texture over the whole render target at the depth z (0..1), render states are set by the caller. */
static void Xray_DrawDepthQuad(IDirect3DDevice9* device, float z) {
    IDirect3DSurface9* target = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target)
        return;
    bool ok = SUCCEEDED(target->GetDesc(&desc));
    target->Release();
    if (!ok)
        return;

    float w = (float)desc.Width, h = (float)desc.Height;
    float quad[4][4] = { // x, y, z, rhw
        { -0.5f,     -0.5f,     z, 1 },
        { w - 0.5f,  -0.5f,     z, 1 },
        { -0.5f,     h - 0.5f,  z, 1 },
        { w - 0.5f,  h - 0.5f,  z, 1 },
    };
    device->SetVertexShader(NULL);
    device->SetFVF(D3DFVF_XYZRHW);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

/** Clears the stencil bits on the whole screen, the other bits are kept. */
static void Xray_ClearStencil(IDirect3DDevice9* device, DWORD bits) {
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_STENCILREF, 0);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, bits);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    Xray_DrawDepthQuad(device, 0);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
}

/** Quad over the whole render target of size w x h, texture coordinates moved by u, v (render states are set by the caller). */
static void Xray_DrawQuadSized(IDirect3DDevice9* device, float w, float h, float u, float v) {
    device->SetVertexShader(NULL);

    // Center fade: 2nd texture coordinates of the fade texture by the position on the screen (not moved by u, v). The texture covers the fade radius
    // around the screen center, the radius is in the 640x480 virtual space (aspect correct, 480 units high), clamped outside to the edge (alpha 1).
    if (xray_fadeQuad) {
        float scale = 480.0f / h / (2 * cg_xrayCenterFadeRadius->value.decimal);  // texture units per pixel
        float u0 = 0.5f - w / 2 * scale, u1 = 0.5f + w / 2 * scale;
        float v0 = 0.5f - h / 2 * scale, v1 = 0.5f + h / 2 * scale;
        float quad[4][8] = { // x, y, z, rhw, u, v, fade u, fade v
            { -0.5f,     -0.5f,     0, 1, 0 + u, 0 + v, u0, v0 },
            { w - 0.5f,  -0.5f,     0, 1, 1 + u, 0 + v, u1, v0 },
            { -0.5f,     h - 0.5f,  0, 1, 0 + u, 1 + v, u0, v1 },
            { w - 0.5f,  h - 0.5f,  0, 1, 1 + u, 1 + v, u1, v1 },
        };
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX2);
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        return;
    }

    float quad[4][6] = { // x, y, z, rhw, u, v
        { -0.5f,     -0.5f,     0, 1, 0 + u, 0 + v },
        { w - 0.5f,  -0.5f,     0, 1, 1 + u, 0 + v },
        { -0.5f,     h - 0.5f,  0, 1, 0 + u, 1 + v },
        { w - 0.5f,  h - 0.5f,  0, 1, 1 + u, 1 + v },
    };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

/** Quad over the whole screen. */
static void Xray_DrawQuad(IDirect3DDevice9* device, float u, float v) {
    Xray_DrawQuadSized(device, (float)xray_maskWidth, (float)xray_maskHeight, u, v);
}

/**
 * Draws the current texture (of size w x h) over the screen once in the middle and 8 times around in up to 3 rings.
 * Radius is in pixels of the texture, strength multiplies the color and alpha, 0 skips the ring.
 */
static void Xray_DrawRings(IDirect3DDevice9* device, UINT w, UINT h, float middle, float radius1, float strength1, float radius2, float strength2, float radius3, float strength3) {
    float dirs[8][2] = { {1,0}, {-1,0}, {0,1}, {0,-1}, {0.7f,0.7f}, {-0.7f,0.7f}, {0.7f,-0.7f}, {-0.7f,-0.7f} };
    float radius[4] = { 0, radius1, radius2, radius3 };
    float strength[4] = { middle, strength1, strength2, strength3 };

    for (int ring = 0; ring < 4; ring++) {
        if (strength[ring] <= 0)
            continue;
        device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(strength[ring], strength[ring], strength[ring], strength[ring]));
        for (int i = 0; i < (ring == 0 ? 1 : 8); i++)
            Xray_DrawQuad(device, dirs[i][0] * radius[ring] / w, dirs[i][1] * radius[ring] / h);
    }
}

/** Alpha of the center fade at the distance from the screen center (in the fade radius): mix(minAlpha, 1, smoothstep(0, 1, distance)). */
static float Xray_CenterFade(float distance, float minAlpha) {
    float t = distance < 0 ? 0 : distance > 1 ? 1 : distance;
    t = t * t * (3 - 2 * t);
    return minAlpha + (1 - minAlpha) * t;
}

/** Creates the center fade texture (again when cg_xrayCenterFadeAlpha changed): color and alpha are the fade, it covers -1..1 of the fade radius. */
static bool Xray_PrepareFadeTexture(IDirect3DDevice9* device) {
    const int size = 128;
    float minAlpha = cg_xrayCenterFadeAlpha->value.decimal;
    if (xray_fade && xray_fadeTextureAlpha == minAlpha)
        return true;
    if (xray_fade) { xray_fade->Release(); xray_fade = NULL; }

    if (FAILED(device->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &xray_fade, NULL))) {
        xray_fade = NULL;
        return false;
    }
    D3DLOCKED_RECT rect;
    if (FAILED(xray_fade->LockRect(0, &rect, NULL, 0))) {
        xray_fade->Release();
        xray_fade = NULL;
        return false;
    }
    for (int y = 0; y < size; y++) {
        DWORD* row = (DWORD*)((char*)rect.pBits + y * rect.Pitch);
        for (int x = 0; x < size; x++) {
            // Texel center in -1..1, the edge texels are 1 (the texture is clamped outside)
            float dx = (x + 0.5f) / size * 2 - 1;
            float dy = (y + 0.5f) / size * 2 - 1;
            bool edge = x == 0 || y == 0 || x == size - 1 || y == size - 1;
            float alpha = edge ? 1.0f : Xray_CenterFade(sqrtf(dx * dx + dy * dy), minAlpha);
            row[x] = D3DCOLOR_COLORVALUE(alpha, alpha, alpha, alpha);
        }
    }
    xray_fade->UnlockRect(0);
    xray_fadeTextureAlpha = minAlpha;
    return true;
}

/** Turns on the center fade in the 2nd texture stage for the next quads: color and alpha are multiplied by the fade texture. */
static void Xray_SetCenterFade(IDirect3DDevice9* device, bool enable) {
    xray_fadeQuad = enable;
    if (enable) {
        device->SetTexture(1, xray_fade);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    } else {
        device->SetTexture(1, NULL);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

/** True if things drawn for hidden players fade near the screen center (cg_xrayCenterFadeRadius). */
bool xray_centerFadeEnabled() {
    return cg_xrayCenterFadeRadius->value.decimal > 0;
}

/**
 * Alpha of the center fade of a thing drawn for a hidden player, by the closest point of its box to the screen center.
 * The box is relative to the screen center in the 640x480 virtual space. 1 if the fade is off.
 */
float xray_centerFadeAlpha(float x0, float y0, float x1, float y1) {
    float radius = cg_xrayCenterFadeRadius->value.decimal;
    if (radius <= 0)
        return 1.0f;
    float x = x0 > 0 ? x0 : x1 < 0 ? x1 : 0;
    float y = y0 > 0 ? y0 : y1 < 0 ? y1 : 0;
    return Xray_CenterFade(sqrtf(x * x + y * y) / radius, cg_xrayCenterFadeAlpha->value.decimal);
}

/** Stencil test that passes where (stencil & mask) == ref, the stencil is not changed. */
static void Xray_SetStencilTest(IDirect3DDevice9* device, DWORD ref, DWORD mask) {
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILREF, ref);
    device->SetRenderState(D3DRS_STENCILMASK, mask);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
}

/** Updates the screen size, creates the outline mask texture with that size, returns true if the screen depth / stencil buffer can be used with it. */
static bool Xray_PrepareMask(IDirect3DDevice9* device, bool outline) {
    IDirect3DSurface9* screen = NULL;
    IDirect3DSurface9* depth = NULL;
    D3DSURFACE_DESC screenDesc, depthDesc;

    if (FAILED(device->GetRenderTarget(0, &screen)) || !screen)
        return false;
    bool hasDesc = SUCCEEDED(screen->GetDesc(&screenDesc));
    screen->Release();
    if (!hasDesc)
        return false;

    if (xray_maskWidth != screenDesc.Width || xray_maskHeight != screenDesc.Height) {
        Xray_ReleaseTextures();
    }
    if (outline && !xray_mask && FAILED(device->CreateTexture(screenDesc.Width, screenDesc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &xray_mask, NULL)))
        xray_mask = NULL;
    xray_maskWidth = screenDesc.Width;
    xray_maskHeight = screenDesc.Height;

    // The depth / stencil buffer can be used with the texture only when it's not multisampled
    bool compatible = false;
    if (SUCCEEDED(device->GetDepthStencilSurface(&depth))) {
        compatible = SUCCEEDED(depth->GetDesc(&depthDesc)) && depthDesc.MultiSampleType == D3DMULTISAMPLE_NONE &&
            depthDesc.Width >= screenDesc.Width && depthDesc.Height >= screenDesc.Height;
        depth->Release();
    }
    return compatible;
}

/** Switches drawing to the outline mask texture or back to the screen, keeps the viewport. */
static void Xray_SetMaskTarget(IDirect3DDevice9* device, bool mask, bool maskWithDepth, IDirect3DSurface9* screen, IDirect3DSurface9* depth) {
    D3DVIEWPORT9 viewport;
    device->GetViewport(&viewport);
    if (mask) {
        IDirect3DSurface9* surface = NULL;
        if (FAILED(xray_mask->GetSurfaceLevel(0, &surface)))
            return;
        device->SetRenderTarget(0, surface);
        device->SetDepthStencilSurface(maskWithDepth ? depth : NULL);
        surface->Release();
    } else {
        device->SetRenderTarget(0, screen);
        device->SetDepthStencilSurface(depth);
    }
    device->SetViewport(&viewport);
}

// Surfaces of tagged players and their entity numbers, while they are sorted
static GfxDrawSurf xray_sortSurfs[XRAY_MAX_SURFS];
static int xray_sortEntnums[XRAY_MAX_SURFS];

/** Moves surfaces of tagged players to the end of the list, so the players are drawn when all opaque world surfaces are in the depth buffer. */
static void Xray_SortPlayersLast(GfxDrawSurf* drawSurfs, int count) {
    GfxDrawSurf* players = xray_sortSurfs;
    int* playerEntnums = xray_sortEntnums;
    int playerCount = 0;
    int otherCount = 0;

    for (int i = 0; i < count; i++) {
        // Entity index from the sort key (see R_DecomposeSort), 0x7fe and more = world
        uint32_t key = drawSurfs[i].sortKey;
        int entnum = (int)key >= 0 ? (key >> 4) & 0xfff : (key >> 19) & 0xfff;

        if (entnum < 0x7fe && (backEnd_entities[entnum].renderfx & RF_XRAY) && playerCount < XRAY_MAX_SURFS) {
            // Grouped by player (insertion keeps the original order inside a player). Skinned players are in world space, so the renderer
            // merges following surfaces with the same material of different players into one draw call - with one team for all of them.
            int j = playerCount++;
            while (j > 0 && playerEntnums[j - 1] > entnum) {
                players[j] = players[j - 1];
                playerEntnums[j] = playerEntnums[j - 1];
                j--;
            }
            players[j] = drawSurfs[i];
            playerEntnums[j] = entnum;
        } else {
            drawSurfs[otherCount++] = drawSurfs[i];
        }
    }
    memcpy(&drawSurfs[otherCount], players, playerCount * sizeof(GfxDrawSurf));
}

/**
 * Called after a draw call of a player is drawn. Marks in the stencil where the player is visible (passes the depth test, also against
 * its own just written depth) and where it is hidden behind the world or players drawn before. The colors are drawn by these marks in Xray_DrawPlayersEnd.
 */
static void Xray_MarkPlayer(GfxDrawPrimArgs* args, GfxEntity* ent, int team) {
    IDirect3DDevice9* device = d3d9_device;
    DWORD visibleBit = team == TEAM_ALLIES ? XRAY_STENCIL_VISIBLE_ALLIES : XRAY_STENCIL_VISIBLE_AXIS;
    DWORD hiddenBit = team == TEAM_ALLIES ? XRAY_STENCIL_HIDDEN_ALLIES : XRAY_STENCIL_HIDDEN_AXIS;

    if (!Xray_CreateStateBlock(device))
        return;

    Xray_SaveState(device);
    Xray_SetFlatColor(device);

    // First player in this pass
    if (!xray_playersDrawn) {
        xray_playersDrawn = true;
        xray_maskUsesStencil = Xray_PrepareMask(device, xray_mode & XRAY_MASK);
        Xray_ClearStencil(device, XRAY_STENCIL_ALL);

        // Back to the vertex shader, declaration and stream of the player
        Xray_RestoreState(device);
        Xray_SaveState(device);
        Xray_SetFlatColor(device);
    }

    #define DRAW() device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, args->baseVertexIndex, args->minVertexIndex, args->numVertices, args->startIndex, args->primCount)

    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);

    // Visible pixels - moved closer to the camera by the hidden threshold, so a part only a little behind the world (feet in the ground) is visible.
    // The bias is in depth units (depth = 1 - zNear / distance), for the threshold in world units at the distance of the player it is zNear * threshold / distance^2.
    float bias = 0;
    if (xray_depthBias && cg_xrayHiddenThreshold->value.decimal > 0) {
        vec3_t delta;
        VectorSubtract(ent->origin, xray_viewOrigin, delta);
        float distanceSquared = DotProduct(delta, delta);
        if (distanceSquared > 1.0f)
            bias = -xray_zNear * cg_xrayHiddenThreshold->value.decimal / distanceSquared;
    }
    Xray_SetDepthBias(device, bias);
    device->SetRenderState(D3DRS_STENCILREF, visibleBit);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, visibleBit);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    DRAW();

    // Hidden pixels
    Xray_SetDepthBias(device, 0);
    device->SetRenderState(D3DRS_STENCILREF, hiddenBit);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, hiddenBit);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_REPLACE);
    DRAW();

    // Multisampled screen - the mask texture can not use the stencil, the whole player silhouette is drawn into it (visible parts too)
    if ((xray_mode & XRAY_MASK) && !xray_maskUsesStencil && xray_mask) {
        IDirect3DSurface9* screen = NULL;
        IDirect3DSurface9* depth = NULL;
        device->GetRenderTarget(0, &screen);
        device->GetDepthStencilSurface(&depth);

        Xray_SetFlatColor(device);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, xray_teamColor[team]);
        Xray_SetMaskTarget(device, true, false, screen, depth);
        if (!xray_maskCleared) {
            device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
            xray_maskCleared = true;
        }
        DRAW();
        Xray_SetMaskTarget(device, false, false, screen, depth);

        if (screen) screen->Release();
        if (depth) depth->Release();
    }

    #undef DRAW

    Xray_RestoreState(device);
}

/**
 * Gaussian blur of the glow texture (0 = small glow, 1 = big glow, both 1/2 of the screen): horizontal pass into a temporary texture and vertical pass back.
 * Fixed function: the texture is added 7 times moved by -3..3 texels * spread with gaussian weights. Leaves the render target set to the texture.
 */
static bool Xray_Blur(IDirect3DDevice9* device, int level, int spread) {
    static const float weights[4] = { 0.216f, 0.191f, 0.131f, 0.070f }; // sigma 2 texels, sum of -3..3 is 1
    UINT w = xray_maskWidth >> 1;
    UINT h = xray_maskHeight >> 1;

    if (!xray_blurTemp[level] && FAILED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &xray_blurTemp[level], NULL)))
        xray_blurTemp[level] = NULL;
    if (!xray_blurTemp[level])
        return false;

    device->SetDepthStencilSurface(NULL);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    Xray_SetTextureModulate(device);

    for (int pass = 0; pass < 2; pass++) {
        IDirect3DTexture9* from = pass == 0 ? xray_blur[level] : xray_blurTemp[level];
        IDirect3DTexture9* to = pass == 0 ? xray_blurTemp[level] : xray_blur[level];
        IDirect3DSurface9* surface = NULL;
        if (FAILED(to->GetSurfaceLevel(0, &surface)))
            return false;
        device->SetTexture(0, NULL); // a texture can not be drawn into while it's bound
        device->SetRenderTarget(0, surface);
        device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        device->SetTexture(0, from);
        for (int k = -3; k <= 3; k++) {
            float weight = weights[k < 0 ? -k : k];
            device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(weight, weight, weight, weight));
            Xray_DrawQuadSized(device, (float)w, (float)h, pass == 0 ? (float)(k * spread) / w : 0, pass == 1 ? (float)(k * spread) / h : 0);
        }
        surface->Release();
    }

    device->SetTexture(0, NULL);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    return true;
}

/**
 * Glow textures from the mask: downsampled to 1/2 of the screen into the small and the big glow texture, then gaussian blur.
 * Leaves the render target set to a glow texture, the caller puts back the screen. False if the textures can not be made.
 */
static bool Xray_MakeGlow(IDirect3DDevice9* device) {
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(0.25f, 0.25f, 0.25f, 0.25f));
    Xray_SetTextureModulate(device);

    // Maximum instead of average if the GPU supports it, so thin or small (far, low LOD) players have the same glow as big ones
    if (xray_blendMax) {
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MAX);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffffffff);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }

    IDirect3DTexture9* source = xray_mask;
    int levels = 0;
    UINT w = xray_maskWidth >> 1;
    UINT h = xray_maskHeight >> 1;
    for (int i = 0; i < 2; i++) {
        IDirect3DSurface9* surface = NULL;
        if (!xray_blur[i] && FAILED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &xray_blur[i], NULL)))
            xray_blur[i] = NULL;
        if (!xray_blur[i] || FAILED(xray_blur[i]->GetSurfaceLevel(0, &surface)))
            break;
        device->SetTexture(0, NULL); // a texture can not be drawn into while it's bound
        device->SetRenderTarget(0, surface); // viewport is set to the whole texture
        device->SetDepthStencilSurface(NULL);
        device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        device->SetTexture(0, source);

        if (xray_blendMax) {
            // Maximum of 4x4 mask texels (16 point samples at -1.5 .. 1.5 mask texels) - thin parts stay at full strength
            // and are a little thicker. The big glow is an exact copy of the small one.
            int taps = i == 0 ? 4 : 1;
            for (int x = 0; x < taps; x++) {
                for (int y = 0; y < taps; y++) {
                    float u = i == 0 ? (x - 1.5f) / (w * 2) : 0;
                    float v = i == 0 ? (y - 1.5f) / (h * 2) : 0;
                    Xray_DrawQuadSized(device, (float)w, (float)h, u, v);
                }
            }
        } else {
            // Average of 4x4 mask texels - 4 linear samples moved by 1 mask texel, each averaging 2x2 (only 2x2 would flicker when moving).
            // The big glow is a copy of the small one (softened a little by the same samples).
            for (int s = 0; s < 4; s++) {
                float u = (s & 1 ? 1.0f : -1.0f) / (w * 2);
                float v = (s & 2 ? 1.0f : -1.0f) / (h * 2);
                Xray_DrawQuadSized(device, (float)w, (float)h, u, v);
            }
        }
        surface->Release();
        source = xray_blur[i];
        levels++;
    }

    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    // Small light glow blurred twice, big glow twice and then once with the samples spread to every 2nd texel (wider)
    return levels == 2 &&
        Xray_Blur(device, 0, 1) && Xray_Blur(device, 0, 1) &&
        Xray_Blur(device, 1, 1) && Xray_Blur(device, 1, 1) && Xray_Blur(device, 1, 2);
}

/**
 * Draws the outline / glow of the mask over the screen. With the stencil marks the mask is made of the hidden and / or the visible parts of players,
 * otherwise (multisampled screen) it was drawn already with the whole players. Fade = the center fade (for hidden players).
 */
static void Xray_DrawMask(IDirect3DDevice9* device, IDirect3DSurface9* screen, IDirect3DSurface9* depth, bool hidden, bool visible, bool fade) {
    Xray_SetFlatColor(device);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);

    // Mask texture with the hidden parts in team color and / or the visible parts, made from the stencil marks
    if (xray_maskUsesStencil) {
        static const DWORD hiddenBits[2] = { XRAY_STENCIL_HIDDEN_AXIS, XRAY_STENCIL_HIDDEN_ALLIES };
        static const DWORD visibleBits[2] = { XRAY_STENCIL_VISIBLE_AXIS, XRAY_STENCIL_VISIBLE_ALLIES };
        D3DCOLOR colors[2] = { xray_teamColor[TEAM_AXIS], xray_teamColor[TEAM_ALLIES] };
        Xray_SetMaskTarget(device, true, true, screen, depth);
        device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        for (int i = 0; i < 2; i++) {
            device->SetRenderState(D3DRS_TEXTUREFACTOR, colors[i]);
            if (hidden) {
                Xray_SetStencilTest(device, hiddenBits[i], hiddenBits[i] | XRAY_STENCIL_VISIBLE);
                Xray_DrawQuad(device, 0, 0);
            }
            if (visible) {
                Xray_SetStencilTest(device, visibleBits[i], visibleBits[i]);
                Xray_DrawQuad(device, 0, 0);
            }
        }
        Xray_SetMaskTarget(device, false, true, screen, depth);
    }

    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // Glow is drawn from a blurred mask
    bool glow = false;
    if (xray_mode & XRAY_GLOW) {
        D3DVIEWPORT9 viewport;
        device->GetViewport(&viewport);
        glow = Xray_MakeGlow(device);
        device->SetRenderTarget(0, screen);
        device->SetDepthStencilSurface(depth);
        device->SetViewport(&viewport);
    }

    // The mask drawn over the screen, only where no player is.
    // The mask is premultiplied (team color * coverage on transparent black), so it's blended as one + (1 - alpha).
    Xray_SetTextureModulate(device);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    Xray_SetStencilTest(device, 0, XRAY_STENCIL_ALL);
    Xray_SetCenterFade(device, fade);

    if (glow) {
        // The blur spreads the mask, so it's amplified: texture * factor * 4 (big glow), * 2 (small glow)
        float bigIntensity = cg_xrayGlowIntensity->value.decimal / 4;
        float smallIntensity = cg_xrayGlowCoreIntensity->value.decimal / 2;

        // Big glow in team color, drawn twice to be denser
        device->SetTexture(0, xray_blur[1]);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE4X);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE4X);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(bigIntensity, bigIntensity, bigIntensity, bigIntensity));
        Xray_DrawQuad(device, 0, 0);
        Xray_DrawQuad(device, 0, 0);

        // Small glow added on top - team color added to team color is lighter (blue + blue = light blue)
        device->SetTexture(0, xray_blur[0]);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(smallIntensity, smallIntensity, smallIntensity, smallIntensity));
        Xray_DrawQuad(device, 0, 0);
    } else {
        // Outline: 2 rings in screen pixels around the sharp mask, opaque and half transparent
        device->SetTexture(0, xray_mask);
        Xray_DrawRings(device, xray_maskWidth, xray_maskHeight, 0, cg_xrayOutlineWidth->value.decimal, 1.0f, cg_xrayOutlineWidth->value.decimal * 2, 0.5f, 0, 0);
    }

    Xray_SetCenterFade(device, false);
}

/** Called after all players are drawn. Draws chams and outline by the stencil marks, then clears the marks. */
static void Xray_DrawPlayersEnd() {
    IDirect3DDevice9* device = d3d9_device;

    if (!Xray_CreateStateBlock(device))
        return;

    Xray_SaveState(device);
    Xray_SetFlatColor(device);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);

    // Chams - team color where a player is hidden and no player is visible
    if (xray_mode & XRAY_CHAMS) {
        Xray_SetStencilTest(device, XRAY_STENCIL_HIDDEN_AXIS, XRAY_STENCIL_HIDDEN_AXIS | XRAY_STENCIL_VISIBLE);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, xray_teamColor[TEAM_AXIS]);
        Xray_DrawQuad(device, 0, 0);
        Xray_SetStencilTest(device, XRAY_STENCIL_HIDDEN_ALLIES, XRAY_STENCIL_HIDDEN_ALLIES | XRAY_STENCIL_VISIBLE);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, xray_teamColor[TEAM_ALLIES]);
        Xray_DrawQuad(device, 0, 0);
    }

    if ((xray_mode & XRAY_MASK) && xray_mask) {
        IDirect3DSurface9* screen = NULL;
        IDirect3DSurface9* depth = NULL;
        device->GetRenderTarget(0, &screen);
        device->GetDepthStencilSurface(&depth);

        // The center fade is only for the hidden parts, so the visible parts (cg_xrayVisible* of the state) are drawn separately without it
        bool fade = xray_centerFadeEnabled() && Xray_PrepareFadeTexture(device);
        if (fade && xray_visibleParts && xray_maskUsesStencil) {
            Xray_DrawMask(device, screen, depth, false, true, false);
            Xray_DrawMask(device, screen, depth, true, false, true);
        } else {
            Xray_DrawMask(device, screen, depth, true, xray_visibleParts, fade);
        }

        if (screen) screen->Release();
        if (depth) depth->Release();
    }

    Xray_SetFlatColor(device);
    Xray_ClearStencil(device, XRAY_STENCIL_ALL);

    Xray_RestoreState(device);
}



// 004ce000    void* CG_AddCEntityToScene(struct GfxEntity* ent @ ecx, struct DObj* obj @ eax, struct centity_t* cent @ esi)
static void* CG_AddCEntityToScene(GfxEntity* ent, void* obj, centity_t* cent)
{
    // CoD2x: Tag the player for the renderer with his team
    int team = clientInfo[cent->nextState.clientNum].team & 3;
    if (xray_mode && xray_showTeam[team]) {
        ent->renderfx |= RF_XRAY | (team << RF_XRAY_TEAM_SHIFT);

        // Players are drawn with depth range 0..0.5, so their depth is lower than the depth of the world in front of them
        if (xray_mode & XRAY_THROUGH_WALLS)
            ent->renderfx |= RF_DEPTHHACK_HALF;
    }
    // CoD2x: End

    // Call the original function
    void* ret;
    ASM_CALL(RETURN(ret), 0x004ce000, 0, ECX(ent), EAX(obj), ESI(cent));
    return ret;
}

static void* CG_AddCEntityToScene_Win32() {
    GfxEntity* ent;
    void* obj;
    centity_t* cent;
    ASM( movr, ent, "ecx" );
    ASM( movr, obj, "eax" );
    ASM( movr, cent, "esi" );

    return CG_AddCEntityToScene(ent, obj, cent);
}


// 10008600 (gfx)    int32_t R_FilterEntityIntoCells_r(void* node, int32_t entnum, float* mins, float* maxs)
static int R_FilterEntityIntoCells_r(void* node, int entnum, float* mins, float* maxs)
{
    // CoD2x: Players are not culled by portals, otherwise they disappear when their cell is not visible
    // -1 = not in any cell, the model is drawn right away
    if (sceneEntities[entnum].renderfx & RF_XRAY) {
        return -1;
    }
    // CoD2x: End

    // Call the original function
    int ret;
    ASM_CALL(RETURN(ret), (gfx_module_addr + 0x00008600), 4, PUSH(node), PUSH(entnum), PUSH(mins), PUSH(maxs));
    return ret;
}


static void Xray_HeadDepthReset();

// 1002f850 (gfx)    void RB_RenderDrawSurfList(struct GfxDrawSurf* drawSurfs @ eax, int32_t count @ ecx, int32_t technique, int32_t order)
static void RB_RenderDrawSurfList(GfxDrawSurf* drawSurfs, int count, int technique, int order)
{
    // CoD2x: Safety - the head info test of the 2D drawing never stays on in the 3D scene
    Xray_HeadDepthReset();
    // CoD2x: End

    // CoD2x: Players are marked in the stencil in the lit pass for chams and outline (only with an 8 bit stencil buffer).
    // They are drawn last, the marks need the whole world in the depth buffer.
    if (technique == TECHNIQUE_LIT)
        xray_hasStencil = Xray_HasStencil(d3d9_device);
    xray_markPlayers = technique == TECHNIQUE_LIT && xray_hasStencil && (xray_mode & (XRAY_CHAMS | XRAY_MASK));
    if (xray_markPlayers && order == 0)
        Xray_SortPlayersLast(drawSurfs, count);
    xray_playersDrawn = false;
    xray_maskCleared = false;
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x0002f850), 2, EAX(drawSurfs), ECX(count), PUSH(technique), PUSH(order));

    // CoD2x: Chams and outline of the players drawn in this pass
    xray_markPlayers = false;
    if (xray_playersDrawn)
        Xray_DrawPlayersEnd();
    // CoD2x: End
}

static void RB_RenderDrawSurfList_Win32(int technique, int order) {
    GfxDrawSurf* drawSurfs;
    int count;
    ASM( movr, drawSurfs, "eax" );
    ASM( movr, count, "ecx" );

    RB_RenderDrawSurfList(drawSurfs, count, technique, order);
}


// 10038a70 (gfx)    void R_DrawIndexedPrimitive(struct GfxDrawPrimArgs* args @ esi)
static void R_DrawIndexedPrimitive(GfxDrawPrimArgs* args)
{
    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x00038a70), 0, ESI(args));

    // CoD2x: Mark visible and hidden parts of players in the lit pass for chams and outline.
    // After the draw, because materials can write the stencil too and would overwrite the marks.
    GfxEntity* ent = backEnd_currentEntity;
    if (xray_markPlayers && ent && (ent->renderfx & RF_XRAY)) {
        Xray_MarkPlayer(args, ent, (ent->renderfx >> RF_XRAY_TEAM_SHIFT) & 3);
    }
    // CoD2x: End
}

static void R_DrawIndexedPrimitive_Win32() {
    GfxDrawPrimArgs* args;
    ASM( movr, args, "esi" );

    R_DrawIndexedPrimitive(args);
}


// 10012270 (gfx)    void R_ReleaseForShutdownOrReset()
static void R_ReleaseForShutdownOrReset()
{
    // CoD2x: The device can not be reset or destroyed while we hold its objects
    Xray_ReleaseResources();
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x00012270), 0);
}



// ======================================================================================================
// Head info depth test - elements of the head info set to "when visible" (cg_head* = 1) are hidden behind the world per pixel, like the 3D
// nation icon of the game (a sprite drawn with the depth test, RB_DrawSpriteCmd). The 2D elements are drawn by the game as usual (CG_DrawRotatedPic,
// UI_DrawText), they only pass a stencil test:
//   1. MARK - a quad over the whole screen at the depth of the head point, tested against the depth of the scene, sets the stencil bit
//      XRAY_STENCIL_HEAD where the point is in front of the world and clears it where it is behind
//   2. TEST_ON / TEST_OFF - around the element, the stencil test passes only where the bit is set
//   3. CLEAR - clears the bit after the head info
// The steps are render commands in the order of the 2D drawing: stretch pics with the material xray_headMarker, which is never drawn - the
// backend function of the command (RB_StretchPicCmd) does the step instead.
// ======================================================================================================

enum { XRAY_HEAD_MARK = 1, XRAY_HEAD_TEST_ON, XRAY_HEAD_TEST_OFF, XRAY_HEAD_CLEAR };
#define XRAY_HEAD_ELEMENT_SPACE 0x1000  // free space of the render command list needed to start an element (element + TEST_OFF + CLEAR)

static int xray_headMarker;                         // address = material of the head info commands
static bool xray_headDepthInstalled = false;        // RB_StretchPicCmd is in the render command table
static bool xray_headStencil = false;               // the last MARK made the stencil bit (the test can be used)
static bool xray_headStencilUsed = false;           // the bit was set since the last CLEAR
static bool xray_headTestOn = false;                // TEST_ON changed the stencil states

// Stencil states of TEST_ON, the engine values are saved and put back by TEST_OFF
static const D3DRENDERSTATETYPE xray_headTestStates[] = {
    D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TWOSIDEDSTENCILMODE,
};
static const DWORD xray_headTestValues[] = { TRUE, D3DCMP_EQUAL, XRAY_STENCIL_HEAD, XRAY_STENCIL_HEAD, 0, FALSE };
#define XRAY_HEAD_TEST_STATES   (int)(sizeof(xray_headTestStates) / sizeof(xray_headTestStates[0]))
static DWORD xray_headTestSaved[XRAY_HEAD_TEST_STATES];


/** Draws the quads collected by the renderer, so they use the current states (the check before RB_EndSurface is inlined in the render commands). */
static void Xray_EndSurface() {
    if (tess_numIndexes || tess_field_24)
        RB_EndSurface();
}

/** Puts back the stencil states saved by TEST_ON, only those the engine did not change since (its state cache must stay right). */
static void Xray_HeadTestOff(IDirect3DDevice9* device) {
    if (!xray_headTestOn)
        return;
    xray_headTestOn = false;
    for (int i = 0; i < XRAY_HEAD_TEST_STATES; i++) {
        DWORD value;
        if (SUCCEEDED(device->GetRenderState(xray_headTestStates[i], &value)) && value == xray_headTestValues[i])
            device->SetRenderState(xray_headTestStates[i], xray_headTestSaved[i]);
    }
}

/**
 * MARK: sets the stencil bit where the head point (in view space: x right, y up, z forward) is in front of the world and clears it elsewhere.
 * The depth of the point is projected with the projection matrix of the scene, the same as the world was drawn with (depth range 0..1).
 */
static void Xray_HeadDepthMark(IDirect3DDevice9* device, float x, float y, float z) {
    xray_headStencil = false;
    char* viewParms = backEnd_viewParms;
    if (!viewParms || !Xray_HasStencil(device) || !Xray_CreateStateBlock(device))
        return;

    // Row vector times the D3D projection matrix (the engine passes it to SetTransform(D3DTS_PROJECTION))
    const float (*proj)[4] = (const float (*)[4])(viewParms + 0x88);
    float clipZ = x * proj[0][2] + y * proj[1][2] + z * proj[2][2] + proj[3][2];
    float clipW = x * proj[0][3] + y * proj[1][3] + z * proj[2][3] + proj[3][3];
    if (clipW <= 0)
        return;
    float depth = clipZ / clipW;
    depth = depth < 0 ? 0 : depth > 1 ? 1 : depth;

    Xray_SaveState(device);
    Xray_SetFlatColor(device);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    Xray_SetDepthBias(device, 0);
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_STENCILREF, XRAY_STENCIL_HEAD);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, XRAY_STENCIL_HEAD);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);    // in front of the world: set
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_ZERO);      // behind: cleared (only this bit, by the write mask)
    Xray_DrawDepthQuad(device, depth);
    Xray_RestoreState(device);

    xray_headStencil = true;
    xray_headStencilUsed = true;
}

/** CLEAR: clears the stencil bit on the whole screen. */
static void Xray_HeadDepthClear(IDirect3DDevice9* device) {
    xray_headStencil = false;
    if (!xray_headStencilUsed || !Xray_CreateStateBlock(device))
        return;
    xray_headStencilUsed = false;

    Xray_SaveState(device);
    Xray_SetFlatColor(device);
    Xray_ClearStencil(device, XRAY_STENCIL_HEAD);
    Xray_RestoreState(device);
}

/** Turns the head info test off and clears its stencil bit if the 2D drawing left them on (its last commands were not executed). */
static void Xray_HeadDepthReset() {
    if (!xray_headTestOn && !xray_headStencilUsed)
        return;
    IDirect3DDevice9* device = d3d9_device;
    Xray_EndSurface();
    Xray_HeadTestOff(device);
    Xray_HeadDepthClear(device);
}

/** Does one step of the head info depth test, called by the backend in the order of the render commands. */
static void Xray_HeadDepthCmd(int op, float x, float y, float z) {
    IDirect3DDevice9* device = d3d9_device;

    // The quads collected before the step are drawn with the states before it
    Xray_EndSurface();

    switch (op) {
        case XRAY_HEAD_MARK:
            Xray_HeadTestOff(device);
            Xray_HeadDepthMark(device, x, y, z);
            break;

        case XRAY_HEAD_TEST_ON:
            if (!xray_headStencil || xray_headTestOn)
                break;
            for (int i = 0; i < XRAY_HEAD_TEST_STATES; i++) {
                if (FAILED(device->GetRenderState(xray_headTestStates[i], &xray_headTestSaved[i])))
                    return;     // nothing is changed yet, the element is drawn without the test
            }
            for (int i = 0; i < XRAY_HEAD_TEST_STATES; i++)
                device->SetRenderState(xray_headTestStates[i], xray_headTestValues[i]);
            xray_headTestOn = true;
            break;

        case XRAY_HEAD_TEST_OFF:
            Xray_HeadTestOff(device);
            break;

        case XRAY_HEAD_CLEAR:
            Xray_HeadTestOff(device);
            Xray_HeadDepthClear(device);
            break;
    }
}


// 1002efe0 (gfx)    void RB_StretchPicCmd(void** execState)
static void RB_StretchPicCmd(void** execState)
{
    // CoD2x: Steps of the head info depth test are stretch pics with the marker material (see xray_headDepthTest), they are not drawn
    const char* cmd = (const char*)*execState;
    if (*(void* const*)(cmd + 0x04) == &xray_headMarker) {
        const float* data = (const float*)(cmd + 0x08);    // x = step, y, width, height = head point in view space
        Xray_HeadDepthCmd((int)data[0], data[1], data[2], data[3]);
        *execState = (void*)(cmd + *(const uint16_t*)(cmd + 0x02));
        return;
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x0002efe0), 1, PUSH(execState));
}


/** Adds a step of the head info depth test to the render commands. */
static void Xray_AddHeadDepthCmd(int op, float x, float y, float z) {
    static const float color[4] = { 1, 1, 1, 1 };
    R_AddCmdDrawStretchPic((float)op, x, y, z, 0, 0, 0, 0, color, &xray_headMarker);
}

/** True if the renderer can hide head info elements behind the world per pixel (xray_headDepthTest), it needs the stencil buffer. */
bool xray_headDepthAvailable() {
    return xray_headDepthInstalled && xray_hasStencil;
}

/**
 * Head info element drawn next is hidden behind the world per pixel, as if it was at the depth of the world point (the head info point
 * of the player). Marked = the depth of this point is already in the stencil (the next element of the same player), set to true.
 * Call xray_headDepthTestEnd after the element and xray_headDepthClear after the head info. False if the element can not be drawn (the render
 * command list is almost full, the end of the test could not be added).
 */
bool xray_headDepthTest(const vec3_t point, bool* marked) {
    // Free space as R_AddCmd* computes it. The test is started only when the whole element (a pic or a name, a few hundred bytes at most)
    // and its closing TEST_OFF and CLEAR commands surely fit, so the test can never stay on because the end was dropped.
    char* list = s_cmdlist;
    if (!xray_headDepthAvailable() || !list)
        return false;
    int space = 0x30000 - *(int*)(list + 0x30000) + *(int*)(list + 0x30004) - 0x2000;
    if (space < XRAY_HEAD_ELEMENT_SPACE)
        return false;

    if (!*marked) {
        // View space of D3D: x right, y up, z forward (viewaxis[1] points left)
        vec3_t delta;
        VectorSubtract(point, cg.refdef.vieworg, delta);
        Xray_AddHeadDepthCmd(XRAY_HEAD_MARK, -DotProduct(delta, cg.refdef.viewaxis[1]), DotProduct(delta, cg.refdef.viewaxis[2]), DotProduct(delta, cg.refdef.viewaxis[0]));
        *marked = true;
    }
    Xray_AddHeadDepthCmd(XRAY_HEAD_TEST_ON, 0, 0, 0);
    return true;
}

/** End of the head info element drawn with xray_headDepthTest. */
void xray_headDepthTestEnd() {
    Xray_AddHeadDepthCmd(XRAY_HEAD_TEST_OFF, 0, 0, 0);
}

/** End of the head info, clears the stencil bit if xray_headDepthTest marked any point. */
void xray_headDepthClear() {
    Xray_AddHeadDepthCmd(XRAY_HEAD_CLEAR, 0, 0, 0);
}



/** Color of the cvar ("r g b"), the team color dvar of the game if the cvar is empty. */
static void Xray_ReadColor(dvar_t* dvar, dvar_t* gameDvar, float r, float g, float b, vec4_t color) {
    color[0] = r; color[1] = g; color[2] = b;
    if (dvar->value.string[0])
        Dvar_StringToColor(dvar, (float (*)[4])color);
    else if (gameDvar)
        Dvar_StringToColor(gameDvar, (float (*)[4])color);
    color[3] = 1.0f;
}

/** Color of the players of the team (team_t), free-for-all players have the axis color. */
void xray_getTeamColor(int team, vec4_t color) {
    memcpy(color, xray_teamColorVec[team & 3], sizeof(vec4_t));
}

/** Called every frame on frame start. */
void xray_frame() {
    // cg_xray: 1 = players through walls, 2 = chams, 3 = outline, 4 = glow
    static const int modes[] = { 0, XRAY_THROUGH_WALLS, XRAY_CHAMS, XRAY_OUTLINE, XRAY_GLOW };
    // States: 1 = outline, 2 = glow
    static const int stateModes[] = { 0, XRAY_OUTLINE, XRAY_GLOW };

    // Who is watching: a real spectator (demo playback is watched like a spectator), a dead player in the spectator session (following teammates)
    // or a playing player
    xray_viewer = XRAY_VIEWER_NONE;
    xray_mode = 0;
    xray_visibleParts = false;
    if (clientState == CLIENT_STATE_ACTIVE && cg.snap) {
        int team = clientInfo[cg.clientNum].team;
        // Spectating = following somebody else or flying free; a playing player is never a spectator, even if his client info still says so
        bool spectating = cg.snap->ps.clientNum != cg.clientNum || (cg.snap->ps.pm_flags & PMF_SPECTATING);
        if (demo_isPlaying || (team == TEAM_SPECTATOR && spectating))
            xray_viewer = XRAY_VIEWER_SPECTATOR;
        else if (spectating)
            xray_viewer = XRAY_VIEWER_DEAD;
        else
            xray_viewer = XRAY_VIEWER_PLAYER;

        // Spectators see everybody, dead players only their teammates, so it tells nothing about the enemies. Playing players have no x-ray (like CS:GO).
        // The global override shows everybody in every state, only with sv_cheats, in demo playback or in debug build.
        bool cheats = XRAY_ALWAYS_ALLOWED || demo_isPlaying || sv_cheats->value.boolean;
        bool all = true;
        dvar_t* style = NULL;
        dvar_t* visible = NULL;
        if (xray_viewer == XRAY_VIEWER_SPECTATOR) {
            style = cg_xraySpectator;
            visible = cg_xrayVisibleSpectator;
        } else if (xray_viewer == XRAY_VIEWER_DEAD) {
            style = cg_xrayDead;
            visible = cg_xrayVisibleDead;
            all = false;
        } else {
            all = false;
        }
        if (cheats && cg_xray->value.integer) {
            xray_mode = modes[cg_xray->value.integer];
            all = true;
        } else if (style)
            xray_mode = stateModes[style->value.integer];
        xray_visibleParts = visible && visible->value.boolean;

        xray_showTeam[TEAM_NONE] = all;                         // free-for-all players have no teammates
        xray_showTeam[TEAM_AXIS] = all || team == TEAM_AXIS;
        xray_showTeam[TEAM_ALLIES] = all || team == TEAM_ALLIES;
        xray_showTeam[TEAM_SPECTATOR] = false;
    }

    // Camera and near plane for the hidden threshold
    static dvar_t* r_znear = NULL;
    if (!r_znear)
        r_znear = Dvar_GetDvarByName("r_znear");
    if (r_znear && r_znear->type == DVAR_TYPE_FLOAT)
        xray_zNear = r_znear->value.decimal;
    VectorCopy(cg.refdef.vieworg, xray_viewOrigin);

    // Team colors
    Xray_ReadColor(cg_xrayColorAllies, g_TeamColor_Allies, 0.2f, 0.4f, 1.0f, xray_teamColorVec[TEAM_ALLIES]);
    Xray_ReadColor(cg_xrayColorAxis, g_TeamColor_Axis, 1.0f, 0.2f, 0.2f, xray_teamColorVec[TEAM_AXIS]);
    memcpy(xray_teamColorVec[TEAM_NONE], xray_teamColorVec[TEAM_AXIS], sizeof(vec4_t));
    memcpy(xray_teamColorVec[TEAM_SPECTATOR], xray_teamColorVec[TEAM_ALLIES], sizeof(vec4_t));
    for (int i = 0; i < 4; i++)
        xray_teamColor[i] = D3DCOLOR_COLORVALUE(xray_teamColorVec[i][0], xray_teamColorVec[i][1], xray_teamColorVec[i][2], 1.0f);
}

// Calls in the renderer DLL replaced by x-ray (offsets from the DLL base)
static const struct { unsigned int offset; void* hook; } xray_gfxCalls[] = {
    { 0x0000885a, (void*)R_FilterEntityIntoCells_r },       // 1000885a  e8a1fdffff  call R_FilterEntityIntoCells_r   (DPVS add of a model entity: draw now on -1, otherwise only when its cell is visible)
    { 0x0002fd89, (void*)RB_RenderDrawSurfList_Win32 },     // 1002fd89  e8c2faffff  call RB_RenderDrawSurfList   (in RB_DrawSurfsCmd)
    { 0x0003a554, (void*)R_DrawIndexedPrimitive_Win32 },    // 1003a554  e817e5ffff  call R_DrawIndexedPrimitive   (in RB_DrawTechnique)
    { 0x0003a56c, (void*)R_DrawIndexedPrimitive_Win32 },    // 1003a56c  e8ffe4ffff  call R_DrawIndexedPrimitive   (in RB_DrawTechnique)
    { 0x00012e70, (void*)R_ReleaseForShutdownOrReset },     // 10012e70  e8fbf3ffff  call R_ReleaseForShutdownOrReset   (device shutdown)
    { 0x0001385b, (void*)R_ReleaseForShutdownOrReset },     // 1001385b  e810eaffff  call R_ReleaseForShutdownOrReset   (lost device reset)
};
#define XRAY_GFX_CALLS  (int)(sizeof(xray_gfxCalls) / sizeof(xray_gfxCalls[0]))
static void* xray_gfxOriginal[XRAY_GFX_CALLS];      // original targets of the calls, put back on unload
static void* xray_gfxOriginalStretchPicCmd = NULL;  // original rb_renderCommandTable[RC_STRETCH_PIC]
static unsigned int xray_gfxPatched = 0;            // base of the renderer DLL the patches are in, 0 = none

/** Called when the game loaded the renderer DLL (game start, vid_restart). */
void xray_rendered() {
    for (int i = 0; i < XRAY_GFX_CALLS; i++)
        xray_gfxOriginal[i] = patch_call(gfx_module_addr + xray_gfxCalls[i].offset, (unsigned int)xray_gfxCalls[i].hook);

    // 10190554  rb_renderCommandTable[0x0f] = RB_StretchPicCmd   (the only call: RB_ExecuteRenderCommands, call [id * 4 + rb_renderCommandTable])
    xray_gfxOriginalStretchPicCmd = rb_renderCommandTable[RC_STRETCH_PIC];
    patch_int32((unsigned int)&rb_renderCommandTable[RC_STRETCH_PIC], (int32_t)RB_StretchPicCmd);
    xray_gfxPatched = gfx_module_addr;

    xray_headDepthInstalled = true;
    xray_headStencil = false;
    xray_headStencilUsed = false;
    xray_headTestOn = false;
}

/** Called before the DLL is unloaded (hot reload). Releases the D3D objects (the device could not be reset with them) and puts back the renderer patches. */
void xray_unload() {
    Xray_ReleaseResources();

    if (xray_gfxPatched && xray_gfxPatched == gfx_module_addr) {
        for (int i = 0; i < XRAY_GFX_CALLS; i++)
            patch_call(gfx_module_addr + xray_gfxCalls[i].offset, (unsigned int)xray_gfxOriginal[i]);
        patch_int32((unsigned int)&rb_renderCommandTable[RC_STRETCH_PIC], (int32_t)xray_gfxOriginalStretchPicCmd);
    }
    xray_gfxPatched = 0;
    xray_headDepthInstalled = false;
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void xray_init() {
    enum dvarFlags_e flags = (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET);  // not saved to the config

    // Styles of cg_xray: 0 = off, 1 = players drawn through walls, 2 = chams, 3 = outline, 4 = glow
    cg_xray =                   Dvar_RegisterInt("cg_xray", 0, 0, 4, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET | XRAY_CHEAT_FLAG));
    // Styles of the states: 0 = none, 1 = outline, 2 = glow (playing players have no x-ray)
    cg_xraySpectator =          Dvar_RegisterInt("cg_xraySpectator", 2, 0, 2, flags);
    cg_xrayDead =               Dvar_RegisterInt("cg_xrayDead", 2, 0, 2, flags);
    // Outline / glow also around the visible parts of players (players in sight too), 0 = only around the hidden parts
    cg_xrayVisibleSpectator =   Dvar_RegisterBool("cg_xrayVisibleSpectator", false, flags);
    cg_xrayVisibleDead =        Dvar_RegisterBool("cg_xrayVisibleDead", false, flags);
    // X-ray and head info of hidden players fade near the screen center: radius in the 640x480 virtual space (0 = off), alpha at the center
    cg_xrayCenterFadeRadius =   Dvar_RegisterFloat("cg_xrayCenterFadeRadius", 80.0f, 0.0f, 320.0f, flags);
    cg_xrayCenterFadeAlpha =    Dvar_RegisterFloat("cg_xrayCenterFadeAlpha", 0.15f, 0.0f, 1.0f, flags);

    cg_xrayHiddenThreshold =    Dvar_RegisterFloat("cg_xrayHiddenThreshold", 24.0f, 0.0f, 256.0f, flags);
    cg_xrayOutlineWidth =       Dvar_RegisterFloat("cg_xrayOutlineWidth", 1.5f, 0.5f, 4.0f, flags);
    cg_xrayGlowIntensity =      Dvar_RegisterFloat("cg_xrayGlowIntensity", 1.0f, 0.0f, 4.0f, flags);
    cg_xrayGlowCoreIntensity =  Dvar_RegisterFloat("cg_xrayGlowCoreIntensity", 0.5f, 0.0f, 2.0f, flags);
    cg_xrayColorAllies =        Dvar_RegisterString("cg_xrayColorAllies", "", flags);
    cg_xrayColorAxis =          Dvar_RegisterString("cg_xrayColorAxis", "", flags);
}

/** Called before the entry point is called. Used to patch the memory. */
void xray_patch() {
    // 004dc816  e8e517ffff  call CG_AddCEntityToScene   (in CG_Player)
    patch_call(0x004dc816, (unsigned int)CG_AddCEntityToScene_Win32);
}
