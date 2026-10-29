#!/usr/bin/env bash
# Generate src/shared/version.h and src/mss32/version.rc from a version string, e.g. 1.4.6.9-test.1
# Used by CI so the build does not depend on the makefile's platform-specific echo recipes.
# (Locally you still use `make prebuild`; this produces the same files.)
set -eu

VER="${1:?usage: gen-version.sh <version>  e.g. 1.4.6.9-test.1}"
BASE="${VER%%-*}"                                  # 1.4.6.9
IFS=. read -r MAJOR MINOR PROTOCOL PATCH <<< "$BASE"
case "$VER" in *-*) ISTEST=1 ;; *) ISTEST=0 ;; esac   # any pre-release suffix (-test./-pr./-dev.) = test build

cat > src/shared/version.h <<EOF
#define APP_VERSION_MAJOR $MAJOR
#define APP_VERSION_MINOR $MINOR
#define APP_VERSION_PROTOCOL $PROTOCOL
#define APP_VERSION_PATCH $PATCH
#define APP_VERSION_IS_TEST $ISTEST
#define APP_VERSION "$VER"
EOF

cat > src/mss32/version.rc <<EOF
1 VERSIONINFO
FILEVERSION $MAJOR,$MINOR,$PROTOCOL,$PATCH
PRODUCTVERSION $MAJOR,$MINOR,$PROTOCOL,$PATCH
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "ProductName", "CoD2x"
            VALUE "ProductVersion", "$VER"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0409, 1200
    END
END
EOF

echo "Generated version.h + version.rc for $VER"
