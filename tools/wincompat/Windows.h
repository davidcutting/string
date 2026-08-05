// Case-shim for cross-compiling from a case-sensitive filesystem.
//
// KTX's lib/astc_codec.cpp does `#include <Windows.h>`. That is fine on Windows (NTFS is
// case-insensitive) but mingw-w64 ships the header as lowercase `windows.h`, so the include fails
// when cross-compiling from Linux. Putting this directory on the include path satisfies the
// capitalised spelling and forwards to the real header.
//
// The lowercase include below resolves to mingw's own header, not to this file, precisely because
// the filesystem IS case-sensitive — the shim cannot recurse into itself.
#pragma once
#include <windows.h>
