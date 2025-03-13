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
VERSION_PROTOCOL = 2

# CoD2x patch version
# Should be increased when new version is released and the changes are backward compatible
VERSION_PATCH = 4

# CoD2x test version
# Should be increased when new version is released and the changes are backward compatible
VERSION_TEST = -test.1
VERSION_IS_TEST = 1

# Full version string
# Example "1.4.1.1"  or  "1.4.1.1-test.1"
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PROTOCOL).$(VERSION_PATCH)$(VERSION_TEST)
VERSION_COMMA = $(VERSION_MAJOR),$(VERSION_MINOR),$(VERSION_PROTOCOL),$(VERSION_PATCH)


# ========================================================================================================
# Directories and Files
# ========================================================================================================

# Output directories
WIN_BIN_DIR = bin/windows
LINUX_BIN_DIR = bin/linux

# Targets
WIN_MSS32_TARGET = $(WIN_BIN_DIR)/mss32.dll
LINUX_TARGET = $(LINUX_BIN_DIR)/libCoD2x.so

# Source and object directories
WIN_MSS32_SRC_DIR = src/mss32
WIN_MSS32_OBJ_DIR = obj/mss32
LINUX_OBJ_DIR = obj/linux
LINUX_SRC_DIR = src/linux
SHARED_SRC_DIR = src/shared
SHARED_OBJ_DIR = obj/shared

RCEDIT = tools/rcedit/rcedit-x86.exe

# Define source files and target paths
ZIP_EXEC = 7za.exe
ZIP_WIN_DIR = zip/windows
ZIP_WIN_FILES = $(WIN_MSS32_TARGET) bin/windows/mss32_original.dll
ZIP_WIN_NAME = CoD2x_$(VERSION)_windows.zip
ZIP_WIN_OUTPUT = $(ZIP_WIN_DIR)/$(ZIP_WIN_NAME)

ZIP_LINUX_DIR = zip/linux
ZIP_LINUX_FILES = $(LINUX_TARGET) bin/linux/cod2_lnxded
ZIP_LINUX_NAME = CoD2x_$(VERSION)_linux.zip
ZIP_LINUX_OUTPUT = $(ZIP_LINUX_DIR)/$(ZIP_LINUX_NAME)

# Source files
MSS32_C_SOURCES = $(wildcard $(WIN_MSS32_SRC_DIR)/*.cpp)
MSS32_ASM_SOURCES = $(wildcard $(WIN_MSS32_SRC_DIR)/*.asm)
LINUX_C_SOURCES = $(wildcard $(LINUX_SRC_DIR)/*.cpp)
SHARED_C_SOURCES = $(wildcard $(SHARED_SRC_DIR)/*.cpp)

# Shared object files for Windows and Linux
SHARED_WIN_OBJECTS = $(patsubst $(SHARED_SRC_DIR)/%.cpp, $(WIN_MSS32_OBJ_DIR)/shared_%.o, $(SHARED_C_SOURCES))
SHARED_LINUX_OBJECTS = $(patsubst $(SHARED_SRC_DIR)/%.cpp, $(LINUX_OBJ_DIR)/shared_%.o, $(SHARED_C_SOURCES))

# Platform-specific object files
WIN_MSS32_OBJECTS = $(patsubst $(WIN_MSS32_SRC_DIR)/%.cpp, $(WIN_MSS32_OBJ_DIR)/%.o, $(MSS32_C_SOURCES)) \
                    $(patsubst $(WIN_MSS32_SRC_DIR)/%.asm, $(WIN_MSS32_OBJ_DIR)/%.o, $(MSS32_ASM_SOURCES)) \
					$(SHARED_WIN_OBJECTS)
LINUX_OBJECTS = $(patsubst $(LINUX_SRC_DIR)/%.cpp, $(LINUX_OBJ_DIR)/%.o, $(LINUX_C_SOURCES)) \
				$(SHARED_LINUX_OBJECTS)


# ========================================================================================================
# Compilation Settings
# ========================================================================================================

# Compilation flags
CFLAGS = -Wall -Wextra -Wno-unused-parameter -g -m32 -lstdc++ -O0
# Flag explanations:
# -Wall: Enable all compiler warnings
# -Wextra: Enable extra compiler warnings
# -Wno-unused-parameter: Disable warnings for unused function parameters
# -g: Include debugging information
# -m32: Generate 32-bit code
# -shared: Create a shared library (windows: DLL, linux: SO)
# -lstdc++: Link with the C++ standard library

# Windows toolchain
WIN_CC = gcc.exe
WIN_AS = nasm
WIN_CFLAGS = $(CFLAGS) -mwindows -static
WIN_LFLAGS = -shared -m32
WIN_ASFLAGS = -f win32	# Output format for NASM (32-bit Windows)
WIN_LIBS = -lkernel32 -lwininet -static-libgcc -static-libstdc++ -ldbghelp -lssl -lcrypto -lws2_32
# -mwindows: Link with the Windows GUI subsystem (no console)
# -static: Link libraries statically

# Linux toolchain
LINUX_CC = gcc
LINUX_CFLAGS = $(CFLAGS) -fPIC
LINUX_LFLAGS = -shared -m32
LINUX_LIBS = -ldl -pthread
# -fPIC: Generate position-independent code
# -shared: Create a shared library (SO)

# ========================================================================================================
# Build Targets and Rules
# ========================================================================================================

# Default target
all: build_mss32_win build_cod2x_linux
	@echo "Build completed for all targets."



$(SHARED_SRC_DIR)/version.h: makefile
	@echo Generating version.h...

	@echo #define APP_VERSION_MAJOR $(VERSION_MAJOR) 			>  $(SHARED_SRC_DIR)\version.h
	@echo #define APP_VERSION_MINOR $(VERSION_MINOR) 			>> $(SHARED_SRC_DIR)\version.h
	@echo #define APP_VERSION_PROTOCOL $(VERSION_PROTOCOL) 	>> $(SHARED_SRC_DIR)\version.h
	@echo #define APP_VERSION_PATCH $(VERSION_PATCH) 			>> $(SHARED_SRC_DIR)\version.h
	@echo #define APP_VERSION_IS_TEST $(VERSION_IS_TEST) 		>> $(SHARED_SRC_DIR)\version.h
	@echo #define APP_VERSION "$(VERSION)" 					>> $(SHARED_SRC_DIR)\version.h
	
	@echo   Done.



# ========================================================================================================
# Windows MSS32
# ========================================================================================================

# MSS32 Windows target
build_mss32_win: $(SHARED_SRC_DIR)/version.h $(WIN_MSS32_TARGET)
	@echo "MSS32 (Windows) build complete."
	@echo.

# Linking with resource file
$(WIN_MSS32_TARGET): $(WIN_MSS32_OBJECTS) $(WIN_MSS32_OBJ_DIR)/version.res
	@echo "Linking $@..."
	$(WIN_CC) $(WIN_LFLAGS) -o $@ $^ $(WIN_LIBS) $(WIN_MSS32_SRC_DIR)/mss32.def
	@echo   Done.
	@echo.

# Compile C++ files
$(WIN_MSS32_OBJ_DIR)/%.o: $(WIN_MSS32_SRC_DIR)/%.cpp | $(WIN_MSS32_OBJ_DIR)
	@echo "Compiling $< for MSS32 (Windows)..."
	$(WIN_CC) $(WIN_CFLAGS) -MMD -MP -c -o $@ $<
	@echo   Done.
	@echo.

# Assemble ASM files
$(WIN_MSS32_OBJ_DIR)/%.o: $(WIN_MSS32_SRC_DIR)/%.asm | $(WIN_MSS32_OBJ_DIR)
	@echo "Assembling $< for MSS32 (Windows)..."
	$(WIN_AS) $(WIN_ASFLAGS) $< -o $@
	@echo   Done.
	@echo.


# Generate version.rc file if makefile has changed
$(TMP)/version.rc: makefile
	@echo Generating version.rc...

	@echo 1 VERSIONINFO                                			>  $(TMP)/version.rc
	@echo FILEVERSION $(VERSION_COMMA)                			>> $(TMP)/version.rc
	@echo PRODUCTVERSION $(VERSION_COMMA)             			>> $(TMP)/version.rc
	@echo BEGIN                                       			>> $(TMP)/version.rc
	@echo     BLOCK "StringFileInfo"                  			>> $(TMP)/version.rc
	@echo     BEGIN                                   			>> $(TMP)/version.rc
	@echo         BLOCK "040904b0"                    			>> $(TMP)/version.rc
	@echo         BEGIN                               			>> $(TMP)/version.rc
	@echo             VALUE "ProductName", "CoD2x"    			>> $(TMP)/version.rc
	@echo             VALUE "ProductVersion", "$(VERSION)" 		>> $(TMP)/version.rc
	@echo         END                                 			>> $(TMP)/version.rc
	@echo     END                                     			>> $(TMP)/version.rc
	@echo     BLOCK "VarFileInfo"                     			>> $(TMP)/version.rc
	@echo     BEGIN                                   			>> $(TMP)/version.rc
	@echo         VALUE "Translation", 0x0409, 1200  			>> $(TMP)/version.rc
	@echo     END                                     			>> $(TMP)/version.rc
	@echo END                                         			>> $(TMP)/version.rc

	@echo   Done.
	@echo.

# Compile version.rc into a .res file
$(WIN_MSS32_OBJ_DIR)/version.res: $(TMP)/version.rc
	@echo Compiling version.res...

	windres $(TMP)\version.rc -O coff -o $(WIN_MSS32_OBJ_DIR)\version.res

	@echo   Done.
	@echo.


# Include dependency files
-include $(WIN_MSS32_OBJ_DIR)/*.d

# Create directories
$(WIN_MSS32_OBJ_DIR) $(WIN_BIN_DIR):
	@if not exist $(subst /,\, $@) mkdir $(subst /,\, $@)



# ========================================================================================================
# Linux
# ========================================================================================================

# CoD2x Linux target
build_cod2x_linux: $(SHARED_SRC_DIR)/version.h $(LINUX_TARGET)
	@echo "CoD2x (Linux) build complete."
	@echo ""

$(LINUX_TARGET): $(LINUX_OBJECTS)
	@echo "Linking to $@..."
	$(LINUX_CC) $(LINUX_LFLAGS) -o $@ $^ $(LINUX_LIBS)
	@echo   Done.
	@echo ""

$(LINUX_OBJ_DIR)/%.o: $(LINUX_SRC_DIR)/%.cpp | $(LINUX_OBJ_DIR)
	@echo "Compiling $< for CoD2x (Linux)..."
	$(LINUX_CC) $(LINUX_CFLAGS) -MMD -MP -c -o $@ $<
	@echo   Done.
	@echo ""


# Include dependency files
-include $(LINUX_OBJ_DIR)/*.d

# Create directories
$(LINUX_OBJ_DIR) $(LINUX_BIN_DIR):
	@mkdir -p $@



# ========================================================================================================
# Shared for Windows and Linux
# ========================================================================================================

# Compile shared functionality objects for Windows
$(WIN_MSS32_OBJ_DIR)/shared_%.o: $(SHARED_SRC_DIR)/%.cpp | $(WIN_MSS32_OBJ_DIR)
	@echo "Compiling $< for shared functionality (Windows)..."
	$(WIN_CC) $(WIN_CFLAGS) -MMD -MP -c -o $@ $<
	@echo   Done.
	@echo.

# Compile shared functionality objects for Linux
$(LINUX_OBJ_DIR)/shared_%.o: $(SHARED_SRC_DIR)/%.cpp | $(LINUX_OBJ_DIR)
	@echo "Compiling $< for shared functionality (Linux)..."
	$(LINUX_CC) $(LINUX_CFLAGS) -MMD -MP -c -o $@ $<
	@echo   Done.
	@echo ""





# ========================================================================================================
# Zip output files for Windows
# ========================================================================================================

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
# Zip output files for Linux
# ========================================================================================================

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



zip_all: zip_win zip_linux




# ========================================================================================================
# Cleaning Up
# ========================================================================================================

clean: zip_win_clean zip_linux_clean
	@echo "Cleaning up build files..."
	@if exist $(subst /,\, $(WIN_MSS32_OBJ_DIR))\*.o del /Q $(subst /,\, $(WIN_MSS32_OBJ_DIR))\*.o
	@if exist $(subst /,\, $(WIN_MSS32_OBJ_DIR))\*.d del /Q $(subst /,\, $(WIN_MSS32_OBJ_DIR))\*.d
	@if exist $(subst /,\, $(LINUX_OBJ_DIR))\*.o del /Q $(subst /,\, $(LINUX_OBJ_DIR))\*.o
	@if exist $(subst /,\, $(LINUX_OBJ_DIR))\*.d del /Q $(subst /,\, $(LINUX_OBJ_DIR))\*.d
	@if exist $(subst /,\, $(WIN_MSS32_TARGET)) del /Q $(subst /,\, $(WIN_MSS32_TARGET))
	@if exist $(subst /,\, $(LINUX_TARGET)) del /Q $(subst /,\, $(LINUX_TARGET))
	@if exist $(subst /,\, $(WIN_MSS32_OBJ_DIR)/version.res) del /Q $(subst /,\, $(WIN_MSS32_OBJ_DIR)/version.res)
	@echo "Done."

# ========================================================================================================
# Phony Targets
# ========================================================================================================

.PHONY: all clean build_mss32_win build_cod2x_linux zip_win zip_win_clean