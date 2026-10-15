/* Aerith.c - HyBootloader, lanceur natif WinCE 7 (Hyundai i20 GB / LG Gen5)
 *
 * Remplacant leger du Aerith.exe de JKD (MFC, 1,3 Mo + DLL) : depose sous le nom
 * Aerith.exe, il affiche un bouton par programme trouve dans le dossier
 * "programmes" a cote de lui (HyLink.exe -> "HyLink"). Aucune configuration.
 *
 *   <dossier de Aerith.exe>\Aerith.exe
 *   <dossier de Aerith.exe>\programmes\*.exe   (+ leurs DLL)
 *
 * Des qu'une autre application passe devant, la fenetre se cache et une icone
 * ronde, flottante et deplacable reste au-dessus de tout : un tap ramene
 * HyBootloader au premier plan. Bouton "-" : retour a l'IHM LG en gardant
 * l'icone ; "X" : quitte tout.
 *
 * Build : hylink\build.ps1 (VS2008 + SDK2wince7, ARMv4I), voir README.md.
 * C89 (declarations en tete de bloc) pour le compilateur C de VS2008.
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <windows.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define APP_TITLE     L"HyBootloader"
#define APP_SUBTITLE  L"by Harlock"
#define PROG_DIR      L"programmes"
#define PROG_DIR_SD   L"\\SDMemory\\programmes"   /* repli si l'exe tourne depuis \ */
#define LISA_EXE      L"\\MMCMemory\\Storage Card\\system\\Lisa.exe"
#define LISA_NAME     L"Lisa.exe"

#define MAIN_CLASS    L"HyBootloaderWnd"
#define BUBBLE_CLASS  L"HyBootloaderBubble"
#define MUTEX_NAME    L"HyBootloader"
#define WM_APP_SHOW   (WM_APP + 1)

#define MAX_PROGS     24
#define TIMER_ID      1
#define TIMER_MS      300

/* Geometrie (ecran 800x480) */
#define HDR_H         112      /* hauteur de l'en-tete */
#define MARGIN        24
#define GAP           16
#define MAX_COLS      4
#define TILE_MAX_W    240
#define TILE_MAX_H    96
#define TBTN          56       /* boutons "-" et "X" */
#define TBTN_TOP      22
#define TBTN_RIGHT    18
#define BUBBLE        64       /* diametre de l'icone flottante */
#define DRAG_SLOP     10       /* au-dela, le toucher est un glisser */

/* Couleurs */
#define COL_BG        RGB( 18,  20,  26)
#define COL_TITLE     RGB(240, 242, 245)
#define COL_SUB       RGB(150, 200, 255)
#define COL_TEXT      RGB(225, 228, 232)
#define COL_DIM       RGB(140, 146, 158)
#define COL_TILE      RGB( 40,  44,  56)
#define COL_ACCENT    RGB( 30, 136, 229)
#define COL_RUN       RGB( 16, 185, 129)
#define COL_CLOSE     RGB(200,  70,  70)

#define HIT_NONE      (-1)
#define HIT_MIN       (-2)
#define HIT_CLOSE     (-3)

typedef struct {
    WCHAR name[48];         /* libelle : nom du fichier sans .exe */
    WCHAR file[MAX_PATH];   /* nom du fichier, compare a szExeFile */
    WCHAR path[MAX_PATH];
    DWORD pid;              /* 0 = ne tourne pas */
    RECT  rc;
} Prog;

static Prog   g_progs[MAX_PROGS];
static int    g_nProgs;
static WCHAR  g_progDir[MAX_PATH];
static WCHAR  g_selfName[MAX_PATH];
static int    g_cx, g_cy;

static HWND   g_main, g_bubble;
static HFONT  g_fTitle, g_fSub, g_fTile, g_fBtn, g_fBubble;
static HBRUSH g_brBg, g_brTile, g_brAccent, g_brRun, g_brClose;
static int    g_pressed = HIT_NONE;
static int    g_grace;              /* ticks ignores apres une mise au premier plan */

static struct {
    int   down, moved;
    POINT start;
    RECT  win;
} g_drag;

/* ------------------------------------------------------------------------- *
 *  Programmes et processus                                                  *
 * ------------------------------------------------------------------------- */

static int IsDir(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* <dossier de l'exe>\programmes, sinon \SDMemory\programmes : le hook firmware
   de JKD recopie Aerith.exe a la racine \, loin de la carte. */
static void FindProgDir(void)
{
    WCHAR self[MAX_PATH];
    WCHAR *slash;

    self[0] = 0;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    slash = wcsrchr(self, L'\\');
    wcscpy(g_selfName, slash ? slash + 1 : self);
    if (slash) *slash = 0;

    _snwprintf(g_progDir, MAX_PATH, L"%s\\" PROG_DIR, self);
    g_progDir[MAX_PATH - 1] = 0;
    if (!IsDir(g_progDir) && IsDir(PROG_DIR_SD))
        wcscpy(g_progDir, PROG_DIR_SD);
}

/* Met a jour le PID de chaque programme et renvoie celui de Lisa (0 si absente). */
static DWORD ScanProcesses(void)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    DWORD lisa = 0;
    int i;

    for (i = 0; i < g_nProgs; i++) g_progs[i].pid = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPNOHEAPS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, LISA_NAME) == 0) lisa = pe.th32ProcessID;
            for (i = 0; i < g_nProgs; i++)
                if (_wcsicmp(pe.szExeFile, g_progs[i].file) == 0)
                    g_progs[i].pid = pe.th32ProcessID;
        } while (Process32Next(snap, &pe));
    }
    CloseToolhelp32Snapshot(snap);
    return lisa;
}

/* Liste programmes\*.exe, triee par nom (l'ordre FAT n'est pas garanti). */
static void ScanPrograms(void)
{
    WIN32_FIND_DATAW fd;
    WCHAR pattern[MAX_PATH];
    HANDLE h;

    g_nProgs = 0;
    _snwprintf(pattern, MAX_PATH, L"%s\\*.exe", g_progDir);
    pattern[MAX_PATH - 1] = 0;

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        Prog p;
        int len, i;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        len = (int)wcslen(fd.cFileName);
        if (len <= 4 || _wcsicmp(fd.cFileName + len - 4, L".exe") != 0) continue;
        if (_wcsicmp(fd.cFileName, g_selfName) == 0 ||
            _wcsicmp(fd.cFileName, L"Aerith.exe") == 0) continue;
        if (g_nProgs >= MAX_PROGS) break;

        memset(&p, 0, sizeof(p));
        len -= 4;
        if (len > 47) len = 47;
        wcsncpy(p.name, fd.cFileName, len);
        wcscpy(p.file, fd.cFileName);
        _snwprintf(p.path, MAX_PATH, L"%s\\%s", g_progDir, fd.cFileName);
        p.path[MAX_PATH - 1] = 0;

        /* insertion triee */
        for (i = g_nProgs; i > 0 && _wcsicmp(g_progs[i - 1].name, p.name) > 0; i--)
            g_progs[i] = g_progs[i - 1];
        g_progs[i] = p;
        g_nProgs++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef struct { DWORD pid; HWND hwnd; } FindCtx;

static BOOL CALLBACK FindWndProc(HWND h, LPARAM lp)
{
    FindCtx *c = (FindCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
        c->hwnd = h;
        return FALSE;
    }
    return TRUE;
}

/* Programme deja lance (par nous ou ailleurs) : on ramene sa fenetre au lieu
   d'en ouvrir un second exemplaire. Sans fenetre visible (agent comme hylink),
   on ne fait rien. */
static void LaunchOrFocus(int i)
{
    PROCESS_INFORMATION pi;

    ScanProcesses();
    if (g_progs[i].pid) {
        FindCtx c;
        c.pid = g_progs[i].pid;
        c.hwnd = NULL;
        EnumWindows(FindWndProc, (LPARAM)&c);
        if (c.hwnd) SetForegroundWindow(c.hwnd);
        return;
    }

    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(g_progs[i].path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        MessageBoxW(g_main, g_progs[i].path, L"Lancement impossible", MB_OK | MB_ICONWARNING);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_progs[i].pid = pi.dwProcessId;
}

/* ------------------------------------------------------------------------- *
 *  Fenetre principale                                                       *
 * ------------------------------------------------------------------------- */

/* WinCE n'exporte pas CreateFont(W) : on passe par CreateFontIndirectW. */
static HFONT MakeFont(int height, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight  = -height;
    lf.lfWeight  = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Tahoma");
    return CreateFontIndirectW(&lf);
}

static void TopButtonRect(int which, RECT *r)
{
    r->right  = g_cx - TBTN_RIGHT - (which == HIT_MIN ? TBTN + 14 : 0);
    r->left   = r->right - TBTN;
    r->top    = TBTN_TOP;
    r->bottom = TBTN_TOP + TBTN;
}

/* Grille centree : 4 colonnes max, tuiles reduites pour tout faire tenir. */
static void Layout(void)
{
    int n = g_nProgs, cols, rows, areaW, areaH, tw, th, x0, y0, i;

    if (n == 0) return;
    cols  = n < MAX_COLS ? n : MAX_COLS;
    rows  = (n + cols - 1) / cols;
    areaW = g_cx - 2 * MARGIN;
    areaH = g_cy - HDR_H - MARGIN;
    tw = (areaW - (cols - 1) * GAP) / cols;
    th = (areaH - (rows - 1) * GAP) / rows;
    if (tw > TILE_MAX_W) tw = TILE_MAX_W;
    if (th > TILE_MAX_H) th = TILE_MAX_H;
    x0 = (g_cx - (cols * tw + (cols - 1) * GAP)) / 2;
    y0 = HDR_H + (areaH - (rows * th + (rows - 1) * GAP)) / 2;

    for (i = 0; i < n; i++) {
        RECT *r = &g_progs[i].rc;
        r->left   = x0 + (i % cols) * (tw + GAP);
        r->top    = y0 + (i / cols) * (th + GAP);
        r->right  = r->left + tw;
        r->bottom = r->top + th;
    }
}

static int HitTest(int x, int y)
{
    POINT p;
    RECT r;
    int i;

    p.x = x; p.y = y;
    TopButtonRect(HIT_CLOSE, &r);
    if (PtInRect(&r, p)) return HIT_CLOSE;
    TopButtonRect(HIT_MIN, &r);
    if (PtInRect(&r, p)) return HIT_MIN;
    for (i = 0; i < g_nProgs; i++)
        if (PtInRect(&g_progs[i].rc, p)) return i;
    return HIT_NONE;
}

static void FillRound(HDC dc, const RECT *r, HBRUSH br, int radius)
{
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r->left, r->top, r->right + 1, r->bottom + 1, radius, radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
}

static void PaintMain(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC dc, mem;
    HBITMAP bmp;
    HGDIOBJ oldBmp, oldFont;
    RECT rc, r;
    int i;

    dc = BeginPaint(hWnd, &ps);
    GetClientRect(hWnd, &rc);

    /* double tampon : pas de scintillement au toucher */
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    oldBmp = SelectObject(mem, bmp);

    FillRect(mem, &rc, g_brBg);
    SetBkMode(mem, TRANSPARENT);

    oldFont = SelectObject(mem, g_fTitle);
    SetTextColor(mem, COL_TITLE);
    SetRect(&r, 0, 20, rc.right, 70);
    DrawTextW(mem, APP_TITLE, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(mem, g_fSub);
    SetTextColor(mem, COL_SUB);
    SetRect(&r, 0, 70, rc.right, 100);
    DrawTextW(mem, APP_SUBTITLE, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(mem, g_fBtn);
    SetTextColor(mem, RGB(255, 255, 255));
    TopButtonRect(HIT_MIN, &r);
    FillRound(mem, &r, g_pressed == HIT_MIN ? g_brAccent : g_brTile, 14);
    DrawTextW(mem, L"\x2014", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    TopButtonRect(HIT_CLOSE, &r);
    FillRound(mem, &r, g_pressed == HIT_CLOSE ? g_brAccent : g_brClose, 14);
    DrawTextW(mem, L"\x00D7", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(mem, g_fTile);
    if (g_nProgs == 0) {
        WCHAR msg[MAX_PATH + 40];
        _snwprintf(msg, MAX_PATH + 40, L"Aucun programme dans\n%s", g_progDir);
        msg[MAX_PATH + 39] = 0;
        SetTextColor(mem, COL_DIM);
        SetRect(&r, MARGIN, g_cy / 2 - 20, rc.right - MARGIN, rc.bottom);
        DrawTextW(mem, msg, -1, &r, DT_CENTER | DT_WORDBREAK);
    }
    for (i = 0; i < g_nProgs; i++) {
        const Prog *p = &g_progs[i];
        r = p->rc;
        FillRound(mem, &r, g_pressed == i ? g_brAccent : g_brTile, 18);
        if (p->pid) {   /* pastille verte : deja en cours */
            HGDIOBJ ob = SelectObject(mem, g_brRun);
            HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, r.right - 20, r.top + 8, r.right - 8, r.top + 20);
            SelectObject(mem, op);
            SelectObject(mem, ob);
        }
        InflateRect(&r, -10, 0);
        SetTextColor(mem, COL_TEXT);
        DrawTextW(mem, p->name, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hWnd, &ps);
}

/* Remet HyBootloader au premier plan (tap sur l'icone, seconde instance). */
static void BringUp(void)
{
    ScanPrograms();
    ScanProcesses();
    Layout();
    g_pressed = HIT_NONE;
    g_grace = 2;
    ShowWindow(g_bubble, SW_HIDE);
    ShowWindow(g_main, SW_SHOW);
    SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(g_main);
    InvalidateRect(g_main, NULL, FALSE);
}

/* Toutes les TIMER_MS : la fenetre n'est visible que si elle est devant ;
   sinon elle se cache et l'icone flottante prend le relais. */
static void UpdateVisibility(void)
{
    HWND fg;
    DWORD pid = 0;

    if (g_grace > 0) { g_grace--; return; }

    fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if ((!fg || pid != GetCurrentProcessId()) && IsWindowVisible(g_main))
        ShowWindow(g_main, SW_HIDE);

    if (IsWindowVisible(g_main)) {
        if (IsWindowVisible(g_bubble)) ShowWindow(g_bubble, SW_HIDE);
    } else {
        if (!IsWindowVisible(g_bubble)) ShowWindow(g_bubble, SW_SHOWNOACTIVATE);
        /* Lisa ou l'appli lancee peuvent aussi etre topmost : on se reaffirme */
        SetWindowPos(g_bubble, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

/* Quitte tout ; relance l'IHM LG si elle ne tourne pas (le JKD l'encapsulait). */
static void Quit(void)
{
    PROCESS_INFORMATION pi;

    KillTimer(g_main, TIMER_ID);
    if (!ScanProcesses() &&
        CreateProcessW(LISA_EXE, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DestroyWindow(g_bubble);
    DestroyWindow(g_main);
}

static LRESULT CALLBACK MainProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int hit;

    switch (msg) {
    case WM_PAINT:
        PaintMain(hWnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* tout est peint dans WM_PAINT */

    case WM_LBUTTONDOWN:
        g_pressed = HitTest((short)LOWORD(lp), (short)HIWORD(lp));
        if (g_pressed != HIT_NONE) {
            SetCapture(hWnd);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_pressed == HIT_NONE) return 0;
        ReleaseCapture();
        hit = HitTest((short)LOWORD(lp), (short)HIWORD(lp));
        if (hit == g_pressed) {
            if (hit == HIT_CLOSE)     { Quit(); return 0; }
            else if (hit == HIT_MIN)  ShowWindow(hWnd, SW_HIDE);
            else                      LaunchOrFocus(hit);
        }
        g_pressed = HIT_NONE;
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_TIMER:
        UpdateVisibility();
        return 0;

    case WM_APP_SHOW:
        BringUp();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) Quit();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* ------------------------------------------------------------------------- *
 *  Icone flottante                                                          *
 * ------------------------------------------------------------------------- */

/* Disque de diametre d en bandes horizontales : le SDK CE n'a pas
   CreateEllipticRgn. Calcul en coordonnees doublees, sans flottant. */
static HRGN DiscRegion(int d)
{
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    int y;

    for (y = 0; y < d; y++) {
        int dy = 2 * y + 1 - d, v = d * d - dy * dy, hw = 0;
        HRGN row;
        while ((hw + 1) * (hw + 1) <= v) hw++;
        row = CreateRectRgn((d - hw) / 2, y, (d + hw + 1) / 2, y + 1);
        CombineRgn(rgn, rgn, row, RGN_OR);
        DeleteObject(row);
    }
    return rgn;
}

static void PaintBubble(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC dc;
    RECT rc;
    HPEN pen;
    HGDIOBJ op, ob, of;

    dc = BeginPaint(hWnd, &ps);
    GetClientRect(hWnd, &rc);
    FillRect(dc, &rc, g_brAccent);

    pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    op = SelectObject(dc, pen);
    ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, 2, 2, rc.right - 2, rc.bottom - 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    of = SelectObject(dc, g_fBubble);
    DrawTextW(dc, L"Hy", -1, &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, of);
    EndPaint(hWnd, &ps);
}

static LRESULT CALLBACK BubbleProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT p;

    switch (msg) {
    case WM_PAINT:
        PaintBubble(hWnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
        p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
        ClientToScreen(hWnd, &p);
        g_drag.down = 1;
        g_drag.moved = 0;
        g_drag.start = p;
        GetWindowRect(hWnd, &g_drag.win);
        SetCapture(hWnd);
        return 0;

    case WM_MOUSEMOVE:
        if (g_drag.down) {
            int dx, dy, x, y;
            p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
            ClientToScreen(hWnd, &p);
            dx = p.x - g_drag.start.x;
            dy = p.y - g_drag.start.y;
            if (!g_drag.moved && abs(dx) <= DRAG_SLOP && abs(dy) <= DRAG_SLOP) return 0;
            g_drag.moved = 1;
            x = g_drag.win.left + dx;
            y = g_drag.win.top + dy;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > g_cx - BUBBLE) x = g_cx - BUBBLE;
            if (y > g_cy - BUBBLE) y = g_cy - BUBBLE;
            SetWindowPos(hWnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_drag.down) {
            int tap = !g_drag.moved;
            g_drag.down = 0;
            ReleaseCapture();
            if (tap) BringUp();
        }
        return 0;

    case WM_CAPTURECHANGED:
        g_drag.down = 0;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* ------------------------------------------------------------------------- *
 *  Point d'entree                                                           *
 * ------------------------------------------------------------------------- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow)
{
    WNDCLASSW wc;
    HANDLE mutex;
    MSG msg;

    (void)hPrev; (void)lpCmdLine; (void)nShow;

    /* Instance unique : un second lancement ramene la premiere devant. */
    mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(MAIN_CLASS, NULL);
        if (w) PostMessageW(w, WM_APP_SHOW, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    g_cx = GetSystemMetrics(SM_CXSCREEN);
    g_cy = GetSystemMetrics(SM_CYSCREEN);
    if (g_cx <= 0) g_cx = 800;
    if (g_cy <= 0) g_cy = 480;

    FindProgDir();

    g_brBg     = CreateSolidBrush(COL_BG);
    g_brTile   = CreateSolidBrush(COL_TILE);
    g_brAccent = CreateSolidBrush(COL_ACCENT);
    g_brRun    = CreateSolidBrush(COL_RUN);
    g_brClose  = CreateSolidBrush(COL_CLOSE);
    g_fTitle   = MakeFont(40, FW_BOLD);
    g_fSub     = MakeFont(22, FW_NORMAL);
    g_fTile    = MakeFont(26, FW_SEMIBOLD);
    g_fBtn     = MakeFont(32, FW_BOLD);
    g_fBubble  = MakeFont(24, FW_BOLD);

    memset(&wc, 0, sizeof(wc));
    wc.hInstance     = hInst;
    wc.lpfnWndProc   = MainProc;
    wc.lpszClassName = MAIN_CLASS;
    if (!RegisterClassW(&wc)) return 1;
    wc.lpfnWndProc   = BubbleProc;
    wc.lpszClassName = BUBBLE_CLASS;
    if (!RegisterClassW(&wc)) return 1;

    /* Topmost tant qu'elle est visible : elle n'est montree qu'au premier plan. */
    g_main = CreateWindowExW(WS_EX_TOPMOST, MAIN_CLASS, APP_TITLE, WS_POPUP,
                             0, 0, g_cx, g_cy, NULL, NULL, hInst, NULL);
    g_bubble = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, BUBBLE_CLASS, APP_TITLE,
                               WS_POPUP, g_cx - BUBBLE - 8, (g_cy - BUBBLE) / 2,
                               BUBBLE, BUBBLE, NULL, NULL, hInst, NULL);
    if (!g_main || !g_bubble) return 1;
    SetWindowRgn(g_bubble, DiscRegion(BUBBLE), FALSE);   /* region possedee par le systeme */

    BringUp();
    SetTimer(g_main, TIMER_ID, TIMER_MS, NULL);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_fTitle);
    DeleteObject(g_fSub);
    DeleteObject(g_fTile);
    DeleteObject(g_fBtn);
    DeleteObject(g_fBubble);
    DeleteObject(g_brBg);
    DeleteObject(g_brTile);
    DeleteObject(g_brAccent);
    DeleteObject(g_brRun);
    DeleteObject(g_brClose);
    CloseHandle(mutex);
    return 0;
}
