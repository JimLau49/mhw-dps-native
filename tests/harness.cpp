// Self-test for the native DPS meter, run inside the CrossOver bottle (wine harness.exe).
// Exercises the guarded memory reads (including a real fault absorbed by the vectored
// handler), pattern scanning, address maps, game data tables, fight log JSON, and the
// full Present-hook + ImGui DX11 path on the bottle's D3D implementation, saving the
// rendered frame as harness-frame.bmp.

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "address_map.h"
#include "common.h"
#include "game/game_reader.h"
#include "game_data.h"
#include "hooks/game_hooks.h"
#include "hooks/hook_manager.h"
#include "log.h"
#include "memory.h"
#include "meter/fight_log_store.h"
#include "meter/meter.h"
#include "render/d3d11_hook.h"
#include "render/input.h"
#include "settings.h"

using namespace dps;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
    std::fflush(stdout);
}

void Section(const char* name) {
    std::printf("\n[%s]\n", name);
    std::fflush(stdout);
}

// ---- memory -------------------------------------------------------------------

void TestMemory() {
    Section("guarded memory reads");
    int value = 42, out = 0;
    Check(mem::Read(reinterpret_cast<uintptr_t>(&value), out) && out == 42, "read a stack int");
    Check(!mem::Read<int>(0, out), "null pointer refused");
    Check(!mem::Read<int>(0x1000, out), "low address refused");
    Check(!mem::Read<int>(0x7FFFFFFFFFFFULL, out), "kernel-range address refused");

    void* reserved = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    Check(reserved && !mem::Read<int>(reinterpret_cast<uintptr_t>(reserved), out), "reserved (uncommitted) page refused");

    void* noAccess = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    Check(noAccess && !mem::Read<int>(reinterpret_cast<uintptr_t>(noAccess), out), "PAGE_NOACCESS page refused");

    void* guard = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD);
    Check(guard && !mem::Read<int>(reinterpret_cast<uintptr_t>(guard), out), "guard page refused");
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(guard, &info, sizeof(info));
    Check((info.Protect & PAGE_GUARD) != 0, "guard page still armed afterwards");

    // Read once (caches the region), free it, read again within the cache window:
    // the pre-check passes and the copy itself faults. That fault must be absorbed.
    auto* page = static_cast<int*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    page[0] = 7;
    Check(mem::Read(reinterpret_cast<uintptr_t>(page), out) && out == 7, "read a committed page");
    VirtualFree(page, 0, MEM_RELEASE);
    const uint64_t faultsBefore = mem::FaultCount();
    const bool freedRead = mem::Read(reinterpret_cast<uintptr_t>(page) + 8, out);
    Check(!freedRead, "read of freed memory fails instead of crashing");
    Check(mem::FaultCount() == faultsBefore + 1, "the access violation went through the vectored handler");

    auto* two = static_cast<char*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD old = 0;
    VirtualProtect(two + 0x1000, 0x1000, PAGE_NOACCESS, &old);
    std::memcpy(two + 0x1000 - 4, "abc", 4);
    std::string text;
    Check(mem::ReadCString(reinterpret_cast<uintptr_t>(two + 0x1000 - 4), 64, text) && text == "abc",
          "string ending right before an inaccessible page");
    std::memcpy(two + 0x1000 - 4, "abcd", 4);
    Check(!mem::ReadCString(reinterpret_cast<uintptr_t>(two + 0x1000 - 4), 64, text), "unterminated string into it refused");

    // Pointer chains: root -> A(+0x10) -> B(+0x8) -> value
    struct Node {
        uintptr_t pad[4];
    };
    static Node b{};
    static Node a{};
    static uintptr_t root = 0;
    b.pad[1] = 1234;
    a.pad[2] = reinterpret_cast<uintptr_t>(&b);
    root = reinterpret_cast<uintptr_t>(&a);
    std::string error;
    const uintptr_t end = mem::Follow(reinterpret_cast<uintptr_t>(&root), {0x10, 0x8}, &error);
    Check(end == reinterpret_cast<uintptr_t>(&b) + 8 && mem::ReadOr<uintptr_t>(end, 0) == 1234, "pointer chain followed");
    a.pad[2] = 0;
    Check(mem::Follow(reinterpret_cast<uintptr_t>(&root), {0x10, 0x8}, &error) == 0 && !error.empty(), "null hop reported");
}

// ---- patterns -------------------------------------------------------------------

void TestPatterns() {
    Section("pattern scanning");
    const uint8_t buffer[] = {0x90, 0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3,
                              0x48, 0x8B, 0x05, 0x55, 0x66, 0x77, 0x88, 0xC3, 0xCC};
    const auto pattern = mem::ParsePattern("48 8B 05 ?? ?? ?? ?? C3");
    const auto hits = mem::Scan(reinterpret_cast<uintptr_t>(buffer), sizeof(buffer), pattern, 4);
    Check(hits.size() == 2 && hits[0] == reinterpret_cast<uintptr_t>(buffer) + 1 &&
              hits[1] == reinterpret_cast<uintptr_t>(buffer) + 9,
          "two wildcard matches in a buffer");
    Check(!mem::ParsePattern("48 8G").Valid(), "malformed pattern rejected");

    mem::ModuleInfo self;
    Check(mem::QueryModule(GetModuleHandleW(nullptr), self) && self.Find(".text") != nullptr, "own PE headers parsed");
    // The guarded-copy routine's body: push rsi; push rdi; mov rdi,rcx; mov rsi,rdx; mov rcx,r8; rep movsb
    const auto own = mem::ScanModule(self, mem::ParsePattern("56 57 48 89 CF 48 89 D6 4C 89 C1 F3 A4"), 2);
    Check(own.size() == 1, "unique code pattern found in this exe's .text");
}

// ---- mid-function render capture ---------------------------------------------------

// Mirrors the game's sMhRender:Assignment site: rax holds a value, then
// "lea rax, [rip+x]" overwrites it. The hook must record the old rax and the function
// must still return the lea's result, with r11 (not touched by the stub) intact.
extern "C" uintptr_t dps_test_render_fn();
extern "C" char dps_test_render_site[];
extern "C" char dps_test_render_data[];
__asm__(
    ".data\n"
    ".p2align 4\n"
    ".globl dps_test_render_data\n"
    "dps_test_render_data: .quad 0x5a5a5a5a5a5a5a5a\n"
    ".text\n"
    ".p2align 4\n"
    ".globl dps_test_render_fn\n"
    "dps_test_render_fn:\n"
    "    movabsq $0x1122334455667788, %rax\n"
    "    movq %rax, %r11\n"
    ".globl dps_test_render_site\n"
    "dps_test_render_site:\n"
    "    leaq dps_test_render_data(%rip), %rax\n"
    "    cmpq %rax, %r11\n"  // r11 must still be the old rax (never equal to the lea result)
    "    jne 1f\n"
    "    xorl %eax, %eax\n"
    "1:  ret\n");

void TestRenderCapture() {
    Section("mid-function render capture hook");
    Check(dps_test_render_fn() == reinterpret_cast<uintptr_t>(dps_test_render_data), "test site works unhooked");
    Check(hooks::InstallRenderCaptureAt(reinterpret_cast<uintptr_t>(dps_test_render_site)), "hook on a lea rax,[rip+x] site");
    Check(dps_test_render_fn() == reinterpret_cast<uintptr_t>(dps_test_render_data), "relocated lea still yields its address");
    Check(hooks::CapturedRender() == 0x1122334455667788ULL, "rax before the lea captured");
}

// ---- address map / data ------------------------------------------------------------

void TestAddressMap() {
    Section("address maps");
    const AddressMap map = AddressMap::Load(421810);
    uintptr_t rva = 0;
    std::vector<int> offsets;
    Check(map.embedded, "421810 map compiled in");
    Check(map.Address("FUN_DEAL_DAMAGE", rva) && rva == 0x01CC5F80, "FUN_DEAL_DAMAGE = 0x01CC5F80");
    Check(map.Address("MONSTER_LIST_ADDRESS", rva) && rva == 0x0500CF40, "MONSTER_LIST_ADDRESS = 0x0500CF40");
    Check(map.Offsets("DAMAGE_OFFSETS", offsets) && offsets == std::vector<int>{0x258, 0x38, 0x450, 0x8, 0x48},
          "DAMAGE_OFFSETS chain");
    Check(AddressMap::Load(421631).embedded, "421631 map compiled in");
    Check(!AddressMap::Load(123456).embedded, "unknown build has no map");

    AddressMap custom;
    custom.Parse("\xEF\xBB\xBF# comment\nAddress X 0x10  # trailing\r\nOffset Y 0x1,0x2\nInstruction Z 0x0F,0x31\n");
    Check(custom.AddressOr("X") == 0x10 && custom.OffsetsOr("Y") == std::vector<int>{1, 2}, "HunterPie map syntax (BOM, CRLF, comments)");
}

void TestGameData() {
    Section("game data");
    Check(data::PartFromNormalSlot(0, 0).value_or(-1) == 2, "Anjanath normal slot 0 -> part 2 (head)");
    Check(std::string(data::PartName(0, 2) ? data::PartName(0, 2) : "") == "Head", "part 2 is named Head");
    Check(std::string(data::WeaponKey(12)) == "HeavyBowgun" && std::string(data::WeaponKey(13)) == "LightBowgun",
          "bowgun ids (12 heavy, 13 light)");
    Check(data::StageLabel(504) != "Stage 504" && data::StageLabel(999) == "Stage 999", "stage labels");
    Check(data::IsLargeMonsterId(0) && data::IsLargeMonsterId(7) && !data::IsLargeMonsterId(3) && !data::IsLargeMonsterId(2),
          "large-monster bitset (Anjanath, Great Jagras yes; Jagras, Aptonoth no)");
    std::printf("  stage 504 = %s, monster 0x4F = %s\n", data::StageLabel(504).c_str(), data::MonsterLabel(0x4F).c_str());

    const uint8_t good[32] = {0xE3, 0x83, 0x8F, 0xE3, 0x83, 0xB3, 0xE3, 0x82, 0xBF, 0xE3, 0x83, 0xBC};  // ハンター
    const uint8_t bad[32] = {0xFF, 0xFE, 0x41};
    const uint8_t control[32] = {'A', 0x07, 'B'};
    std::string name;
    Check(game::DecodeHunterName(good, sizeof(good), name) && name == "ハンター", "UTF-8 hunter name decoded");
    Check(!game::DecodeHunterName(bad, sizeof(bad), name), "invalid UTF-8 rejected");
    Check(!game::DecodeHunterName(control, sizeof(control), name), "control characters rejected");

    Check(ParseHotkey("F9").vk == VK_F9 && ParseHotkey("ctrl+shift+F8").ctrl && ParseHotkey("Ctrl+Shift+F8").shift &&
              ParseHotkey("0x77").vk == 0x77 && ParseHotkey("Numpad5").vk == VK_NUMPAD5,
          "hotkey parsing");
    Check(ThousandsSep(1234567) == "1,234,567" && ThousandsSep(999) == "999" && ThousandsSep(-1000) == "-1,000", "thousands separators");
}

// ---- fight logs -------------------------------------------------------------------

HANDLE g_saved = nullptr;
std::string g_savedName;

void TestFightLogs() {
    Section("fight logs");
    const std::wstring root = JoinPath(GameDirectory(), L"harness-data");
    const std::wstring logs = JoinPath(root, L"logs");
    EnsureDirectory(logs);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(JoinPath(logs, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW(JoinPath(logs, data.cFileName).c_str());
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    meter::FightLogStore store;
    store.Init(root);
    Check(store.Available(), "logs directory usable");

    meter::FightLog log;
    log.questId = 101;
    log.questName = "Harness Hunt";
    log.result = "complete";
    log.stageId = 101;
    log.stage = data::StageLabel(101);
    log.startedAtMs = UnixMillisNow() - 300000;
    log.endedAtMs = UnixMillisNow();
    log.durationSeconds = 300.0f;
    log.timerSource = "quest";
    log.rewards = {true, 12000, 450, 9};
    log.players.push_back({0, "ハンター", true, "LongSword", 30000, 100.0f, 60.0f, 1});
    log.players.push_back({1, "Teammate%s", false, "", 20000, 66.7f, 40.0f, 0});
    log.monsters.push_back({"m1", 0, "Anjanath", 0, 25000.0f, 0.0f, 3.0f, 290.0f});
    meter::LogHit hit;
    hit.t = 12.5f;
    hit.monster = "m1";
    hit.damage = 120;
    hit.crit = true;
    hit.part = 2;
    hit.partName = "Head";
    hit.action = "WP_03::SPIRIT_COMBO";
    log.hits.push_back(hit);
    meter::LogEvent event;
    event.t = 290.0f;
    event.type = "death";
    event.monster = "m1";
    log.events.push_back(event);
    meter::LogSample sample;
    sample.t = 2.0f;
    sample.damage = {100, 50, 0, 0};
    log.samples.push_back(sample);

    g_saved = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    store.SaveAsync(log, [](const std::string& name) {
        g_savedName = name;
        SetEvent(g_saved);
    });
    Check(WaitForSingleObject(g_saved, 10000) == WAIT_OBJECT_0, "async save finished");

    std::string text;
    Check(ReadWholeFile(JoinPath(logs, Utf8ToWide(g_savedName)), text), "log file written");
    const auto json = nlohmann::json::parse(text, nullptr, false);
    Check(json.is_object() && json["schemaVersion"] == 2 && json["players"][0]["name"] == "ハンター" &&
              json["hits"][0]["partName"] == "Head" && json["players"][0]["carts"] == 1 && !json["players"][1].contains("carts") &&
              !json["players"][1].contains("weapon") && json["rewards"]["zenny"] == 12000,
          "schema 2 fields and optional-field omission");

    std::string indexText;
    Check(ReadWholeFile(JoinPath(logs, L"index.json"), indexText), "index.json written");
    const auto index = nlohmann::json::parse(indexText, nullptr, false);
    Check(index.is_array() && index.size() == 1 && index[0]["totalDamage"] == 50000 && index[0]["weapon"] == "LongSword",
          "index entry (total damage, local weapon)");

    // Trials feed the personal bests.
    meter::FightLog trial = log;
    trial.kind = "trial";
    trial.questId = 0;
    trial.result = "trial";
    trial.durationSeconds = 60.0f;
    trial.startedAtMs += 1000;
    trial.players.resize(1);
    ResetEvent(g_saved);
    store.SaveAsync(trial, [](const std::string& name) {
        g_savedName = name;
        SetEvent(g_saved);
    });
    WaitForSingleObject(g_saved, 10000);
    Check(store.BestTrial("LongSword", 60).value_or(-1) == 30000 && !store.BestTrial("LongSword", 90), "personal best lookup");

    meter::FightLogStore reloaded;
    reloaded.Init(root);
    Check(reloaded.History().size() == 2 && reloaded.BestTrial("LongSword", 60).value_or(-1) == 30000, "history and index reload");
    std::printf("  saved %s\n", g_savedName.c_str());
}

// ---- D3D11 + Present hook + ImGui ------------------------------------------------------

struct Readback {
    int captureAt = 20;  // frame number to capture
    bool done = false;
    int frame = 0;
    UINT width = 0, height = 0;
    size_t changed = 0;
    std::vector<uint8_t> pixels;  // BGRA rows, bottom-up for BMP
};
Readback g_readback;
const float kClear[4] = {0.10f, 0.40f, 0.20f, 1.0f};

void SaveBmp(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    std::string out(reinterpret_cast<const char*>(&file), sizeof(file));
    out.append(reinterpret_cast<const char*>(&info), sizeof(info));
    out.append(reinterpret_cast<const char*>(bgra.data()), bgra.size());
    WriteFileAtomic(path, out);
}

void AfterDraw(IDXGISwapChain* swapChain, ID3D11DeviceContext* context) {
    if (g_readback.done || ++g_readback.frame < g_readback.captureAt)
        return;
    g_readback.done = true;

    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
        return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (device && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            g_readback.width = desc.Width;
            g_readback.height = desc.Height;
            g_readback.pixels.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
            const uint8_t clearR = static_cast<uint8_t>(kClear[0] * 255.0f + 0.5f);
            const uint8_t clearG = static_cast<uint8_t>(kClear[1] * 255.0f + 0.5f);
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                uint8_t* dst = g_readback.pixels.data() + static_cast<size_t>(desc.Height - 1 - y) * desc.Width * 4;
                for (UINT x = 0; x < desc.Width; ++x) {
                    const uint8_t r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2];
                    dst[x * 4 + 0] = b;  // R8G8B8A8 -> BMP BGRA
                    dst[x * 4 + 1] = g;
                    dst[x * 4 + 2] = r;
                    dst[x * 4 + 3] = 255;
                    if (std::abs(r - clearR) > 8 || std::abs(g - clearG) > 8)
                        ++g_readback.changed;
                }
            }
            context->Unmap(staging, 0);
        }
        staging->Release();
    }
    if (device)
        device->Release();
    back->Release();
}

bool g_useProbe = false;

void TestRender(int frames, bool flip) {
    Section(flip ? "D3D11 Present hook + ImGui (flip model)" : "D3D11 Present hook + ImGui");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DpsHarness";
    RegisterClassExW(&wc);
    // The overlay only attaches to windows titled like the game's.
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"MONSTER HUNTER: WORLD(421810) harness", WS_OVERLAPPEDWINDOW, 100, 100,
                                  960, 600, nullptr, nullptr, wc.hInstance, nullptr);
    Check(window != nullptr, "test window created");
    render::SetTargetWindow(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    RECT client{};
    GetClientRect(window, &client);

    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(d3d11, path, MAX_PATH);
    std::printf("  d3d11.dll: %s\n", WideToUtf8(path).c_str());

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = static_cast<UINT>(client.right - client.left);
    desc.BufferDesc.Height = static_cast<UINT>(client.bottom - client.top);
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain,
                              &device, &level, &context);
    Check(SUCCEEDED(hr) && swapChain && device, "D3D11 device + swap chain");
    if (FAILED(hr) || !swapChain) {
        std::printf("  hr = 0x%08lX\n", static_cast<unsigned long>(hr));
        return;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC adapterDesc{};
            adapter->GetDesc(&adapterDesc);
            std::printf("  adapter: %s, feature level 0x%x\n", WideToUtf8(adapterDesc.Description).c_str(), static_cast<int>(level));
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    if (!render::Installed()) {
        render::SetAfterDrawCallback(&AfterDraw);
        if (g_useProbe)
            Check(render::InstallViaDummyDevice(), "Present hooked through the persistent probe device");
        else
            Check(render::InstallForSwapChain(swapChain, "harness"), "Present/ResizeBuffers hooked (MinHook on this D3D implementation)");
    }

    // A fake hunt so the real overlay path draws party rows (with CJK names).
    meter::OverlayView view;
    view.show = true;
    view.elapsed = 312.0f;
    view.members = {{0, "ハンター", 41230, 132.1f, 48.1f, true},
                    {1, "사냥꾼", 23011, 73.8f, 26.8f, false},
                    {2, "猎人 Hunter", 15880, 50.9f, 18.5f, false},
                    {3, "Hunter%d", 5645, 18.1f, 6.6f, false}};
    view.total = 85766;
    view.showCarts = true;
    view.deaths = 1;
    view.maxDeaths = 3;
    view.monsters = {{"Barroth", 2156.0f, 2156.0f}, {"Rathian", 1540.0f, 3540.0f}, {"Shrieking Legiana", 610.0f, 4908.0f}};
    meter::Instance().PublishForTest(view);
    render::input::SetMenuOpen(true);

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (back) {
        device->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
    }

    const long long before = render::PresentCount();
    for (int i = 0; i < frames; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (flip && rtv) {
            // Flip model: buffer 0 always refers to the current back buffer in D3D11.
            context->OMSetRenderTargets(1, &rtv, nullptr);
        }
        if (rtv)
            context->ClearRenderTargetView(rtv, kClear);
        swapChain->Present(1, 0);
    }
    const long long presented = render::PresentCount() - before;
    Check(presented == frames, "every Present went through the hook");
    std::printf("  %s\n", render::Describe().c_str());

    if (!flip) {
        Check(g_readback.done && g_readback.width > 0, "back buffer read back after the overlay drew");
        std::printf("  %zu of %u pixels differ from the clear colour\n", g_readback.changed, g_readback.width * g_readback.height);
        Check(g_readback.changed > 5000, "overlay pixels present in the frame (F9 panel open)");
        if (!g_readback.pixels.empty()) {
            const std::wstring bmp = JoinPath(GameDirectory(), L"harness-panel.bmp");
            SaveBmp(bmp, g_readback.width, g_readback.height, g_readback.pixels);
            std::printf("  wrote %s\n", WideToUtf8(bmp).c_str());
        }

        // Normal play: panel closed, only the meter and monster bars.
        render::input::SetMenuOpen(false);
        g_readback = Readback{};
        g_readback.captureAt = 10;
        for (int i = 0; i < 15; ++i) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (rtv)
                context->ClearRenderTargetView(rtv, kClear);
            swapChain->Present(1, 0);
        }
        Check(g_readback.done && g_readback.changed > 2000, "meter and monster bars drawn with the panel closed");
        if (!g_readback.pixels.empty()) {
            const std::wstring bmp = JoinPath(GameDirectory(), L"harness-frame.bmp");
            SaveBmp(bmp, g_readback.width, g_readback.height, g_readback.pixels);
            std::printf("  wrote %s\n", WideToUtf8(bmp).c_str());
        }

        // Expedition with a friend: your hooked damage plus the estimated row.
        meter::OverlayView expedition;
        expedition.show = true;
        expedition.expedition = true;
        expedition.elapsed = 95.0f;
        expedition.members = {{0, "ハンター", 12400, 130.5f, 61.1f, true}, {1, "~사냥꾼", 7900, 83.2f, 38.9f, false}};
        expedition.total = 20300;
        expedition.hint = "Expedition  |  F7 resets  |  DPS while fighting\n~ = monster HP lost minus yours";
        expedition.monsters = {{"Rathalos", 3100.0f, 5200.0f}};
        meter::Instance().PublishForTest(expedition);
        g_readback = Readback{};
        g_readback.captureAt = 10;
        for (int i = 0; i < 15; ++i) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (rtv)
                context->ClearRenderTargetView(rtv, kClear);
            swapChain->Present(1, 0);
        }
        Check(g_readback.done && g_readback.changed > 2000, "expedition meter drawn");
        if (!g_readback.pixels.empty()) {
            const std::wstring bmp = JoinPath(GameDirectory(), L"harness-expedition.bmp");
            SaveBmp(bmp, g_readback.width, g_readback.height, g_readback.pixels);
            std::printf("  wrote %s\n", WideToUtf8(bmp).c_str());
        }
    }

    // Resize through the hook (the overlay must release its view first or this fails).
    const HRESULT resized = swapChain->ResizeBuffers(0, 800, 500, DXGI_FORMAT_UNKNOWN, 0);
    Check(SUCCEEDED(resized), "ResizeBuffers through the hook");
    if (rtv)
        rtv->Release();
    rtv = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (back) {
        device->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
    }
    for (int i = 0; i < 10; ++i) {
        if (rtv) {
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->ClearRenderTargetView(rtv, kClear);
        }
        swapChain->Present(1, 0);
    }
    Check(true, "frames after resize");

    render::input::SetMenuOpen(false);
    if (rtv)
        rtv->Release();
    context->ClearState();
    context->Release();
    swapChain->Release();
    device->Release();
    DestroyWindow(window);
}

}  // namespace

int main(int argc, char** argv) {
    log::Init(JoinPath(GameDirectory(), L"harness.log"));
    std::printf("MHW DPS Meter native harness %s\n", kBuildStamp);
    mem::Init();
    hooks::Init();
    EnsureDirectory(DataDirectory());
    settings::Load();

    bool render = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-render") == 0)
            render = false;
        if (std::strcmp(argv[i], "--probe") == 0)
            g_useProbe = true;
    }

    TestMemory();
    TestPatterns();
    TestRenderCapture();
    TestAddressMap();
    TestGameData();
    TestFightLogs();
    if (render) {
        TestRender(60, false);
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    std::printf("log: %s\n", WideToUtf8(log::Path()).c_str());
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
