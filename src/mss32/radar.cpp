#include "drawing.h"

#include <cfloat>

#include "shared.h"
#include "drawing.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_definitions.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_file.h"


#define mouse_offset_x              (((int*)0x0098fda8))
#define mouse_offset_y              (((int*)0x0098fdb0))
#define mapName                     (*((char(*)[0x3f])0x0196ffa0))
#define gameType                    (*((char(*)[0x3f])0x019953b0))
#define sv_cheats                   (*((dvar_t**)0x00c5c5cc))
#define demo_isPlaying              (*((int*)0x0064a170))
#define cg_hudCompassSize           (*((dvar_t**)0x0166bb34))
#define g_TeamColor_Allies          (*((dvar_t**)0x017d03bc))
#define g_TeamColor_Axis            (*((dvar_t**)0x01897e1c))

#define radar_width 100
#define radar_height 100
#define radar_scale 2
#define radar_calibration_scale 4

dvar_t* cg_drawRadar;
dvar_t* cg_hudRadarScale;
dvar_t* cg_hudRadarMapImage;
dvar_t* cg_hudRadarMapImageOffsetX;
dvar_t* cg_hudRadarMapImageOffsetY;
dvar_t* cg_hudRadarEntityScale;
dvar_t* cg_hudRadarEntityOffsetX;
dvar_t* cg_hudRadarEntityOffsetY;
dvar_t* cg_hudRadarMapImageRotation;
dvar_t* cg_hudRadarOffsetX;
dvar_t* cg_hudRadarOffsetY;
dvar_t* cg_hudRadarPlayersNumberSwitch;
dvar_t* cg_hudRadarColor;

char radar_lastMapName[64] = {0};

int cg_hudRadarCalibrateState = 0;
float cg_hudRadarCalibrate_point1_X = 0;
float cg_hudRadarCalibrate_point1_Y = 0;
float cg_hudRadarCalibrate_point2_X = 0;
float cg_hudRadarCalibrate_point2_Y = 0;

float cg_hudRadarCalibrate_point1_worldX = 0;
float cg_hudRadarCalibrate_point1_worldY = 0;
float cg_hudRadarCalibrate_point2_worldX = 0;
float cg_hudRadarCalibrate_point2_worldY = 0;

#define MAX_FIRE_EVENTS 128
struct RadarFireEvent { float x, y, w, h, angle; int startTime; };
static RadarFireEvent radarFireEvents[MAX_FIRE_EVENTS];


void cg_hudRadarCalibrate() {

    // Disable radar if not enabled, or not in spectator team. Enable if cheats are enabled or demo is playing
    if (sv_cheats->value.integer == 0 && demo_isPlaying == 0) {
        Com_Printf("Radar calibration is only available when cheats are enabled or demo is playing.\n");
        return;
    }

    if (cg_drawRadar->value.boolean == false) {
        Com_Printf("Radar is not enabled.\n");
        return;
    }

    switch (cg_hudRadarCalibrateState) {
        case 0:
            cg_hudRadarCalibrateState = 1;

            // Init
            cg_hudRadarCalibrate_point1_X = radar_width * radar_calibration_scale;
            cg_hudRadarCalibrate_point1_Y = 0;
            cg_hudRadarCalibrate_point2_X = radar_width * radar_calibration_scale;
            cg_hudRadarCalibrate_point2_Y = 0;
            break;

        case 1:
            cg_hudRadarCalibrateState = 2;
            break;

        case 2:
            cg_hudRadarCalibrateState = 3;
            break;

        case 3:
            cg_hudRadarCalibrateState = 4;
            break;

        case 4:
            cg_hudRadarCalibrateState = 0;

            // Compute the scale and offset
            float diffRadarX = (cg_hudRadarCalibrate_point2_X - cg_hudRadarCalibrate_point1_X) / radar_calibration_scale;
            float diffRadarY = (cg_hudRadarCalibrate_point2_Y - cg_hudRadarCalibrate_point1_Y) / radar_calibration_scale;
            float diffWorldX = (cg_hudRadarCalibrate_point2_worldX - cg_hudRadarCalibrate_point1_worldX);
            float diffWorldY = (cg_hudRadarCalibrate_point2_worldY - cg_hudRadarCalibrate_point1_worldY);

            float lengthRadar = sqrtf(diffRadarX * diffRadarX + diffRadarY * diffRadarY);
            float lengthWorld = sqrtf(diffWorldX * diffWorldX + diffWorldY * diffWorldY);
            float scale = lengthRadar / lengthWorld;

            float offsetX = ((cg_hudRadarCalibrate_point1_X /*- cg_hudRadarOffsetX->value.decimal*/) / radar_calibration_scale) - (cg_hudRadarCalibrate_point1_worldX * scale);
            float offsetY = ((cg_hudRadarCalibrate_point1_Y /*+ cg_hudRadarOffsetY->value.decimal*/) / radar_calibration_scale) + (cg_hudRadarCalibrate_point1_worldY * scale) - radar_height;

            Dvar_SetFloat(cg_hudRadarEntityScale, scale);
            Dvar_SetFloat(cg_hudRadarEntityOffsetX, offsetX);
            Dvar_SetFloat(cg_hudRadarEntityOffsetY, offsetY);

            Com_Printf("Calibration done\n");
            Com_Printf("Point 1:        %f, %f\n", cg_hudRadarCalibrate_point1_X, cg_hudRadarCalibrate_point1_Y);
            Com_Printf("Point 2:        %f, %f\n", cg_hudRadarCalibrate_point2_X, cg_hudRadarCalibrate_point2_Y);
            Com_Printf("World Point 1:  %f, %f\n", cg_hudRadarCalibrate_point1_worldX, cg_hudRadarCalibrate_point1_worldY);
            Com_Printf("World Point 2:  %f, %f\n", cg_hudRadarCalibrate_point2_worldX, cg_hudRadarCalibrate_point2_worldY);
            Com_Printf("Diff Radar:     %f, %f\n", diffRadarX, diffRadarY);
            Com_Printf("Diff World:     %f, %f\n", diffWorldX, diffWorldY);
            Com_Printf("Length Radar:   %f\n", lengthRadar);
            Com_Printf("Length World:   %f\n", lengthWorld);
            Com_Printf("---\n");
            Com_Printf("Scale:          %f\n", scale);
            Com_Printf("OffsetX:        %f\n", offsetX);
            Com_Printf("OffsetY:        %f\n", offsetY);

            break;
    }
}






void compute_coordinates(float& x, float& y, float& w, float& h, vec3_t origin, float baseWidth, float baseHeight, int rotation, float radar_w, float radar_h) {
    
    float mapOffsetX = cg_hudRadarEntityOffsetX->value.decimal;
    float mapOffsetY = cg_hudRadarEntityOffsetY->value.decimal;
    float mapScale = cg_hudRadarEntityScale->value.decimal;
    float scale = cg_hudRadarScale->value.decimal * radar_scale;

    vec3_t origin_scaled;
    VectorScale(origin, mapScale * scale, origin_scaled);

    w = baseWidth * scale;
    h = baseHeight * scale;
    x = origin_scaled[0] + cg_hudRadarOffsetX->value.decimal;
    y = -origin_scaled[1] + cg_hudRadarOffsetY->value.decimal + radar_h;
    x += mapOffsetX * scale;
    y += mapOffsetY * scale;

    // Apply rotation to objective positions (0, 90, 180, -90, -180)
    float centerX = cg_hudRadarOffsetX->value.decimal + radar_w / 2.0f;
    float centerY = cg_hudRadarOffsetY->value.decimal + radar_h / 2.0f;
    float relX = x - centerX;
    float relY = y - centerY;
    float rotatedX = relX, rotatedY = relY;

    switch (rotation) {
        case 0:
        rotatedX = relX;
        rotatedY = relY;
        break;
        case 90:
        rotatedX = -relY;
        rotatedY = relX;
        break;
        case 180:
        case -180:
        rotatedX = -relX;
        rotatedY = -relY;
        break;
        case -90:
        rotatedX = relY;
        rotatedY = -relX;
        break;
        default:
        // No rotation or unsupported angle
        break;
    }

    x = centerX + rotatedX;
    y = centerY + rotatedY;

    x += cg_hudRadarMapImageOffsetX->value.decimal;
    y += cg_hudRadarMapImageOffsetY->value.decimal;
}


void radar_draw()
{
    clientInfo_t* player_ci = &clientInfo[ cg.clientNum ];


    // Disable radar if not enabled, or not in spectator team. Enable if cheats are enabled or demo is playing
    if (cg_drawRadar->value.boolean == false || (player_ci->team != TEAM_SPECTATOR && sv_cheats->value.integer == 0 && demo_isPlaying == 0)) {
        return;
    }

    // Radar map image not set, skipping radar draw
    if (cg_hudRadarMapImage->value.string[0] == '\0') {
        return;
    }

    
    float x, y, w, h;
    float radar_x, radar_y, radar_w, radar_h;
    
    horizontalAlign_e horizontalAlign = HORIZONTAL_ALIGN_LEFT;
    verticalAlign_e verticalAlign = VERTICAL_ALIGN_TOP;
    
    float scale = cg_hudRadarScale->value.decimal * radar_scale;
    int rotation = cg_hudRadarMapImageRotation->value.integer;


    radar_w = radar_width * scale;
    radar_h = radar_width * scale;

    if (cg_hudRadarCalibrateState > 0) {
        radar_x = 0;
        radar_y = 0;
        radar_w = radar_width * radar_calibration_scale;
        radar_h = radar_height * radar_calibration_scale;
        rotation = 0;

        switch (cg_hudRadarCalibrateState) {
            case 2: // set radar point 1
            case 4: // set radar point 2
            {
                vec4_t bgColor = { 0, 0, 0, .9f };
                UI_DrawHandlePic(0, 0, 640, 480, HORIZONTAL_ALIGN_FULLSCREEN, VERTICAL_ALIGN_FULLSCREEN, bgColor, shaderWhite);
                break;
            }
        }
    }

    radar_x = cg_hudRadarOffsetX->value.decimal;
    radar_y = cg_hudRadarOffsetY->value.decimal;

    x = radar_x + cg_hudRadarMapImageOffsetX->value.decimal;
    y = radar_y + cg_hudRadarMapImageOffsetY->value.decimal;
    w = radar_w;
    h = radar_h;
    
    // Draw radar background
    //vec4_t radarColorBg = { 0, 0, 0.1, 0.3f };
    //CG_DrawRotatedPic(x, y, w, h, horizontalAlign, verticalAlign, rotation, radarColorBg, shaderWhite);

    vec4_t colRadar = { 1, 1, 1, 1 };
    if (cg_hudRadarColor->value.vec3) {
        colRadar[0] = cg_hudRadarColor->value.vec3[0];
        colRadar[1] = cg_hudRadarColor->value.vec3[1];
        colRadar[2] = cg_hudRadarColor->value.vec3[2];
    }

    // Draw radar map image
    materialHandle_t* radarMaterial = CG_RegisterMaterialNoMip(cg_hudRadarMapImage->value.string, MATERIAL_TYPE_DEFAULT);
    CG_DrawRotatedPic(x, y, w, h, horizontalAlign, verticalAlign, rotation, colRadar, radarMaterial);


    // Draw calibration UI
    if (cg_hudRadarCalibrateState > 0) {
        vec4_t textColor = { 1, 1, 1, 1 };
        // Each odd second switch color
        if (cg.snap->ps.commandTime % 2000 > 1000) {
            textColor[0] = 1;
        } else {
            textColor[0] = 0;
        }

        UI_DrawText("Calibrating radar", INT_MAX, fontNormal, 0, 100, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_TOP, .5f, textColor, TEXT_STYLE_NORMAL);

        vec4_t incompletedPointColor = { 1, 1, 0, .5f };
        vec4_t completedPointColor = { 0, 1, 0, .5f };

        float pointSize = 10;

        switch (cg_hudRadarCalibrateState) {
            case 1: {          
                cg_hudRadarCalibrate_point1_worldX = cg.refdef.vieworg[0];
                cg_hudRadarCalibrate_point1_worldY = cg.refdef.vieworg[1];
                break;
            }
            case 2: {
                cg_hudRadarCalibrate_point1_X += *mouse_offset_x / 10.0f;
                cg_hudRadarCalibrate_point1_Y += *mouse_offset_y / 10.0f;
                break;
            }
            case 3: {
                cg_hudRadarCalibrate_point2_worldX = cg.refdef.vieworg[0];
                cg_hudRadarCalibrate_point2_worldY = cg.refdef.vieworg[1];
                break;;
            }
            case 4: {
                cg_hudRadarCalibrate_point2_X += *mouse_offset_x / 10.0f;
                cg_hudRadarCalibrate_point2_Y += *mouse_offset_y / 10.0f;
                break;
            }
        }


        if (cg_hudRadarCalibrateState >= 1) {
            UI_DrawText("1. Move the camera to the first position on the map", INT_MAX, fontNormal, 0, 120, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_TOP, 0.2f, textColor, TEXT_STYLE_SHADOWED);
        }

        if (cg_hudRadarCalibrateState >= 2) {
            UI_DrawText("2. Select current location on the map", INT_MAX, fontNormal, 0, 130, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_TOP, 0.2f, textColor, TEXT_STYLE_SHADOWED);
            float* color = cg_hudRadarCalibrateState == 1 ? incompletedPointColor : completedPointColor;
            CG_DrawRotatedPic(x + cg_hudRadarCalibrate_point1_X - pointSize/2, y + cg_hudRadarCalibrate_point1_Y - pointSize/2, pointSize, pointSize, horizontalAlign, verticalAlign, 0, color, shaderWhite);
            CG_DrawRotatedPic(x + cg_hudRadarCalibrate_point1_X - 1, y + cg_hudRadarCalibrate_point1_Y - 1, 2, 2, horizontalAlign, verticalAlign, 0, colRed, shaderWhite);
        }

        if (cg_hudRadarCalibrateState >= 3) {
            UI_DrawText("3. Move the camera to the second position on the map", INT_MAX, fontNormal, 0, 140, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_TOP, 0.2f, textColor, TEXT_STYLE_SHADOWED);
        }
        if (cg_hudRadarCalibrateState >= 4) {
            UI_DrawText("4. Select current location on the map", INT_MAX, fontNormal, 0, 150, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_TOP, 0.2f, textColor, TEXT_STYLE_SHADOWED);
            float* color = cg_hudRadarCalibrateState == 2 ? incompletedPointColor : completedPointColor;
            CG_DrawRotatedPic(x + cg_hudRadarCalibrate_point2_X - pointSize/2, y + cg_hudRadarCalibrate_point2_Y - pointSize/2, pointSize, pointSize, horizontalAlign, verticalAlign, 0, color, shaderWhite);
            CG_DrawRotatedPic(x + cg_hudRadarCalibrate_point2_X - 1, y + cg_hudRadarCalibrate_point2_Y - 1, 2, 2, horizontalAlign, verticalAlign, 0, colRed, shaderWhite);
        }

        return;
    }

    

    // Draw objectives
    for (int i = 0; i < 0x10; i++)
    {
        objective_s obj = cg.snap->ps.objective[i];

        if (obj.state != 4) // 4 = active
            continue;

        vec3_t origin;

        if (obj.entNum == 1023) {
            VectorCopy(obj.origin, origin);
        } else {
            VectorCopy(cg_entities[obj.entNum].lerpOrigin, origin); // objective is pointing to a player
        }

        materialHandle_t* shader;
        if (obj.icon <= 0) {
            shader = shaderObjective;
        } else {
            char buffer[0x40];
            if (!CG_GetShaderConfigString(obj.icon, buffer, sizeof(buffer) - 1))
                shader = shaderObjective;
            else
                shader = CG_RegisterMaterialNoMip(buffer, MATERIAL_TYPE_DEFAULT);
        }

        float objectiveSize = 8;

        compute_coordinates(x, y, w, h, origin, objectiveSize, objectiveSize, rotation, radar_w, radar_h);

        CG_DrawRotatedPic(x - w / 2, y - h / 2, w, h, horizontalAlign, verticalAlign, 0, colWhite, shader);       
        //CG_DrawRotatedPic(x, y, 1, 1, horizontalAlign, verticalAlign, 0, colRed, shaderWhite); // debug
    }



    // Get array of 5 allies players and 5 axis players
    int alliesCount = 0;
    int axisCount = 0;
    int allies[5] = {0};
    int axis[5] = {0};
    for (int i = 0; i < 64; i++) {
        centity_t* cent = &cg_entities[i];
        clientInfo_t* ci = &clientInfo[cent->currentState.clientNum];

        if (cent->currentState.eType != ET_PLAYER || /*!(cent->currentValid & 1) ||*/ ci->infoValid != qtrue || ci->team >= TEAM_SPECTATOR)
            continue;

        if (ci->team == TEAM_ALLIES && alliesCount < 5) {
            allies[alliesCount++] = i;
        } else if (ci->team == TEAM_AXIS && axisCount < 5) {
            axis[axisCount++] = i;
        }
    }


    // Draw nades & smokes
    for (int i = 64; i < MAX_GENTITIES; i++)
    {
        if (cg_entities[i].currentState.eType == 4 && cg_entities[i].currentValid == 513)
        {
            //if (g_pMath.WorldToScreen(cg_entities[i].lerpOrigin, screen))
            //    g_pDraw.CG_DrawStringExt(screen[0], screen[1], "GRENADE", colorGreenish, 1, 1, 1);
        }
    }

    // Draw firing events
    for (int i = 0; i < MAX_FIRE_EVENTS; ++i) {
        RadarFireEvent& evt = radarFireEvents[i];
        if (evt.startTime == 0)
            continue;
        int timeFiredDelta = cg.time - evt.startTime;
        if (timeFiredDelta > 0 && timeFiredDelta < 1000) {
            float fireScale = 4.0f;
            float alpha = 1.0f - (timeFiredDelta / 1000.0f);
            vec4_t fireColor = { 1, 0.5f, 0.5f, alpha };
            CG_DrawRotatedPic(
                evt.x - evt.w * fireScale / 2,
                evt.y - evt.h * fireScale / 2,
                evt.w * fireScale,
                evt.h * fireScale,
                horizontalAlign,
                verticalAlign,
                evt.angle,
                fireColor,
                CG_RegisterMaterialNoMip("radar_player_fire", MATERIAL_TYPE_DEFAULT)
            );
        }
    }

    // Draw players
	for(int i = 0; i < 64; i++ )
	{
		centity_t* cent = &cg_entities[ i ];
		clientInfo_t* ci = &clientInfo[ cent->currentState.clientNum ];
        compassWeaponFire_t* weaponFire = &cg.compassWeaponFireEndTime[i];

        static int weaponFireTimeEndTimeLast[64];

		if((
			cent->currentState.eType == ET_PLAYER && 
			!(cent->currentState.eFlags & 1) &&
            !(cent->currentState.eFlags & 0x20000) && // dead and in dead session
            (cent->currentValid & 1) && // dead and spectator mode

			ci->infoValid == qtrue &&
			ci->team < TEAM_SPECTATOR)
        ) {
            // Get color of team based on g_TeamColor_Allies and g_TeamColor_Axis
            vec4_t color;
            Vector4Copy(colWhite, color);
            if (ci->team == TEAM_ALLIES) {
                dvar_t* alliesColor = Dvar_GetDvarByName("g_TeamColor_Allies");
                if (alliesColor) Dvar_StringToColor(alliesColor, &color);
                color[3] = 1; // alpha
                
            } else if (ci->team == TEAM_AXIS) {
                dvar_t* axisColor = Dvar_GetDvarByName("g_TeamColor_Axis");
                if (axisColor) Dvar_StringToColor(axisColor, &color);
                color[3] = 1; // alpha
            }

            // Coordinates
		    vec3_t origin;
            VectorCopy(cent->currentState.pos.trBase, origin);
            float playerSize = 10;
            compute_coordinates(x, y, w, h, origin, playerSize, playerSize, rotation, radar_w, radar_h);

            // Rotation
            float playerRotation = (cent->nextState.apos.trBase[YAW] - 90 - rotation) * -1; // -90 to align with the map

            // Firing of the player
            // Because lastTimeFired is fire time + weapon fire delay, we need to save current time to be able to calculate the delta
            if (weaponFireTimeEndTimeLast[i] != weaponFire->lastFireEndTime) {
                weaponFireTimeEndTimeLast[i] = weaponFire->lastFireEndTime;

                // When player fires, add a new event or reuse an old one
                // Find a free slot or reuse the oldest
                int slot = -1;
                for (int j = 0; j < MAX_FIRE_EVENTS; ++j) {
                    if (radarFireEvents[j].startTime == 0 || cg.time - radarFireEvents[j].startTime > 1000) {
                        slot = j;
                        break;
                    }
                }
                if (slot == -1) slot = 0; // fallback

                radarFireEvents[slot].x = x;
                radarFireEvents[slot].y = y;
                radarFireEvents[slot].w = w;
                radarFireEvents[slot].h = h;
                radarFireEvents[slot].angle = playerRotation;
                radarFireEvents[slot].startTime = cg.time;
            }

            float player_arrow_scale = 1;
            float player_circle_scale = 1;
            float* player_arrow_color = colWhite;
            // Highlight of followed player
            if (cg.snap && i == cg.snap->ps.clientNum) {
                player_arrow_scale = 1.5f;
                player_circle_scale = 1.2f;
                player_arrow_color = colYellow;
            } 
            // Draw player
            CG_DrawRotatedPic(x - w*player_arrow_scale/2, y - h*player_arrow_scale/2, w*player_arrow_scale, h*player_arrow_scale, horizontalAlign, verticalAlign, 
                playerRotation, player_arrow_color, CG_RegisterMaterialNoMip("radar_player_arrow", MATERIAL_TYPE_DEFAULT));
            CG_DrawRotatedPic(x - w*player_circle_scale/2, y - h*player_circle_scale/2, w*player_circle_scale, h*player_circle_scale, horizontalAlign, verticalAlign, 
                playerRotation, color, CG_RegisterMaterialNoMip("radar_player_circle", MATERIAL_TYPE_DEFAULT));


            // Player number generation
            int playerTeamNumber = i;
            if (ci->team == TEAM_ALLIES) {
                for (int j = 0; j < alliesCount; j++) {
                    if (allies[j] == i) {
                        playerTeamNumber = cg_hudRadarPlayersNumberSwitch->value.boolean ? j + 6 : j + 1;
                        if (playerTeamNumber == 10) playerTeamNumber = 0;
                        break;
                    }
                }
            } else if (ci->team == TEAM_AXIS) {
                for (int j = 0; j < axisCount; j++) {
                    if (axis[j] == i) {
                        playerTeamNumber = cg_hudRadarPlayersNumberSwitch->value.boolean ? j + 1 : j + 6;
                        if (playerTeamNumber == 10) playerTeamNumber = 0;
                        break;
                    }
                }
            }

            // Player number text size
            const char* strNum = va("%i", playerTeamNumber);
            float textNumScale = 0.01f * player_circle_scale;
            float textNumWidth = UI_TextWidth(strNum, 0, fontNormal, textNumScale * playerSize * scale);
            float textNumHeight = UI_TextHeight(fontNormal, textNumScale * playerSize * scale);

            // Draw player number
            UI_DrawText(strNum, INT_MAX, fontBig, x - textNumWidth/2.0, y + textNumHeight/2.0, horizontalAlign, verticalAlign, playerSize * textNumScale * scale, 
                colWhite, TEXT_STYLE_SHADOWED);
		}
	}
}




/** Called every frame on frame start. */
void radar_frame() {
    // Check for map change
    if (strcmp(mapName, radar_lastMapName) != 0) {
        strncpy(radar_lastMapName, mapName, sizeof(radar_lastMapName) - 1);
        radar_lastMapName[sizeof(radar_lastMapName) - 1] = '\0';

        // No map name
        if (radar_lastMapName[0] == '\0') {
            Dvar_SetString(cg_hudRadarMapImage, "");
            return;
        }

        const char* file = nullptr;
        // Read file
        int i = FS_ReadFile(va("maps/mp/%s.radar", mapName), (void**)&file);

        if (i <= 0 || !file) {
            Com_Printf("^1Radar file not found: maps/%s.radar\n", mapName);
            Dvar_SetString(cg_hudRadarMapImage, "");
            return;
        }

        const char* data = file;

        // Parse radar file values into temporary variables
        Com_Parse(&data); // skip header
        char* imageName = Com_Parse(&data);
        if (!imageName || imageName[0] == '\0') {
            Com_Error(ERR_DROP, "Invalid image name in radar file: maps/%s.radar\n", mapName);
            return;
        }
        char imageNameTemp[128];
        strncpy(imageNameTemp, imageName, sizeof(imageNameTemp) - 1);
        imageNameTemp[sizeof(imageNameTemp) - 1] = '\0';

        Com_Parse(&data); // skip header
        char* entityScaleStr = Com_Parse(&data);
        if (!entityScaleStr || entityScaleStr[0] == '\0') {
            Com_Error(ERR_DROP, "Invalid scale in radar file: maps/%s.radar\n", mapName);
            return;
        }
        float entityScale = (float)atof(entityScaleStr);

        Com_Parse(&data); // skip header
        char* entityOffsetXStr = Com_Parse(&data);
        if (!entityOffsetXStr || entityOffsetXStr[0] == '\0') {
            Com_Error(ERR_DROP, "Invalid offsetX in radar file: maps/%s.radar\n", mapName);
            return;
        }
        float entityOffsetX = (float)atof(entityOffsetXStr);

        Com_Parse(&data); // skip header
        char* entityOffsetYStr = Com_Parse(&data);
        if (!entityOffsetYStr || entityOffsetYStr[0] == '\0') {
            Com_Error(ERR_DROP, "Invalid offsetY in radar file: maps/%s.radar\n", mapName);
            return;
        }
        float entityOffsetY = (float)atof(entityOffsetYStr);

        Com_Parse(&data); // skip header
        char* rotationStr = Com_Parse(&data);
        if (!rotationStr || rotationStr[0] == '\0') {
            Com_Error(ERR_DROP, "Invalid rotation in radar file: maps/%s.radar\n", mapName);
            return;
        }
        int rotation = atoi(rotationStr);

        Com_Parse(&data); // skip header
        char* imageOffsetXStr = Com_Parse(&data);
        if (!imageOffsetXStr || imageOffsetXStr[0] == '\0') {
            Com_Error(ERR_DROP, "Invalid imageOffsetX in radar file: maps/%s.radar\n", mapName);
            return;
        }
        float imageOffsetX = (float)atof(imageOffsetXStr);

        Com_Parse(&data); // skip header
        char* imageOffsetYStr = Com_Parse(&data);
        if (!imageOffsetYStr || imageOffsetYStr[0] == '\0') {
            Com_Error(ERR_DROP, "Invalid imageOffsetY in radar file: maps/%s.radar\n", mapName);
            return;
        }
        float imageOffsetY = (float)atof(imageOffsetYStr);


        FS_FreeFile((void*)file);

        // Now set Dvars after file is freed
        Dvar_SetString(cg_hudRadarMapImage, imageNameTemp);
        Dvar_SetFloat(cg_hudRadarMapImageOffsetX, imageOffsetX);
        Dvar_SetFloat(cg_hudRadarMapImageOffsetY, imageOffsetY);
        Dvar_SetFloat(cg_hudRadarEntityScale, entityScale);
        Dvar_SetFloat(cg_hudRadarEntityOffsetX, entityOffsetX);
        Dvar_SetFloat(cg_hudRadarEntityOffsetY, entityOffsetY);
        Dvar_SetInt(cg_hudRadarMapImageRotation, rotation);

    }
}

/** Called once when DLL hot-reloading is activated. Only in debug mode. */
void radar_unload() {
    Cmd_RemoveCommand("cg_hudRadarCalibrate");
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void radar_init() {
    Cmd_AddCommand("cg_hudRadarCalibrate", cg_hudRadarCalibrate);

    cg_drawRadar =                      Dvar_RegisterBool  ("cg_drawRadar", false,                                  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarScale =                  Dvar_RegisterFloat ("cg_hudRadarScale", 1,              -FLT_MAX, FLT_MAX,  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarMapImage =               Dvar_RegisterString("cg_hudRadarMapImage", "",                              (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarMapImageOffsetX =        Dvar_RegisterFloat ("cg_hudRadarMapImageOffsetX", 0,    -FLT_MAX, FLT_MAX,  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarMapImageOffsetY =        Dvar_RegisterFloat ("cg_hudRadarMapImageOffsetY", 0,    -FLT_MAX, FLT_MAX,  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarMapImageRotation =       Dvar_RegisterInt   ("cg_hudRadarMapImageRotation", 0,   -180,     180,      (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarEntityScale =            Dvar_RegisterFloat ("cg_hudRadarEntityScale", 1,        0,        1.0f,     (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarEntityOffsetX =          Dvar_RegisterFloat ("cg_hudRadarEntityOffsetX", 0,      -FLT_MAX, FLT_MAX,  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarEntityOffsetY =          Dvar_RegisterFloat ("cg_hudRadarEntityOffsetY", 0,      -FLT_MAX, FLT_MAX,  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarOffsetX =                Dvar_RegisterFloat ("cg_hudRadarOffsetX", 5,            -FLT_MAX, FLT_MAX,  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarOffsetY =                Dvar_RegisterFloat ("cg_hudRadarOffsetY", 20,           -FLT_MAX, FLT_MAX,  (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarPlayersNumberSwitch =    Dvar_RegisterBool  ("cg_hudRadarPlayersNumberSwitch",    false,             (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudRadarColor =                  Dvar_RegisterVec3  ("cg_hudRadarColor",                  1, 1, 1, 0, 1,     (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));


    /*#if DEBUG
    Dvar_SetBool(cg_hudRadar, true);
    #endif*/

}

/** Called before the entry point is called. Used to patch the memory. */
void radar_patch() {

}