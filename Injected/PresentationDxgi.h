#pragma once

#include <windows.h>
#include "../Common/Protocol.h"

// DXGI presentation observation only. These functions are compiled into the
// existing ReflexProbe64.dll; no second injected DLL or presentation override.
namespace PresentationDxgi {
void Initialize(ReflexProbeProtocol::SharedState* shared);
// Limit DXGI instrumentation to modules belonging to the game's installation.
// The resolver caller check excludes Steam / GOG overlay code.
bool IsApplicationModule(HMODULE module);
bool IsApplicationCaller(const void* returnAddress);
bool PatchImports(HMODULE module);
FARPROC ResolveDynamic(HMODULE module, LPCSTR name, FARPROC original);
}
