// ImGui's DX11 backend calls D3DCompile(); the Makefile renames that call to
// DpsD3DCompile so the compiler DLL is found at runtime instead of being a load-time
// import (the game folder is scanned before the bottle has loaded any D3D DLLs).

#include <windows.h>

#include <d3dcompiler.h>

#include <atomic>

#include "log.h"

namespace {

pD3DCompile Resolve() {
    static const wchar_t* const kNames[] = {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"};
    for (const wchar_t* name : kNames) {
        HMODULE module = GetModuleHandleW(name);
        if (!module)
            module = LoadLibraryW(name);
        if (!module)
            continue;
        auto fn = reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile"));
        if (fn) {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(module, path, MAX_PATH);
            LOG_INFO("D3DCompile from %s", path);
            return fn;
        }
    }
    LOG_ERROR("no d3dcompiler_4x.dll with D3DCompile found; the overlay cannot create its shaders");
    return nullptr;
}

}  // namespace

extern "C" HRESULT WINAPI DpsD3DCompile(LPCVOID data, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                        ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                        ID3DBlob** code, ID3DBlob** errors) {
    static const pD3DCompile compile = Resolve();
    if (!compile)
        return E_FAIL;
    ID3DBlob* localErrors = nullptr;
    const HRESULT hr = compile(data, size, name, defines, include, entry, target, flags1, flags2, code, errors ? errors : &localErrors);
    ID3DBlob* report = errors ? *errors : localErrors;
    if (FAILED(hr))
        LOG_ERROR("D3DCompile %s failed 0x%08lX: %s", target ? target : "?", static_cast<unsigned long>(hr),
                  report ? static_cast<const char*>(report->GetBufferPointer()) : "(no message)");
    if (localErrors)
        localErrors->Release();
    return hr;
}
