# CoD2x
![alt text](images/cod2-window.png)

CoD2x is a modification of Call of Duty 2. It is build on top of 1.3 version.
It focuses on fixing bugs and adding new features to the game.

⚠️ Disclaimer
- The CoD2x patch is a community-made, unofficial modification for Call of Duty 2.
- It is provided free of charge, “as is”, without any warranty of functionality or support.
- You use it entirely at your own risk. The authors are not affiliated with Activision or Infinity Ward and take no responsibility for any issues or damages resulting from its use.
- To use this patch, you must own a legitimate, original copy of Call of Duty 2 purchased from an official distributor.
- This software may be terminated or withdrawn at any time

# Version history
- 2026-03-25 - **[1.4.6.6](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.6.6)** - **current**

<details>
  <summary>Older versions</summary>

- 2026-03-24: **[1.4.6.5](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.6.5)**
- 2026-02-22: **[1.4.6.4](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.6.4)**
- 2025-11-18: **[1.4.6.3](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.6.3)**
- 2025-11-06: **[1.4.6.2](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.6.2)**
- 2025-11-04: **[1.4.6.1](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.6.1)**
- 2025-07-25: **[1.4.4.6](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.4.6)**
- 2025-07-12: **[1.4.4.5](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.4.5)**
- 2025-07-04: **[1.4.4.4](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.4.4)**
- 2025-06-27: **[1.4.4.3](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.4.3)**
- 2025-06-23: **[1.4.4.2](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.4.2)**
- 2025-06-20: **[1.4.4.1](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.4.1)**
- 2025-06-08: **[1.4.3.5](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.3.5)**
- 2025-06-06: **[1.4.3.4](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.3.4)**
- 2025-06-06: **[1.4.3.3](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.3.3)**
- 2025-06-02: **[1.4.3.2](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.3.2)**
- 2025-05-25: **[1.4.3.1](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.3.1)**
- 2025-03-06: **[1.4.2.3](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.2.3)**
- 2025-03-02: **[1.4.2.2](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.2.2)**
- 2025-01-23: **[1.4.2.1](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.2.1)**
- 2025-01-23: **[1.4.1.1](https://github.com/callofduty2x/CoD2x/releases/tag/v1.4.1.1)**

</details>


# Features

#### Changes
- Dialog "Run in safe mode?" is removed
- Dialog "Set Optimal Settings?" and "Recommended Settings Update" is removed
- New text in the console to indicate that CoD2x is loaded
- Changed auto-update server with ability to download the latest version of CoD2x
  - ![alt text](images/cod2-auto-update.png)
- Ability to connect both original 1.3 servers and new 1.4 servers
- New server errors descriptions when non-compatible clients tried to connect to the server
  - ![alt text](images/cod2-different-version-error.png)
- Fixed black screen on startup - caused by missing microphone / sound device
- Added requirement to run the game as administrator to fix problems with VirtualStore folder and the need to have write access into Program Files folder (as it was possible in Windows XP)
  - ![alt text](images/cod2-run-as-admin.png)
- Fixed "+smoke" bug
- Animations - https://www.youtube.com/watch?v=oQkTuwQnI-8
  - Reworked the player character animations
    - Stance animation
      - crouch-prone peak
      - crouch-stand peak
      - prone-crouch peak
        - ![alt text](images/cod2-stance-fix.gif)
        - ![alt text](images/cod2-clip-fix.gif)
    - No leg swing
    - Nade throwing animation is canceled imidiately when the player crouches / prones
    - Head movement on ladder - head is rotated according to player's yaw angle
    - Prone high body fixed
    - Weapon stabilization
      - Torso rotation now match the player's yaw angle
      - It fixes weapon "waving"
      - It fixes diagonal bug
- Fixed "iwd/sum name mismatch" error caused by too many iwd files
- Fixed negative protocol number when formating IP and port
- Disabled console prints:
  - localized string warning
  - Bot warning prints
  - "==== ShutdownGame ===="
  - "gamename: Call of Duty 2"
  - "gamedate: Jun 23 2006"
- Smaller font size of text
  - When downloading IWD file
  - Text "RECORDING" in the left bottom corner when recording demo
- Behavior tweaks: automatically set `developer 2` and `sv_cheats 1` when playing demos
- Increased number of visible cvars in console
- Increased dvar limit from 1280 to 4096
- DDoS protection via rate limiter for incoming UDP packets
- Shogun consistent pellet spread
  - ![alt text](images/pellet_spread.png)
  - changeable via new cvar `g_shotgun_spread_fix`

#### Improvements
- Added windowed and borderless window mode:
    - windowed mode: `r_fullscreen 0`
    - borderless mode: `r_fullscreen 0` and `r_mode [screen resolution]`
    - fullscreen mode: `r_fullscreen 1` (default)
- Added support for rinput (raw input for mouse movement) 
    - ![alt text](images/cod2-rinput.png)
    - `m_rinput 0` - turned off, uses Windows cursor possition to get movement (default)
    - `m_rinput 1` - raw mouse movement, dedicated thread, as external Rinput application
    - `m_rinput 2` - raw mouse movement, integrated directly into the game
    - The game is also measuring the refresh rate of the mouse:
        - ![alt text](images/cod2-rinput-hz.png)
        - `m_rinput_hz` - shows the current refresh rate of your mouse in real time
        - `m_rinput_max` - shows the maximum measured refresh rate
    - If the external Rinput application is used, the internal `m_rinput` is automatically turned off
- Added possibility to restrict FPS via mod (like zPAM) into range 125 - 250 using new cvar `com_maxfps_limit` (the cvar is cheat protected and can be set only by the server)
  - ![alt text](images/cod2-com-max-fps.png)
- Ignoring custom IWD mods on game start to avoid runtime errors (only files starting with 'iw_' or 'localized_' are loaded)
- Set sv_cheats 1 on disconnect to allow to play demo without the need to do devmap
- Improved error message `recursive error after: Can´t create Dvar 'xxx': 1280 dvars already exist`:
    - ![alt text](images/cod2-error-dvars-1280.png)
- Added informative texts into console with changes that has been applied when connected to the server:
    - ![alt text](images/cod2-changes-console.png)
- Added crash reporter
  - ![alt text](images/cod2-crash-error.png)
- Added process freeze detection
  - ![alt text](images/cod2-freeze-error.png)
  - It can be disabled by setting `/com_freezeWatch 0`
- Tickrate
  - ![alt text](images/cod2-tick-wireshark.png)
  - Server FPS maximum increased from 30 to 40 (`sv_fps 40`)
  - Client max snaps increased from 30 to 40 (`snaps 40`)
  - Client max packet value increased from 100 to 125 (`cl_maxpackets 125`)
    - This allows to send exactly 125 packets at 250FPS (instead of ~83.3)
- HWID
  - An unique hardware ID is generated for each computer
  - The HWID is used to ban cheaters on the server via commands `/banClient <clientId>` and `/unbanAll` (bans are saved in file `main\ban.txt`)
  - The HWID is replacing GUID that was generated by PunkBuster, which is no longer supported
  - The HWID is an non-zero unsigned 32-bit integer, like `560978975`
  - You can see your HWID in the game by checking cvar `/cl_hwid`
  - ![alt text](images/cod2-hwid.png)
- Added possibility to change the master via cvars
  - client: `cl_masterServer`, `cl_masterPort`
  - server: `sv_master1`, `sv_master2`, `sv_master3`
    - `sv_master1` is forced to be `cod2master.activision.com`
    - `sv_master2` is forced to be `master.cod2x.me`
    - `sv_master3` is customizable
- New cvar `sv_cracked` to make the server accept players with invalid CDKEY
- New cvar `showPacketStrings` to show network communication data
- New commands `/increase` and `/decrease` to increase or decrease the value of a cvar
- Reversed function for third person, added new cvar `cg_thirdPersonMode 1` that rotates directly around player without collision with the world
- Added possibility to set CPU affinity
  - `com_affinity -1` (default) - use maximum available CPU cores
  - `com_affinity 0` - disable automatically setting CPU affinity
  - `com_affinity <number>` - set specified number of cores (e.g. `com_affinity 3` will use first 3 cores)
- Fixed sounds of invisible players caused by PVS
- Changeable LODs (Level of Detail) for models
  - `r_lodScale <value>`
    - `r_lodScale 1` (default) - original LODs
    - `r_lodScale 0.5` - better model detail in distance
    - `r_lodScale 0` - best model detail in distance
- Disabled VirtualStore folder redirection by checking for privileges
- New GSC functions - list in [GSC functions](#gsc-functions)
- Match system
- Automatic demo uploading
- Detection of uninitialized sound system
- Radar
- vMIX support
- URL protocol to launch the game from web links: `cod2x://`
- Toggle killfeed custom color rendering (`con_printDoubleColors`);
- Bullet trace debugging (`cg_debugBullets`);
- Automatic zPAM updates
- Smarter IWD handling and configs: always use `main/config_mp.cfg`; improved filtering to prevent sum/name mismatch; for demos only IWDs used at record-time are loaded; for listen servers only the latest zPAM files are loaded; assets in `movie` are included for demo playback; automatic extraction of `iw_CoD2x_01.iwd`.
- Cvar to disable saving changes to config via cvar `com_writeConfig`


# GSC functions
### Level
```markdown
- `http_fetch` - Fetches data from an HTTP endpoint asynchronously. Allows specifying HTTP method, data, headers, and callbacks for success or error handling.

- `websocket_connect` - Establishes a WebSocket connection to a specified URL with optional headers and callbacks for connection, message, close, and error events.
- `websocket_sendText` - Sends a text message over an active WebSocket connection.
- `websocket_close` - Closes an active WebSocket connection by its connection ID.

- `matchUploadData` - Uploads match-related data to the server with optional callbacks for success or error handling.
- `matchSetData` - Sets global match data using key-value pairs.
- `matchGetData` - Retrieves global match data for a specified key.
- `matchRedownloadData` - Redownloads match-related data from the server.
- `matchClearData` - Clears all match-related progress data.
- `matchIsActivated` - Checks if the match system is currently activated.
- `matchCancel` - Cancels the ongoing match with an optional reason.
- `matchFinish` - Marks the match as finished, kicking all players and performing a fast restart.
```

### Player
```markdown
- `getIp` - Retrieves the IP address of the player.
- `getHWID` - Retrieves the hardware ID (HWID2) of the player, used for unique identification.
- `getCDKeyHash` - Retrieves the MD5 hash of the player's CD key, which is sent during connection.
- `getAuthorizationStatus` - Retrieves the authorization status of the player's CD key, such as validity or ban status.

- `getViewOrigin` - Retrieves the player's current view origin as a 3D vector.
- `getStance` - Retrieves the player's current stance (e.g., stand, crouch, prone).
- `isUsingTurret` - Checks if the player is currently using a turret.

- `matchPlayerGetData` - Retrieves match-related data for a player using a specified key.
- `matchPlayerSetData` - Sets match-related data for a player using key-value pairs.
- `matchPlayerIsAllowed` - Checks if a player is allowed to join the match based on their credentials.
```

# How to install (client on Windows)
1. You need original Call of Duty 2 with version [1.3](https://www.moddb.com/games/call-of-duty-2/downloads/call-of-duty-2-pc-patch-v-13) installed.
2. Download latest version of CoD2x - [CoD2x_1.4.6.6_windows.zip](https://github.com/eyza-cod2/CoD2x/releases/download/v1.4.6.6/CoD2x_1.4.6.6_windows.zip)
3. Extract these files from the archive to the Call of Duty 2 folder, replacing any existing file:
    - 📄 mss32.dll
    - 📄 mss32_original.dll
4. Final structure should look like this:
    - 📁 Call of Duty 2
        - 📁 Docs
        - 📁 main
        - 📁 miles
        - 📁 pb
        - 📄 CoD2MP_s.exe
        - 📄 CoD2SP_s.exe
        - 📄 gfx_d3d_mp_x86_s.dll
        - 📄 gfx_d3d_x86_s.dll
        - 📄 **mss32.dll**
        - 📄 **mss32_original.dll**
        - 📄 ... (other files)
5. The archive also contains additional files that are not required to be installed:
    - 📄 CoD2x Installation and uninstallation manual.txt


# How to uninstall (client on Windows)
1. Delete the following files:
    - 📄 mss32.dll
2. Rename following file:
    - 📄 mss32_original.dll  ->  📄 mss32.dll


# How to install (server on Linux)
1. Download latest version of CoD2x - [CoD2x_1.4.6.6_linux.zip](https://github.com/eyza-cod2/CoD2x/releases/download/v1.4.6.6/CoD2x_1.4.6.6_linux.zip)
2. Extract this file from the archive to the Call of Duty 2 folder:
    - 📄 libCoD2x.so
3. Final structure should look like this:
    - 📁 Call of Duty 2
        - 📁 main
        - 📁 pb
        - 📄 cod2_lnxded
        - 📄 libCoD2x.so
4. The archive also contains additional files that are not required to be installed:
    - 📄 CoD2x Installation and uninstallation manual.txt
    - 📄 cod2_lnxded    (official 1.3 game version)
5. Update chmod for the libCoD2x.so file to be executable:
    - `chmod +x libCoD2x.so`
6. Run the game with LD_PRELOAD, for example:
    - `LD_PRELOAD=libCoD2x.so ./cod2_lnxded +exec server.cfg`
    - 🛈 Its not common that server providers support adding this, only server administrators can do that.
7. Auto-update is enabled by default. Make sure to enable UDP port 20720. If you want to disable auto-update, set `sv_update "0"` or disable UDP port 20720. On server start, the server will check for the latest version of CoD2x and download it if available. File libCoD2x.so will be replaced with the latest version. Process restart is needed to apply the update. 


# Administrator rights
Since version 1.4.4.6, the game does not require administrator rights to run if the game is located in folder outside of Program Files.

If the game is located in Program Files, you need to run the game as administrator to avoid problems with VirtualStore folder.

The VirtualStore folder is a feature introduced in Windows Vista and later to provide compatibility for legacy applications that try to write files to protected system locations, such as `C:\Program Files`. When a program without administrator rights attempts to write to these folders, Windows silently redirects the write operations to a user-specific location.

This allows older applications to run without errors, but it can cause confusion because files may not be where the application or user expects them to be. For example, config files and IWD files might end up in the VirtualStore folder instead of the actual game directory.

Possible locations for the game:
- `C:\Program Files\Activision\Call of Duty 2`
- `C:\Program Files (x86)\Activision\Call of Duty 2`
- `C:\Users\<username>\AppData\Local\VirtualStore\Program Files\Activision\Call of Duty 2`
- `C:\Users\<username>\AppData\Local\VirtualStore\Program Files (x86)\Activision\Call of Duty 2`

Possible locations for the game in registry:
- `HKEY_LOCAL_MACHINE\SOFTWARE\Activision\Call of Duty 2`
- `HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Activision\Call of Duty 2`
- `HKEY_CURRENT_USER\SOFTWARE\Classes\VirtualStore\MACHINE\SOFTWARE\Activision\Call of Duty 2`
- `HKEY_CURRENT_USER\SOFTWARE\Classes\VirtualStore\MACHINE\SOFTWARE\Wow6432Node\Activision\Call of Duty 2`

In CoD2x, we check for write access to the game folder on startup. If the game is located in Program Files and the user does not have write access, we show a error message box:
![cod2-write-access-error](images/cod2-write-access-error.png)

Running the game as administrator will fix this problem, but **it decreases the security of the system**. Thats why we recommend to move the game to a different folder, such as `C:\Games\Call of Duty 2` and uncheck the "Run as administrator" option and compatibility mode in the properties of the game executable, if possible.

Why its recommended to uncheck also compatibility mode?
Compatibility mode also runs the game with administrator rights.

Try to run the game without administrator and compatibility mode first, and if it works without problems, you can leave it that way. If you encounter problems with the game, you can try to run the game with compatibility mode, but you decrease the security of your system.

In registry, when the game runs with administrator rights, we are moving old values from VirtualStore to the original location, so the game always uses the latest saved values.



# How to install for developers
1. Clone this repository.
2. Copy .iwd files from original Call of Duty 2 1.3 main folder to `./bin/windows/main` folder in this repository (iw_01..iw_15 and localized_english_iw00..localized_english_iw11)
3. Instal MinGW-w64 by Brecht Sanders [winlibs-i686-posix-dwarf-gcc-14.2.0-mingw-w64msvcrt-12.0.0-r2](https://github.com/brechtsanders/winlibs_mingw/releases/download/14.2.0posix-19.1.1-12.0.0-msvcrt-r2/winlibs-i686-posix-dwarf-gcc-14.2.0-mingw-w64msvcrt-12.0.0-r2.zip) into `./tools/mingw`

4. Install WSL (Windows Subsystem for Linux)
  - `wsl --install Ubuntu-18.04`


5. Run these commands in WSL:
    - `wsl --install` (should install 'Ubuntu' instance name)
    - `sudo apt update`
    - `sudo dpkg --add-architecture i386`
    - `sudo apt update`
    - `sudo apt install -y make ninja-build gcc gdb gdbserver:i386 build-essential`
    - `sudo apt install -y libc6:i386 libstdc++5:i386 libgcc1:i386` (runtime libraries to run original CoD2) 
    - `sudo apt install -y libssl-dev:i386` (development libraries for OpenSSL)
    - `sudo apt install -y gcc-multilib libc6-dev:i386` (development libraries to compile new code)
    - `sudo apt-get install gcc-multilib g++-multilib` (some dependencies for compiling)
    - Install cmake >=3.25.2 (on Ubuntu 18.04 cmake version is 3.10, we need that for new options):
    - `sudo apt install -y software-properties-common lsb-release ca-certificates gnupg wget`
    - `wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | sudo apt-key add -`
    - `sudo apt-add-repository "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main"`
    - `sudo apt update`
    - `sudo apt install -y cmake`
    - `exit`
    - `wsl --set-default Ubuntu-18.04`
    - |
    - Setup tmux: (enable mouse scrolling)
    - `nano ~/.tmux.conf`
      - write `set -g mouse on`
      - Ctrl+X, Y, Enter
    - `tmux source-file ~/.tmux.conf`
    - |
    - How to get IP of WSL virtual machine to connect from Windows to Linux CoD2x server: 
    - `ip addr show eth0 | grep inet | awk '{ print $2; }' | sed 's/\/.*$//'`
6. Run VSCode as administrator (:warning: needed to be able to run CoD2 also as administrator while debugging)
7. Use VSCode to compile, run and debug

# Repository layout
- 📁 **bin**
    - 📁 **linux** - *Linux server binaries*
    - 📁 **windows** - *Windows game binaries from original CD with 1.3 patch applied*
- 📁 **src**
    - 📁 **linux** - *code related only for Linux server*
    - 📁 **mss32** - *code related for Windows, mimicking mss32.dll*
    - 📁 **shared** - *code shared for both Linux server and Windows version*
    - 📁 **other** - *reversed / testing code*
- 📁 **tools** - *contains external tools for coding, compiling, etc..*
- 📁 **zip** - *zip files are generated here*

To avoid source code duplication, the code that is used for both Linux and Windows is placed in `📁src / 📁shared` folder. Its primarily a server side code. 

The code that is related only for Windows version or Linux server is placed in particular folders `📁src / 📁mss32` or `📁src / 📁linux`, both sharing the shared folder.



# How the Windows mss32.dll works
CoD2MP_s.exe has following dynamic libraries:
- WINMM.dll       
- WSOCK32.dll     
- mss32.dll       <-- *hooking this library*
- d3d9.dll        
- DSOUND.dll      
- KERNEL32.dll    
- USER32.dll      
- GDI32.dll       
- ADVAPI32.dll    
- SHELL32.dll     

These libraries are loaded at runtime when the CoD2MP_s.exe is started.

File mss32.dll is a Miles Sound System dynamic link library used for audio playback.
This file is replaced with our custom mss32.dll, which exports the same functions as the original mss32.dll.

The CoD2MP_s.exe imports 364 functions from original mss32.dll:
- _AIL_set_sample_adpcm_block_size@8
- _AIL_enumerate_3D_providers@12
- _AIL_end_sample@4
- _AIL_set_3D_position@16
- ...

Our version of mss32.dll has the same exported functions as original.
These functions act as a proxy to the original functions in the original mss32.dll.
    
    [CoD2MP_s.exe]
        ↓
    [mss32.dll]            <- this file
        ↓
    [mss32_original.dll]   <- original mss32.dll

When our mss32.dll is loaded, it loads mss32_original.dll and redirect all exported functions.
It also runs patching process that modifies the game memory to fix some bugs and add new features.


# References
- [CoD2rev_Server](https://github.com/voron00/CoD2rev_Server)
- [CoD4x_Server](https://github.com/callofduty4x/CoD4x_Server)
- [zk_libcod](https://github.com/ibuddieat/zk_libcod)
- [Enemy-Territory](https://github.com/id-Software/Enemy-Territory)


# Logo
![alt text](images/logo.png)