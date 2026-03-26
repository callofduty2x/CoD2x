#include "vmix.h"

#include "../shared/http_client.h"
#include "shared.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"


dvar_t* cl_vmix;
dvar_t* cl_vmix_debug;
dvar_t* cl_vmix_url;
dvar_t* cl_vmix_webbrowser_url_prefix;
dvar_t* cl_vmix_webbrowser_url_suffix;
dvar_t* cl_vmix_webbrowser_sourceName;
dvar_t* cl_vmix_scr_spectatedUserId;
dvar_t* cl_vmix_scr_spectatedUserName;
dvar_t* cl_vmix_scr_spectatedUserTeam;
dvar_t* cl_vmix_scr_spectatedHWID;
dvar_t* cl_vmix_scr_data;
dvar_t* cl_vmix_camera_overlayNum;
dvar_t* cl_vmix_cameraMode;
dvar_t* cl_vmix_playerIds;
dvar_t* cl_vmix_player_ids[10];
dvar_t* cl_vmix_player_sourceNames[10];
const char* vmix_lastActiveCameraSourceName = nullptr;

HttpClient* vmix_httpClient = nullptr;

void vmix_send_command(std::string url) {
    if (cl_vmix_debug->value.boolean) {
        Com_Printf("VMix Sending command: %s\n", url.c_str());
    }
    vmix_httpClient->get(
        url.c_str(),
        [url](const HttpClient::Response& res) {
            if (res.status != 200 && res.status != 201) {
                Com_Printf("^1VMix %s failed, invalid status: %d\n%s\n", url.c_str(), res.status, res.body.c_str());
                return;
            }
            if (cl_vmix_debug->value.boolean) {
                Com_Printf("^2VMix %s response: %d, %s\n", url.c_str(), res.status, res.body.c_str());
            }
        },
        [url](const std::string& error) {
            Com_Printf("^1VMix %s error: %s\n", url.c_str(), error.c_str());
        }, 5000
    );
}

void vmix_send_command_overlayInputIn(const char* inputName, int overlayNum) {
    std::string function = "OverlayInput" + std::to_string(overlayNum) + "In"; 
    vmix_send_command(std::string(cl_vmix_url->value.string) + "/api/?Function=" + function + "&Input=" + inputName);
}

void vmix_send_command_overlayInputOff(const char* inputName, int overlayNum) {
    std::string function = "OverlayInput" + std::to_string(overlayNum) + "Off"; 
    vmix_send_command(std::string(cl_vmix_url->value.string) + "/api/?Function=" + function + "&Input=" + inputName);
}

void vmix_send_command_webbrowserNavigate(const char* inputName, const char* url) {
    vmix_send_command(std::string(cl_vmix_url->value.string) + "/api/?Function=BrowserNavigate&Input=" + inputName +"&Value=" + url);
}

/** Called every frame on frame start. */
void vmix_frame() {


    if (cl_vmix && cl_vmix->value.boolean && clientState == CLIENT_STATE_ACTIVE) {
        // VMix integration is enabled
        if (vmix_httpClient == nullptr) {
            vmix_httpClient = new HttpClient();
        }

        vmix_httpClient->poll();

        if (!cl_vmix_scr_spectatedUserId->modified && !cl_vmix_scr_spectatedHWID->modified) {
            return; // No change
        }
        cl_vmix_scr_spectatedUserId->modified = false;
        cl_vmix_scr_spectatedHWID->modified = false;

        const char* spectatedUserId = cl_vmix_scr_spectatedUserId->value.string;
        const char* spectatedUserName = cl_vmix_scr_spectatedUserName->value.string;
        const char* spectatedUserTeam = cl_vmix_scr_spectatedUserTeam->value.string;
        const char* spectatedHWID = cl_vmix_scr_spectatedHWID->value.string;
        const char* scrData = cl_vmix_scr_data->value.string;

        bool cameraInputActivated = false;
        const char* foundCameraSourceName = nullptr;
       
        // Try to find if spectated player match any of configured player IDs for cameras
        for (int i = 0; i < 10; ++i) {
            const char *playerId = cl_vmix_player_ids[i]->value.string;

            if (cl_vmix_debug->value.boolean)
                Com_Printf("Comparing spectatedHWID: '%s' with playerId[%d]: '%s'\n", spectatedHWID, i, playerId);

            if (spectatedHWID[0] && playerId[0] && strcmp(spectatedHWID, playerId) == 0) 
            {
                if (cl_vmix_debug->value.boolean)
                    Com_Printf("  Matched\n");

                const char* sourceName = cl_vmix_player_sourceNames[i]->value.string;

                // Turn on new source
                if (cl_vmix_cameraMode->value.integer == 1) {

                    vmix_send_command_overlayInputIn(sourceName, cl_vmix_camera_overlayNum->value.integer);

                    vmix_lastActiveCameraSourceName = sourceName;
                    cameraInputActivated = true;
                }
          
                foundCameraSourceName = sourceName;
                break;
            }
        }

        if (cameraInputActivated == false && vmix_lastActiveCameraSourceName != nullptr) {
            // Turn off last source
            vmix_send_command_overlayInputOff(vmix_lastActiveCameraSourceName, cl_vmix_camera_overlayNum->value.integer);

            vmix_lastActiveCameraSourceName = nullptr;
        }

        
        // Update web browser overlay
        if (true) {

            // http://web.com/userImages/photo.html#UUID|HWID|CAMERA_SOURCE|TEAM_NUM|NAME|DATA (#UUID123456|HWID123456|192.168.1.1|1|eyza|...|...|...)
            std::string webBrowserUrl = std::string(cl_vmix_webbrowser_url_prefix->value.string);

            webBrowserUrl += std::string(spectatedUserId) + "|" + spectatedHWID + "|";
            
            if (cl_vmix_cameraMode->value.integer == 2 && foundCameraSourceName != nullptr) {
                webBrowserUrl += std::string(foundCameraSourceName);
            }
            std::string modifiedUserName = spectatedUserName;
            std::replace(modifiedUserName.begin(), modifiedUserName.end(), '|', ' ');
            webBrowserUrl += std::string("|") + spectatedUserTeam + "|" + modifiedUserName + "|" + scrData;

            char encodedBuffer[1024];
            size_t encodedLength = HttpClient::url_encode(webBrowserUrl.c_str(), webBrowserUrl.length(), encodedBuffer, sizeof(encodedBuffer));
            webBrowserUrl = std::string(encodedBuffer, encodedLength);

            const char* inputSourceName = cl_vmix_webbrowser_sourceName->value.string;
            vmix_send_command_webbrowserNavigate(inputSourceName, webBrowserUrl.c_str());
        }


    } else {
        // VMix integration is disabled
        if (vmix_httpClient != nullptr) {

            // Hide cameras
            for (int i = 0; i < 10; ++i) {
                const char* sourceName = cl_vmix_player_sourceNames[i]->value.string;
                if (sourceName[0] != '\0') {
                    vmix_send_command_overlayInputOff(sourceName, cl_vmix_camera_overlayNum->value.integer);
                }
            }

            // Update web browser
            if (cl_vmix_webbrowser_sourceName->value.string[0] != '\0') {
                vmix_send_command_webbrowserNavigate(cl_vmix_webbrowser_sourceName->value.string, cl_vmix_webbrowser_url_prefix->value.string);
            }

            vmix_httpClient->poll_max(200); // Poll to finish any pending requests

            delete vmix_httpClient;
            vmix_httpClient = nullptr;
        }
    }
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void vmix_init() {
    cl_vmix =                       Dvar_RegisterBool  ("cl_vmix", false, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_debug =                 Dvar_RegisterBool  ("cl_vmix_debug", false, DVAR_CHANGEABLE_RESET);
    cl_vmix_scr_spectatedUserId =   Dvar_RegisterString("cl_vmix_scr_spectatedUserId", "", DVAR_CHANGEABLE_RESET); // Cvar controlled by script
    cl_vmix_scr_spectatedUserName = Dvar_RegisterString("cl_vmix_scr_spectatedUserName", "", DVAR_CHANGEABLE_RESET); // Cvar controlled by script
    cl_vmix_scr_spectatedUserTeam = Dvar_RegisterString("cl_vmix_scr_spectatedUserTeam", "", DVAR_CHANGEABLE_RESET); // Cvar controlled by script
    cl_vmix_scr_spectatedHWID =     Dvar_RegisterString("cl_vmix_scr_spectatedHWID", "", DVAR_CHANGEABLE_RESET); // Cvar controlled by script
    cl_vmix_scr_data =              Dvar_RegisterString("cl_vmix_scr_data", "", DVAR_CHANGEABLE_RESET); // Cvar controlled by script
    cl_vmix_playerIds =             Dvar_RegisterString("cl_vmix_scr_playerIds", "", DVAR_CHANGEABLE_RESET); // Cvar controlled by script
    cl_vmix_url =                   Dvar_RegisterString("cl_vmix_cfg_url", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_webbrowser_url_prefix = Dvar_RegisterString("cl_vmix_cfg_webbrowser_url_prefix", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_webbrowser_url_suffix = Dvar_RegisterString("cl_vmix_cfg_webbrowser_url_suffix", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_webbrowser_sourceName = Dvar_RegisterString("cl_vmix_cfg_webbrowser_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_camera_overlayNum =     Dvar_RegisterInt   ("cl_vmix_cfg_camera_overlayNum", 1, 1, 4, (dvarFlags_e)(DVAR_CHANGEABLE_RESET)); // overlay 1-4 to use in vMix
    cl_vmix_cameraMode =            Dvar_RegisterInt   ("cl_vmix_cfg_camera_mode", 1, 1, 2, (dvarFlags_e)(DVAR_CHANGEABLE_RESET)); // 1=source in vmix, 2=source send to browser
    cl_vmix_player_ids[0] =         Dvar_RegisterString("cl_vmix_cfg_camera_player1_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[0] = Dvar_RegisterString("cl_vmix_cfg_camera_player1_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[1] =         Dvar_RegisterString("cl_vmix_cfg_camera_player2_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[1] = Dvar_RegisterString("cl_vmix_cfg_camera_player2_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[2] =         Dvar_RegisterString("cl_vmix_cfg_camera_player3_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[2] = Dvar_RegisterString("cl_vmix_cfg_camera_player3_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[3] =         Dvar_RegisterString("cl_vmix_cfg_camera_player4_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[3] = Dvar_RegisterString("cl_vmix_cfg_camera_player4_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[4] =         Dvar_RegisterString("cl_vmix_cfg_camera_player5_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[4] = Dvar_RegisterString("cl_vmix_cfg_camera_player5_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[5] =         Dvar_RegisterString("cl_vmix_cfg_camera_player6_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[5] = Dvar_RegisterString("cl_vmix_cfg_camera_player6_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[6] =         Dvar_RegisterString("cl_vmix_cfg_camera_player7_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[6] = Dvar_RegisterString("cl_vmix_cfg_camera_player7_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[7] =         Dvar_RegisterString("cl_vmix_cfg_camera_player8_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[7] = Dvar_RegisterString("cl_vmix_cfg_camera_player8_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[8] =         Dvar_RegisterString("cl_vmix_cfg_camera_player9_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[8] = Dvar_RegisterString("cl_vmix_cfg_camera_player9_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_ids[9] =         Dvar_RegisterString("cl_vmix_cfg_camera_player10_id", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cl_vmix_player_sourceNames[9] = Dvar_RegisterString("cl_vmix_cfg_camera_player10_sourceName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void vmix_patch() {

}
