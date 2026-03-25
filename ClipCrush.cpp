/*
  ClipCrush - Clipboard Video Compressor for Discord
  Build: g++ -O2 -o ClipCrush.exe ClipCrush.cpp -lole32 -lshell32 -luser32 -mwindows
  Or use install.bat — it does everything.
*/

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
using std::min;
using std::max;
#include <io.h>
#include <fcntl.h>
#include <mmsystem.h>

// ── Console output helpers (UTF-8 aware) ─────────────────────────────────────
static HANDLE hStdOut;

void cls() { 
    COORD c = {0,0}; 
    DWORD w; 
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hStdOut, &csbi);
    FillConsoleOutputCharacter(hStdOut, ' ', csbi.dwSize.X * csbi.dwSize.Y, c, &w);
    SetConsoleCursorPosition(hStdOut, c);
}

void gotoxy(int x, int y) {
    COORD c = {(SHORT)x, (SHORT)y};
    SetConsoleCursorPosition(hStdOut, c);
}

void setColor(int fg, int bg = 0) {
    SetConsoleTextAttribute(hStdOut, (WORD)(bg << 4 | fg));
}

void hideCursor() {
    CONSOLE_CURSOR_INFO ci = {1, FALSE};
    SetConsoleCursorInfo(hStdOut, &ci);
}

void showCursor() {
    CONSOLE_CURSOR_INFO ci = {1, TRUE};
    SetConsoleCursorInfo(hStdOut, &ci);
}

// ── Compression strategy ──────────────────────────────────────────────────────
struct Strategy {
    int    crf;
    bool   twoPass;
    double scale;    // 1.0 = original, 0.5 = half
    int    fps;      // 0 = original
    int    targetKb; // target bitrate for 2-pass (kbps), 0 = pure CRF
};

// Target: 9.5 MB = 9728 KB. We use 9.2 MB as the ceiling to stay under 10 MB.
static const double TARGET_MB = 9.2;
static const int    TARGET_KB = (int)(TARGET_MB * 1024);

Strategy pickStrategy(double sizeMB, double durationSec) {
    Strategy s;
    s.fps     = 0;
    s.scale   = 1.0;
    s.twoPass = true; // always 2-pass for size accuracy

    // ── Math-exact target ──────────────────────────────────────────────────
    // Reserve 96 kbps for audio, everything else goes to video
    static const int AUDIO_KBPS = 96;
    double totalKbps   = durationSec > 0 ? (TARGET_KB * 8.0) / durationSec : 2000;
    double videoKbps   = totalKbps - AUDIO_KBPS;

    // ── Resolution scaling based on how hard we need to squeeze ───────────
    // Below these video bitrate thresholds, drop resolution to keep quality
    // Rule of thumb: 1080p needs ~2000+ kbps, 720p ~1000+, 480p ~500+
    if (videoKbps < 350) {
        s.scale = 0.4;   // ~480p or below — extreme squeeze
        s.fps   = 20;
    } else if (videoKbps < 600) {
        s.scale = 0.5;   // ~480p
        s.fps   = 24;
    } else if (videoKbps < 1000) {
        s.scale = 0.75;  // ~720p
        s.fps   = 24;
    } else if (videoKbps < 1800) {
        s.scale = 0.75;  // ~720p, ok fps
    }
    // else: full resolution, original fps — bitrate is comfortable

    // ── CRF: used in pass 1 as a quality hint, pass 2 does the real work ──
    // Lower CRF = better quality skeleton for pass 2 to work from
    // We stay in the 20-26 range — no point going higher, pass 2 controls size
    if (videoKbps < 500)       s.crf = 26;
    else if (videoKbps < 1000) s.crf = 24;
    else if (videoKbps < 2000) s.crf = 22;
    else                       s.crf = 20;

    s.targetKb = (int)videoKbps;

    // ── Sanity clamps ──────────────────────────────────────────────────────
    if (s.targetKb < 150)  s.targetKb = 150;   // below this = unwatchable
    if (s.targetKb > 8000) s.targetKb = 8000;

    return s;
}

// ── FFmpeg helpers ────────────────────────────────────────────────────────────
std::wstring getExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::wstring ffmpegPath() {
    return getExeDir() + L"ffmpeg.exe";
}

// Run FFmpeg and parse duration from stderr for a probe
double probeDuration(const std::wstring& input) {
    std::wstring cmd = L"cmd /c \"\"" + ffmpegPath() + L"\" -i \"" + input + L"\"\" 2>&1";
    
    FILE* pipe = _wpopen(cmd.c_str(), L"r");
    if (!pipe) return 60.0;

    double dur = 60.0;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        // Parse "Duration: HH:MM:SS.ss"
        const char* d = strstr(buf, "Duration:");
        if (d) {
            int h, m; float sec;
            if (sscanf(d, "Duration: %d:%d:%f", &h, &m, &sec) == 3) {
                dur = h * 3600.0 + m * 60.0 + sec;
            }
        }
    }
    _pclose(pipe);
    return dur;
}

// ── Progress bar ──────────────────────────────────────────────────────────────
struct ProgressState {
    int    barY;
    double duration;
    double sizeMB;
    int    pass;
    int    totalPasses;
    std::wstring inputName;
    std::wstring tier;
};

void drawHeader(const ProgressState& ps) {
    setColor(15, 0); // bright white
    gotoxy(2, 1);
    printf("  ClipCrush  ");
    setColor(8, 0); // dark gray
    printf("v1.0");
    
    gotoxy(2, 3);
    setColor(7, 0);
    printf("File : ");
    setColor(11, 0); // bright cyan
    // Print filename (narrow to 50 chars)
    std::wstring name = ps.inputName;
    if (name.size() > 50) name = L"..." + name.substr(name.size() - 47);
    wprintf(L"%-52ls", name.c_str());
    
    gotoxy(2, 4);
    setColor(7, 0);
    printf("Size : ");
    setColor(14, 0); // yellow
    printf("%.1f MB", ps.sizeMB);
    setColor(7, 0);
    printf("  Strategy: ");
    setColor(13, 0); // magenta
    // wcout-safe print via WriteConsoleW
    WriteConsoleW(hStdOut, ps.tier.c_str(), (DWORD)ps.tier.size(), NULL, NULL);
    
    gotoxy(2, 5);
    setColor(7, 0);
    printf("Pass : ");
    setColor(14, 0);
    printf("%d / %d", ps.pass, ps.totalPasses);
    
    gotoxy(2, 7);
    setColor(8, 0);
    printf("  %-70s", ""); // clear line
}

static void drawBar(int y, double pct, double timeSec, double durationSec) {
    const int W = 50;
    int filled = (int)round(pct * W / 100.0);
    filled = max(0, min(W, filled));

    gotoxy(2, y);
    setColor(8, 0);
    printf("  [");
    for (int i = 0; i < W; i++) {
        if (i < filled) { setColor(10, 0); printf("|"); }  // green
        else            { setColor(8, 0);  printf("-"); }  // dark
    }
    setColor(8, 0);
    printf("] ");
    setColor(15, 0);
    printf("%5.1f%%", pct);
    
    // ETA
    if (pct > 0.5 && durationSec > 0) {
        double eta = (timeSec / (pct / 100.0)) - timeSec;
        gotoxy(2, y + 1);
        setColor(8, 0);
        printf("  Time: %.0fs / ETA: %.0fs     ", timeSec, eta > 0 ? eta : 0);
    }
}

// ── Run FFmpeg with live progress ─────────────────────────────────────────────
bool runFFmpeg(const std::wstring& args, ProgressState& ps) {
    std::wstring cmd = L"cmd /c \"\"" + ffmpegPath() + L"\" " + args + L"\" 2>&1";
    
    FILE* log = _wfopen((getExeDir() + L"clipcrush_debug.log").c_str(), L"a");
    if (log) { fwprintf(log, L"CMD: %ls\n\n", cmd.c_str()); fclose(log); }

    FILE* pipe = _wpopen(cmd.c_str(), L"r");
    if (!pipe) return false;

    FILE* log2 = _wfopen((getExeDir() + L"clipcrush_debug.log").c_str(), L"a");

    // Read char-by-char so we catch \r-terminated progress lines from FFmpeg
    char buf[2048];
    int  bufPos = 0;
    bool ok = false;
    double curTime = 0;
    int ch;

    while ((ch = fgetc(pipe)) != EOF) {
        if (ch == '\r' || ch == '\n') {
            if (bufPos > 0) {
                buf[bufPos] = '\0';
                if (log2) fprintf(log2, "%s\n", buf);

                const char* t = strstr(buf, "time=");
                if (t) {
                    int h, m; float sec;
                    if (sscanf(t, "time=%d:%d:%f", &h, &m, &sec) == 3) {
                        curTime = h * 3600.0 + m * 60.0 + sec;
                        double pct = ps.duration > 0
                            ? min(100.0, curTime / ps.duration * 100.0)
                            : 0;
                        drawBar(ps.barY, pct, curTime, ps.duration);
                    }
                }
                if (strstr(buf, "muxing overhead")) ok = true;
                bufPos = 0;
            }
        } else {
            if (bufPos < (int)sizeof(buf) - 1)
                buf[bufPos++] = (char)ch;
        }
    }

    if (log2) fclose(log2);
    int ret = _pclose(pipe);
    if (ret == 0) ok = true;
    return ok;
}

// ── Build FFmpeg filter chain ─────────────────────────────────────────────────
std::wstring buildVF(const Strategy& s) {
    std::wstring vf = L"";
    bool hasFilter = false;

    if (s.scale < 1.0) {
        // scale to % of original keeping aspect ratio
        wchar_t buf[64];
        swprintf(buf, 64, L"scale=trunc(iw*%.2f/2)*2:trunc(ih*%.2f/2)*2", s.scale, s.scale);
        vf += buf;
        hasFilter = true;
    }
    if (s.fps > 0) {
        if (hasFilter) vf += L",";
        wchar_t buf[32];
        swprintf(buf, 32, L"fps=%d", s.fps);
        vf += buf;
        hasFilter = true;
    }
    return hasFilter ? (L"-vf \"" + vf + L"\"") : L"";
}

// ── Compress ──────────────────────────────────────────────────────────────────
bool compress(const std::wstring& input, const std::wstring& output, 
              double sizeMB, ProgressState& ps) {
    double dur = probeDuration(input);
    ps.duration = dur;
    
    Strategy s = pickStrategy(sizeMB, dur);

    // Build tier description string
    wchar_t tierBuf[128];
    if (s.twoPass)
        swprintf(tierBuf, 128, L"2-pass VBR  %d kbps  CRF%d%s%s",
            s.targetKb, s.crf,
            s.scale < 1.0 ? L"  scale" : L"",
            s.fps > 0 ? L"  fps" : L"");
    else
        swprintf(tierBuf, 128, L"CRF %d  (single-pass)", s.crf);
    ps.tier = tierBuf;

    std::wstring vf = buildVF(s);

    // Null device for pass 1 log
    std::wstring nullOut = L"NUL";

    if (s.twoPass) {
        ps.totalPasses = 2;
        
        // Pass 1
        ps.pass = 1;
        drawHeader(ps);

        wchar_t p1[1024];
        swprintf(p1, 1024,
            L"-y -i \"%ls\" %ls "
            L"-c:v libx264 -crf %d -b:v %dk "
            L"-pass 1 -passlogfile \"%ls_log\" "
            L"-an -f null %ls",
            input.c_str(), vf.c_str(),
            s.crf, s.targetKb,
            output.c_str(), nullOut.c_str());

        bool ok1 = runFFmpeg(p1, ps);
        if (!ok1) return false;

        drawBar(ps.barY, 100.0, dur, dur);

        // Pass 2
        ps.pass = 2;
        drawHeader(ps);

        wchar_t p2[1024];
        swprintf(p2, 1024,
            L"-y -i \"%ls\" %ls "
            L"-map 0:v:0 -map 0:a:0 "
            L"-c:v libx264 -crf %d -b:v %dk "
            L"-pass 2 -passlogfile \"%ls_log\" "
            L"-c:a aac -b:a 96k "
            L"-movflags +faststart "
            L"\"%ls\"",
            input.c_str(), vf.c_str(),
            s.crf, s.targetKb,
            output.c_str(),
            output.c_str());

        return runFFmpeg(p2, ps);
    } else {
        ps.totalPasses = 1;
        ps.pass        = 1;
        drawHeader(ps);

        wchar_t cmd[1024];
        swprintf(cmd, 1024,
            L"-y -i \"%ls\" %ls "
            L"-map 0:v:0 -map 0:a:0 "
            L"-c:v libx264 -crf %d "
            L"-c:a aac -b:a 96k "
            L"-movflags +faststart "
            L"\"%ls\"",
            input.c_str(), vf.c_str(),
            s.crf,
            output.c_str());

        return runFFmpeg(cmd, ps);
    }
}

// ── Clipboard helpers ─────────────────────────────────────────────────────────
std::wstring getClipboardFile() {
    if (!OpenClipboard(NULL)) return L"";
    HANDLE h = GetClipboardData(CF_HDROP);
    if (!h) { CloseClipboard(); return L""; }

    HDROP drop = (HDROP)GlobalLock(h);
    if (!drop) { CloseClipboard(); return L""; }

    wchar_t buf[MAX_PATH] = {0};
    UINT cnt = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    std::wstring result;
    if (cnt > 0) {
        DragQueryFileW(drop, 0, buf, MAX_PATH);
        result = buf;
    }
    GlobalUnlock(h);
    CloseClipboard();
    return result;
}

bool isVideoFile(const std::wstring& path) {
    static const wchar_t* exts[] = {
        L".mp4", L".mkv", L".mov", L".avi", L".webm",
        L".flv", L".wmv", L".m4v", L".ts", L".3gp", nullptr
    };
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    for (int i = 0; exts[i]; i++) {
        if (lower.size() > wcslen(exts[i]) &&
            lower.compare(lower.size() - wcslen(exts[i]), wcslen(exts[i]), exts[i]) == 0)
            return true;
    }
    return false;
}

bool setClipboardFile(const std::wstring& path) {
    // Pack as CF_HDROP so Ctrl+V pastes the file
    size_t pathLen = path.size() + 1;
    size_t bufSize = sizeof(DROPFILES) + (pathLen + 1) * sizeof(wchar_t);

    HGLOBAL hg = GlobalAlloc(GHND, bufSize);
    if (!hg) return false;

    DROPFILES* df = (DROPFILES*)GlobalLock(hg);
    df->pFiles  = sizeof(DROPFILES);
    df->fWide   = TRUE;
    df->pt      = {0, 0};
    df->fNC     = FALSE;

    wchar_t* dest = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
    wcscpy(dest, path.c_str());
    dest[pathLen] = L'\0'; // double-null terminate

    GlobalUnlock(hg);

    if (!OpenClipboard(NULL)) { GlobalFree(hg); return false; }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hg);
    CloseClipboard();
    return true;
}

// ── Output path: same dir as input, suffix _compressed ───────────────────────
std::wstring makeOutputPath(const std::wstring& input) {
    size_t dot = input.find_last_of(L'.');
    std::wstring base = (dot != std::wstring::npos) ? input.substr(0, dot) : input;
    return base + L"_compressed.mp4";
}

// ── Bring console window to foreground ───────────────────────────────────────
void bringConsoleToFront() {
    // Give Windows a moment to create the console window
    Sleep(80);
    HWND hw = GetConsoleWindow();
    if (hw) {
        ShowWindow(hw, SW_RESTORE);
        SetForegroundWindow(hw);
        BringWindowToTop(hw);
        SetFocus(hw);
    }
}

// ── Hotkey message loop ───────────────────────────────────────────────────────
#define HOTKEY_ID 9001

void doCompress() {
    AllocConsole();
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    freopen("CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"ClipCrush - Debug");

    printf("  [DEBUG] Hotkey fired!\n");

    std::wstring videoPath = getClipboardFile();
    printf("  [DEBUG] Clipboard path: ");
    if (videoPath.empty()) {
        printf("(empty)\n");
    } else {
        wprintf(L"%ls\n", videoPath.c_str());
    }

    if (videoPath.empty() || !isVideoFile(videoPath)) {
        setColor(12, 0);
        puts("\n  [!] Clipboard does not contain a video file.");
        setColor(7, 0);
        puts("      Copy a video file first, then press Ctrl+Alt+Shift+V.\n");
        Sleep(4000);
        FreeConsole();
        return;
    }

    // ─ Get file size ─
    WIN32_FILE_ATTRIBUTE_DATA fa;
    GetFileAttributesExW(videoPath.c_str(), GetFileExInfoStandard, &fa);
    ULONGLONG bytes = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    double sizeMB = bytes / (1024.0 * 1024.0);

    // Already small enough
    if (sizeMB < 9.5) {
        AllocConsole();
        hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleOutputCP(CP_UTF8);
        setColor(10, 0);
        printf("\n  [OK] File is %.1f MB — already under 10 MB. Nothing to do.\n", sizeMB);
        setColor(7, 0);
        Sleep(2000);
        FreeConsole();
        return;
    }

    // ─ Open terminal window ─
    AllocConsole();
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    freopen("CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"ClipCrush");
    // Disable QuickEdit mode — clicking the console pauses pipe reads otherwise
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD consoleMode = 0;
    GetConsoleMode(hIn, &consoleMode);
    SetConsoleMode(hIn, consoleMode & ~ENABLE_QUICK_EDIT_MODE & ~ENABLE_MOUSE_INPUT);

    hideCursor();
    bringConsoleToFront();

    // Resize console
    SMALL_RECT rect = {0, 0, 79, 14};
    SetConsoleWindowInfo(hStdOut, TRUE, &rect);
    cls();

    // ─ Set up progress state ─
    ProgressState ps;
    ps.barY      = 9;
    ps.sizeMB    = sizeMB;
    ps.duration  = 0;
    ps.pass      = 1;
    ps.totalPasses = 1;

    // Extract just filename for display
    size_t sl = videoPath.find_last_of(L"\\/");
    ps.inputName = (sl != std::wstring::npos) ? videoPath.substr(sl + 1) : videoPath;
    ps.tier = L"Analyzing...";

    drawHeader(ps);

    // ─ Compress ─
    std::wstring output = makeOutputPath(videoPath);
    bool ok = compress(videoPath, output, sizeMB, ps);

    if (ok) {
        // Final progress bar at 100%
        drawBar(ps.barY, 100.0, ps.duration, ps.duration);

        // Show result
        WIN32_FILE_ATTRIBUTE_DATA fa2;
        GetFileAttributesExW(output.c_str(), GetFileExInfoStandard, &fa2);
        ULONGLONG outBytes = ((ULONGLONG)fa2.nFileSizeHigh << 32) | fa2.nFileSizeLow;
        double outMB = outBytes / (1024.0 * 1024.0);

        gotoxy(2, 12);
        setColor(10, 0);
        printf("  Done! %.1f MB -> %.1f MB  (saved %.0f%%)", 
            sizeMB, outMB, (1.0 - outMB / sizeMB) * 100.0);

        // ─ Copy to clipboard ─
        setClipboardFile(output);

        gotoxy(2, 13);
        if (outMB > 9.5) {
            setColor(12, 0);
            printf("  [!] Still %.1f MB — too big for Discord! Try a shorter clip.", outMB);
            MessageBeep(MB_ICONEXCLAMATION);
            Sleep(5000);
            // Delete temp file — too big, user can't use it anyway
            DeleteFileW(output.c_str());
        } else {
            setColor(11, 0);
            printf("  Copied to clipboard! Paste it wherever you need (Ctrl+V).");
            PlaySound(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC);
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
            nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
            nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
            wcscpy_s(nid.szTip,       L"ClipCrush");
            wcscpy_s(nid.szInfoTitle, L"ClipCrush \u2014 Done!");
            wchar_t balloon[128];
            swprintf(balloon, 128, L"%.1f MB \u2192 %.1f MB \u2014 ready to paste!", sizeMB, outMB);
            wcscpy_s(nid.szInfo, balloon);
            Shell_NotifyIconW(NIM_ADD,    &nid);
            Shell_NotifyIconW(NIM_DELETE, &nid);

            // ── Ask user what to do with the compressed file ──────────────────
            showCursor();
            gotoxy(2, 14);
            setColor(7, 0);
            printf("  Done pasting?  ");
            setColor(14, 0);
            printf("[1]");
            setColor(7, 0);
            printf(" Keep file   ");
            setColor(14, 0);
            printf("[2]");
            setColor(7, 0);
            printf(" Delete file  ");
            setColor(8, 0);
            printf("(press 1 or 2)");

            // Flush and wait for 1 or 2
            FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
            while (true) {
                HANDLE hIn2 = GetStdHandle(STD_INPUT_HANDLE);
                // Re-enable for reading, but keep QuickEdit off
                DWORD mode2 = 0;
                GetConsoleMode(hIn2, &mode2);
                SetConsoleMode(hIn2, (mode2 | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT) & ~ENABLE_QUICK_EDIT_MODE);

                INPUT_RECORD ir;
                DWORD read = 0;
                if (ReadConsoleInputW(hIn2, &ir, 1, &read) && read > 0) {
                    if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
                        WCHAR ch = ir.Event.KeyEvent.uChar.UnicodeChar;
                        if (ch == L'1') {
                            gotoxy(2, 15);
                            setColor(10, 0);
                            printf("  File kept. Closing...");
                            Sleep(1200);
                            break;
                        } else if (ch == L'2') {
                            DeleteFileW(output.c_str());
                            gotoxy(2, 15);
                            setColor(10, 0);
                            printf("  File deleted. Closing...");
                            Sleep(1200);
                            break;
                        }
                    }
                }
            }
            hideCursor();
        }
    } else {
        gotoxy(2, 12);
        setColor(12, 0);
        printf("  [ERROR] Compression failed. Check that FFmpeg is in the same folder.");
        Sleep(4000);
    }

    showCursor();
    FreeConsole();

    // Clean up 2-pass log files
    std::wstring logBase = output + L"_log";
    DeleteFileW((logBase + L"-0.log").c_str());
    DeleteFileW((logBase + L"-0.log.mbtree").c_str());
}

// ── WinMain ───────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Register Ctrl+Alt+Shift+V globally
    if (!RegisterHotKey(NULL, HOTKEY_ID,
        MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'V')) {
        MessageBoxW(NULL,
            L"Could not register hotkey Ctrl+Alt+Shift+V.\n"
            L"Another app may be using it.",
            L"ClipCrush", MB_ICONERROR);
        return 1;
    }

    // Tray icon (minimal — just a notification)
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_ID) {
            doCompress();
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(NULL, HOTKEY_ID);
    return 0;
}