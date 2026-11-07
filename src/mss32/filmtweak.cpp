/*
 * Film tweaks - color adjustment of the 3D scene, like r_filmTweak* in CoD4.
 *
 * Cvars (archived, applied live, the defaults change nothing):
 *   r_filmTweakEnable          0/1         master switch, 1 by default so a change of the values below is visible immediately
 *   r_filmTweakContrast        0 .. 4      1 = unchanged, contrast around the middle grey: (c - 0.5) * contrast + 0.5
 *   r_filmTweakBrightness     -1 .. 1      0 = unchanged, added to the color
 *   r_filmTweakGamma         0.5 .. 2      1 = unchanged, pow(c, 1 / gamma), bigger = brighter shadows
 *   r_filmTweakSaturation      0 .. 2      1 = unchanged, 0 = grey, 2 = doubled - the world
 *   r_filmTweakSkySaturation   0 .. 2      the same for the sky
 *   r_filmTweakColor           vec3 0 .. 2 multipliers of red, green and blue
 *   r_filmTweakDarkTint        vec3 0 .. 2 color multiplier of dark pixels, blended by the brightness of the pixel
 *   r_filmTweakLightTint       vec3 0 .. 2 to the light tint for bright pixels (like CoD4)
 * Example: pixel 0.40 0.30 0.20 with contrast 1.2, brightness 0.05 and saturation 0.5:
 *   contrast + brightness 0.43 0.31 0.19, luminance 0.332, saturation 0.381 0.321 0.261
 *
 * How it works (renderer DLL gfx_d3d_mp_x86_s.dll):
 *   - The frontend R_RenderScene (gfx_funcs slot, called once per frame by CG_DrawActive) adds the render commands of the 3D scene.
 *     After it we add our own command FILMTWEAK_CMD_ID. Render commands run in order in the backend, so ours runs after the whole
 *     scene (sky, world, effects, DX9 glow and sun flare) and before the HUD, console and menus that are added later.
 *   - The command id 34 is free: rb_renderCommandTable has the commands 0 .. 33 followed by zeros, and the backend
 *     (RB_ExecuteRenderCommands) calls rb_renderCommandTable[id] without a range check.
 *   - The backend function copies the back buffer to a texture with StretchRect (also resolves MSAA) and draws it back over the
 *     scene viewport with our ps_2_0 shader (filmtweak.hlsl). Both DX9 and DX7 (r_rendererPreference) use the same IDirect3DDevice9,
 *     DX7 only selects other material techniques of the game, so both go through exactly the same code.
 *   - Sky vs world: the scene is cleared to depth 1.0, the sky is drawn first and does not write depth (TECHNIQUE_SKY = 1), the world
 *     is drawn after it. So the pixels that still have depth 1.0 at the end show the sky. The quad is drawn at depth 1.0 twice: with
 *     the depth test LESSEQUAL only on the sky pixels with the sky saturation, with GREATER on the world with the world saturation.
 *     (Marking the sky in stencil would not work - the sky is drawn before the world, the marks would stay under the world too.)
 *   - All values are computed in the frontend and stored in the command, the backend reads only the command.
 *   - When everything is neutral, or r_filmTweakEnable is 0, no command is added (zero cost).
 *
 * r_gamma (game cvar, 0.5 .. 3, 1 = unchanged) uses the same pass over the whole frame:
 *   - The game sets r_gamma as the hardware gamma ramp of the device (RB_EndFrame -> RB_SetGammaRamp). That works only in
 *     fullscreen, changes the whole monitor and stays changed when the game crashes. CoD2x keeps the hardware ramp identity
 *     and the frontend R_EndFrame (gfx_funcs slot, called once per frame after all drawing) adds our command with only the
 *     gamma as the last command of the frame, so it covers the scene, HUD, menus and console - in fullscreen and windowed
 *     mode, without touching the monitor. Screenshots include it, as they read the back buffer after the commands.
 *   Example: r_gamma 1.5, pixel 0.20 -> pow(0.20, 1 / 1.5) = 0.34, pixel 0.80 -> 0.86 (dark parts get brighter the most).
 */

#include "filmtweak.h"

#include <windows.h>
#include <d3d9.h>
#include <string.h>

#include "shared.h"
#include "hook.h"
#include "drawing.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"
#include "filmtweak_ps.h"


dvar_t* r_filmTweakEnable;
dvar_t* r_filmTweakContrast;
dvar_t* r_filmTweakBrightness;
dvar_t* r_filmTweakGamma;
dvar_t* r_filmTweakSaturation;
dvar_t* r_filmTweakSkySaturation;
dvar_t* r_filmTweakColor;
dvar_t* r_filmTweakDarkTint;
dvar_t* r_filmTweakLightTint;



// ======================================================================================================
// Engine
// ======================================================================================================

// Renderer (gfx dll)
#define d3d9_device             (*((IDirect3DDevice9**)(gfx_module_addr + 0x001d1bf8)))
#define tess_numIndexes         (*((int*)(gfx_module_addr + 0x009817d0)))         // tess + 0x14, quads collected for the current material
#define tess_field_24           (*((int*)(gfx_module_addr + 0x009817e0)))         // tess + 0x24, other primitives collected (RB_EndSurface draws both)
#define s_cmdlist               (*((char**)(gfx_module_addr + 0x00438780)))       // render commands being added: commands at 0, +0x30000 used bytes, +0x30004 extra size, +0x30008 last command
#define rb_renderCommandTable   ((void**)(gfx_module_addr + 0x00190518))           // backend functions by command id (RB_ExecuteRenderCommands)
#define rg_registered           (*((bool*)(gfx_module_addr + 0x001d4d00)))       // rg, first byte: renderer initialized (R_EndFrame does nothing without it)
#define r_gamma                 (*((dvar_t**)(gfx_module_addr + 0x001ce62c)))
#define r_ignoreHwGamma         (*((dvar_t**)(gfx_module_addr + 0x001ce700)))

#define CMDLIST_SIZE            0x30000

// Our render command, the first free id after the 34 commands of the game (0 = end of the list .. 33 = RB_TouchAllImagesCmd)
#define FILMTWEAK_CMD_ID        34

// Renderer functions, copied by the engine into the table gfx_funcs (0x0068a1e8) after the renderer is loaded
typedef void (__cdecl *R_RenderScene_t)(refdef_t* refdef);
#define gfx_funcs_R_RenderScene (*((R_RenderScene_t*)0x0068a27c))

typedef void (__cdecl *R_EndFrame_t)();
#define gfx_funcs_R_EndFrame    (*((R_EndFrame_t*)0x0068a2ac))

// Original renderer functions, saved when ours are put into gfx_funcs
static R_RenderScene_t filmtweak_R_RenderScene = NULL;
static R_EndFrame_t filmtweak_R_EndFrame = NULL;

// 100122a5  e8a6160100  call R_ShutdownRenderTargets   (in R_ReleaseForShutdownOrReset, before the device is reset or destroyed)
#define FILMTWEAK_SHUTDOWN_CALL 0x000122a5
#define FILMTWEAK_SHUTDOWN_FUNC 0x00023950

// 10033fd5  e866ccffff  call RB_SetGammaRamp   (in RB_EndFrame, when r_gamma or r_ignorehwgamma changed, the only call in use)
#define FILMTWEAK_GAMMARAMP_CALL 0x00033fd5
#define FILMTWEAK_GAMMARAMP_FUNC 0x00030c40



// ======================================================================================================
// Render command
// ======================================================================================================

// Shader constants of one pass, registers c0 .. c4 of filmtweak.hlsl
struct FilmTweakConstants {
    float contrast[4];      // c0 xyz = contrast
    float bias[4];          // c1 xyz = brightness + 0.5 - 0.5 * contrast
    float tintDark[4];      // c2 xyz = dark tint * color multipliers
    float tintDelta[4];     // c3 xyz = (light tint - dark tint) * color multipliers
    float params[4];        // c4 x = saturation, y = 1 / gamma
};

struct FilmTweakCmd {
    uint16_t id;            // FILMTWEAK_CMD_ID
    uint16_t byteCount;     // size of the command, the backend moves to the next command by it
    int32_t x, y, width, height;        // viewport of the scene in pixels (refdef)
    int32_t splitSky;       // 1 = separate passes for the sky and the world, 0 = one pass with the world constants
    FilmTweakConstants world;
    FilmTweakConstants sky;
};

static bool filmtweak_installed = false;        // our command, R_RenderScene and the shutdown hook are in the renderer
static unsigned int filmtweak_installedGfx = 0; // gfx_module_addr the hooks were installed into

// D3D objects, released before the device is reset or destroyed
static IDirect3DTexture9* filmtweak_texture = NULL;     // copy of the back buffer
static UINT filmtweak_textureWidth = 0;
static UINT filmtweak_textureHeight = 0;
static D3DFORMAT filmtweak_textureFormat = D3DFMT_UNKNOWN;
static IDirect3DPixelShader9* filmtweak_shader = NULL;
static IDirect3DStateBlock9* filmtweak_state = NULL;   // engine values of the states we change
static IDirect3DVertexDeclaration9* filmtweak_savedDecl = NULL;  // engine vertex declaration (not in the state block)
static DWORD filmtweak_savedFVF = 0;
static int filmtweak_supported = -1;            // -1 = not checked yet, 0 = the GPU has no pixel shader 2.0, 1 = ok
static bool filmtweak_failureShown = false;
static bool filmtweak_hwGammaRamp = false;      // the hardware ramp has r_gamma (the shader pass is not available)
static const char* volatile filmtweak_failureReason = NULL; // failure to print on the main thread (static string)

static const float filmtweak_one[3] = { 1, 1, 1 };


/** Releases all D3D objects, must be done before the device is reset or destroyed. */
static void FilmTweak_ReleaseResources() {
    if (filmtweak_texture) { filmtweak_texture->Release(); filmtweak_texture = NULL; }
    if (filmtweak_shader) { filmtweak_shader->Release(); filmtweak_shader = NULL; }
    if (filmtweak_state) { filmtweak_state->Release(); filmtweak_state = NULL; }
    filmtweak_textureWidth = 0;
    filmtweak_textureHeight = 0;
    filmtweak_textureFormat = D3DFMT_UNKNOWN;
    filmtweak_supported = -1;
}

/** Prints the reason once and turns the pass off until the device is created again. */
static void FilmTweak_Fail(const char* reason) {
    filmtweak_supported = 0;
    // r_gamma falls back to the hardware gamma ramp of the game (RB_SetGammaRamp on the next RB_EndFrame)
    if (r_gamma && !filmtweak_hwGammaRamp)
        r_gamma->modified = true;
    if (filmtweak_failureShown)
        return;
    filmtweak_failureShown = true;
    // Called by the backend, maybe on the render thread - Com_Printf is not thread safe, printed by filmtweak_frame
    filmtweak_failureReason = reason;
}

// Render states of the pass (ZENABLE and ZFUNC are changed per quad), also the states saved and restored by the state block
static const struct { D3DRENDERSTATETYPE state; DWORD value; } filmtweak_renderStates[] = {
    { D3DRS_ZENABLE, D3DZB_FALSE }, { D3DRS_ZFUNC, D3DCMP_ALWAYS }, { D3DRS_ZWRITEENABLE, FALSE },
    { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
    { D3DRS_STENCILENABLE, FALSE }, { D3DRS_TWOSIDEDSTENCILMODE, FALSE },
    { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE },
    { D3DRS_FOGENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
    { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
    { D3DRS_DEPTHBIAS, 0 }, { D3DRS_SLOPESCALEDEPTHBIAS, 0 }, { D3DRS_CLIPPLANEENABLE, 0 },
};

// Sampler states of the screen copy in sampler 0 (pixel exact)
static const struct { D3DSAMPLERSTATETYPE state; DWORD value; } filmtweak_samplerStates[] = {
    { D3DSAMP_MINFILTER, D3DTEXF_POINT }, { D3DSAMP_MAGFILTER, D3DTEXF_POINT }, { D3DSAMP_MIPFILTER, D3DTEXF_NONE },
    { D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP }, { D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP }, { D3DSAMP_SRGBTEXTURE, FALSE },
};

static void FilmTweak_SetPassStates(IDirect3DDevice9* device) {
    for (size_t i = 0; i < sizeof(filmtweak_renderStates) / sizeof(filmtweak_renderStates[0]); i++)
        device->SetRenderState(filmtweak_renderStates[i].state, filmtweak_renderStates[i].value);
    for (size_t i = 0; i < sizeof(filmtweak_samplerStates) / sizeof(filmtweak_samplerStates[0]); i++)
        device->SetSamplerState(0, filmtweak_samplerStates[i].state, filmtweak_samplerStates[i].value);
}

/** State block with all states set by the pass, to save and restore the engine values. */
static bool FilmTweak_CreateStateBlock(IDirect3DDevice9* device) {
    if (filmtweak_state)
        return true;

    D3DVIEWPORT9 viewport;
    device->GetViewport(&viewport);

    // Only the list of states is recorded, keep the engine state in case the driver applies them while recording
    IDirect3DStateBlock9* engineState = NULL;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &engineState)))
        return false;

    device->BeginStateBlock();
    FilmTweak_SetPassStates(device);
    device->SetTexture(0, NULL);
    device->SetPixelShader(NULL);
    device->SetVertexShader(NULL);
    float zero[5][4] = {};
    device->SetPixelShaderConstantF(0, &zero[0][0], 5);   // the engine may cache its constants too
    device->SetStreamSource(0, NULL, 0, 0);     // DrawPrimitiveUP resets stream 0
    device->SetViewport(&viewport);
    // The vertex declaration is saved separately in FilmTweak_SaveState - a recorded SetFVF would restore FVF 0 instead of the engine declaration

    if (FAILED(device->EndStateBlock(&filmtweak_state)))
        filmtweak_state = NULL;

    engineState->Apply();
    engineState->Release();
    return filmtweak_state != NULL;
}

/** Saves the engine values of all states changed by the pass. */
static void FilmTweak_SaveState(IDirect3DDevice9* device) {
    filmtweak_state->Capture();
    device->GetVertexDeclaration(&filmtweak_savedDecl);
    device->GetFVF(&filmtweak_savedFVF);
}

/**
 * Restores the engine values of all states changed by the pass. The engine caches its states and does not set them again,
 * so everything must be exactly as before (a missing vertex declaration stops drawing of everything that uses it).
 */
static void FilmTweak_RestoreState(IDirect3DDevice9* device) {
    filmtweak_state->Apply();
    if (filmtweak_savedDecl) {
        device->SetVertexDeclaration(filmtweak_savedDecl);
        filmtweak_savedDecl->Release();
        filmtweak_savedDecl = NULL;
    } else {
        device->SetFVF(filmtweak_savedFVF);
    }
}

/** Creates the shader, the state block and the texture of the size and format of the render target. */
static bool FilmTweak_PrepareResources(IDirect3DDevice9* device, const D3DSURFACE_DESC* targetDesc) {
    if (filmtweak_supported == 0)
        return false;

    if (filmtweak_supported == -1) {
        D3DCAPS9 caps;
        if (FAILED(device->GetDeviceCaps(&caps)) || caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
            FilmTweak_Fail("the graphics card does not support pixel shader 2.0");
            return false;
        }
        filmtweak_supported = 1;
    }

    if (!filmtweak_shader && FAILED(device->CreatePixelShader((const DWORD*)filmtweak_psBytecode, &filmtweak_shader))) {
        filmtweak_shader = NULL;
        FilmTweak_Fail("the pixel shader could not be created");
        return false;
    }

    if (!FilmTweak_CreateStateBlock(device)) {
        FilmTweak_Fail("the state block could not be created");
        return false;
    }

    if (filmtweak_texture && (filmtweak_textureWidth != targetDesc->Width || filmtweak_textureHeight != targetDesc->Height ||
                              filmtweak_textureFormat != targetDesc->Format)) {
        filmtweak_texture->Release();
        filmtweak_texture = NULL;
    }
    if (!filmtweak_texture) {
        if (FAILED(device->CreateTexture(targetDesc->Width, targetDesc->Height, 1, D3DUSAGE_RENDERTARGET, targetDesc->Format,
                                         D3DPOOL_DEFAULT, &filmtweak_texture, NULL))) {
            filmtweak_texture = NULL;
            FilmTweak_Fail("the screen copy texture could not be created");
            return false;
        }
        filmtweak_textureWidth = targetDesc->Width;
        filmtweak_textureHeight = targetDesc->Height;
        filmtweak_textureFormat = targetDesc->Format;
    }

    // The pass works again (after a reset) - put the hardware ramp back to identity, r_gamma is applied by the pass
    if (filmtweak_hwGammaRamp && r_gamma)
        r_gamma->modified = true;

    return true;
}

/**
 * Quad over the rectangle of the render target of size w x h, with the texture coordinates of the same pixels.
 * It is at the far plane (depth 1.0), the depth test against the scene splits the sky and the world.
 */
static void FilmTweak_DrawQuad(IDirect3DDevice9* device, const RECT* rect, float w, float h) {
    const float z = 1.0f;
    float x0 = (float)rect->left, y0 = (float)rect->top, x1 = (float)rect->right, y1 = (float)rect->bottom;
    float quad[4][6] = { // x, y, z, rhw, u, v (-0.5 maps the pixel centers to the texel centers)
        { x0 - 0.5f, y0 - 0.5f, z, 1, x0 / w, y0 / h },
        { x1 - 0.5f, y0 - 0.5f, z, 1, x1 / w, y0 / h },
        { x0 - 0.5f, y1 - 0.5f, z, 1, x0 / w, y1 / h },
        { x1 - 0.5f, y1 - 0.5f, z, 1, x1 / w, y1 / h },
    };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

/** True if the depth buffer belongs to the render target (same size and multisampling), so it has the depth of the scene. */
static bool FilmTweak_HasSceneDepth(IDirect3DDevice9* device, const D3DSURFACE_DESC* targetDesc) {
    IDirect3DSurface9* depth = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(device->GetDepthStencilSurface(&depth)) || !depth)
        return false;
    bool ok = SUCCEEDED(depth->GetDesc(&desc));
    depth->Release();
    return ok && desc.Width >= targetDesc->Width && desc.Height >= targetDesc->Height &&
           desc.MultiSampleType == targetDesc->MultiSampleType && desc.MultiSampleQuality == targetDesc->MultiSampleQuality;
}

/** Applies the color pass to the scene viewport of the current render target (the back buffer). */
static void FilmTweak_Apply(IDirect3DDevice9* device, const FilmTweakCmd* cmd) {
    IDirect3DSurface9* target = NULL;
    D3DSURFACE_DESC targetDesc;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target)
        return;
    if (FAILED(target->GetDesc(&targetDesc)) || !FilmTweak_PrepareResources(device, &targetDesc)) {
        target->Release();
        return;
    }

    // Scene viewport, inside the render target
    RECT rect;
    rect.left = cmd->x < 0 ? 0 : cmd->x;
    rect.top = cmd->y < 0 ? 0 : cmd->y;
    rect.right = cmd->x + cmd->width > (int)targetDesc.Width ? (int)targetDesc.Width : cmd->x + cmd->width;
    rect.bottom = cmd->y + cmd->height > (int)targetDesc.Height ? (int)targetDesc.Height : cmd->y + cmd->height;
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        target->Release();
        return;
    }

    // Copy of the scene (resolves MSAA)
    IDirect3DSurface9* copy = NULL;
    HRESULT hr = filmtweak_texture->GetSurfaceLevel(0, &copy);
    if (SUCCEEDED(hr)) {
        hr = device->StretchRect(target, &rect, copy, &rect, D3DTEXF_NONE);
        copy->Release();
    }
    target->Release();
    if (FAILED(hr))
        return;

    FilmTweak_SaveState(device);

    D3DVIEWPORT9 viewport = { 0, 0, targetDesc.Width, targetDesc.Height, 0.0f, 1.0f };
    device->SetViewport(&viewport);
    device->SetVertexShader(NULL);
    device->SetPixelShader(filmtweak_shader);
    device->SetTexture(0, filmtweak_texture);
    FilmTweak_SetPassStates(device);    // depth test off

    float w = (float)targetDesc.Width, h = (float)targetDesc.Height;
    if (cmd->splitSky && FilmTweak_HasSceneDepth(device, &targetDesc)) {
        // Sky: the depth is still 1.0 from the clear (quad depth 1.0 <= scene depth)
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetPixelShaderConstantF(0, (const float*)&cmd->sky, 5);
        FilmTweak_DrawQuad(device, &rect, w, h);

        // World: anything drawn in front of the far plane
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
    }
    device->SetPixelShaderConstantF(0, (const float*)&cmd->world, 5);
    FilmTweak_DrawQuad(device, &rect, w, h);

    FilmTweak_RestoreState(device);
}


/** Backend function of our render command, called by RB_ExecuteRenderCommands in the order of the commands. */
static void FilmTweak_Cmd(void** execState) {
    const FilmTweakCmd* cmd = (const FilmTweakCmd*)*execState;
    *execState = (void*)((const char*)cmd + cmd->byteCount);

    // The quads collected before the command are drawn first, with their states (the check before RB_EndSurface is inlined in the render commands)
    if (tess_numIndexes || tess_field_24)
        RB_EndSurface();

    FilmTweak_Apply(d3d9_device, cmd);
}



// ======================================================================================================
// Frontend
// ======================================================================================================

/** Fills the shader constants of one pass. */
static void FilmTweak_SetConstants(FilmTweakConstants* c, float contrast, float brightness, float gamma, float saturation,
                                   const float* color, const float* darkTint, const float* lightTint) {
    if (gamma < 0.5f)   // also protects against division by zero
        gamma = 0.5f;
    // The command is zeroed by FilmTweak_AllocCmd
    for (int i = 0; i < 3; i++) {
        c->contrast[i] = contrast;
        c->bias[i] = brightness + 0.5f - 0.5f * contrast;
        c->tintDark[i] = darkTint[i] * color[i];
        c->tintDelta[i] = (lightTint[i] - darkTint[i]) * color[i];
    }
    c->params[0] = saturation;
    c->params[1] = 1.0f / gamma;
}

static bool FilmTweak_IsOne(const float* v) {
    return v[0] == 1.0f && v[1] == 1.0f && v[2] == 1.0f;
}

/**
 * Space for our command at the end of the render command list, NULL if it is full. Like R_AddCmd* of the game, but 4 bytes
 * are always left for the end of the list that R_EndFrame_Internal adds.
 */
static FilmTweakCmd* FilmTweak_AllocCmd() {
    char* list = s_cmdlist;
    if (!list)
        return NULL;
    int used = *(int*)(list + CMDLIST_SIZE);
    int size = (int)sizeof(FilmTweakCmd);
    if (CMDLIST_SIZE - used < size + 4)
        return NULL;
    FilmTweakCmd* cmd = (FilmTweakCmd*)(list + used);
    *(int*)(list + CMDLIST_SIZE) = used + size;
    *(int*)(list + CMDLIST_SIZE + 4) += size;
    *(char**)(list + CMDLIST_SIZE + 8) = (char*)cmd;

    memset(cmd, 0, sizeof(*cmd));
    cmd->id = FILMTWEAK_CMD_ID;
    cmd->byteCount = (uint16_t)size;
    return cmd;
}

/** Adds the r_gamma pass over the whole frame, if r_gamma changes anything. */
static void FilmTweak_AddGammaCmd() {
    if (!filmtweak_installed || !r_gamma || filmtweak_supported == 0)
        return;
    if (r_ignoreHwGamma && r_ignoreHwGamma->value.boolean)   // the game ignores r_gamma with it
        return;

    float gamma = r_gamma->value.decimal;
    if (gamma == 1.0f)
        return;

    FilmTweakCmd* cmd = FilmTweak_AllocCmd();
    if (!cmd)
        return;

    // Whole frame from 0 0, the rectangle is cut to the render target in the backend
    cmd->width = 0x10000;
    cmd->height = 0x10000;
    FilmTweak_SetConstants(&cmd->world, 1.0f, 0.0f, gamma, 1.0f, filmtweak_one, filmtweak_one, filmtweak_one);
}

/** Adds our render command after the scene, if the film tweaks change anything. */
static void FilmTweak_AddSceneCmd(const refdef_t* refdef) {
    if (!r_filmTweakEnable || !r_filmTweakEnable->value.boolean || !filmtweak_installed)
        return;

    float contrast = r_filmTweakContrast->value.decimal;
    float brightness = r_filmTweakBrightness->value.decimal;
    float gamma = r_filmTweakGamma->value.decimal;
    float saturation = r_filmTweakSaturation->value.decimal;
    float skySaturation = r_filmTweakSkySaturation->value.decimal;
    const float* color = r_filmTweakColor->value.vec3;
    const float* darkTint = r_filmTweakDarkTint->value.vec3;
    const float* lightTint = r_filmTweakLightTint->value.vec3;

    // Nothing to change - no pass at all
    if (contrast == 1.0f && brightness == 0.0f && gamma == 1.0f && saturation == 1.0f && skySaturation == 1.0f &&
        FilmTweak_IsOne(color) && FilmTweak_IsOne(darkTint) && FilmTweak_IsOne(lightTint))
        return;

    FilmTweakCmd* cmd = FilmTweak_AllocCmd();
    if (!cmd)
        return;

    cmd->x = refdef->x;
    cmd->y = refdef->y;
    cmd->width = refdef->width;
    cmd->height = refdef->height;
    cmd->splitSky = (skySaturation != saturation) ? 1 : 0;
    FilmTweak_SetConstants(&cmd->world, contrast, brightness, gamma, saturation, color, darkTint, lightTint);
    FilmTweak_SetConstants(&cmd->sky, contrast, brightness, gamma, skySaturation, color, darkTint, lightTint);
}


// 10025b90 (gfx)    void R_RenderScene(struct refdef_t* refdef)
static void R_RenderScene(refdef_t* refdef)
{
    // CoD2x: the scene adds no commands when the renderer skips it (r_norefresh, no world), the pass is added only after a scene
    char* list = s_cmdlist;
    int usedBefore = list ? *(int*)(list + CMDLIST_SIZE) : 0;
    // CoD2x: End

    filmtweak_R_RenderScene(refdef);

    // CoD2x: color pass over the finished 3D scene, before the 2D drawing
    if (list && list == s_cmdlist && *(int*)(list + CMDLIST_SIZE) != usedBefore)
        FilmTweak_AddSceneCmd(refdef);
    // CoD2x: End
}


// 10022390 (gfx)    void R_EndFrame()
static void R_EndFrame()
{
    // CoD2x: r_gamma over the whole frame - the last command, after the scene, HUD, menus and console
    if (rg_registered)
        FilmTweak_AddGammaCmd();
    // CoD2x: End

    filmtweak_R_EndFrame();
}


// 10030c40 (gfx)    void RB_SetGammaRamp(uint16_t* ramp @ eax)
static void RB_SetGammaRamp(uint16_t* ramp)
{
    // CoD2x: the hardware ramp stays identity, r_gamma is applied by the shader pass (R_EndFrame).
    // Without the pass (no pixel shader 2.0) the game ramp is kept, so r_gamma still works in fullscreen.
    uint16_t identity[256];
    filmtweak_hwGammaRamp = (filmtweak_supported == 0);
    if (!filmtweak_hwGammaRamp) {
        for (int i = 0; i < 256; i++)
            identity[i] = (uint16_t)((i << 8) | i);
        ramp = identity;
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + FILMTWEAK_GAMMARAMP_FUNC), 0, EAX(ramp));
}

static void RB_SetGammaRamp_Win32()
{
    uint16_t* ramp;
    ASM( movr, ramp, "eax" );

    RB_SetGammaRamp(ramp);
}


// 10023950 (gfx)    void R_ShutdownRenderTargets()
static void R_ShutdownRenderTargets()
{
    // CoD2x: The device can not be reset or destroyed while we hold its objects (called by R_ReleaseForShutdownOrReset)
    FilmTweak_ReleaseResources();
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + FILMTWEAK_SHUTDOWN_FUNC), 0);
}



// ======================================================================================================
// Module
// ======================================================================================================

/**
 * Puts our command into the renderer and R_RenderScene into the renderer function table. The engine loads the renderer
 * and fills the table at game start and vid_restart, so it is checked every frame.
 */
static void FilmTweak_HookRenderer()
{
    if (!gfx_module_addr || !gfx_funcs_R_RenderScene)
        return;

    // Renderer DLL loaded again (vid_restart, often at the same address) - our patches are not in it.
    // The D3D objects of the previous device were released by R_ShutdownRenderTargets before it was destroyed.
    void** slot = &rb_renderCommandTable[FILMTWEAK_CMD_ID];
    if (*slot != (void*)FilmTweak_Cmd) {
        filmtweak_installed = false;
        filmtweak_installedGfx = gfx_module_addr;
        if (*slot == NULL) {
            patch_int32((unsigned int)slot, (int32_t)FilmTweak_Cmd);
            patch_call(gfx_module_addr + FILMTWEAK_SHUTDOWN_CALL, (unsigned int)R_ShutdownRenderTargets);
            patch_call(gfx_module_addr + FILMTWEAK_GAMMARAMP_CALL, (unsigned int)RB_SetGammaRamp_Win32);
            // The next RB_EndFrame sets the identity ramp, in case the game set the gamma before the hook (fullscreen)
            if (r_gamma)
                r_gamma->modified = true;
            filmtweak_installed = true;
        } else {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logger_add("Film tweaks: render command 34 is already used");
            }
        }
    }

    if (filmtweak_installed && gfx_funcs_R_RenderScene != R_RenderScene) {
        filmtweak_R_RenderScene = gfx_funcs_R_RenderScene;
        gfx_funcs_R_RenderScene = R_RenderScene;
    }
    if (filmtweak_installed && gfx_funcs_R_EndFrame && gfx_funcs_R_EndFrame != R_EndFrame) {
        filmtweak_R_EndFrame = gfx_funcs_R_EndFrame;
        gfx_funcs_R_EndFrame = R_EndFrame;
    }
}

/** Called every frame on frame start. */
void filmtweak_frame() {
    FilmTweak_HookRenderer();

    // Failure of the backend (render thread), printed on the main thread
    const char* reason = filmtweak_failureReason;
    if (reason) {
        filmtweak_failureReason = NULL;
        Com_Printf("CoD2x: ^3Film tweaks are not available: %s^7\n", reason);
    }
}

/** Called before the DLL is unloaded (hot reload). */
void filmtweak_unload() {
    bool patched = filmtweak_installed && gfx_module_addr == filmtweak_installedGfx;

    // The render thread may still draw the previous frame with our command - wait for it before the command is removed
    if (patched)
        R_SyncRenderThread();

    if (gfx_funcs_R_RenderScene == R_RenderScene)
        gfx_funcs_R_RenderScene = filmtweak_R_RenderScene;
    if (gfx_funcs_R_EndFrame == R_EndFrame)
        gfx_funcs_R_EndFrame = filmtweak_R_EndFrame;

    if (patched) {
        FilmTweak_ReleaseResources();
        patch_int32((unsigned int)&rb_renderCommandTable[FILMTWEAK_CMD_ID], 0);
        patch_call(gfx_module_addr + FILMTWEAK_SHUTDOWN_CALL, gfx_module_addr + FILMTWEAK_SHUTDOWN_FUNC);
        patch_call(gfx_module_addr + FILMTWEAK_GAMMARAMP_CALL, gfx_module_addr + FILMTWEAK_GAMMARAMP_FUNC);
        // The hardware ramp is identity and the gamma pass is gone - the next RB_EndFrame puts r_gamma into the ramp again
        if (r_gamma)
            r_gamma->modified = true;
    }
    filmtweak_installed = false;
    filmtweak_installedGfx = 0;
}

/** Called only once on game start after common initialization. */
void filmtweak_init() {
    enum dvarFlags_e flags = (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET);

    // Ranges like r_filmTweak* in CoD4, saturation instead of desaturation (also above 1)
    r_filmTweakEnable =         Dvar_RegisterBool("r_filmTweakEnable", true, flags);
    r_filmTweakContrast =       Dvar_RegisterFloat("r_filmTweakContrast", 1.0f, 0.0f, 4.0f, flags);
    r_filmTweakBrightness =     Dvar_RegisterFloat("r_filmTweakBrightness", 0.0f, -1.0f, 1.0f, flags);
    r_filmTweakGamma =          Dvar_RegisterFloat("r_filmTweakGamma", 1.0f, 0.5f, 2.0f, flags);
    r_filmTweakSaturation =     Dvar_RegisterFloat("r_filmTweakSaturation", 1.0f, 0.0f, 2.0f, flags);
    r_filmTweakSkySaturation =  Dvar_RegisterFloat("r_filmTweakSkySaturation", 1.0f, 0.0f, 2.0f, flags);
    r_filmTweakColor =          Dvar_RegisterVec3("r_filmTweakColor", 1.0f, 1.0f, 1.0f, 0.0f, 2.0f, flags);
    r_filmTweakDarkTint =       Dvar_RegisterVec3("r_filmTweakDarkTint", 1.0f, 1.0f, 1.0f, 0.0f, 2.0f, flags);
    r_filmTweakLightTint =      Dvar_RegisterVec3("r_filmTweakLightTint", 1.0f, 1.0f, 1.0f, 0.0f, 2.0f, flags);
}
