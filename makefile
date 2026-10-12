# ========================================================================================================
# Version
# ========================================================================================================
# Uses a.b.c.d versioning style using this sequence:
#      a - major       - always 1
#      b - minor       - always 4
#      c - protocol    - CoD2x breaking change that affects both client and server
#      d - patch       - CoD2x non-breaking change to client or server that is backward compatible
# Sequence:
#      1.3
#      1.4.1.1         - first release
#      1.4.1.2         - patch to client or server, backward compatible
#      1.4.1.3-test.1  - test version
#      1.4.1.3-test.2
#      1.4.1.3
#      1.4.2.1         - new protocol, breaking change, old client can not connect new server
#      1.4.2.2
#      ...

# CoD2 version, increased from 1.3 to 1.4
VERSION_MAJOR = 1
VERSION_MINOR = 4

# CoD2x protocol version
# Should be increased only if the changed functionalities that affect both server and client
# Newer client can connect older server
# Older client can not connect newer server
VERSION_PROTOCOL = 6

# CoD2x patch version
# Should be increased when new version is released and the changes are backward compatible
VERSION_PATCH = 8

# CoD2x test version (e.g. -test.1)
VERSION_TEST =
VERSION_IS_TEST = 0

# Full version string
# Example "1.4.1.1"  or  "1.4.1.1-test.1"
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PROTOCOL).$(VERSION_PATCH)$(VERSION_TEST)
VERSION_COMMA = $(VERSION_MAJOR),$(VERSION_MINOR),$(VERSION_PROTOCOL),$(VERSION_PATCH)


# ========================================================================================================
# Directories and Files
# ========================================================================================================
DEBUG ?= 1

ifeq ($(DEBUG),1)
	BUILD_TYPE = Debug
else
	BUILD_TYPE = Release
endif

CMAKE = cmake

# Windows settings
WIN_BUILD_DIR = build\win-$(BUILD_TYPE)
WIN_GAME_DIR  = bin\windows
WIN_GENERATOR = "Ninja"
WIN_TARGET    = mss32

# Linux settings
LINUX_BUILD_DIR = build/linux-$(BUILD_TYPE)
LINUX_GAME_DIR  = bin/linux
LINUX_GENERATOR = "Ninja"
LINUX_TARGET    = linux



# ========================================================================================================
# Clean all
# ========================================================================================================
.PHONY: clean

clean: clean_win clean_linux



# =========================================
# Windows targets
# =========================================
.PHONY: rebuild_win build_win configure_win clean_win

rebuild_win: clean_win build_win

configure_win:
	@echo ">> Configuring Windows $(BUILD_TYPE) build...";
	$(CMAKE) -S . -B $(WIN_BUILD_DIR) -G $(WIN_GENERATOR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build_win: prebuild configure_win
	$(CMAKE) --build $(WIN_BUILD_DIR) --target $(WIN_TARGET) --parallel
ifeq ($(BUILD_TYPE),Release)
	@echo Re-building with accurate mss32 symbols...
	$(CMAKE) --build $(WIN_BUILD_DIR) --target $(WIN_TARGET) --parallel
endif

	@echo Renaming mss32.build.dll to mss32.dll + deleting old versions...
	@del /Q "$(WIN_GAME_DIR)\mss32.dll*" >nul 2>&1
	@del /Q "$(WIN_GAME_DIR)\mss32_hotreload_*.dll" >nul 2>&1
	@cmd /Q /C "cd "$(WIN_GAME_DIR)" && for /L %i in (1,1,10) do if not exist mss32.dll.%i.old (ren mss32.dll mss32.dll.%i.old >nul 2>&1 & goto :done) & :done"
	@move /Y "$(WIN_GAME_DIR)\mss32.build.dll" "$(WIN_GAME_DIR)\mss32.dll" >nul 2>&1
	@echo Done.
	@echo.

clean_win:
ifeq ($(OS),Windows_NT)
	@if exist "build/win-Release" rmdir /S /Q "build/win-Release"
	@if exist "build/win-Debug" rmdir /S /Q "build/win-Debug"
else
	@rm -rf build/win-Release build/win-Debug
endif


# =========================================
# Linux targets
# =========================================
.PHONY: rebuild_linux build_linux configure_linux clean_linux

rebuild_linux: clean build_linux

configure_linux:
	@echo ">> Configuring Linux $(BUILD_TYPE) build...";
	$(CMAKE) -S . -B $(LINUX_BUILD_DIR) -G $(LINUX_GENERATOR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build_linux: prebuild configure_linux
	@echo ">> Building Linux $(BUILD_TYPE)...";
	$(CMAKE) --build $(LINUX_BUILD_DIR) --target $(LINUX_TARGET) --parallel

clean_linux:
ifeq ($(OS),Windows_NT)
	@if exist "build/linux-Release" rmdir /S /Q "build/linux-Release"
	@if exist "build/linux-Debug" rmdir /S /Q "build/linux-Debug"
else
	@rm -rf build/linux-Release build/linux-Debug
endif




# ========================================================================================================
# Prebuild
# ========================================================================================================
.PHONY: prebuild

prebuild: src/shared/version.h src/mss32/version.rc
	@echo "Current working directory: $(CURDIR)"


ifeq ($(OS),Windows_NT)
    ECHO_QUOTE =
    ECHO_QUOTE_ESCAPED = "
else
    ECHO_QUOTE = "
	ECHO_QUOTE_ESCAPED = \"
endif

# Rule to generate version.h every time the makefile changes
VERSION_H = src/shared/version.h
src/shared/version.h: makefile
	@echo Generating version.h...

	@echo $(ECHO_QUOTE)#define APP_VERSION_MAJOR $(VERSION_MAJOR)$(ECHO_QUOTE) 										>  $(VERSION_H)
	@echo $(ECHO_QUOTE)#define APP_VERSION_MINOR $(VERSION_MINOR)$(ECHO_QUOTE) 										>> $(VERSION_H)
	@echo $(ECHO_QUOTE)#define APP_VERSION_PROTOCOL $(VERSION_PROTOCOL)$(ECHO_QUOTE) 								>> $(VERSION_H)
	@echo $(ECHO_QUOTE)#define APP_VERSION_PATCH $(VERSION_PATCH)$(ECHO_QUOTE) 										>> $(VERSION_H)
	@echo $(ECHO_QUOTE)#define APP_VERSION_IS_TEST $(VERSION_IS_TEST)$(ECHO_QUOTE) 									>> $(VERSION_H)
	@echo $(ECHO_QUOTE)#define APP_VERSION $(ECHO_QUOTE_ESCAPED)$(VERSION)$(ECHO_QUOTE_ESCAPED)$(ECHO_QUOTE) 		>> $(VERSION_H)
	
	@echo $(VERSION) writed to $(VERSION_H)
	@echo   Done.

ifeq ($(OS),Windows_NT)
# Generate version.rc file if makefile has changed
VERSION_RC = src/mss32/version.rc
src/mss32/version.rc: makefile
	@echo Generating version.rc...

	@echo 1 VERSIONINFO                                			>  $(VERSION_RC)
	@echo FILEVERSION $(VERSION_COMMA)                			>> $(VERSION_RC)
	@echo PRODUCTVERSION $(VERSION_COMMA)             			>> $(VERSION_RC)
	@echo BEGIN                                       			>> $(VERSION_RC)
	@echo     BLOCK "StringFileInfo"                  			>> $(VERSION_RC)
	@echo     BEGIN                                   			>> $(VERSION_RC)
	@echo         BLOCK "040904b0"                    			>> $(VERSION_RC)
	@echo         BEGIN                               			>> $(VERSION_RC)
	@echo             VALUE "ProductName", "CoD2x"    			>> $(VERSION_RC)
	@echo             VALUE "ProductVersion", "$(VERSION)" 		>> $(VERSION_RC)
	@echo         END                                 			>> $(VERSION_RC)
	@echo     END                                     			>> $(VERSION_RC)
	@echo     BLOCK "VarFileInfo"                     			>> $(VERSION_RC)
	@echo     BEGIN                                   			>> $(VERSION_RC)
	@echo         VALUE "Translation", 0x0409, 1200  			>> $(VERSION_RC)
	@echo     END                                     			>> $(VERSION_RC)
	@echo END                                         			>> $(VERSION_RC)

	@echo   Done.
	@echo.
endif


# ========================================================================================================
# Create .cpp and .h template
# ========================================================================================================
# Depend on the files to not overwrite existing files
template: $(DIR)/$(CLASSNAME).h $(DIR)/$(CLASSNAME).cpp

$(DIR)/$(CLASSNAME).h $(DIR)/$(CLASSNAME).cpp: src/_template.h.in src/_template.cpp.in
	@powershell -Command "Get-Content src/_template.h.in | ForEach-Object { $$_ -replace '@CLASSNAME@','$(CLASSNAME)' }" > $(DIR)/$(CLASSNAME).h
	@powershell -Command "Get-Content src/_template.cpp.in | ForEach-Object { $$_ -replace '@CLASSNAME@','$(CLASSNAME)' }" > $(DIR)/$(CLASSNAME).cpp
	@echo Created $(DIR)/$(CLASSNAME).h and $(DIR)/$(CLASSNAME).cpp
	
	@code --reuse-window $(DIR)/$(CLASSNAME).cpp


# ========================================================================================================
# Ziping
# ========================================================================================================
.PHONY: zip_all
zip_all: zip_win zip_linux

ZIP_EXEC = 7za.exe


# ========================================================================================================
# Zip windows files
# ========================================================================================================
.PHONY: zip_win zip_win_clean

# Define source files and target paths
ZIP_WIN_DIR = zip/windows
ZIP_WIN_FILES = bin/windows/mss32.dll bin/windows/mss32_original.dll
ZIP_WIN_NAME = CoD2x_$(VERSION)_windows.zip
ZIP_WIN_OUTPUT = $(ZIP_WIN_DIR)/$(ZIP_WIN_NAME)

# Rule to create the zip file
$(ZIP_WIN_OUTPUT): $(ZIP_WIN_FILES) | $(ZIP_WIN_DIR)
	@echo "Copying files to $(subst /,\, $(ZIP_WIN_DIR))..."
	@for %%f in ($(subst /,\, $(ZIP_WIN_FILES))) do copy %%f $(subst /,\, $(ZIP_WIN_DIR)) >nul
	@echo "Creating zip archive $(subst /,\, $(ZIP_WIN_OUTPUT)) using 7-Zip..."
	@echo $(ZIP_EXEC) a -tzip "$(subst /,\, $(ZIP_WIN_OUTPUT))" "$(subst /,\, $(ZIP_WIN_DIR))\*"
	@cd $(subst /,\, $(ZIP_WIN_DIR)) && $(ZIP_EXEC) a -tzip "$(ZIP_WIN_NAME)" *
	@echo "Zip archive created at $(subst /,\, $(ZIP_WIN_OUTPUT))."

# Rule to ensure the zip directory exists
$(ZIP_WIN_DIR):
	@echo "Creating directory $(ZIP_WIN_DIR)..."
	@mkdir -p $(ZIP_WIN_DIR)

zip_win: zip_win_clean $(ZIP_WIN_OUTPUT)

zip_win_clean:
	@echo "Cleaning up $(subst /,\, $(ZIP_WIN_DIR)), preserving manual..."
	@if exist $(subst /,\, $(ZIP_WIN_DIR)) ( \
	    for %%f in ($(subst /,\, $(ZIP_WIN_DIR))\*) do ( \
	        if /I not "%%~nxf"=="CoD2x Installation and uninstallation manual.txt" del /Q "%%f" \
	    ) \
	)


# ========================================================================================================
# Zip Linux files
# ========================================================================================================
.PHONY: zip_linux zip_linux_clean

ZIP_LINUX_DIR = zip/linux
ZIP_LINUX_FILES = bin/linux/libCoD2x.so bin/linux/cod2_lnxded
ZIP_LINUX_NAME = CoD2x_$(VERSION)_linux.zip
ZIP_LINUX_OUTPUT = $(ZIP_LINUX_DIR)/$(ZIP_LINUX_NAME)

# Rule to create the Linux zip file
$(ZIP_LINUX_OUTPUT): $(ZIP_LINUX_FILES) | $(ZIP_LINUX_DIR)
	@echo "Copying files to $(subst /,\, $(ZIP_LINUX_DIR))..."
	@for %%f in ($(subst /,\, $(ZIP_LINUX_FILES))) do copy %%f $(subst /,\, $(ZIP_LINUX_DIR)) >nul
	@echo "Creating zip archive $(subst /,\, $(ZIP_LINUX_OUTPUT)) using 7-Zip..."
	@echo $(ZIP_EXEC) a -tzip "$(subst /,\, $(ZIP_LINUX_OUTPUT))" "$(subst /,\, $(ZIP_LINUX_DIR))\*"
	@cd $(subst /,\, $(ZIP_LINUX_DIR)) && $(ZIP_EXEC) a -tzip "$(ZIP_LINUX_NAME)" *
	@echo "Zip archive created at $(subst /,\, $(ZIP_LINUX_OUTPUT))."

# Rule to ensure the Linux zip directory exists
$(ZIP_LINUX_DIR):
	@echo "Creating directory $(ZIP_LINUX_DIR)..."
	@mkdir -p $(ZIP_LINUX_DIR)

zip_linux: zip_linux_clean $(ZIP_LINUX_OUTPUT)

zip_linux_clean:
	@echo "Cleaning up $(subst /,\, $(ZIP_LINUX_DIR)), preserving manual..."
	@if exist $(subst /,\, $(ZIP_LINUX_DIR)) ( \
	    for %%f in ($(subst /,\, $(ZIP_LINUX_DIR))\*) do ( \
	        if /I not "%%~nxf"=="CoD2x Installation and uninstallation manual.txt" del /Q "%%f" \
	    ) \
	)


# ========================================================================================================
# Zip embedded IWD files
# ========================================================================================================
.PHONY: zip_iwd

# IWD number, increase for each new IWD created
IWD_NUM ?= 01

IWD_DIR = src/embedded/iw_CoD2x_$(IWD_NUM)
IWD_FILE = iw_CoD2x_$(IWD_NUM).iwd

zip_iwd:
	@echo Zipping files from $(IWD_DIR) into $(IWD_FILE) ...
	@cd $(IWD_DIR) && if exist "..\$(IWD_FILE)" del "..\$(IWD_FILE)" && $(ZIP_EXEC) a -tzip "..\$(IWD_FILE)" *
	@echo Done: $(IWD_FILE)
