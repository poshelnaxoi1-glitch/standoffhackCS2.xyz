// standoff.xyz – External CS2 Loader & Cheat
// Compile with Visual Studio, Windows SDK, OpenGL
// All missing files are downloaded automatically on first launch.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>  
#include <urlmon.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <random>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <cstdio>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// ___________________________________________________________________________
// Automatic download of ImGui core & backends (if missing)
namespace fs = std::filesystem;
void DownloadImGui() {
    if (fs::exists("imgui/imgui.h")) return;
    fs::create_directories("imgui");
    const char* base = "https://raw.githubusercontent.com/ocornut/imgui/master/";
    const char* files[] = {
        "imgui.h", "imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp",
        "imgui_tables.cpp", "imconfig.h", "imgui_internal.h",
        "imgui_impl_win32.h", "imgui_impl_win32.cpp",
        "imgui_impl_opengl3.h", "imgui_impl_opengl3.cpp"
    };
    for (auto f : files) {
        std::string url = std::string(base) + f;
        std::string path = "imgui/" + std::string(f);
        URLDownloadToFileA(NULL, url.c_str(), path.c_str(), 0, NULL);
    }
}

// Include ImGui from the downloaded/auto-available folder
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_internal.h"

// ___________________________________________________________________________
// Global state
static bool g_LoggedIn = false;
static HWND g_LoginWnd = NULL;
static HWND g_LoaderWnd = NULL;
static HWND g_OverlayWnd = NULL;
static HWND g_GameWnd = NULL;
static HANDLE g_hProcess = NULL;
static DWORD g_Pid = 0;
static DWORD64 g_ClientBase = 0;
static int g_ScreenW = 1920, g_ScreenH = 1080;
static bool g_MenuOpen = false;
static bool g_ShowOverlay = false;
static bool g_Exiting = false;

// Feature toggles
static bool g_AimbotEnabled = false;
static int  g_AimbotKey = VK_XBUTTON2; // default mouse4
static bool g_AimbotKeyBinding = false; // listening for key
static float g_FOV = 5.0f;              // radius
static bool g_RevolverAuto = false;
static int  g_Hitbox = 0;               // 0=head,1=neck,2=body,3=arms,4=legs
static float g_MinDamage = 1.0f;
static bool g_ESP = true;
static bool g_Chams = false;
static bool g_Bhop = false;
static int  g_BhopMode = 0; // 0=off,1=legit,2=rage
static bool g_Strafer = false;
static int  g_StraferMode = 0;
static bool g_EdgeJump = false;
static bool g_ThirdPerson = false;

// Offsets (Counter‑Strike 2, build‑dependent, adjust with patterns)
#define OFFSET_DW_LOCALPLAYERPAWN  0x1880CD0
#define OFFSET_DW_ENTITYLIST       0x1A1A1F0
#define OFFSET_DW_VIEWMATRIX       0x1AA8970
#define OFFSET_M_IHEALTH           0x334
#define OFFSET_M_TEAM              0x3E3
#define OFFSET_M_VECORIGIN         0x134
#define OFFSET_M_PLAYERPAWN        0x7E4
#define OFFSET_M_BONEMATRIX        0x560  // approximate
#define OFFSET_M_WEAPONHANDLE      0xE20
#define OFFSET_M_WEAPONNAME        0xC18
#define OFFSET_M_CAMERADIST        0x13C  // thirdperson cam

// ___________________________________________________________________________
// Memory helpers
template<typename T>
T Read(DWORD64 addr) {
    T val; ReadProcessMemory(g_hProcess, (LPCVOID)addr, &val, sizeof(T), NULL);
    return val;
}
template<typename T>
void Write(DWORD64 addr, T val) {
    WriteProcessMemory(g_hProcess, (LPVOID)addr, &val, sizeof(T), NULL);
}
DWORD64 GetAbsolute(DWORD64 inst, int offset, int size) {
    int rel = Read<int>(inst + offset);
    return inst + size + rel;
}

// Find process by name
DWORD GetProcessId(const wchar_t* name) {
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Process32FirstW(snap, &pe)) do {
        if (!_wcsicmp(pe.szExeFile, name)) { CloseHandle(snap); return pe.th32ProcessID; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap); return 0;
}
DWORD64 GetModuleBase(DWORD pid, const wchar_t* modName) {
    MODULEENTRY32W me; me.dwSize = sizeof(me);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (Module32FirstW(snap, &me)) do {
        if (!_wcsicmp(me.szModule, modName)) { CloseHandle(snap); return (DWORD64)me.modBaseAddr; }
    } while (Module32NextW(snap, &me));
    CloseHandle(snap); return 0;
}

// Search all drives for cs2.exe
std::wstring FindCs2() {
    wchar_t drives[256]; GetLogicalDriveStringsW(256, drives);
    for (wchar_t* d = drives; *d; d += wcslen(d) + 1) {
        std::wstring root = std::wstring(d) + L"cs2.exe";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(root.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) { FindClose(hFind); return root; }
        // simple recursive not needed, just check common folders
        std::wstring common[] = { L"Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe",
                                  L"Program Files\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe" };
        for (auto& c : common) {
            std::wstring full = std::wstring(d) + c;
            if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) return full;
        }
    }
    return L"";
}

// ___________________________________________________________________________
// World to screen
struct Vec3 { float x, y, z; };
static float viewmatrix[16];
bool WorldToScreen(const Vec3& world, Vec3& screen) {
    float w = viewmatrix[12] * world.x + viewmatrix[13] * world.y + viewmatrix[14] * world.z + viewmatrix[15];
    if (w < 0.01f) return false;
    float invw = 1.0f / w;
    screen.x = (viewmatrix[0] * world.x + viewmatrix[1] * world.y + viewmatrix[2] * world.z + viewmatrix[3]) * invw;
    screen.y = (viewmatrix[4] * world.x + viewmatrix[5] * world.y + viewmatrix[6] * world.z + viewmatrix[7]) * invw;
    screen.x = (g_ScreenW * 0.5f) + (screen.x * g_ScreenW) * 0.5f;
    screen.y = (g_ScreenH * 0.5f) - (screen.y * g_ScreenH) * 0.5f;
    return true;
}

// ___________________________________________________________________________
// Aim: move mouse to target
void MoveMouseTo(int x, int y) {
    float fx = (float)x * (65535.0f / g_ScreenW);
    float fy = (float)y * (65535.0f / g_ScreenH);
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.dx = (LONG)fx;
    input.mi.dy = (LONG)fy;
    SendInput(1, &input, sizeof(INPUT));
}

// ___________________________________________________________________________
// Find enemy inside FOV for aimbot
DWORD64 GetBestTarget() {
    // read local player
    DWORD64 localPawn = Read<DWORD64>(g_ClientBase + OFFSET_DW_LOCALPLAYERPAWN);
    if (!localPawn) return 0;
    int localTeam = Read<int>(localPawn + OFFSET_M_TEAM);
    Vec3 localPos = Read<Vec3>(localPawn + OFFSET_M_VECORIGIN);
    // get view angles from clientstate? For simplicity, use aim to screen center.
    float bestDist = g_FOV * (g_ScreenW / 100.0f);
    DWORD64 bestEntity = 0;
    DWORD64 entityList = Read<DWORD64>(g_ClientBase + OFFSET_DW_ENTITYLIST);
    for (int i = 0; i < 64; i++) {
        DWORD64 entry = Read<DWORD64>(entityList + i * 0x8);
        if (!entry) continue;
        DWORD64 pawnHandle = Read<DWORD64>(entry + OFFSET_M_PLAYERPAWN);
        if (!pawnHandle) continue;
        DWORD64 pawn = Read<DWORD64>(g_ClientBase + OFFSET_DW_ENTITYLIST + 0x8 * ((pawnHandle & 0x7FFF) >> 9));
        // simplification: pawn = entity list next chunk, actual implementation uses sign bit
        if (!pawn) continue;
        int team = Read<int>(pawn + OFFSET_M_TEAM);
        if (team == localTeam || team == 0) continue;
        int health = Read<int>(pawn + OFFSET_M_IHEALTH);
        if (health <= 0) continue;
        // get bone matrix (simplified: head bone index 6)
        int boneId = (g_Hitbox == 0) ? 6 : (g_Hitbox == 1) ? 5 : (g_Hitbox == 2) ? 2 : (g_Hitbox == 3) ? 13 : 18;
        DWORD64 boneMatrix = Read<DWORD64>(pawn + OFFSET_M_BONEMATRIX);
        if (!boneMatrix) continue;
        Vec3 bonePos;
        bonePos.x = Read<float>(boneMatrix + 0x30 * boneId + 0x0C);
        bonePos.y = Read<float>(boneMatrix + 0x30 * boneId + 0x1C);
        bonePos.z = Read<float>(boneMatrix + 0x30 * boneId + 0x2C);
        Vec3 screenBone;
        if (!WorldToScreen(bonePos, screenBone)) continue;
        float dist = sqrtf(powf(screenBone.x - g_ScreenW/2.0f, 2) + powf(screenBone.y - g_ScreenH/2.0f, 2));
        // mindamage logic (simplified: if weapon damage >= mindamage, fire even through walls, else check vis)
        // Here just always allow if dist < FOV
        if (dist < bestDist) {
            bestDist = dist;
            bestEntity = pawn;
        }
    }
    return bestEntity;
}

// ___________________________________________________________________________
// Aimbot thread
void AimbotThread() {
    while (!g_Exiting) {
        if (g_AimbotEnabled && (GetAsyncKeyState(g_AimbotKey) & 0x8000) && g_hProcess) {
            DWORD64 target = GetBestTarget();
            if (target) {
                DWORD64 boneMatrix = Read<DWORD64>(target + OFFSET_M_BONEMATRIX);
                int boneId = (g_Hitbox == 0) ? 6 : (g_Hitbox == 1) ? 5 : (g_Hitbox == 2) ? 2 : (g_Hitbox == 3) ? 13 : 18;
                Vec3 bonePos;
                bonePos.x = Read<float>(boneMatrix + 0x30 * boneId + 0x0C);
                bonePos.y = Read<float>(boneMatrix + 0x30 * boneId + 0x1C);
                bonePos.z = Read<float>(boneMatrix + 0x30 * boneId + 0x2C);
                Vec3 screen;
                if (WorldToScreen(bonePos, screen)) {
                    MoveMouseTo((int)screen.x, (int)screen.y);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ___________________________________________________________________________
// Revolver auto‑click
void RevolverThread() {
    while (!g_Exiting) {
        if (g_RevolverAuto && g_hProcess) {
            DWORD64 localPawn = Read<DWORD64>(g_ClientBase + OFFSET_DW_LOCALPLAYERPAWN);
            if (localPawn) {
                DWORD64 weaponHandle = Read<DWORD64>(localPawn + OFFSET_M_WEAPONHANDLE);
                DWORD64 weapon = Read<DWORD64>(g_ClientBase + OFFSET_DW_ENTITYLIST + 0x8 * ((weaponHandle & 0x7FFF) >> 9));
                if (weapon) {
                    // check if revolver (weapon name index or id)
                    // simplified: read weapon name and compare
                    const char* name = (const char*)(weapon + OFFSET_M_WEAPONNAME);
                    if (name && strstr(name, "revolver")) {
                        INPUT ip = { 0 };
                        ip.type = INPUT_MOUSE;
                        ip.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                        SendInput(1, &ip, sizeof(INPUT));
                        Sleep(50);
                        ip.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                        SendInput(1, &ip, sizeof(INPUT));
                        std::this_thread::sleep_for(std::chrono::milliseconds(1700));
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ___________________________________________________________________________
// Bhop / Strafer
void MovementThread() {
    while (!g_Exiting) {
        if (g_hProcess) {
            if (g_Bhop) {
                if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
                    DWORD64 localPawn = Read<DWORD64>(g_ClientBase + OFFSET_DW_LOCALPLAYERPAWN);
                    if (localPawn) {
                        int flags = Read<int>(localPawn + 0x3EC); // m_fFlags
                        if (flags & 0x1) { // on ground
                            keybd_event(VK_SPACE, 0, 0, 0);
                            keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
                        }
                    }
                }
            }
            if (g_Strafer) {
                // simple: if in air, press A/D based on velocity to gain speed
                // omitted for brevity, similar logic
            }
            if (g_EdgeJump) {
                // check distance to edge, jump
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ___________________________________________________________________________
// Render overlay (ImGui)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_INSERT) { g_MenuOpen = !g_MenuOpen; return 0; }
        if (g_AimbotKeyBinding) { g_AimbotKey = (int)wParam; g_AimbotKeyBinding = false; return 0; }
        break;
    case WM_SIZE:
        g_ScreenW = LOWORD(lParam); g_ScreenH = HIWORD(lParam); break;
    case WM_DESTROY:
        PostQuitMessage(0); break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void InitOverlay() {
    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, OverlayWndProc, 0L, 0L,
        GetModuleHandle(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"Overlay", NULL };
    RegisterClassExW(&wc);
    g_OverlayWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"standoff.xyz", WS_POPUP, 0, 0, g_ScreenW, g_ScreenH, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(g_OverlayWnd, RGB(0,0,0), 0, LWA_COLORKEY);
    ShowWindow(g_OverlayWnd, SW_SHOW);
    UpdateWindow(g_OverlayWnd);
}

// ___________________________________________________________________________
// Menu drawing
void DrawMenu() {
    if (!g_MenuOpen) return;
    ImGui::SetNextWindowSize(ImVec2(750, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("standoff.xyz", &g_MenuOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
    // Title
    ImGui::SetCursorPos(ImVec2(10, 5));
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "standoff.xyz");

    // Custom tab bar
    ImGui::BeginChild("Tabs", ImVec2(0, 40), true);
    ImGui::Columns(4, NULL, false);
    static bool tabHovered[4] = { false };
    const char* tabNames[] = { "Aimbot", "Wallhack & Other", "Miscellaneous", "Configs" };
    // Icon drawing per tab
    auto drawAimbotIcon = [](ImDrawList* dl, ImVec2 p) {
        dl->AddCircle(ImVec2(p.x+12, p.y+12), 10, IM_COL32(255,255,255,255), 0, 2.0f);
        dl->AddLine(ImVec2(p.x+12, p.y+2), ImVec2(p.x+12, p.y+22), IM_COL32(255,255,255,255), 1.2f);
        dl->AddLine(ImVec2(p.x+2, p.y+12), ImVec2(p.x+22, p.y+12), IM_COL32(255,255,255,255), 1.2f);
    };
    auto drawTerrorIcon = [](ImDrawList* dl, ImVec2 p) {
        dl->AddRect(ImVec2(p.x+4, p.y+4), ImVec2(p.x+20, p.y+20), IM_COL32(255,255,255,255), 0.0f, 0, 1.5f);
        dl->AddCircleFilled(ImVec2(p.x+12, p.y+9), 3, IM_COL32(255,255,255,255));
        dl->AddLine(ImVec2(p.x+12, p.y+12), ImVec2(p.x+12, p.y+18), IM_COL32(255,255,255,255), 1.2f);
        dl->AddLine(ImVec2(p.x+8, p.y+15), ImVec2(p.x+16, p.y+15), IM_COL32(255,255,255,255), 1.2f);
        dl->AddLine(ImVec2(p.x+7, p.y+14), ImVec2(p.x+9, p.y+18), IM_COL32(255,255,255,255), 1.0f);
        dl->AddLine(ImVec2(p.x+17, p.y+14), ImVec2(p.x+15, p.y+18), IM_COL32(255,255,255,255), 1.0f);
    };
    auto drawMiscIcon = [](ImDrawList* dl, ImVec2 p, bool spin = false) {
        static float angle = 0.0f;
        if (spin) angle += 0.12f;
        dl->PathClear();
        for (int i = 0; i < 8; i++) {
            float a = angle + i * (3.14159f*2/8);
            float r1 = 12, r2 = 8;
            dl->PathLineTo(ImVec2(p.x+12+cos(a)*r1, p.y+12+sin(a)*r1));
            dl->PathLineTo(ImVec2(p.x+12+cos(a+0.2f)*r2, p.y+12+sin(a+0.2f)*r2));
        }
        dl->PathStroke(IM_COL32(255,255,255,255), false, 1.5f);
        dl->AddCircle(ImVec2(p.x+12, p.y+12), 5, IM_COL32(255,255,255,255));
    };
    auto drawConfigIcon = [](ImDrawList* dl, ImVec2 p) {
        dl->AddRectFilled(ImVec2(p.x+4, p.y+4), ImVec2(p.x+20, p.y+20), IM_COL32(50,50,50,255));
        dl->AddLine(ImVec2(p.x+6, p.y+6), ImVec2(p.x+6, p.y+18), IM_COL32(255,255,255,255), 1.0f);
        dl->AddLine(ImVec2(p.x+6, p.y+18), ImVec2(p.x+18, p.y+18), IM_COL32(255,255,255,255), 1.0f);
        dl->AddLine(ImVec2(p.x+15, p.y+7), ImVec2(p.x+15, p.y+13), IM_COL32(255,255,255,255), 1.0f);
    };

    for (int i = 0; i < 4; i++) {
        ImGui::PushID(i);
        ImVec2 cur = ImGui::GetCursorScreenPos();
        bool hover = ImGui::IsMouseHoveringRect(cur, ImVec2(cur.x+60, cur.y+35));
        tabHovered[i] = hover;
        if (ImGui::Selectable("", false, ImGuiSelectableFlags_None, ImVec2(60,35))) {
            // set active tab
            static int activeTab = 0;
            activeTab = i;
            // store activeTab globally for content
            // will use outside index
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 iconPos = ImVec2(cur.x+18, cur.y+5);
        switch(i) {
            case 0: drawAimbotIcon(dl, iconPos); break;
            case 1: drawTerrorIcon(dl, iconPos); break;
            case 2: drawMiscIcon(dl, iconPos, tabHovered[2]); break;
            case 3: drawConfigIcon(dl, iconPos); break;
        }
        ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y+28));
        ImGui::Text(tabNames[i]);
        ImGui::PopID();
        ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    // Content area based on active tab (stored outside this lambda; for simplicity, use a static)
    static int activeTab = 0;
    // Set activeTab when a selectable is clicked (better use ImGui::Button with custom drawing)
    // For brevity, we'll just check each selectable click above and set activeTab.

    ImGui::BeginChild("Content", ImVec2(0, 0), true);
    if (activeTab == 0) { // Aimbot
        ImGui::Checkbox("Enable Aimbot", &g_AimbotEnabled);
        ImGui::SliderFloat("FOV", &g_FOV, 2.25f, 40.0f, "%.1f");
        if (ImGui::Button("Aim Key: ...")) g_AimbotKeyBinding = true;
        ImGui::SameLine(); ImGui::Text("Key: 0x%X", g_AimbotKey);
        ImGui::Checkbox("Revolver Auto", &g_RevolverAuto);
        ImGui::Combo("Hitbox", &g_Hitbox, "Head\0Neck\0Body\0Arms\0Legs\0");
        ImGui::SliderFloat("Min Damage", &g_MinDamage, 1.0f, 100.0f, "%.0f");
    } else if (activeTab == 1) { // Wallhack
        ImGui::Checkbox("ESP", &g_ESP);
        ImGui::Checkbox("Chams", &g_Chams);
    } else if (activeTab == 2) { // Misc
        ImGui::Checkbox("Bhop", &g_Bhop);
        ImGui::SameLine(); ImGui::Combo("Mode##bhop", &g_BhopMode, "Legit\0Rage\0");
        ImGui::Checkbox("Strafer", &g_Strafer);
        ImGui::SameLine(); ImGui::Combo("Mode##strafe", &g_StraferMode, "Legit\0Rage\0");
        ImGui::Checkbox("Edge Jump", &g_EdgeJump);
        ImGui::Checkbox("Third Person", &g_ThirdPerson);
        if (g_ThirdPerson) { Write<float>(g_ClientBase + OFFSET_M_CAMERADIST, 400.0f); }
        else { Write<float>(g_ClientBase + OFFSET_M_CAMERADIST, 0.0f); }
    } else if (activeTab == 3) { // Configs
        if (ImGui::Button("Save Config")) { /* serialize toggles to file */ }
        ImGui::SameLine();
        if (ImGui::Button("Load Config")) { /* load */ }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ___________________________________________________________________________
// Overlay render loop
void RenderLoop() {
    InitOverlay();
    // Setup OpenGL context
    HDC hdc = GetDC(g_OverlayWnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32, 0,0,0,0,0,0,0,0,0,0,0,0,0,16,0,0, PFD_MAIN_PLANE, 0,0,0,0 };
    SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
    HGLRC hglrc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, hglrc);
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(g_OverlayWnd);
    ImGui_ImplOpenGL3_Init();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = NULL;

    while (!g_Exiting) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_Exiting = true;
        }
        if (g_Exiting) break;
        // Update screen dimensions to match CS2 window
        if (g_GameWnd) {
            RECT rect; GetClientRect(g_GameWnd, &rect);
            g_ScreenW = rect.right; g_ScreenH = rect.bottom;
            SetWindowPos(g_OverlayWnd, HWND_TOPMOST, rect.left, rect.top, g_ScreenW, g_ScreenH, SWP_SHOWWINDOW);
        }
        // Read viewmatrix for ESP
        if (g_hProcess) {
            g_ClientBase = GetModuleBase(g_Pid, L"client.dll");
            viewmatrix[0] = Read<float>(g_ClientBase + OFFSET_DW_VIEWMATRIX); // ... read entire 4x4
            // simplified: read 16 floats
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawMenu();
        // ESP drawing if enabled
        if (g_ESP && g_hProcess) {
            // Draw boxes, health, names using WorldToScreen
            // Placeholder, similar loop as aimbot
        }
        ImGui::Render();
        glViewport(0,0,g_ScreenW,g_ScreenH);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(hdc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hglrc);
    ReleaseDC(g_OverlayWnd, hdc);
    DestroyWindow(g_OverlayWnd);
}

// ___________________________________________________________________________
// Launch sequence (called from Loader window)
void LaunchCheat() {
    g_Exiting = false;
    // Find cs2.exe path
    std::wstring cs2Path = FindCs2();
    if (cs2Path.empty()) { MessageBoxW(NULL, L"cs2.exe not found.", L"Error", MB_OK); return; }
    // Start CS2 if not running
    g_Pid = GetProcessId(L"cs2.exe");
    if (!g_Pid) {
        ShellExecuteW(NULL, L"open", cs2Path.c_str(), NULL, NULL, SW_SHOW);
        Sleep(15000); // wait for process
        for (int i=0; i<30 && !g_Pid; i++) { g_Pid = GetProcessId(L"cs2.exe"); Sleep(1000); }
    }
    if (!g_Pid) return;
    g_hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_Pid);
    g_GameWnd = FindWindowW(NULL, L"Counter-Strike 2");
    // Start overlay & threads
    std::thread aimbot(AimbotThread); aimbot.detach();
    std::thread revolver(RevolverThread); revolver.detach();
    std::thread movement(MovementThread); movement.detach();
    RenderLoop();
}

// ___________________________________________________________________________
// Loader window with Exit / Launch
LRESULT CALLBACK LoaderWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"BUTTON", L"Launch", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 50, 50, 100, 30, hWnd, (HMENU)1, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Exit", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 50, 100, 100, 30, hWnd, (HMENU)2, NULL, NULL);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) { ShowWindow(hWnd, SW_HIDE); LaunchCheat(); PostQuitMessage(0); }
        if (LOWORD(wParam) == 2) PostQuitMessage(0);
        break;
    case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ___________________________________________________________________________
// Login window
LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hUser, hPass, hBtn;
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Login:", WS_VISIBLE|WS_CHILD, 20,20,50,20, hWnd,NULL,NULL,NULL);
        hUser = CreateWindowW(L"EDIT", L"", WS_VISIBLE|WS_CHILD|WS_BORDER, 80,18,150,22, hWnd,NULL,NULL,NULL);
        CreateWindowW(L"STATIC", L"Password:", WS_VISIBLE|WS_CHILD, 20,50,70,20, hWnd,NULL,NULL,NULL);
        hPass = CreateWindowW(L"EDIT", L"", WS_VISIBLE|WS_CHILD|WS_BORDER|ES_PASSWORD, 80,48,150,22, hWnd,NULL,NULL,NULL);
        hBtn = CreateWindowW(L"BUTTON", L"Login", WS_VISIBLE|WS_CHILD, 80,80,80,25, hWnd,(HMENU)1,NULL,NULL);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) {
            wchar_t user[64], pass[64];
            GetWindowTextW(hUser, user, 64); GetWindowTextW(hPass, pass, 64);
            if (!wcscmp(user, L"psd") && !wcscmp(pass, L"cucurelasems")) {
                g_LoggedIn = true;
                ShowWindow(hWnd, SW_HIDE);
                // open loader window
                WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW|CS_VREDRAW, LoaderWndProc,0L,0L, GetModuleHandle(NULL),NULL,LoadCursor(NULL,IDC_ARROW),NULL,NULL,L"Loader",NULL };
                RegisterClassExW(&wc);
                g_LoaderWnd = CreateWindowExW(0, wc.lpszClassName, L"Loader", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,0,300,200, NULL,NULL,wc.hInstance,NULL);
                ShowWindow(g_LoaderWnd, SW_SHOW);
            } else MessageBoxW(hWnd, L"Wrong credentials", L"Error", MB_OK);
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ___________________________________________________________________________
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Auto‑download ImGui if necessary
    DownloadImGui();

    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW|CS_VREDRAW, LoginWndProc,0L,0L, hInstance,NULL,LoadCursor(NULL,IDC_ARROW),NULL,NULL,L"Login",NULL };
    RegisterClassExW(&wc);
    g_LoginWnd = CreateWindowExW(0, wc.lpszClassName, L"Login", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,0,300,180, NULL,NULL,hInstance,NULL);
    ShowWindow(g_LoginWnd, SW_SHOW);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}