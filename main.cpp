#include <windows.h>
#include <mmsystem.h>
#include <string>
#include <vector>

bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool IsNextSceneTriggered() {
    if ((GetAsyncKeyState(VK_RETURN) & 0x8000) ||
        (GetAsyncKeyState(VK_SPACE) & 0x8000) ||
        (GetAsyncKeyState(VK_RIGHT) & 0x8000)) {
        return true;
    }

    JOYINFOEX joy;
    joy.dwSize = sizeof(JOYINFOEX);
    joy.dwFlags = JOY_RETURNBUTTONS;
    if (joyGetPosEx(JOYSTICKID1, &joy) == JOYERR_NOERROR) {
        if (joy.dwButtons != 0) {
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.find_last_of("\\/"));

    std::vector<std::string> playlist;
    bool hasIntro = false;

    std::string introPath = dir + "\\intro.mp4";
    if (FileExists(introPath)) {
        playlist.push_back(introPath);
        hasIntro = true;
    }

    for (int i = 1; i <= 50; ++i) {
        std::string gpPath = dir + "\\gameplay" + std::to_string(i) + ".mp4";
        if (FileExists(gpPath)) {
            playlist.push_back(gpPath);
        }
    }

    if (playlist.empty()) {
        return 0;
    }

    // Buat layar hitam full-screen di belakang video agar transisi antar klip mulus
    WNDCLASSA wc = {};
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "GTA6Backdrop";
    RegisterClassA(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    HWND hBackdrop = CreateWindowExA(
        0, "GTA6Backdrop", "Grand Theft Auto VI",
        WS_POPUP | WS_VISIBLE, 0, 0, screenW, screenH,
        NULL, NULL, hInstance, NULL
    );

    SetEnvironmentVariableA("SDL_RENDER_DRIVER", "software");
    SetEnvironmentVariableA("SDL_AUDIODRIVER", "directsound");

    std::string ffplay = dir + "\\ffplay.exe";
    size_t idx = 0;
    ULONGLONG lastSwitchTime = GetTickCount64();

    while (true) {
        std::string cmd = "\"" + ffplay + "\" -fs -noborder -autoexit -loglevel quiet \"" + playlist[idx] + "\"";
        std::vector<char> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back('\0');

        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;

        if (CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir.c_str(), &si, &pi)) {
            while (WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT) {
                MSG msg;
                while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }

                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    TerminateProcess(pi.hProcess, 0);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    DestroyWindow(hBackdrop);
                    return 0;
                }

                if ((GetTickCount64() - lastSwitchTime > 600) && IsNextSceneTriggered()) {
                    lastSwitchTime = GetTickCount64();
                    TerminateProcess(pi.hProcess, 0);
                    WaitForSingleObject(pi.hProcess, 500);
                    break;
                }
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        idx++;
        if (idx >= playlist.size()) {
            idx = (hasIntro && playlist.size() > 1) ? 1 : 0;
        }
    }

    DestroyWindow(hBackdrop);
    return 0;
}
