#pragma once

#include <windows.h>
#include "../Common/Protocol.h"

// DXGI presentation observation and independently opt-in live Present overrides.
// Compiled into the existing ReflexProbe64.dll; no second injected DLL.
namespace PresentationDxgi {
bool Initialize(ProbeProtocol::SharedState* shared);
// Limit DXGI instrumentation to modules belonging to the game's installation.
// The resolver caller check excludes Steam / GOG overlay code.
bool IsApplicationModule(HMODULE module);
bool IsApplicationCaller(const void* returnAddress);
bool IsDynamicExportName(const char* name);
bool PatchImports(HMODULE module);
FARPROC ResolveDynamic(HMODULE module, LPCSTR name, FARPROC original);
}
