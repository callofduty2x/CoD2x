cmake_minimum_required(VERSION 3.10)

#set(CMAKE_USE_RELATIVE_PATHS ON)

project(CoD2x LANGUAGES C CXX)

# symbols_mss32.h / symbols_libcod2x.h are generated build outputs (not committed). Regenerate them from
# the linked binary (Release, two-pass). GENERATE_SYMBOLS=OFF skips it (e.g. to unblock CI while the
# toolchain is being sorted) - the build then ships with the placeholder (fewer resolved names, still works).
option(GENERATE_SYMBOLS "Regenerate symbols_*.h from the linked binary after linking (Release)" ON)

# Write an empty placeholder if the generated header is missing, so the first (pass-1) compile succeeds.
if (WIN32 AND NOT EXISTS "${CMAKE_SOURCE_DIR}/src/mss32/symbols_mss32.h")
  file(WRITE "${CMAKE_SOURCE_DIR}/src/mss32/symbols_mss32.h"
    "#ifndef SYMBOLS_MSS32_H\n#define SYMBOLS_MSS32_H\n#include <stdint.h>\nstruct Mss32Symbol { uint32_t rva; uint32_t len; const char* name; };\nstatic inline const Mss32Symbol* symbols_mss32_find(uint32_t) { return nullptr; }\n#endif\n")
endif()
if (UNIX AND NOT WIN32 AND NOT EXISTS "${CMAKE_SOURCE_DIR}/src/linux/symbols_libcod2x.h")
  file(WRITE "${CMAKE_SOURCE_DIR}/src/linux/symbols_libcod2x.h"
    "#ifndef SYMBOLS_LIBCOD2X_H\n#define SYMBOLS_LIBCOD2X_H\n#include <stdint.h>\nstruct LibCod2xSymbol { uint32_t rva; uint32_t len; const char* name; };\nstatic inline const LibCod2xSymbol* symbols_libcod2x_find(uint32_t) { return nullptr; }\n#endif\n")
endif()


# -----------------------------------
# Function that collects all files with given extensions from a base directory (recursively)
# -----------------------------------
function(find_files_by_extension OUT_LIST BASE_DIR)
  set(full_base "${CMAKE_SOURCE_DIR}/${BASE_DIR}")
  if (EXISTS "${full_base}")
    set(local "")
    foreach(ext IN LISTS ARGN)
      file(GLOB_RECURSE found "${full_base}/*.${ext}")
      foreach(f ${found})
        file(RELATIVE_PATH relf "${CMAKE_SOURCE_DIR}" "${f}")
        list(APPEND local "${relf}")
      endforeach()
    endforeach()
    set(${OUT_LIST} ${local} PARENT_SCOPE)
  else()
    message(WARNING "Directory not found: ${full_base}")
    set(${OUT_LIST} "" PARENT_SCOPE)
  endif()
endfunction()

# Shared sources (C/C++)
find_files_by_extension(SHARED_SRC "src/shared" c cpp)


# Define files to embed
set(EMBED_FILES
    ${CMAKE_SOURCE_DIR}/src/embedded/iw_CoD2x_01.iwd
)

# Pick objcopy output format depending on platform
if(WIN32)
    set(OBJCOPY_FORMAT pe-i386)
else()
    set(OBJCOPY_FORMAT elf32-i386)
endif()

set(EMBED_OBJECTS "")

foreach(FILE ${EMBED_FILES})
    # Extract directory and filename
    get_filename_component(FILE_DIR ${FILE} DIRECTORY)
    get_filename_component(NAME ${FILE} NAME)

    # Replace '.' with '_' so symbol names are valid
    string(REPLACE "." "_" SYMBOL ${NAME})

    set(OUT_OBJ ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.o)
    set_source_files_properties(${OUT_OBJ} PROPERTIES
        GENERATED TRUE
        EXTERNAL_OBJECT TRUE
    )
    add_custom_command(
        OUTPUT ${OUT_OBJ}
        # Run objcopy with working directory set to FILE_DIR
        COMMAND ${CMAKE_COMMAND} -E chdir ${FILE_DIR}
            ${CMAKE_OBJCOPY}
                -I binary
                -O ${OBJCOPY_FORMAT}
                --binary-architecture i386
                ${NAME} ${OUT_OBJ}
        DEPENDS ${FILE}
        COMMENT "Embedding ${FILE}"
    )

    list(APPEND EMBED_OBJECTS ${OUT_OBJ})
endforeach()



# -----------------------------------
# Windows: mss32.dll (C/C++ + NASM + RC)
# -----------------------------------
if (WIN32)
  enable_language(ASM_NASM)
  enable_language(RC)

  find_files_by_extension(MSS32_CC  "src/mss32" c cpp asm rc def)

  add_library(mss32 SHARED
    ${MSS32_CC}
    ${SHARED_SRC}
    ${EMBED_OBJECTS}
  )

  target_compile_features(mss32 PRIVATE cxx_std_17)

  set_target_properties(mss32 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mss32.build"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/windows"
  )

  target_include_directories(mss32 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shared"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mss32"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/openssl_mingw/include/"
  )

  # Per-language compile options, with DEBUG=1 defined in Debug mode
  target_compile_options(mss32 PRIVATE
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:
        -Wall -Wextra -Wno-unused-parameter -Wno-address-of-packed-member
        -g -m32 -msse -O0
        -pipe # pass assembly to the assembler in memory; temp .s files in %TEMP% got corrupted by antivirus scans during parallel builds
        -DCJSON_CDECL=__cdecl
        -DMG_TLS=MG_TLS_OPENSSL -DMG_ENABLE_POSIX_FS=0
        -fdiagnostics-color=always
        $<$<CONFIG:Debug>:-DDEBUG=1>
        # EXCEPTION_TEST=ON compiles in the crash-handler self test (triggered at runtime with
        # "+set exception_test <mode>"). Off for shipping builds. See src/mss32/exception.cpp.
        $<$<BOOL:${EXCEPTION_TEST}>:-DEXCEPTION_TEST=1>
    >
    $<$<COMPILE_LANGUAGE:ASM_NASM>:
        -f win32
    >
  )

  target_link_libraries(mss32 PRIVATE
    kernel32 wininet dbghelp ole32 oleaut32 uuid gdi32
    "${CMAKE_SOURCE_DIR}/tools/openssl_mingw/lib/libssl.a"
    "${CMAKE_SOURCE_DIR}/tools/openssl_mingw/lib/libcrypto.a"
    ws2_32 crypt32
    -static-libgcc -static-libstdc++ -static pthread stdc++
  )

  

  # Generate symbols_mss32.h from the just-linked (pre-strip) DLL.
  # Only runs for Release builds; Debug builds skip this to avoid the two-pass requirement.
  # Must be listed BEFORE the strip command so the COFF symbol table is still intact.
  if (CMAKE_BUILD_TYPE STREQUAL "Release" AND GENERATE_SYMBOLS)
    # nm: the vendored toolchain when present (local), else nm from PATH (CI). Override with -DMSS32_NM=...
    if (NOT DEFINED MSS32_NM)
      if (EXISTS "${CMAKE_SOURCE_DIR}/tools/mingw/bin/nm.exe")
        set(MSS32_NM "${CMAKE_SOURCE_DIR}/tools/mingw/bin/nm.exe")
      else()
        set(MSS32_NM "nm")
      endif()
    endif()
    # PowerShell is the one interpreter present on every Windows build host (local cmd/mingw build
    # AND the CI runner); bash is not, and a stray C:\Windows\System32\bash.exe is the WSL launcher,
    # which can't take Windows paths. So Windows always uses the PowerShell generator (Linux uses bash).
    add_custom_command(TARGET mss32 POST_BUILD
      COMMAND powershell -ExecutionPolicy Bypass -File
          "${CMAKE_SOURCE_DIR}/src/other/generate_mss32_symbols_h.ps1"
          -DllPath "$<TARGET_FILE:mss32>"
          -OutHdr  "${CMAKE_SOURCE_DIR}/src/mss32/symbols_mss32.h"
          -NmExe   "${MSS32_NM}"
      COMMENT "Generating symbols_mss32.h"
      VERBATIM)
  endif()

  # Optional strip after link (for release packaging)
  if (CMAKE_BUILD_TYPE STREQUAL "Release")
    if (NOT CMAKE_STRIP)
      find_program(CMAKE_STRIP NAMES strip)
    endif()
    if (CMAKE_STRIP)
      add_custom_command(TARGET mss32 POST_BUILD
        COMMAND echo Stripping mss32.dll...
        COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:mss32>"
        COMMENT "Stripping mss32.dll"
        VERBATIM)
    else()
      message(WARNING "STRIP_MSS32=ON but 'strip' not found in PATH.")
    endif()
  endif()

endif()

# -----------------------------------
# Linux: linux.so
# -----------------------------------
if (UNIX AND NOT WIN32)

  find_files_by_extension(LNX_CC "src/linux" c cpp)

  add_library(linux SHARED
    ${LNX_CC}
    ${SHARED_SRC}
    ${EMBED_OBJECTS}
  )

  target_compile_features(linux PRIVATE cxx_std_17)

  message(STATUS "Found Linux source files: ${LNX_CC}")

  set_target_properties(linux PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libCoD2x"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/linux"
  )

  target_include_directories(linux PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shared"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/linux"
  )

  target_compile_options(linux PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -m32 -msse -O0 -fPIC
    -DMG_TLS=MG_TLS_OPENSSL
    -fdiagnostics-color=always
    $<$<CONFIG:Debug>:-DDEBUG=1>
    # EXCEPTION_TEST=ON compiles in the crash-handler self test (triggered at runtime with the
    # COD2X_EXCEPTION_TEST env var). Off for shipping builds. See src/linux/exception.cpp.
    $<$<BOOL:${EXCEPTION_TEST}>:-DEXCEPTION_TEST=1>
  )

  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32 -shared -static-libgcc -static-libstdc++")

  target_link_libraries(linux PRIVATE
    /usr/lib/i386-linux-gnu/libssl.a
    /usr/lib/i386-linux-gnu/libcrypto.a
    dl pthread
  )

  # Generate symbols_libcod2x.h from the just-linked .so so the crash handler can resolve our own
  # (static) functions. One-build-behind pattern, like symbols_mss32.h; Release only (two-pass build).
  if (CMAKE_BUILD_TYPE STREQUAL "Release" AND GENERATE_SYMBOLS)
    add_custom_command(TARGET linux POST_BUILD
      COMMAND bash "${CMAKE_SOURCE_DIR}/src/other/generate_libcod2x_symbols.sh"
              "$<TARGET_FILE:linux>" "${CMAKE_SOURCE_DIR}/src/linux/symbols_libcod2x.h" "nm"
      COMMENT "Generating symbols_libcod2x.h"
      VERBATIM)
  endif()

endif()