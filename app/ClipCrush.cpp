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
using std::max;
using std::min;
#include <io.h>
#include <fcntl.h>
#include <mmsystem.h>

// ── Console output helpers (UTF-8 aware) ─────────────────────────────────────
static HANDLE hStdOut;

void cls()
{
    COORD c = {0, 0};
    DWORD w;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hStdOut, &csbi);
    FillConsoleOutputCharacter(hStdOut, ' ', csbi.dwSize.X * csbi.dwSize.Y, c, &w);
    SetConsoleCursorPosition(hStdOut, c);
}

void gotoxy(int x, int y)
{
    COORD c = {(SHORT)x, (SHORT)y};
    SetConsoleCursorPosition(hStdOut, c);
}

void setColor(int fg, int bg = 0)
{
    SetConsoleTextAttribute(hStdOut, (WORD)(bg << 4 | fg));
}

void hideCursor()
{
    CONSOLE_CURSOR_INFO ci = {1, FALSE};
    SetConsoleCursorInfo(hStdOut, &ci);
}

void showCursor()
{
    CONSOLE_CURSOR_INFO ci = {1, TRUE};
    SetConsoleCursorInfo(hStdOut, &ci);
}

// ── Compression strategy ──────────────────────────────────────────────────────
struct Strategy
{
    int crf;
    bool twoPass;
    double scale; // 1.0 = original, 0.5 = half
    int fps;      // 0 = original
    int targetKb; // target bitrate for 2-pass (kbps), 0 = pure CRF
};

// Target: 9.5 MB = 9728 KB. We use 9.2 MB as the ceiling to stay under 10 MB.
static const double TARGET_MB = 9.2;
static const int TARGET_KB = (int)(TARGET_MB * 1024);

Strategy pickStrategy(double sizeMB, double durationSec)
{
    Strategy s;
    s.fps = 0;
    s.scale = 1.0;
    s.twoPass = true; // always 2-pass for size accuracy

    // If file is already close to target, use a gentler single-pass CRF encode
    // instead of hammering it with 2-pass VBR
    if (sizeMB < TARGET_MB * 1.4)
    {
        s.twoPass = false;
        s.crf = 22;
        s.targetKb = 0;
        return s;
    }

    // ── Math-exact target ──────────────────────────────────────────────────
    // Reserve 96 kbps for audio, everything else goes to video
    static const int AUDIO_KBPS = 96;
    double totalKbps = durationSec > 0 ? (TARGET_KB * 8.0) / durationSec : 2000;
    double videoKbps = totalKbps - AUDIO_KBPS;

    // Factor in source bitrate — if source is already low bitrate,
    // don't blindly scale resolution; the problem is duration not density
    double srcKbps = (sizeMB > 0 && durationSec > 0)
                         ? (sizeMB * 1024.0 * 8.0) / durationSec
                         : 9999;
    // If source bitrate is already near target, trust CRF more than VBR math
    if (srcKbps < videoKbps * 1.3)
        videoKbps = srcKbps * 0.85;

    // ── Resolution scaling based on how hard we need to squeeze ───────────
    // Below these video bitrate thresholds, drop resolution to keep quality
    // Rule of thumb: 1080p needs ~2000+ kbps, 720p ~1000+, 480p ~500+
    if (videoKbps < 350)
    {
        s.scale = 0.4; // ~480p or below — extreme squeeze
        s.fps = 20;
    }
    else if (videoKbps < 600)
    {
        s.scale = 0.5; // ~480p
        s.fps = 24;
    }
    else if (videoKbps < 1000)
    {
        s.scale = 0.75; // ~720p
        s.fps = 24;
    }
    else if (videoKbps < 1800)
    {
        s.scale = 0.75; // ~720p, ok fps
    }
    // else: full resolution, original fps — bitrate is comfortable

    // ── CRF: used in pass 1 as a quality hint, pass 2 does the real work ──
    // Lower CRF = better quality skeleton for pass 2 to work from
    // We stay in the 20-26 range — no point going higher, pass 2 controls size
    if (videoKbps < 500)
        s.crf = 26;
    else if (videoKbps < 1000)
        s.crf = 24;
    else if (videoKbps < 2000)
        s.crf = 22;
    else
        s.crf = 20;

    s.targetKb = (int)videoKbps;

    // ── Sanity clamps ──────────────────────────────────────────────────────
    if (s.targetKb < 150)
        s.targetKb = 150; // below this = unwatchable
    if (s.targetKb > 8000)
        s.targetKb = 8000;

    return s;
}

// ── FFmpeg helpers ────────────────────────────────────────────────────────────
std::wstring getExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::wstring ffmpegPath()
{
    return getExeDir() + L"ffmpeg.exe";
}

// Run FFmpeg and parse duration from stderr for a probe
double probeDuration(const std::wstring &input)
{
    std::wstring cmd = L"cmd /c \"\"" + ffmpegPath() + L"\" -i \"" + input + L"\"\" 2>&1";

    FILE *pipe = _wpopen(cmd.c_str(), L"r");
    if (!pipe)
        return 60.0;

    double dur = 60.0;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe))
    {
        // Parse "Duration: HH:MM:SS.ss"
        const char *d = strstr(buf, "Duration:");
        if (d)
        {
            int h, m;
            float sec;
            if (sscanf(d, "Duration: %d:%d:%f", &h, &m, &sec) == 3)
            {
                dur = h * 3600.0 + m * 60.0 + sec;
            }
        }
    }
    _pclose(pipe);
    return dur;
}

// ── Progress bar ──────────────────────────────────────────────────────────────
struct ProgressState
{
    int barY;
    double duration;
    double sizeMB;
    int pass;
    int totalPasses;
    std::wstring inputName;
    std::wstring tier;
    // Unified progress tracking
    double wallStart;  // GetTickCount64 at compression start
    double passOffset; // how much 0-100% progress pass 1 contributes
};

static volatile HANDLE gFfmpegProcess = NULL;
static volatile bool gUserCancelled = false;

static double tickNow()
{
    return (double)GetTickCount64() / 1000.0;
}

void drawFullHeader(const ProgressState &ps)
{
    setColor(15, 0);
    gotoxy(2, 1);
    printf("  ClipCrush  ");
    setColor(8, 0);
    printf("v1.0");

    gotoxy(2, 3);
    setColor(7, 0);
    printf("File : ");
    setColor(11, 0);
    std::wstring name = ps.inputName;
    if (name.size() > 50)
        name = L"..." + name.substr(name.size() - 47);
    wprintf(L"%-52ls", name.c_str());

    gotoxy(2, 4);
    setColor(7, 0);
    printf("Size : ");
    setColor(14, 0);
    printf("%.1f MB", ps.sizeMB);
}

void drawHeader(const ProgressState &ps)
{
    // Updates only the live fields — doesn't touch the info block rows
    gotoxy(2, 4);
    setColor(7, 0);
    printf("Size : ");
    setColor(14, 0);
    printf("%.1f MB", ps.sizeMB);
    setColor(7, 0);
    printf("  Strategy: ");
    setColor(13, 0);
    WriteConsoleW(hStdOut, ps.tier.c_str(), (DWORD)ps.tier.size(), NULL, NULL);
    printf("          ");

    gotoxy(2, 5);
    setColor(7, 0);
    printf("Pass : ");
    setColor(14, 0);
    printf("%d / %d", ps.pass, ps.totalPasses);
}

static void drawBar(int y, double unifiedPct, double passPct, int pass, int totalPasses, double wallElapsed, double estTotal)
{
    const int W = 46;

    // ── Per-pass bar ──
    gotoxy(2, y);
    setColor(8, 0);
    printf("  Pass %d/%d [", pass, totalPasses);
    int filledPass = (int)round(passPct * W / 100.0);
    filledPass = max(0, min(W, filledPass));
    for (int i = 0; i < W; i++)
    {
        if (i < filledPass)
        {
            setColor(13, 0);
            printf("|");
        } // magenta
        else
        {
            setColor(8, 0);
            printf("-");
        }
    }
    setColor(8, 0);
    printf("] ");
    setColor(15, 0);
    printf("%5.1f%%", passPct);

    // ── Unified bar ──
    gotoxy(2, y + 2);
    setColor(8, 0);
    printf("  Overall  [");
    int filledUni = (int)round(unifiedPct * W / 100.0);
    filledUni = max(0, min(W, filledUni));
    for (int i = 0; i < W; i++)
    {
        if (i < filledUni)
        {
            setColor(10, 0);
            printf("|");
        } // green
        else
        {
            setColor(8, 0);
            printf("-");
        }
    }
    setColor(8, 0);
    printf("] ");
    setColor(15, 0);
    printf("%5.1f%%", unifiedPct);

    // ── ETA line ──
    gotoxy(2, y + 4);
    if (unifiedPct > 2.0 && estTotal > 0)
    {
        double eta = estTotal - wallElapsed;
        if (eta < 0)
            eta = 0;
        setColor(8, 0);
        printf("  Elapsed: %.0fs / ETA: %.0fs     ", wallElapsed, eta);
    }
    else
    {
        setColor(8, 0);
        printf("  Elapsed: %.0fs                  ", wallElapsed);
    }
}

// ── Run FFmpeg with live progress ─────────────────────────────────────────────
bool runFFmpeg(const std::wstring &args, ProgressState &ps, double localStart, double localEnd)
{
    std::wstring cmd = L"\"" + ffmpegPath() + L"\" " + args;

    // Create pipes for stdout+stderr
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::wstring cmdMut = cmd; // CreateProcessW needs non-const buffer
    if (!CreateProcessW(NULL, &cmdMut[0], NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return false;
    }
    CloseHandle(hWritePipe); // close our copy so ReadFile gets EOF when process ends
    InterlockedExchangePointer((volatile PVOID *)&gFfmpegProcess, pi.hProcess);

    char buf[2048];
    int bufPos = 0;
    bool ok = false;

    static const int SAMPLES = 12;
    double sampleTime[SAMPLES] = {};
    double samplePct[SAMPLES] = {};
    int sampleIdx = 0;
    int sampleCount = 0;

    char ch;
    DWORD bytesRead;
    while (ReadFile(hReadPipe, &ch, 1, &bytesRead, NULL) && bytesRead > 0)
    {
        if (ch == '\r' || ch == '\n')
        {
            if (bufPos > 0)
            {
                buf[bufPos] = '\0';

                const char *t = strstr(buf, "time=");
                if (t)
                {
                    int h, m;
                    float sec;
                    if (sscanf(t, "time=%d:%d:%f", &h, &m, &sec) == 3)
                    {
                        double curTime = h * 3600.0 + m * 60.0 + sec;
                        double localPct = ps.duration > 0
                                              ? min(100.0, curTime / ps.duration * 100.0)
                                              : 0;
                        double unified = localStart + (localPct / 100.0) * (localEnd - localStart);
                        unified = min(unified, localEnd);

                        double wallNow = tickNow();
                        double wallElapsed = wallNow - ps.wallStart;

                        sampleTime[sampleIdx] = wallNow;
                        samplePct[sampleIdx] = unified;
                        sampleIdx = (sampleIdx + 1) % SAMPLES;
                        if (sampleCount < SAMPLES)
                            sampleCount++;

                        double estTotal = 0;
                        if (sampleCount >= 4 && unified > 2.0)
                        {
                            int n = sampleCount;
                            double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
                            for (int i = 0; i < n; i++)
                            {
                                int idx = (sampleIdx - n + i + SAMPLES) % SAMPLES;
                                double x = samplePct[idx], y = sampleTime[idx];
                                sumX += x;
                                sumY += y;
                                sumXX += x * x;
                                sumXY += x * y;
                            }
                            double denom = n * sumXX - sumX * sumX;
                            if (fabs(denom) > 1e-9)
                            {
                                double a = (n * sumXY - sumX * sumY) / denom;
                                double b = (sumY - a * sumX) / n;
                                estTotal = (a * 100.0 + b) - ps.wallStart;
                            }
                        }

                        drawBar(ps.barY, unified, localPct, ps.pass, ps.totalPasses, wallElapsed, estTotal);
                    }
                }
                bufPos = 0;
            }
        }
        else
        {
            if (bufPos < (int)sizeof(buf) - 1)
                buf[bufPos++] = ch;
        }
    }

    CloseHandle(hReadPipe);

    DWORD exitCode = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    InterlockedExchangePointer((volatile PVOID *)&gFfmpegProcess, NULL);

    if (exitCode == 0)
        ok = true;
    return ok;
}

// ── Build FFmpeg filter chain ─────────────────────────────────────────────────
std::wstring buildVF(const Strategy &s)
{
    std::wstring vf = L"";
    bool hasFilter = false;

    if (s.scale < 1.0)
    {
        // scale to % of original keeping aspect ratio
        wchar_t buf[64];
        swprintf(buf, 64, L"scale=trunc(iw*%.2f/2)*2:trunc(ih*%.2f/2)*2", s.scale, s.scale);
        vf += buf;
        hasFilter = true;
    }
    if (s.fps > 0)
    {
        if (hasFilter)
            vf += L",";
        wchar_t buf[32];
        swprintf(buf, 32, L"fps=%d", s.fps);
        vf += buf;
        hasFilter = true;
    }
    return hasFilter ? (L"-vf \"" + vf + L"\"") : L"";
}

// ── Compress ──────────────────────────────────────────────────────────────────
bool compress(const std::wstring &input, const std::wstring &output,
              double sizeMB, ProgressState &ps)
{
    double dur = ps.duration > 0 ? ps.duration : probeDuration(input);
    ps.duration = dur;

    Strategy s = pickStrategy(sizeMB, dur);

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
    std::wstring nullOut = L"NUL";

    ps.wallStart = tickNow();

    if (s.twoPass)
    {
        ps.totalPasses = 2;
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

        // Pass 1 is fast (no audio, null output) — give it ~30% of the bar
        bool ok1 = runFFmpeg(p1, ps, 0.0, 30.0);
        if (!ok1)
            return false;

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

        return runFFmpeg(p2, ps, 30.0, 100.0);
    }
    else
    {
        ps.totalPasses = 1;
        ps.pass = 1;
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

        return runFFmpeg(cmd, ps, 0.0, 100.0);
    }
}

// ── Clipboard helpers ─────────────────────────────────────────────────────────
std::wstring getClipboardFile()
{
    if (!OpenClipboard(NULL))
        return L"";
    HANDLE h = GetClipboardData(CF_HDROP);
    if (!h)
    {
        CloseClipboard();
        return L"";
    }

    HDROP drop = (HDROP)GlobalLock(h);
    if (!drop)
    {
        CloseClipboard();
        return L"";
    }

    wchar_t buf[MAX_PATH] = {0};
    UINT cnt = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    std::wstring result;
    if (cnt > 0)
    {
        DragQueryFileW(drop, 0, buf, MAX_PATH);
        result = buf;
    }
    GlobalUnlock(h);
    CloseClipboard();
    return result;
}

bool isVideoFile(const std::wstring &path)
{
    static const wchar_t *exts[] = {
        L".mp4", L".mkv", L".mov", L".avi", L".webm",
        L".flv", L".wmv", L".m4v", L".ts", L".3gp", nullptr};
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    for (int i = 0; exts[i]; i++)
    {
        if (lower.size() > wcslen(exts[i]) &&
            lower.compare(lower.size() - wcslen(exts[i]), wcslen(exts[i]), exts[i]) == 0)
            return true;
    }
    return false;
}

bool setClipboardFile(const std::wstring &path)
{
    // Pack as CF_HDROP so Ctrl+V pastes the file
    size_t pathLen = path.size() + 1;
    size_t bufSize = sizeof(DROPFILES) + (pathLen + 1) * sizeof(wchar_t);

    HGLOBAL hg = GlobalAlloc(GHND, bufSize);
    if (!hg)
        return false;

    DROPFILES *df = (DROPFILES *)GlobalLock(hg);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    df->pt = {0, 0};
    df->fNC = FALSE;

    wchar_t *dest = (wchar_t *)((BYTE *)df + sizeof(DROPFILES));
    wcscpy(dest, path.c_str());
    dest[pathLen] = L'\0'; // double-null terminate

    GlobalUnlock(hg);

    if (!OpenClipboard(NULL))
    {
        GlobalFree(hg);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hg);
    CloseClipboard();
    return true;
}

// ── Output path: same dir as input, suffix _compressed ───────────────────────
std::wstring getTempOutputPath()
{
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"ClipCrush_compressed.mp4";
}

// ── Bring console window to foreground ───────────────────────────────────────
void bringConsoleToFront()
{
    HWND hw = NULL;
    for (int i = 0; i < 40 && !hw; i++)
    { // up to 400ms
        Sleep(10);
        hw = GetConsoleWindow();
    }
    if (hw)
    {
        ShowWindow(hw, SW_RESTORE);
        SetForegroundWindow(hw);
        BringWindowToTop(hw);
        SetFocus(hw);
    }
}

// ── Hotkey message loop ───────────────────────────────────────────────────────
#define HOTKEY_ID 9001

BOOL WINAPI ctrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
    {
        HANDLE h = (HANDLE)InterlockedExchangePointer((volatile PVOID *)&gFfmpegProcess, NULL);
        if (h && h != INVALID_HANDLE_VALUE)
        {
            gUserCancelled = true;
            TerminateProcess(h, 1);
        }
        return TRUE;
    }
    return FALSE;
}

void showDegradationInfo(const Strategy &s, double sizeMB, double durationSec)
{
    // rows 1-5 are reserved for drawFullHeader + drawHeader
    // info block starts at row 7
    gotoxy(2, 7);
    setColor(15, 0);
    printf("  Compression plan:");

    int row = 8;

    if (s.scale < 1.0)
    {
        gotoxy(2, row++);
        setColor(12, 0);
        printf("  [!] ");
        setColor(7, 0);
        printf("Resolution : ");
        setColor(14, 0);
        printf("scaled to %.0f%% of original", s.scale * 100.0);
    }
    else
    {
        gotoxy(2, row++);
        setColor(10, 0);
        printf("  [+] ");
        setColor(7, 0);
        printf("Resolution : ");
        setColor(10, 0);
        printf("unchanged            ");
    }

    if (s.fps > 0)
    {
        gotoxy(2, row++);
        setColor(12, 0);
        printf("  [!] ");
        setColor(7, 0);
        printf("Frame rate : ");
        setColor(14, 0);
        printf("capped to %d fps     ", s.fps);
    }
    else
    {
        gotoxy(2, row++);
        setColor(10, 0);
        printf("  [+] ");
        setColor(7, 0);
        printf("Frame rate : ");
        setColor(10, 0);
        printf("unchanged            ");
    }

    double origKbps = (sizeMB > 0 && durationSec > 0)
                          ? (sizeMB * 1024.0 * 8.0) / durationSec
                          : 0;
    gotoxy(2, row++);
    if (origKbps > 0 && s.targetKb < (int)(origKbps * 0.9))
    {
        setColor(12, 0);
        printf("  [!] ");
        setColor(7, 0);
        printf("Bitrate    : ");
        setColor(14, 0);
        printf("%.0f kbps -> %d kbps (-%.0f%%)    ",
               origKbps, s.targetKb,
               (1.0 - (double)s.targetKb / origKbps) * 100.0);
    }
    else
    {
        setColor(10, 0);
        printf("  [+] ");
        setColor(7, 0);
        printf("Bitrate    : ");
        setColor(10, 0);
        printf("%d kbps               ", s.targetKb);
    }

    gotoxy(2, row++);
    setColor(8, 0);
    printf("  [-] ");
    setColor(7, 0);
    printf("Audio      : re-encoded to AAC 96 kbps");

    gotoxy(2, row++);
    setColor(8, 0);
    printf("  [-] ");
    setColor(7, 0);
    printf("Target     : ");
    setColor(11, 0);
    printf("%.1f MB  (from %.1f MB)", TARGET_MB, sizeMB);

    row++;
    gotoxy(2, row);
    setColor(8, 0);
    printf("  ----------------------------------------");
    row++;
    gotoxy(2, row);
    setColor(8, 0);
    printf("  If the plan looks too aggressive, press ");
    setColor(12, 0);
    printf("Ctrl+C");
    setColor(8, 0);
    printf(" to cancel.");
}

void doCompress()
{
    AllocConsole();
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    freopen("CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"ClipCrush");
    // Register AFTER AllocConsole so it applies to this console
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Fail fast — check ffmpeg exists before touching anything else
    if (GetFileAttributesW(ffmpegPath().c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        setColor(12, 0);
        puts("\n  [!] ffmpeg.exe not found next to ClipCrush.exe.");
        setColor(7, 0);
        puts("      Download FFmpeg and place ffmpeg.exe in the same folder.\n");
        Sleep(4000);
        FreeConsole();
        return;
    }

    std::wstring videoPath = getClipboardFile();

    if (videoPath.empty() || !isVideoFile(videoPath))
    {
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

    // ─ Open terminal window (single setup path) ─
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    freopen("CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"ClipCrush");

    // Already small enough
    if (sizeMB < 9.5)
    {
        setColor(10, 0);
        printf("\n  [OK] File is %.1f MB — already under 10 MB. Nothing to do.\n", sizeMB);
        setColor(7, 0);
        Sleep(2000);
        FreeConsole();
        return;
    }
    // Disable QuickEdit mode — clicking the console pauses pipe reads otherwise
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD consoleMode = 0;
    GetConsoleMode(hIn, &consoleMode);
    SetConsoleMode(hIn, consoleMode & ~ENABLE_QUICK_EDIT_MODE & ~ENABLE_MOUSE_INPUT);

    hideCursor();
    bringConsoleToFront();

    // Resize console
    SMALL_RECT rect = {0, 0, 79, 24};
    SetConsoleWindowInfo(hStdOut, TRUE, &rect);
    cls();

    // ─ Set up progress state ─
    ProgressState ps;
    ps.barY = 8;
    ps.sizeMB = sizeMB;
    ps.duration = 0;
    ps.pass = 1;
    ps.totalPasses = 1;

    // Extract just filename for display
    size_t sl = videoPath.find_last_of(L"\\/");
    ps.inputName = (sl != std::wstring::npos) ? videoPath.substr(sl + 1) : videoPath;
    ps.tier = L"Analyzing...";

    cls();
    drawFullHeader(ps);

    double durPreview = probeDuration(videoPath);
    Strategy sPreview = pickStrategy(sizeMB, durPreview);

    showDegradationInfo(sPreview, sizeMB, durPreview);
    ps.barY = 16;
    ps.duration = durPreview; // cache so compress() doesn't probe again

    // ─ Compress ─
    std::wstring output = getTempOutputPath();
    // Delete any previous temp file before starting
    DeleteFileW(output.c_str());
    bool ok = compress(videoPath, output, sizeMB, ps);

    if (ok)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa2;
        GetFileAttributesExW(output.c_str(), GetFileExInfoStandard, &fa2);
        ULONGLONG outBytes = ((ULONGLONG)fa2.nFileSizeHigh << 32) | fa2.nFileSizeLow;
        double outMB = outBytes / (1024.0 * 1024.0);
        double wallElapsed = tickNow() - ps.wallStart;

        cls();

        // ── Header ───────────────────────────────────────────────────────
        gotoxy(2, 1);
        setColor(15, 0);
        printf("  ClipCrush  ");
        setColor(8, 0);
        printf("v1.0");

        if (outMB > 9.5)
        {
            // ── Failed to fit ─────────────────────────────────────────────
            gotoxy(2, 4);
            setColor(12, 0);
            printf("  [!] Still too big after compression");

            gotoxy(2, 6);
            setColor(7, 0);
            printf("  Result  : ");
            setColor(14, 0);
            printf("%.1f MB  (target was %.1f MB)", outMB, TARGET_MB);

            gotoxy(2, 7);
            setColor(7, 0);
            printf("  Source  : ");
            setColor(7, 0);
            printf("%.1f MB", sizeMB);

            gotoxy(2, 9);
            setColor(8, 0);
            printf("  The clip is probably too long to fit.");
            gotoxy(2, 10);
            setColor(8, 0);
            printf("  Try trimming it before compressing.");

            MessageBeep(MB_ICONEXCLAMATION);
            DeleteFileW(output.c_str());
        }
        else
        {
            // ── Success ───────────────────────────────────────────────────
            setClipboardFile(output);

            gotoxy(2, 4);
            setColor(10, 0);
            printf("  Done!");

            gotoxy(2, 6);
            setColor(7, 0);
            printf("  Size       : ");
            setColor(14, 0);
            printf("%.1f MB", sizeMB);
            setColor(8, 0);
            printf("  ->  ");
            setColor(10, 0);
            printf("%.1f MB", outMB);
            setColor(8, 0);
            printf("   (saved ");
            setColor(15, 0);
            printf("%.0f%%", (1.0 - outMB / sizeMB) * 100.0);
            setColor(8, 0);
            printf(")");

            gotoxy(2, 7);
            setColor(7, 0);
            printf("  Time       : ");
            setColor(11, 0);
            printf("%.0f sec", wallElapsed);

            gotoxy(2, 9);
            setColor(11, 0);
            printf("  Copied to clipboard — just Ctrl+V to paste.");

            PlaySound(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC);
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
            nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
            nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
            wcscpy_s(nid.szTip, L"ClipCrush");
            wcscpy_s(nid.szInfoTitle, L"ClipCrush \u2014 Done!");
            wchar_t balloon[128];
            swprintf(balloon, 128, L"%.1f MB \u2192 %.1f MB \u2014 ready to paste!", sizeMB, outMB);
            wcscpy_s(nid.szInfo, balloon);
            Shell_NotifyIconW(NIM_ADD, &nid);
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
    }
    else
    {
        cls();
        gotoxy(2, 1);
        setColor(15, 0);
        printf("  ClipCrush  ");
        setColor(8, 0);
        printf("v1.0");

        gotoxy(2, 4);
        if (gUserCancelled)
        {
            setColor(14, 0);
            printf("  Cancelled.");
        }
        else
        {
            setColor(12, 0);
            printf("  [ERROR] Compression failed.");
            gotoxy(2, 5);
            setColor(8, 0);
            printf("  Make sure ffmpeg.exe is in the same folder.");
        }
        gUserCancelled = false;
    }

    // ── Press any key to close ────────────────────────────────────────────
    showCursor();
    gotoxy(2, 12);
    setColor(8, 0);
    printf("  Press any key to close...");
    {
        HANDLE hConIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
        FlushConsoleInputBuffer(hConIn);
        INPUT_RECORD ir;
        DWORD nread;
        while (true)
        {
            ReadConsoleInputW(hConIn, &ir, 1, &nread);
            if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown)
                break;
        }
        CloseHandle(hConIn);
    }

    HWND hwConsole = GetConsoleWindow();
    FreeConsole();
    if (hwConsole)
        PostMessage(hwConsole, WM_CLOSE, 0, 0);

    // Clean up all known 2-pass log file variants
    std::wstring logBase = output + L"_log";
    DeleteFileW((logBase + L"-0.log").c_str());
    DeleteFileW((logBase + L"-0.log.mbtree").c_str());
    DeleteFileW((logBase + L"-0.log.cutree").c_str());
    DeleteFileW((logBase + L".log").c_str());
    DeleteFileW((logBase + L".log.mbtree").c_str());
}

// ── WinMain ───────────────────────────────────────────────────────────────────
static DWORD WINAPI compressThread(LPVOID)
{
    doCompress();
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    if (!RegisterHotKey(NULL, HOTKEY_ID,
                        MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'V'))
    {
        MessageBoxW(NULL,
                    L"Could not register hotkey Ctrl+Alt+Shift+V.\n"
                    L"Another app may be using it.",
                    L"ClipCrush", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_ID)
        {
            // Only spawn one compression at a time
            static HANDLE hThread = NULL;
            if (hThread == NULL || WaitForSingleObject(hThread, 0) == WAIT_OBJECT_0)
            {
                if (hThread)
                    CloseHandle(hThread);
                hThread = CreateThread(NULL, 0, compressThread, NULL, 0, NULL);
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(NULL, HOTKEY_ID);
    return 0;
}