/*
 * Renderer services for CoD2x modules (film tweaks, picture in picture, x-ray, ...), see renderer.h.
 *
 * How it works (renderer DLL gfx_d3d_mp_x86_s.dll):
 *   - Render commands: the frontend adds commands into s_cmdlist, the backend (RB_ExecuteRenderCommands, maybe on the render
 *     thread) calls rb_renderCommandTable[id] for each of them in order, without a range check. The table has the 34 commands
 *     of the game (0 = end of the list .. 33 = RB_TouchAllImagesCmd) followed by only two zeros (ids 34, 35) and other data.
 *     CoD2x uses the id RENDERER_CMD_ID for all its commands: the command starts with the type of our command, and our
 *     backend function Renderer_ExecCmd calls the function of the type.
 *   - Callbacks: R_RenderScene and R_EndFrame are replaced in the renderer function table gfx_funcs (filled by the engine
 *     after the renderer is loaded - checked every frame). The calls of R_ReleaseForShutdownOrReset (device destroyed or
 *     reset) are hooked, so all modules release their D3D objects before it.
 *   - Additional views (Renderer_RenderView): R_RenderScene is called once more with another refdef. Things to know:
 *     - A scene is not one block of render commands. Each pass (clear, depth, opaque, effects, ...) reserves one place in the
 *       commands once per frame ("delayed drawing") and the commands of every view for that pass are chained to it, so two
 *       scenes would be drawn interleaved pass by pass, and the clear of the frame would be done only once, full screen.
 *       R_DoneRenderingViews (called by the game once per frame after the HUD) closes the chains and resets the places.
 *       It is called before and after the view, so each scene is one complete block. The Direct3D 9 scene also opens a
 *       block for the rest of its view (closed by R_EndView at the end of the screen) - R_EndView is called before each.
 *     - The draw surfaces of a scene are a list in frontEndDataOut started by R_ClearScene (once per frame) and sorted in
 *       place by R_RenderScene, while its commands keep the start and count of the list. The view starts its own list
 *       after the surfaces so far, otherwise it would reorder the list of the main scene under its commands.
 *     - World surfaces are added once per scene number (R_ClearScene increases it) - the view gets a new number.
 *     - Models of entities are added only by the first scene that sees them (state 5) - they are set back to skinned (4),
 *       so the view adds them too and reuses the skinning of the frame (R_SkinXModel / R_SkinSceneDObj return for state > 3).
 *     - The view starts with the clear of the frame like the first view of a frame (D3D Clear of the current viewport).
 *       Our viewport command before the view sets the D3D viewport to its rectangle, so only the rectangle is cleared - the
 *       viewport command of the game (13) is not applied before that clear. Its full screen viewport command of the frame
 *       clear pass is changed to the rectangle too (call-site hook of R_AddCmdSetViewport). The Direct3D 9 scene also switches
 *       render targets (a switch resets the D3D viewport to the full target) - in a view the backend of the render target
 *       command sets the rectangle again, until our end of view command. After the views the viewport is
 *       set back to the main view for the 2D drawing, which keeps the last viewport.
 */

#include "renderer.h"

#include <windows.h>
#include <string.h>

#include "../shared/patch.h"
#include "../shared/logger.h"
#include "../shared/cod2_dvars.h"


// Renderer functions, copied by the engine into the table gfx_funcs (0x0068a1e8) after the renderer is loaded
typedef void (__cdecl *R_RenderScene_t)(refdef_t* refdef);
#define gfx_funcs_R_RenderScene (*((R_RenderScene_t*)0x0068a27c))

typedef void (__cdecl *R_EndFrame_t)();
#define gfx_funcs_R_EndFrame    (*((R_EndFrame_t*)0x0068a2ac))

// Sets the origin and the scale of the level of detail of models from the refdef (CG_DrawActiveFrame, once per frame)
typedef void (__cdecl *R_SetLodOrigin_t)(const refdef_t* refdef);
#define gfx_funcs_R_SetLodOrigin (*((R_SetLodOrigin_t*)0x0068a394))   // 10025ab0

// The command id of all CoD2x commands, the first free id after the 34 commands of the game
#define RENDERER_CMD_ID         34

// Backend state of the device (dxState)
#define dx_targetWidth          (*((int*)(gfx_module_addr + 0x009ba51c)))          // size of the current render target, RB_ClearScreenCmd clears this viewport
#define dx_targetHeight         (*((int*)(gfx_module_addr + 0x009ba520)))
#define dx_viewport             ((int*)(gfx_module_addr + 0x009ba530))             // dxState.viewport x, y, width, height - last viewport set to the device (1003ed10 sets it only when it differs)
#define dx_renderTargets        ((char*)(gfx_module_addr + 0x001d4820))            // render targets, 0x14 bytes each: +0x0c width, +0x10 height (0 = back buffer)
#define DX_RENDER_TARGET_SIZE   0x14

// 10032550 (gfx)    void RB_SetRenderTargetCmd(void** cmds)    rb_renderCommandTable[14]
#define RENDERER_RENDERTARGET_CMD   14
#define RENDERER_RENDERTARGET_FUNC  0x00032550

// Game functions called by the renderer (cod2_funcs, the table the game gives to the renderer, gfx 101d49d8)
#define cod2_funcs_FX_DrawScheduledEffects (*((void**)(gfx_module_addr + 0x001d4ca4)))   // 004a2950, called by R_RenderScene (10025770)

// Effects system of the game (exe), see FX_DrawAll (004a0a30)
#define fx_enable               (*((dvar_t**)0x00609fd8))
#define fx_draw                 (*((dvar_t**)0x0096b610))
#define fx_cull                 (*((dvar_t**)0x0096b648))
#define fx_effects              (*((void***)0x005d7680))   // active effects (not bolted to a model)
#define fx_effectsCount         (*((int*)0x01183880))      // drawn part of fx_effects (set by FX_DrawScheduledEffects)
#define fx_boltEffects          (*((void***)0x005d767c))   // active effects bolted to a model
#define fx_boltEffectsCount     (*((int*)0x0117c7a0))
#define fx_helper               (*((char**)0x005d7684))    // theFxHelper: camera of the effects system (FxHelper::AdjustCamera)
#define FXHELPER_PREV_PLANES    0xf0    // number of the frustum planes of the previous frame, FxHelper::CullSpherePreviousFrame (0049b300)

// 10025495  e856cbffff  call R_AddCmdSetViewport   (in the pass that clears the frame, before the first view of a frame)
#define RENDERER_SETVIEWPORT_CALL   0x00025495
#define RENDERER_SETVIEWPORT_FUNC   0x00021ff0

// 10012e70  e8fbf3ffff  call R_ReleaseForShutdownOrReset   (device shutdown)
// 1001385b  e810eaffff  call R_ReleaseForShutdownOrReset   (lost device reset)
static const unsigned int renderer_releaseCalls[] = { 0x00012e70, 0x0001385b };
#define RENDERER_RELEASE_FUNC       0x00012270
#define RENDERER_RELEASE_CALLS      (int)(sizeof(renderer_releaseCalls) / sizeof(renderer_releaseCalls[0]))

#define RENDERER_MAX_CMD_TYPES      16
#define RENDERER_MAX_CALLBACKS      8

// Header of our render commands, the data of the type follows
struct RendererCmd {
    uint16_t id;            // RENDERER_CMD_ID
    uint16_t byteCount;     // size of the command with the data, the backend moves to the next command by it
    int32_t type;           // Renderer_RegisterCmd
};

// Type 0: viewport of an additional view
struct RendererViewportCmd {
    int32_t x, y, width, height;
};
static void Renderer_ViewportCmd(const void* data);
#define RENDERER_CMD_VIEWPORT       0

// Type 1: end of an additional view (no data)
static void Renderer_ViewEndCmd(const void* data);
#define RENDERER_CMD_VIEWEND        1

static Renderer_CmdFunc_t renderer_cmdFuncs[RENDERER_MAX_CMD_TYPES] = { Renderer_ViewportCmd, Renderer_ViewEndCmd };
static int renderer_numCmdTypes = 2;

// Backend (render thread): the viewport of the additional view being drawn
static bool renderer_backendInView = false;
static RendererViewportCmd renderer_backendViewport;

// Callbacks of the modules, in the order of registration
template <typename F> struct RendererCallbacks {
    F funcs[RENDERER_MAX_CALLBACKS];
    int count;
    void add(F func) { if (count < RENDERER_MAX_CALLBACKS) funcs[count++] = func; }
};
static RendererCallbacks<Renderer_SceneFunc_t> renderer_sceneCallbacks;
static RendererCallbacks<Renderer_Func_t> renderer_endFrameCallbacks;
static RendererCallbacks<Renderer_Func_t> renderer_releaseCallbacks;

// Patches in the renderer DLL
static unsigned int renderer_patchedGfx = 0;        // gfx_module_addr the patches are in, 0 = none

// Original renderer functions, saved when ours are put into gfx_funcs
static R_RenderScene_t renderer_R_RenderScene = NULL;
static R_EndFrame_t renderer_R_EndFrame = NULL;

// Additional views
static bool renderer_inSceneCallbacks = false;      // Renderer_RenderView is allowed
static bool renderer_viewsRendered = false;         // the viewport must be set back to the main view
static bool renderer_viewsLastFrame = false;        // additional views were rendered in the last frame
static const refdef_t* renderer_view = NULL;        // view being rendered


// ======================================================================================================
// Render commands
// ======================================================================================================

/** Backend function of our command id, called by RB_ExecuteRenderCommands in the order of the commands. */
static void Renderer_ExecCmd(void** execState) {
    const RendererCmd* cmd = (const RendererCmd*)*execState;
    *execState = (void*)((const char*)cmd + cmd->byteCount);

    // The quads collected before the command are drawn first, with their states
    Renderer_FlushTess();

    if (cmd->type >= 0 && cmd->type < renderer_numCmdTypes && renderer_cmdFuncs[cmd->type])
        renderer_cmdFuncs[cmd->type](cmd + 1);
}

int Renderer_RegisterCmd(Renderer_CmdFunc_t func) {
    if (renderer_numCmdTypes >= RENDERER_MAX_CMD_TYPES)
        return -1;
    renderer_cmdFuncs[renderer_numCmdTypes] = func;
    return renderer_numCmdTypes++;
}

bool Renderer_CmdsAvailable() {
    return renderer_patchedGfx && renderer_patchedGfx == gfx_module_addr && rb_renderCommandTable[RENDERER_CMD_ID] == (void*)Renderer_ExecCmd;
}

/**
 * Space for a command at the end of the render command list. Like R_AddCmd* of the game, but 4 bytes are always left
 * for the end of the list that R_EndFrame_Internal adds.
 */
void* Renderer_AddCmd(int type, int dataSize) {
    if (!Renderer_CmdsAvailable() || type < 0 || type >= renderer_numCmdTypes || dataSize < 0)
        return NULL;
    char* list = s_cmdlist;
    if (!list)
        return NULL;
    int size = ((int)sizeof(RendererCmd) + dataSize + 3) & ~3;
    int used = *(int*)(list + CMDLIST_SIZE);
    if (size > 0xffff || CMDLIST_SIZE - used < size + 4)
        return NULL;
    RendererCmd* cmd = (RendererCmd*)(list + used);
    *(int*)(list + CMDLIST_SIZE) = used + size;
    *(int*)(list + CMDLIST_SIZE + 4) += size;
    *(char**)(list + CMDLIST_SIZE + 8) = (char*)cmd;

    memset(cmd, 0, size);
    cmd->id = RENDERER_CMD_ID;
    cmd->byteCount = (uint16_t)size;
    cmd->type = type;
    return cmd + 1;
}


// ======================================================================================================
// Callbacks
// ======================================================================================================

void Renderer_AddSceneCallback(Renderer_SceneFunc_t func) { renderer_sceneCallbacks.add(func); }
void Renderer_AddEndFrameCallback(Renderer_Func_t func) { renderer_endFrameCallbacks.add(func); }
void Renderer_AddReleaseCallback(Renderer_Func_t func) { renderer_releaseCallbacks.add(func); }

/** Releases the D3D objects of all modules. */
static void Renderer_ReleaseModules() {
    for (int i = 0; i < renderer_releaseCallbacks.count; i++)
        renderer_releaseCallbacks.funcs[i]();
}


// ======================================================================================================
// Additional views
// ======================================================================================================

/**
 * Renderer frontend: adds the render command 13 that sets the D3D viewport in pixels. The game adds it only once per frame,
 * full screen, before the clear of the frame (the scene commands set the viewport of their view, the 2D drawing keeps the last one).
 */
// 10021ff0 (gfx)    void R_AddCmdSetViewport(int32_t x, int32_t y, int32_t width, int32_t height)
static void R_AddCmdSetViewport(int x, int y, int width, int height)
{
    // CoD2x: an additional view clears only its rectangle
    if (renderer_view) {
        x = renderer_view->x;
        y = renderer_view->y;
        width = renderer_view->width;
        height = renderer_view->height;
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + RENDERER_SETVIEWPORT_FUNC), 4, PUSH(x), PUSH(y), PUSH(width), PUSH(height));
}

/**
 * Backend of the viewport command: sets the D3D viewport to the rectangle of the view right away. The clear of the frame
 * (RB_ClearScreenCmd) asks for the viewport of the full render target and clears the current D3D viewport, but the engine
 * sets the device only when the wanted viewport differs from the last one it set (dxState.viewport). That one is set to the
 * full render target here, so the clear keeps our rectangle. Without it, after another view it would be the rectangle of
 * that view, the clear would set the full viewport and wipe the screen.
 */
static void Renderer_SetViewViewport() {
    const RendererViewportCmd* cmd = &renderer_backendViewport;
    D3DVIEWPORT9 viewport = { (DWORD)cmd->x, (DWORD)cmd->y, (DWORD)cmd->width, (DWORD)cmd->height, 0.0f, 1.0f };
    d3d9_device->SetViewport(&viewport);

    dx_viewport[0] = 0;
    dx_viewport[1] = 0;
    dx_viewport[2] = dx_targetWidth;
    dx_viewport[3] = dx_targetHeight;
}

static void Renderer_ViewportCmd(const void* data) {
    renderer_backendViewport = *(const RendererViewportCmd*)data;
    renderer_backendInView = true;
    Renderer_SetViewViewport();
}

static void Renderer_ViewEndCmd(const void* data) {
    (void)data;
    renderer_backendInView = false;
}

/**
 * Backend of the render command 14: sets the render target (RB_SetRenderTarget). Setting another D3D render target resets
 * the D3D viewport to the full target and the engine marks the full target as the current viewport. The Direct3D 9 scene
 * starts the frame with a switch to another screen sized target (depth pass) and a full clear - in an additional view
 * that would clear and draw over the whole screen. In a view the rectangle of the view is set again after the switch,
 * for the targets of the screen size (not for smaller ones like shadow maps).
 */
// 10032550 (gfx)    void RB_SetRenderTargetCmd(void** cmds)
static void RB_SetRenderTargetCmd(void** execState)
{
    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + RENDERER_RENDERTARGET_FUNC), 1, PUSH(execState));

    // CoD2x: keep the rectangle of the additional view
    if (renderer_backendInView &&
        dx_targetWidth == *(int*)(dx_renderTargets + 0x0c) && dx_targetHeight == *(int*)(dx_renderTargets + 0x10))
        Renderer_SetViewViewport();
    // CoD2x: End
}

/**
 * Draws the active effects for the camera of the effects system, used instead of FX_DrawScheduledEffects in additional
 * views. FX_DrawScheduledEffects is meant to run once per frame: it frees and adds effects, updates the active lists and
 * appends to the list of drawn effects that is reset only once per frame - running it again for a view changed the state
 * of the effects (a crash on map_restart, when the effects are restarted). Like the draw part of FX_DrawAll, without the
 * sort: each effect is culled by the camera (vtable + 0x0c) and drawn (vtable + 0x10), nothing is changed.
 */
static void Renderer_DrawEffects() {
    typedef bool (__thiscall *FxCull_t)(void* effect);
    typedef void (__thiscall *FxDraw_t)(void* effect);

    if (!fx_enable || !fx_enable->value.boolean || !fx_draw || !fx_draw->value.boolean)
        return;
    bool cull = fx_cull && fx_cull->value.boolean;

    void** lists[2] = { fx_effects, fx_boltEffects };
    int counts[2] = { fx_effectsCount, fx_boltEffectsCount };
    for (int l = 0; l < 2; l++) {
        if (!lists[l])
            continue;
        for (int i = 0; i < counts[l]; i++) {
            void* effect = lists[l][i];
            void** vtable = *(void***)effect;
            if (cull && ((FxCull_t)vtable[0x0c / 4])(effect))
                continue;
            ((FxDraw_t)vtable[0x10 / 4])(effect);
        }
    }
}

/** Target of the call instruction at the offset in the renderer DLL, 0 if it is not a call. */
static unsigned int Renderer_CallTarget(unsigned int offset) {
    unsigned int call = gfx_module_addr + offset;
    return *(unsigned char*)call == 0xe8 ? call + 5 + *(int32_t*)(call + 1) : 0;
}

bool Renderer_RenderView(refdef_t* view) {
    if (!renderer_inSceneCallbacks || !renderer_R_RenderScene || !Renderer_CmdsAvailable() ||
        Renderer_CallTarget(RENDERER_SETVIEWPORT_CALL) != (unsigned int)R_AddCmdSetViewport)
        return false;
    char* frontEnd = frontEndDataOut;
    if (!frontEnd || view->width <= 0 || view->height <= 0)
        return false;

    // The scenes so far as one finished block, the view starts new chains after it. The Direct3D 9 scene leaves the last
    // block of its view open (closed by the game at the end of the screen) - it is closed first, otherwise its jump would
    // be issued without a target and the backend would crash. The commands added after it are drawn in order.
    R_EndView(0);
    R_DoneRenderingViews();

    // The clear of the frame at the start of the view clears only its rectangle - without the command it would wipe the screen
    RendererViewportCmd* viewport = (RendererViewportCmd*)Renderer_AddCmd(RENDERER_CMD_VIEWPORT, sizeof(RendererViewportCmd));
    if (!viewport)
        return false;
    viewport->x = view->x;
    viewport->y = view->y;
    viewport->width = view->width;
    viewport->height = view->height;

    // Own list of draw surfaces after the ones of the previous scenes, like R_ClearScene does for a frame
    scene_drawSurfs = frontEnd + 8 + *(int*)(frontEnd + 4) * 8;
    scene_numDrawSurfs = 0;
    // New scene number, the world surfaces already added by the main scene are marked with the old one
    scene_number++;
    // Level of detail of the models by the camera of the view (R_SkinXModel picks the LOD by the distance from the LOD origin)
    gfx_funcs_R_SetLodOrigin(view);

    // Models of entities added (5) or skinned (4) by the previous scenes are added only by the first scene that sees them,
    // with its level of detail - back to new (0): the view computes the bounds and skins them again with its own LOD and
    // adds their surfaces. R_UpdateXModelBounds starts only from 0 (it waits on 1 for another thread), the skinned
    // surfaces of the previous scenes stay valid (the skinning allocates new ones).
    int numSceneEnts = scene_numSceneEnts;
    if (numSceneEnts > SCENE_ENT_MAX)
        numSceneEnts = SCENE_ENT_MAX;
    for (int i = 0; i < numSceneEnts; i++) {
        int* state = (int*)(scene_sceneEnts + i * SCENE_ENT_SIZE + SCENE_ENT_STATE);
        if (*state == 4 || *state == 5)
            *state = 0;
    }

    // The effects drawn by the scene (FX_DrawScheduledEffects) face the camera of the effects system and are culled by it -
    // set to the main view by CG_DrawActiveFrame, so the sprites of the view would face the main camera
    CL_FX_AdjustCamera(view);

    // The effects are only drawn again for the view, their state is updated once per frame by the main scene
    void* drawScheduledEffects = cod2_funcs_FX_DrawScheduledEffects;
    cod2_funcs_FX_DrawScheduledEffects = (void*)Renderer_DrawEffects;

    renderer_view = view;
    renderer_R_RenderScene(view);
    R_EndView(0);
    R_DoneRenderingViews();
    Renderer_AddCmd(RENDERER_CMD_VIEWEND, 0);
    renderer_view = NULL;

    cod2_funcs_FX_DrawScheduledEffects = drawScheduledEffects;

    renderer_viewsRendered = true;
    return true;
}


// ======================================================================================================
// Hooks
// ======================================================================================================

// 10025b90 (gfx)    void R_RenderScene(struct refdef_t* refdef)
static void R_RenderScene(refdef_t* refdef)
{
    // CoD2x: the scene adds no commands when the renderer skips it (r_norefresh, no world)
    char* list = s_cmdlist;
    int usedBefore = list ? *(int*)(list + CMDLIST_SIZE) : 0;

    // With additional views the effects must spawn also where the main view does not look: the scene updates the effects
    // (FX_DrawScheduledEffects) and an effect spawns its elements only when the previous frame of the main view could see
    // them (FxHelper::CullSpherePreviousFrame). No planes of the previous frame = nothing culled. The drawing of the main
    // view uses the planes of the current frame, it stays the same.
    char* fxHelper = fx_helper;
    int prevPlanes = 0;
    bool fxNoPrevCull = renderer_viewsLastFrame && fxHelper;
    renderer_viewsLastFrame = false;
    if (fxNoPrevCull) {
        prevPlanes = *(int*)(fxHelper + FXHELPER_PREV_PLANES);
        *(int*)(fxHelper + FXHELPER_PREV_PLANES) = 0;
    }
    // CoD2x: End

    renderer_R_RenderScene(refdef);

    // CoD2x: End of the effects without culling by the previous frame
    if (fxNoPrevCull && fxHelper == fx_helper && *(int*)(fxHelper + FXHELPER_PREV_PLANES) == 0)
        *(int*)(fxHelper + FXHELPER_PREV_PLANES) = prevPlanes;
    // CoD2x: End

    // CoD2x: callbacks of the modules after the scene, before the 2D drawing
    if (!list || list != s_cmdlist || *(int*)(list + CMDLIST_SIZE) == usedBefore)
        return;
    renderer_inSceneCallbacks = true;
    for (int i = 0; i < renderer_sceneCallbacks.count; i++)
        renderer_sceneCallbacks.funcs[i](refdef);
    renderer_inSceneCallbacks = false;

    // After additional views back to the main view: the viewport (the 2D drawing keeps the last one), the effects camera
    // and the level of detail
    if (renderer_viewsRendered) {
        renderer_viewsRendered = false;
        renderer_viewsLastFrame = true;
        R_AddCmdSetViewport(refdef->x, refdef->y, refdef->width, refdef->height);
        CL_FX_AdjustCamera(refdef);
        gfx_funcs_R_SetLodOrigin(refdef);
    }
    // CoD2x: End
}

// 10022390 (gfx)    void R_EndFrame()
static void R_EndFrame()
{
    // CoD2x: callbacks of the modules - their commands are the last ones of the frame
    if (rg_registered) {
        for (int i = 0; i < renderer_endFrameCallbacks.count; i++)
            renderer_endFrameCallbacks.funcs[i]();
    }
    // CoD2x: End

    renderer_R_EndFrame();
}

// 10012270 (gfx)    void R_ReleaseForShutdownOrReset()
static void R_ReleaseForShutdownOrReset()
{
    // CoD2x: The device can not be reset or destroyed while the modules hold its objects
    Renderer_ReleaseModules();
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, (gfx_module_addr + RENDERER_RELEASE_FUNC), 0);
}


// ======================================================================================================
// Module
// ======================================================================================================

/** Called when the game loaded the renderer DLL (game start, vid_restart). */
void renderer_rendered() {
    renderer_patchedGfx = gfx_module_addr;

    // Our patches may already be there when this runs again for the same DLL (hot reload)
    void** slot = &rb_renderCommandTable[RENDERER_CMD_ID];
    if (*slot == NULL)
        patch_int32((unsigned int)slot, (int32_t)Renderer_ExecCmd);
    else if (*slot != (void*)Renderer_ExecCmd)
        logger_add("Renderer: render command %d is already used, CoD2x render commands are not available", RENDERER_CMD_ID);

    void** renderTargetSlot = &rb_renderCommandTable[RENDERER_RENDERTARGET_CMD];
    if (*renderTargetSlot == (void*)(gfx_module_addr + RENDERER_RENDERTARGET_FUNC))
        patch_int32((unsigned int)renderTargetSlot, (int32_t)RB_SetRenderTargetCmd);
    else if (*renderTargetSlot != (void*)RB_SetRenderTargetCmd)
        logger_add("Renderer: unexpected render target command, additional views may draw over the screen");

    unsigned int target = Renderer_CallTarget(RENDERER_SETVIEWPORT_CALL);
    if (target == gfx_module_addr + RENDERER_SETVIEWPORT_FUNC)
        patch_call(gfx_module_addr + RENDERER_SETVIEWPORT_CALL, (unsigned int)R_AddCmdSetViewport);
    else if (target != (unsigned int)R_AddCmdSetViewport) {
        logger_add("Renderer: unexpected code at the viewport call, additional views are not available");
    }

    for (int i = 0; i < RENDERER_RELEASE_CALLS; i++)
        patch_call(gfx_module_addr + renderer_releaseCalls[i], (unsigned int)R_ReleaseForShutdownOrReset);
}

/** Called every frame on frame start. The engine fills gfx_funcs after the renderer is loaded (game start, vid_restart). */
void renderer_frame() {
    if (!gfx_module_addr || renderer_patchedGfx != gfx_module_addr)
        return;

    if (gfx_funcs_R_RenderScene && gfx_funcs_R_RenderScene != R_RenderScene) {
        renderer_R_RenderScene = gfx_funcs_R_RenderScene;
        gfx_funcs_R_RenderScene = R_RenderScene;
    }
    if (gfx_funcs_R_EndFrame && gfx_funcs_R_EndFrame != R_EndFrame) {
        renderer_R_EndFrame = gfx_funcs_R_EndFrame;
        gfx_funcs_R_EndFrame = R_EndFrame;
    }
}

/**
 * Called before the DLL is unloaded (hot reload), before the other renderer modules unload: waits until the render thread
 * finished the frame with our commands, releases the D3D objects of the modules and removes the patches.
 */
void renderer_unload() {
    if (gfx_funcs_R_RenderScene == R_RenderScene)
        gfx_funcs_R_RenderScene = renderer_R_RenderScene;
    if (gfx_funcs_R_EndFrame == R_EndFrame)
        gfx_funcs_R_EndFrame = renderer_R_EndFrame;

    if (renderer_patchedGfx && renderer_patchedGfx == gfx_module_addr) {
        // The render thread may still draw the previous frame with our commands and objects
        R_SyncRenderThread();
        Renderer_ReleaseModules();

        if (Renderer_CmdsAvailable())
            patch_int32((unsigned int)&rb_renderCommandTable[RENDERER_CMD_ID], 0);
        if (rb_renderCommandTable[RENDERER_RENDERTARGET_CMD] == (void*)RB_SetRenderTargetCmd)
            patch_int32((unsigned int)&rb_renderCommandTable[RENDERER_RENDERTARGET_CMD], gfx_module_addr + RENDERER_RENDERTARGET_FUNC);
        if (Renderer_CallTarget(RENDERER_SETVIEWPORT_CALL) == (unsigned int)R_AddCmdSetViewport)
            patch_call(gfx_module_addr + RENDERER_SETVIEWPORT_CALL, gfx_module_addr + RENDERER_SETVIEWPORT_FUNC);
        for (int i = 0; i < RENDERER_RELEASE_CALLS; i++)
            patch_call(gfx_module_addr + renderer_releaseCalls[i], gfx_module_addr + RENDERER_RELEASE_FUNC);
    }
    renderer_patchedGfx = 0;
}
