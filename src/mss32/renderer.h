#ifndef RENDERER_H
#define RENDERER_H

/*
 * Renderer DLL (gfx_d3d_mp_x86_s.dll): engine globals and functions used by CoD2x, and the services CoD2x modules use to
 * draw in it - own render commands, callbacks after the scene / at the end of the frame / before the device is released,
 * and additional views of the world. The renderer is loaded at runtime (game start, vid_restart), all addresses are
 * offsets from gfx_module_addr. See renderer.cpp for how it works.
 */

#include <stdint.h>
#include <d3d9.h>

#include "shared.h"
#include "hook.h"
#include "../shared/cod2_client.h"


// ======================================================================================================
// Engine globals
// ======================================================================================================

#define d3d9_device             (*((IDirect3DDevice9**)(gfx_module_addr + 0x001d1bf8)))
#define rg_registered           (*((bool*)(gfx_module_addr + 0x001d4d00)))        // rg, first byte: renderer initialized (R_EndFrame does nothing without it)
#define tess_numIndexes         (*((int*)(gfx_module_addr + 0x009817d0)))          // tess + 0x14, quads collected for the current material
#define tess_field_24           (*((int*)(gfx_module_addr + 0x009817e0)))          // tess + 0x24, other primitives collected (RB_EndSurface draws both)
#define s_cmdlist               (*((char**)(gfx_module_addr + 0x00438780)))        // render commands being added: commands at 0, +0x30000 used bytes, +0x30004 extra size, +0x30008 last command
#define rb_renderCommandTable   ((void**)(gfx_module_addr + 0x00190518))            // backend functions by command id (RB_ExecuteRenderCommands)
#define CMDLIST_SIZE            0x30000

// Scene being built by the frontend (R_ClearScene resets it once per frame)
#define frontEndDataOut         (*((char**)(gfx_module_addr + 0x001da9e0)))        // data of the frame: +4 number of draw surfaces, +8 draw surfaces (8 bytes each)
#define scene_number            (*((int*)(gfx_module_addr + 0x008cc300)))          // scene counter - world surfaces and static models are added once per scene number
#define scene_numSceneEnts      (*((int*)(gfx_module_addr + 0x008cc30c)))          // scene entities of the frame (models of entities: players, script models, ...)
#define scene_drawSurfs         (*((char**)(gfx_module_addr + 0x008cc8bc)))        // first draw surface of the scene (in frontEndDataOut)
#define scene_numDrawSurfs      (*((int*)(gfx_module_addr + 0x008cc8b8)))          // draw surfaces added for the scene, sorted in place by R_RenderScene
#define scene_sceneEnts         ((char*)(gfx_module_addr + 0x008cc8c4))            // GfxSceneEntity[], SCENE_ENT_SIZE bytes each
#define SCENE_ENT_SIZE          0x34
#define SCENE_ENT_STATE         0x0c    // 0-1 new, 2 bounds done, 3 skinning, 4 skinned, 5 surfaces added (skipped by later scenes)
#define SCENE_ENT_MAX           2046


// ======================================================================================================
// Engine functions
// ======================================================================================================

/**
 * Renderer backend: draws the quads collected for the current material (tess) with its render states and empties the collection.
 * The render commands call it when the material changes, or before they change the device state.
 */
// 1003ad30 (gfx)    struct shaderCommands_t* RB_EndSurface()
inline void RB_EndSurface() {
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x0003ad30), 0);
}

/**
 * Renderer backend: draws the quads collected so far, before a command changes the device state (the check before
 * RB_EndSurface is inlined in the render commands of the game).
 */
inline void Renderer_FlushTess() {
    if (tess_numIndexes || tess_field_24)
        RB_EndSurface();
}

/**
 * Renderer frontend: waits until the render thread (r_smp_backend) has finished the frame it is drawing, once per frame
 * (the engine's wait function, cod2_funcs, at most 4 s). Does nothing before the renderer is initialized.
 */
// 10020fc0 (gfx)    void R_SyncRenderThread()
inline void R_SyncRenderThread() {
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x00020fc0), 0);
}

/**
 * Renderer frontend: finishes the delayed drawing of all views of the frame. Each pass of a scene (clear, depth, opaque,
 * effects, ...) reserves one place in the render commands once per frame and the commands of every view for that pass are
 * chained to it, so the views are drawn pass by pass. This closes all the chains and resets the places, the next scene
 * starts new ones after the commands added so far. Called by SCR_UpdateFrame after the HUD (R_EndView), once per frame.
 */
// 10013360 (gfx)    void R_DoneRenderingViews()     (jumps to 10029950)
inline void R_DoneRenderingViews() {
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x00013360), 0);
}

/**
 * Renderer frontend: ends the delayed drawing of the view. The Direct3D 9 scene (10025430) ends with one more delayed
 * block for the view (everything drawn after the scene - the HUD, menus, console) that stays open until this call; it
 * closes the block (return command, the jump before the block skips it) and adds an empty one for the last pass.
 * Only blocks that are open are closed, the Direct3D 7 scene leaves none. Called by SCR_DrawScreenField with view 0
 * at the end of the screen, before R_DoneRenderingViews issues the blocks.
 */
// 10013310 (gfx)    void R_EndView(int32_t view)
inline void R_EndView(int view) {
    ASM_CALL(RETURN_VOID, (gfx_module_addr + 0x00013310), 1, PUSH(view));
}


// ======================================================================================================
// CoD2x renderer services
// ======================================================================================================

/**
 * Own render commands. All of them use one free command id of the game (the table has room for only two more), the type
 * picks the backend function. Register the type once in *_init, add commands in the frontend (main thread). The backend
 * function gets the data of the command after the collected quads were drawn (RB_EndSurface), in the order of the commands.
 */
typedef void (*Renderer_CmdFunc_t)(const void* data);
int Renderer_RegisterCmd(Renderer_CmdFunc_t func);          // type of the command, -1 if there is no free type
void* Renderer_AddCmd(int type, int dataSize);              // zeroed data of a new command, NULL if not available (list full, renderer not hooked)
bool Renderer_CmdsAvailable();                              // the commands are in the renderer (the next frame runs them)

/**
 * Called after the frontend R_RenderScene of the view of the player (CG_DrawActive), before the 2D drawing - only when
 * the scene was drawn (not with r_norefresh or without a world). Callbacks run in the order of registration and may add
 * commands and render additional views (Renderer_RenderView). Commands added before a view apply to the scenes so far:
 * film tweaks (registered first) color only the main scene, not the views of picture in picture.
 */
typedef void (*Renderer_SceneFunc_t)(const refdef_t* refdef);
void Renderer_AddSceneCallback(Renderer_SceneFunc_t func);

/** Called before the frontend R_EndFrame - the commands added here are the last ones of the frame (after HUD, menus, console). */
typedef void (*Renderer_Func_t)();
void Renderer_AddEndFrameCallback(Renderer_Func_t func);

/**
 * Called before the D3D device is reset or destroyed (R_ReleaseForShutdownOrReset) - release all D3D objects created
 * in the default pool, the device can not be reset while they exist. May run on the render thread.
 */
void Renderer_AddReleaseCallback(Renderer_Func_t func);

/**
 * Renders one more view of the world into its viewport (view->x, y, width, height in pixels), drawn over the scenes
 * rendered so far. Only from a scene callback. False if the view was not rendered.
 */
bool Renderer_RenderView(refdef_t* view);


void renderer_rendered();
void renderer_frame();
void renderer_unload();

#endif
