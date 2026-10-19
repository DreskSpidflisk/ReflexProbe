#pragma once

#include <windows.h>
#include "../Common/Protocol.h"

// DXGI presentation observation only. These functions are compiled into the
// existing ReflexProbe64.dll; no second injected DLL or presentation override.
namespace PresentationDxgi {
void Initialize(ReflexProbeProtocol::SharedState* shared);
bool PatchImports(HMODULE module);
FARPROC ResolveDynamic(HMODULE module, LPCSTR name, FARPROC original);
}
