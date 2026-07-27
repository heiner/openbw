// Empty stub so bots written for Windows (which #include <Windows.h> for their
// DllMain / handle types) compile against OpenBW. The real DLL entry is replaced
// by web/bot/bot_entry.cpp, and no Win32 API is actually called on our path.
#pragma once
