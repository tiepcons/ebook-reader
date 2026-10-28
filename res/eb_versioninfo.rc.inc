#include <winver.h>
#include "eb_version.h"

#define EB_STR2(x) #x
#define EB_STR(x) EB_STR2(x)

VS_VERSION_INFO VERSIONINFO
FILEVERSION EB_VERSION_MAJOR, EB_VERSION_MINOR, EB_VERSION_PATCH, 0
PRODUCTVERSION EB_VERSION_MAJOR, EB_VERSION_MINOR, EB_VERSION_PATCH, 0
FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
FILEFLAGS 0
FILEOS VOS_NT_WINDOWS32
FILETYPE EB_VI_FILETYPE
FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "CompanyName", "Ebook Reader contributors"
            VALUE "FileDescription", EB_VI_DESCRIPTION
            VALUE "FileVersion", EB_STR(EB_VERSION_MAJOR) "." EB_STR(EB_VERSION_MINOR) "." EB_STR(EB_VERSION_PATCH) ".0"
            VALUE "InternalName", EB_VI_INTERNAL
            VALUE "LegalCopyright", "AGPL-3.0-or-later, Ebook Reader contributors"
            VALUE "OriginalFilename", EB_VI_FILENAME
            VALUE "ProductName", "Ebook Reader"
            VALUE "ProductVersion", EB_VERSION_STRING
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0409, 0x04B0
    END
END
