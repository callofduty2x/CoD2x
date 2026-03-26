@echo off
:: The first argument is the Windows path
set "WINPATH=%~1"
set "ARGS=%~2"

:: Convert the Windows path to a WSL path using wslpath:
:: We use a FOR /F loop to capture the output of the wslpath command.
for /f "usebackq tokens=*" %%i in (`wsl wslpath -u "%WINPATH%"`) do set "WSLPATH=%%i"
echo WSL path: %WSLPATH%
echo.

:: Create necessary directories and copy files to WSL
echo Copying files to WSL...
:: Create the base directory ~/CoD2x if it doesn't exist
wsl bash -c "mkdir -p ~/CoD2x"
wsl bash -c "cp -rv \"%WSLPATH%/bin/linux/cod2_lnxded\" ~/CoD2x/"
wsl bash -c "cp -rv \"%WSLPATH%/bin/linux/libCoD2x.so\" ~/CoD2x/"
:: Check if ~/CoD2x/main exists; if not, create it and copy files from the Windows "main" directory
wsl bash -c "if [ ! -d ~/CoD2x/main ]; then echo 'Directory ~/CoD2x/main does not exist, copying...'; mkdir -p ~/CoD2x/main && cp -ru \"%WSLPATH%/bin/windows/main\"/* ~/CoD2x/main && echo 'Files copied successfully.'; else echo 'Directory ~/CoD2x/main already exists, skipping.'; fi"

:: Sync .IWD files, so the content is the same, (copy only if they differ or do not exist)
echo Synchronizing files from %WSLPATH%/bin/windows/main/ to ~/CoD2x/main/...
wsl bash -c "rsync -av --delete --include='*.iwd' --include='*.gsc' --exclude='*' \"%WSLPATH%/bin/windows/main/\" ~/CoD2x/main/"

:: Update launch_args_linux.cfg
wsl bash -c "cp -v \"%WSLPATH%/bin/windows/main/launch_args_linux.cfg\" ~/CoD2x/main/"


:: Linux saves server settings into special folder ~/.callofduty2/
:: It ignores any players/ folder in the main directory

:: Remove ~/CoD2x/main/players/ folder
echo Removing ~/CoD2x/main/players/ folder...
wsl bash -c "rm -rf ~/CoD2x/main/players/"

:: Copy config_mp.cfg to ~/.callofduty2/main/config_mp_server.cfg
echo Updating config_mp_server.cfg...
wsl bash -c "cp -v \"%WSLPATH%/bin/windows/main/players/default/config_mp.cfg\" ~/.callofduty2/main/config_mp_server.cfg"

echo.
wsl bash -c "cd ~/CoD2x && echo Files in && pwd"
wsl bash -c "cd ~/CoD2x && find ."
echo.
wsl bash -c "cd ~/.callofduty2 && echo Files in && pwd"
wsl bash -c "cd ~/.callofduty2 && find ."
echo.

:: Mirroring network mode allow the WSL2 instance to access the same network as the host machine
:: Its required to allow CoD2 server to be accessed from the network correctly
echo Setting up network mirroring mode...
set WSL_CONFIG=%USERPROFILE%\.wslconfig

:: Check if .wslconfig already exists
if exist "%WSL_CONFIG%" (
    echo %WSL_CONFIG% already exists at. No changes made.
) else (
    :: Create .wslconfig with mirrored networking mode
    echo Creating .wslconfig at %WSL_CONFIG%
    (
        echo [wsl2]
        echo networkingMode=mirrored
    ) > "%WSL_CONFIG%"

    :: Shut down WSL to apply changes
    echo Shutting down WSL to apply configuration...
    wsl --shutdown
)

:: Print WSL IP address
echo.
echo WSL IP address:
wsl hostname -I
echo.

:: Terminate any existing gdbserver and tmux sessions to avoid conflicts
echo Terminating any existing gdbserver instances...
wsl pkill gdbserver

echo Terminating any existing tmux sessions...
wsl tmux kill-session -t gdbsession 2>nul
wsl tmux kill-session -t appsession 2>nul

:: Start gdbserver to launch the app in a tmux session named 'gdbsession' with valid window size
echo.
echo Starting gdbserver in tmux session 'gdbsession'...
set cmd="cd ~/CoD2x && LD_PRELOAD=~/CoD2x/libCoD2x.so gdbserver :1234 ./cod2_lnxded \"%ARGS%\"; bash"
echo %cmd%
wsl tmux new-session -d -s gdbsession %cmd%

:: Open a new Windows Terminal window and attach to the 'gdbsession' tmux session to view and interact with the app's console
echo Opening a new terminal window to interact with the application's console...
wt.exe wsl tmux attach-session -t gdbsession