#ifndef DRAWING_H
#define DRAWING_H

#include "../shared/assembly.h"
#include "../shared/cod2_definitions.h"


typedef void* materialHandle_t;
typedef void* fontHandle_t;

extern vec4_t colWhite;
extern vec4_t colBlack;
extern vec4_t colRed;
extern vec4_t colGreen;
extern vec4_t colBlue;
extern vec4_t colYellow;

#define fontNormal		        *((fontHandle_t**)0x01979120)
#define fontNormalBold	        *((fontHandle_t**)0x0197911c)
#define fontSmall               *((fontHandle_t**)0x01979114)
#define fontBig			        *((fontHandle_t**)0x01979110)
#define fontConsole		        *((fontHandle_t**)0x01979118)
#define fontExtraBigSmall       *((fontHandle_t**)0x01979124)

#define shaderWhite             *((materialHandle_t**)0x00966bfc)
#define shaderConsole           *((materialHandle_t**)0x00966c00)
#define shaderFontByLanguage    *((materialHandle_t**)0x00966c04)
#define shaderCursor            *((materialHandle_t**)0x00d5290c)

#define shaderObjective                 *((materialHandle_t**)0x014eb4e8)   // yellow star icon
#define shaderObjectiveFriendly         *((materialHandle_t**)0x014eb4ec)   // green friendly arrow icon
#define shaderObjectiveFriendlyChat     *((materialHandle_t**)0x014eb4f0)   // red exclamation mark icon


// Edge relative placement values for rect->h_align and rect->v_align
enum horizontalAlign_e {
    HORIZONTAL_ALIGN_SUBLEFT = 0,        // left edge of a 4:3 screen (safe area not included)
    HORIZONTAL_ALIGN_LEFT = 1,           // left viewable (safe area) edge
    HORIZONTAL_ALIGN_CENTER = 2,         // center of the screen (reticle)
    HORIZONTAL_ALIGN_RIGHT = 3,          // right viewable (safe area) edge
    HORIZONTAL_ALIGN_FULLSCREEN = 4,     // disregards safe area
    HORIZONTAL_ALIGN_NOSCALE = 5,        // uses exact parameters - neither adjusts for safe area nor scales for screen size
    HORIZONTAL_ALIGN_TO640 = 6,          // scales a real-screen resolution x down into the 0 - 640 range
    HORIZONTAL_ALIGN_CENTER_SAFEAREA = 7 // center of the safearea
};

enum verticalAlign_e {
    VERTICAL_ALIGN_SUBTOP = 0,           // top edge of the 4:3 screen (safe area not included)
    VERTICAL_ALIGN_TOP = 1,              // top viewable (safe area) edge
    VERTICAL_ALIGN_CENTER = 2,           // center of the screen (reticle)
    VERTICAL_ALIGN_BOTTOM = 3,           // bottom viewable (safe area) edge
    VERTICAL_ALIGN_FULLSCREEN = 4,       // disregards safe area
    VERTICAL_ALIGN_NOSCALE = 5,          // uses exact parameters - neither adjusts for safe area nor scales for screen size
    VERTICAL_ALIGN_TO480 = 6,            // scales a real-screen resolution y down into the 0 - 480 range
    VERTICAL_ALIGN_CENTER_SAFEAREA = 7   // center of the safe area
};

enum materialType_e {
    MATERIAL_TYPE_DOLAR = 0x0, // $raw, $cursor, $bigfont, $extrabigfont
    MATERIAL_TYPE_UI1 = 0x1,   // ui/assets/3_cursor3, ui/assets/sliderbutt_1
    MATERIAL_TYPE_UI2 = 0x3,  // white, console, ui/assets/scrollbar.tga, ui/assets/sliderbutt_1, menu/art/unknownmap, loadscreen
    MATERIAL_TYPE_EFFECT = 0x6,  // tracer, sun
    MATERIAL_TYPE_DEFAULT = 0x7, // icons (stance, logometer, objective, killicon, ...
    MATERIAL_TYPE_SKY = 0x9
};

enum textStyle_e {
    TEXT_STYLE_NORMAL = 0,              // normal text
    TEXT_STYLE_BLINK = 1,               // fast blinking
    TEXT_STYLE_PULSE = 2,               // slow pulsing
    TEXT_STYLE_SHADOWED = 3,            // drop shadow ( need a color for this )
    TEXT_STYLE_OUTLINED = 4,            // drop shadow ( need a color for this )
    TEXT_STYLE_OUTLINESHADOWED = 5,     // drop shadow ( need a color for this )
    TEXT_STYLE_SHADOWEDMORE = 6         // drop shadow ( need a color for this )
};


/**
 * Register a material handle by name.
 */
inline materialHandle_t* CG_RegisterMaterial(const char* name, materialType_e type) {
    ASM_CALL_RETURN(materialHandle_t*, 0x00402160, 0, ECX(name), EAX(type))
}

/**
 * Register a material handle by name with no mipmaps.
 */
inline materialHandle_t* CG_RegisterMaterialNoMip(const char* name, materialType_e type) {
    ASM_CALL_RETURN(materialHandle_t*, 0x00402160, 0, ECX(name), EAX(type))
}




/**
 * Draws a text with possibility to align it in 640x480 screen
 */
inline void UI_DrawText(const char* text, int len, fontHandle_t* font, float x, float y, horizontalAlign_e horzAlign, verticalAlign_e vertAlign, float scale, vec4_t color, textStyle_e style) {
    ASM_CALL(RETURN_VOID, 0x005322c0, 7, PUSH(text), PUSH(len), ESI(font), PUSH(x), PUSH(y), EDI(horzAlign), EBX(vertAlign), PUSH(scale), PUSH(color), PUSH(style));
}

/**
 * Returns the width of the text in pixels
 */
inline int UI_TextWidth(const char* text, int len, fontHandle_t* font, float scale) {
    ASM_CALL_RETURN(int, 0x00532250, 3, PUSH(text), PUSH(len), PUSH(scale), ESI(font))
}

/**
 * Returns the height of the text in pixels
 */
inline int UI_TextHeight(fontHandle_t* font, float scale) {
    ASM_CALL_RETURN(int, 0x00532290, 1, PUSH(scale), ESI(font))
}



/**
 * Draws a picture with abbility to flip it (with negative width or height)
 * Internally CalcScreenPlacement() and R_AddCmdDrawStretchPic() are called
 */
inline void UI_DrawHandlePic(float x, float y, float w, float h, int horizontalAlign, int verticalAlign, vec4_t color, void* shader) {
    ASM_CALL(RETURN_VOID, 0x00546620, 6, PUSH(x), PUSH(y), PUSH(w), PUSH(h), EDI(horizontalAlign), EBX(verticalAlign), PUSH(color), PUSH(shader));
}

/**
 * Draws a picture with abbility to rotate around the center
 * Internally CalcScreenPlacement() and R_AddCmdDrawQuadPic() are called
 */
inline void CG_DrawRotatedPic(float x, float y, float w, float h, horizontalAlign_e horizontalAlign, verticalAlign_e verticalAlign, float rotation, vec4_t color, void* shader) {
    ASM_CALL(RETURN_VOID, 0x004c2460, 7, PUSH(x), PUSH(y), PUSH(w), PUSH(h), EDI(horizontalAlign), EBX(verticalAlign), PUSH(rotation), PUSH(color), PUSH(shader));
}


void drawing_frame();
void drawing_init();
void drawing_patch();


/*
#define GFX_FUNCTIONS_ADDR 0x0068a1e8
GFX functions:
0	0	sub_10013180;
1	1	R_BeginRegistration;
2	2	R_RegisterModel;
3	3	R_RegisterInlineModel;
4	4	Material_RegisterHandle;
5	5	R_RegisterRawImage;
6	6	Material_IsDefault;
7	7	R_LoadWorld;
8	8	R_GetWorldBounds;
9	9	R_FinishLoadingModels;
10	a	R_SetIgnorePrecacheErrors;
11	b	R_GetIgnorePrecacheErrors;
12	c	R_GetMinSpecImageMemory;
13	d	R_GetMaterialName;
14	e	R_GetMaterialSubimageCount;
15	f	R_IsMaterialRefractive;
16	10	R_GetFarPlaneDist;
17	11	R_EndRegistration;
18	12	R_ClearScene;
19	13	sub_10020fb0;
20	14	R_AddRefEntityToScene;
21	15	sub_10020fa0;
22	16	sub_10020f80;
23	17	&data_10020f90;
24	18	sub_10013040;
25	19	R_DefaultVertexFrames;
26	1a	R_AddPolyToScene;
27	1b	R_AddLightToScene;
28	1c	R_InterpretSunLightParseParams;
29	1d	R_ResetSunLightParseParams;
30	1e	R_SetCullDist;
31	1f	R_SetFog;
32	20	R_SwitchFog;
33	21	R_ArchiveFogState;
34	22	R_ClearFogs;
35	23	R_SetSunLightOverride;
36	24	R_ResetSunLightOverride;
37	25	R_RenderScene;
38	26	R_BeginDelayedDrawing;
39	27	R_EndDelayedDrawing;
40	28	R_IssueDelayedDrawing;
41	29	R_ClearFlares;
42	2a	R_AddCmdSetMaterialColor;
43	2b	R_AddCmdDrawStretchPic;
44	2c	R_AddCmdDrawStretchPicRotate;
45	2d	R_AddCmdDrawStretchRaw;
46	2e	R_AddCmdDrawQuadPic;
47	2f	R_AddCmdDrawSprite;
48	30	R_BeginFrame;
49	31	R_EndFrame;
50	32	R_BeginDebugFrame;
51	33	R_EndDebugFrame;
52	34	R_EndView;
53	35	R_DoneRenderingViews;
54	36	R_AddCmdSaveScreen;
55	37	R_AddCmdBlendSavedScreen;
56	38	R_AddCmdClearScreen;
57	39	R_AddCmdSetViewport;
58	3a	R_MarkFragments;
59	3b	R_ModelBounds;
60	3c	R_TrackStatistics;
61	3d	R_PickMaterial;
62	3e	R_RegisterFont;
63	3f	R_ResetImageAllocations;
64	40	R_FreeImageAllocations;
65	41	R_BeginCubemapShot;
66	42	R_EndCubemapShot;
67	43	R_SaveCubemapShot;
68	44	R_LightingFromCubemapShots;
69	45	R_LocateDebugStrings;
70	46	R_LocateDebugLines;
71	47	R_AddPlume;
72	48	R_ShutdownDebug;
73	49	RB_UpdateColor;
74	4a	R_NormalizedTextScale;
75	4b	R_TextWidth;
76	4c	R_TextHeight;
77	4d	R_DrawText;
78	4e	R_AddCmdDrawTextInSpace;
79	4f	R_ConsoleTextWidth;
80	50	R_DrawConsoleText;
81	51	R_AddCmdDrawTextWithCursor;
82	52	R_DObjGetSurfMaterials;
83	53	R_DObjReplaceMaterial;
84	54	R_ParseSunLight;
85	55	Material_Duplicate;
86	56	R_DuplicateFont;
97	61	sub_1002d8e0;
98	62	sub_10020600;
99	63	sub_1002d320;
100	64	sub_1002d280;
101	65	sub_1002d270;
102	66	sub_1002d260;
103	67	sub_10020fc0;
104	68	R_AbortRenderCommands;
105	69	RB_IsGpuFenceFinished;
106	6a	RB_GpuSyncDelay;
107	6b	R_SetLodOrigin;
*/

#endif