// retropad - a Petzold-style Win32 notepad clone implemented in mostly plain C.
// Keeps the classic menus/accelerators, word wrap, status bar, find/replace,
// font picker, and basic file load/save with BOM detection.

#include <limits.h>
#include <wchar.h>

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include "resource.h"
#include "file_io.h"
#include "shlobj_core.h"

#define APP_TITLE      L"retropad"
#define UNTITLED_NAME  L"Untitled"
#define MAX_PATH_BUFFER 1024
#define DEFAULT_WIDTH  640
#define DEFAULT_HEIGHT 480
#define DEFAULT_MARGINS 250

typedef struct AppState {
    HWND hwndMain;
    HWND hwndEdit;
    HWND hwndStatus;
    HFONT hFont;
    WCHAR currentPath[MAX_PATH_BUFFER];
    BOOL wordWrap;
    BOOL statusVisible;
    BOOL statusBeforeWrap;
    BOOL modified;
    BOOL convertCRLF;
    TextEncoding encoding;
    FINDREPLACEW find;
    HWND hFindDlg;
    HWND hReplaceDlg;
    UINT findFlags;
    WCHAR findText[128];
    WCHAR replaceText[128];
} AppState;

static AppState g_app = {0};
static HINSTANCE g_hInst = NULL;
static UINT g_findMsg = 0;

static void UpdateTitle(HWND hwnd);
static void CreateEditControl(HWND hwnd);
static void UpdateLayout(HWND hwnd);
static BOOL PromptSaveChanges(HWND hwnd);
static BOOL DoFileOpen(HWND hwnd);
static BOOL DoFileSave(HWND hwnd, BOOL saveAs);
static void DoFileNew(HWND hwnd);
static void SetWordWrap(HWND hwnd, BOOL enabled);
static void ToggleStatusBar(HWND hwnd, BOOL visible);
static void UpdateStatusBar(HWND hwnd);
static void ShowFindDialog(HWND hwnd);
static void ShowReplaceDialog(HWND hwnd);
static BOOL DoFindNext(BOOL reverse);
static void DoSelectFont(HWND hwnd);
static void InsertTimeDate(HWND hwnd);
static void HandleFindReplace(LPFINDREPLACE lpfr);
static BOOL LoadDocumentFromPath(HWND hwnd, LPCWSTR path);
static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);

static BOOL SaveSetting(LPCWSTR label, const BYTE *data, DWORD size);
static BOOL LoadSetting(LPCWSTR label, BYTE *data, DWORD size);
static int SetSelectedFont(HWND hwnd, LOGFONTW* lf);
static void InitializeFont(HWND hwnd);
static void DoPageSetup(HWND hwnd);
static void DoPrint(HWND hwnd);
static void DoConvertCRLF(HWND hwnd);
static BOOL AddFinalCRLF(WCHAR** buffer, int* len);
static LPCWSTR StripQuotes(LPWSTR path);
static void DoRegisterExtension(HWND hwnd);
static void ToggleCRLF(HWND hwnd, BOOL visible);
BOOL IsRunningAsAdmin();

static BOOL PruneRegistryKey(HWND hwnd, LPCWSTR path, BOOL quiet);
static HKEY CreateRegistryKey(HWND hwnd, LPCWSTR path);
static BOOL SetRegistryValue(HWND hwnd, HKEY hKey, LPCWSTR name, DWORD type, const BYTE* data, DWORD len);
static BOOL CloseRegistryKey(HWND hwnd, HKEY hKey);
static BOOL RegisterAppPath(HWND hwnd, LPCWSTR appPath);
static BOOL RegisterFileType(HWND hwnd);
static BOOL RegisterAppAssociation(HWND hwnd, LPCWSTR appPath);

static BOOL GetEditText(HWND hwndEdit, WCHAR **bufferOut, int *lengthOut) {
    int length = GetWindowTextLengthW(hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
    if (!buffer) return FALSE;
    GetWindowTextW(hwndEdit, buffer, length + 1);
    if (lengthOut) *lengthOut = length;
    *bufferOut = buffer;
    return TRUE;
}

static BOOL FindInEdit(HWND hwndEdit, const WCHAR *needle, BOOL matchCase, BOOL searchDown, DWORD startPos, DWORD *outStart, DWORD *outEnd) {
    if (!needle || needle[0] == L'\0') return FALSE;

    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(hwndEdit, &text, &len)) return FALSE;

    size_t needleLen = wcslen(needle);
    WCHAR *haystack = NULL;
    WCHAR *needleBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
    if (!needleBuf) {
        HeapFree(GetProcessHeap(), 0, text);
        return FALSE;
    }
    StringCchCopyW(needleBuf, needleLen + 1, needle);

    if (!matchCase) {
        haystack = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, (len+1) * sizeof(WCHAR));
        if (!haystack) {
            HeapFree(GetProcessHeap(), 0, text);
            HeapFree(GetProcessHeap(), 0, needleBuf);
            return FALSE;
        }
        StringCchCopyW(haystack, len + 1, text);
        CharLowerBuffW(haystack, len);
        CharLowerBuffW(needleBuf, (DWORD)needleLen);
    }
    else {
        haystack = text;
    }

    if (startPos > (DWORD)len) startPos = (DWORD)len;

    WCHAR *found = NULL;
    if (searchDown) {
        found = wcsstr(haystack + startPos, needleBuf);
        if (!found && startPos > 0) {
            found = wcsstr(haystack, needleBuf);
        }
    } else {
        WCHAR *p = haystack;
        while ((p = wcsstr(p, needleBuf)) != NULL) {
            DWORD idx = (DWORD)(p - haystack);
            if (idx < startPos) {
                found = p;
                p++;
            } else {
                break;
            }
        }
        if (!found && startPos < (DWORD)len) {
            p = haystack + startPos;
            while ((p = wcsstr(p, needleBuf)) != NULL) {
                found = p;
                p++;
            }
        }
    }

    BOOL result = FALSE;
    if (found) {
        DWORD pos = (DWORD)(found - haystack);
        *outStart = pos;
        *outEnd = pos + (DWORD)needleLen;
        result = TRUE;
    }
    if (!matchCase && haystack != text) {
        HeapFree(GetProcessHeap(), 0, haystack);
    }
    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    return result;
}

static int ReplaceAllOccurrences(HWND hwndEdit, const WCHAR *needle, const WCHAR *replacement, BOOL matchCase) {
    if (!needle || needle[0] == L'\0') return 0;

    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(hwndEdit, &text, &len)) return 0;

    size_t needleLen = wcslen(needle);
    size_t replLen = replacement ? wcslen(replacement) : 0;

    WCHAR *searchBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    WCHAR *needleBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
    if (!searchBuf || !needleBuf) {
        HeapFree(GetProcessHeap(), 0, text);
        if (searchBuf) HeapFree(GetProcessHeap(), 0, searchBuf);
        if (needleBuf) HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }
    StringCchCopyW(searchBuf, len + 1, text);
    StringCchCopyW(needleBuf, needleLen + 1, needle);

    if (!matchCase) {
        CharLowerBuffW(searchBuf, len);
        CharLowerBuffW(needleBuf, (DWORD)needleLen);
    }

    int count = 0;
    WCHAR *p = searchBuf;
    while ((p = wcsstr(p, needleBuf)) != NULL) {
        count++;
        p += needleLen;
    }
    if (count == 0) {
        HeapFree(GetProcessHeap(), 0, text);
        HeapFree(GetProcessHeap(), 0, searchBuf);
        HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }

    size_t newLen = (size_t)len - (size_t)count * needleLen + (size_t)count * replLen;
    if (newLen > INT_MAX / sizeof(WCHAR)) {
        HeapFree(GetProcessHeap(), 0, text);
        HeapFree(GetProcessHeap(), 0, searchBuf);
        HeapFree(GetProcessHeap(), 0, needleBuf);
        MessageBoxW(NULL, L"Replacement text too large.", L"retropad", MB_ICONERROR);
        return 0;
    }
    WCHAR *result = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (newLen + 1) * sizeof(WCHAR));
    if (!result) {
        HeapFree(GetProcessHeap(), 0, text);
        HeapFree(GetProcessHeap(), 0, searchBuf);
        HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }

    WCHAR *dst = result;
    WCHAR *searchCur = searchBuf;
    WCHAR *origCur = text;
    while ((p = wcsstr(searchCur, needleBuf)) != NULL) {
        size_t delta = (size_t)(p - searchCur);
        CopyMemory(dst, origCur, delta * sizeof(WCHAR));
        dst += delta;
        origCur += delta;
        searchCur += delta;

        if (replLen) {
            CopyMemory(dst, replacement, replLen * sizeof(WCHAR));
            dst += replLen;
        }
        origCur += needleLen;
        searchCur += needleLen;
    }
    size_t tail = wcslen(origCur);
    CopyMemory(dst, origCur, tail * sizeof(WCHAR));
    dst += tail;
    *dst = L'\0';

    SetWindowTextW(hwndEdit, result);
    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, searchBuf);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    HeapFree(GetProcessHeap(), 0, result);
    SendMessageW(hwndEdit, EM_SETMODIFY, TRUE, 0);
    g_app.modified = TRUE;
    UpdateTitle(g_app.hwndMain);
    return count;
}

static void UpdateTitle(HWND hwnd) {
    WCHAR name[MAX_PATH_BUFFER];
    if (g_app.currentPath[0]) {
        WCHAR *fileName = (WCHAR *)wcsrchr(g_app.currentPath, L'\\');
        fileName = fileName ? fileName + 1 : g_app.currentPath;
        StringCchCopyW(name, MAX_PATH_BUFFER, fileName);
    } else {
        StringCchCopyW(name, MAX_PATH_BUFFER, UNTITLED_NAME);
    }

    WCHAR title[MAX_PATH_BUFFER + 32];
    StringCchPrintfW(title, ARRAYSIZE(title), L"%s%s - %s", (g_app.modified ? L"*" : L""), name, APP_TITLE);
    SetWindowTextW(hwnd, title);
}

static void ApplyFontToEdit(HWND hwndEdit, HFONT font) {
    SendMessageW(hwndEdit, WM_SETFONT, (WPARAM)font, TRUE);
}

static void CreateEditControl(HWND hwnd) {
    if (g_app.hwndEdit) {
        DestroyWindow(g_app.hwndEdit);
    }

    DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL;
    if (!g_app.wordWrap) {
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    }

    g_app.hwndEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, style, 0, 0, 0, 0, hwnd, (HMENU)1, g_hInst, NULL);
    if (g_app.hwndEdit && g_app.hFont) {
        ApplyFontToEdit(g_app.hwndEdit, g_app.hFont);
    }
    SendMessageW(g_app.hwndEdit, EM_SETLIMITTEXT, 0, 0); // allow large files
    UpdateLayout(hwnd);
}

static void ToggleStatusBar(HWND hwnd, BOOL visible) {
    g_app.statusVisible = visible;
    if (visible) {
        if (!g_app.hwndStatus) {
            g_app.hwndStatus = CreateStatusWindowW(WS_CHILD | SBARS_SIZEGRIP, L"", hwnd, 2);
        }
        ShowWindow(g_app.hwndStatus, SW_SHOW);
    } else if (g_app.hwndStatus) {
        ShowWindow(g_app.hwndStatus, SW_HIDE);
    }
    UpdateLayout(hwnd);
    UpdateStatusBar(hwnd);
    SaveSetting((LPCWSTR)L"StatusBarVisible", (const BYTE*)&visible, sizeof(visible));
}

static void ToggleCRLF(HWND hwnd, BOOL visible) {
    g_app.convertCRLF = visible;
    SaveSetting((LPCWSTR)L"ConvertCRLF", (const BYTE*)&visible, sizeof(visible));
}

static void UpdateLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    int statusHeight = 0;
    if (g_app.statusVisible && g_app.hwndStatus) {
        SendMessageW(g_app.hwndStatus, WM_SIZE, 0, 0);
        RECT sbrc;
        GetWindowRect(g_app.hwndStatus, &sbrc);
        statusHeight = sbrc.bottom - sbrc.top;
        MoveWindow(g_app.hwndStatus, 0, rc.bottom - statusHeight, rc.right, statusHeight, TRUE);
    }

    if (g_app.hwndEdit) {
        MoveWindow(g_app.hwndEdit, 0, 0, rc.right, rc.bottom - statusHeight, TRUE);
    }
}

static BOOL PromptSaveChanges(HWND hwnd) {
    if (!g_app.modified) return TRUE;

    WCHAR prompt[MAX_PATH_BUFFER + 64];
    const WCHAR *name = g_app.currentPath[0] ? g_app.currentPath : UNTITLED_NAME;
    StringCchPrintfW(prompt, ARRAYSIZE(prompt), L"Do you want to save changes to %s?", name);
    int res = MessageBoxW(hwnd, prompt, APP_TITLE, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (res == IDYES) {
        return DoFileSave(hwnd, FALSE);
    }
    return res == IDNO;
}

static LPCWSTR StripQuotes(LPWSTR path) {

    if (path[0] == (WCHAR)'"') {
        ++path;
    }
    int len = wcslen(path);
    if (path[len - 1] == (WCHAR)'"') {
        path[len - 1] = 0;
    }
    return path;
}

static BOOL LoadDocumentFromPath(HWND hwnd, LPCWSTR path) {

    path = StripQuotes((LPWSTR)path);

    WCHAR *text = NULL;
    TextEncoding enc = ENC_UTF8;
    if (!LoadTextFile(hwnd, path, &text, NULL, &enc)) {
        return FALSE;
    }

    SetWindowTextW(g_app.hwndEdit, text);

    // optionally convert LF to CRLF 
    if (g_app.convertCRLF && wcsstr(text, L"\n") && !wcsstr(text, L"\r\n")) {
        DoConvertCRLF(hwnd);
    }

    HeapFree(GetProcessHeap(), 0, text);
    StringCchCopyW(g_app.currentPath, ARRAYSIZE(g_app.currentPath), path);
    g_app.encoding = enc;
    SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
    g_app.modified = FALSE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
    return TRUE;
}

static BOOL DoFileOpen(HWND hwnd) {
    if (!PromptSaveChanges(hwnd)) return FALSE;

    WCHAR path[MAX_PATH_BUFFER] = L"";
    if (!OpenFileDialog(hwnd, path, ARRAYSIZE(path))) {
        return FALSE;
    }
    return LoadDocumentFromPath(hwnd, path);
}

static BOOL DoFileSave(HWND hwnd, BOOL saveAs) {
    WCHAR path[MAX_PATH_BUFFER];
    if (saveAs || g_app.currentPath[0] == L'\0') {
        path[0] = L'\0';
        if (g_app.currentPath[0]) {
            StringCchCopyW(path, ARRAYSIZE(path), g_app.currentPath);
        }
        if (!SaveFileDialog(hwnd, path, ARRAYSIZE(path))) {
            return FALSE;
        }
        StringCchCopyW(g_app.currentPath, ARRAYSIZE(g_app.currentPath), path);
    } else {
        StringCchCopyW(path, ARRAYSIZE(path), g_app.currentPath);
    }

    int len = GetWindowTextLengthW(g_app.hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!buffer) return FALSE;
    GetWindowTextW(g_app.hwndEdit, buffer, len + 1);

    BOOL ok = SaveTextFile(hwnd, path, buffer, len, g_app.encoding);
    HeapFree(GetProcessHeap(), 0, buffer);
    if (ok) {
        SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
        g_app.modified = FALSE;
        UpdateTitle(hwnd);
    }
    return ok;
}

static void DoFileNew(HWND hwnd) {
    if (!PromptSaveChanges(hwnd)) return;
    SetWindowTextW(g_app.hwndEdit, L"");
    g_app.currentPath[0] = L'\0';
    g_app.encoding = ENC_UTF8;
    SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
    g_app.modified = FALSE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
}

static void SetWordWrap(HWND hwnd, BOOL enabled) {
    if (g_app.wordWrap == enabled) return;
    g_app.wordWrap = enabled;
    HWND edit = g_app.hwndEdit;
    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(edit, &text, &len)) {
        return;
    }
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);

    CreateEditControl(hwnd);
    SetWindowTextW(g_app.hwndEdit, text);
    SendMessageW(g_app.hwndEdit, EM_SETSEL, start, end);
    HeapFree(GetProcessHeap(), 0, text);

    if (enabled) {
        g_app.statusBeforeWrap = g_app.statusVisible;
        ToggleStatusBar(hwnd, FALSE);
        EnableMenuItem(GetMenu(hwnd), IDM_OPTIONS_STATUS_BAR, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_GRAYED);
    } else {
        ToggleStatusBar(hwnd, g_app.statusBeforeWrap);
        EnableMenuItem(GetMenu(hwnd), IDM_OPTIONS_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
        EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_ENABLED);
    }
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
}

static void UpdateStatusBar(HWND hwnd) {
    if (!g_app.statusVisible || !g_app.hwndStatus) return;
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    int line = (int)SendMessageW(g_app.hwndEdit, EM_LINEFROMCHAR, selStart, 0) + 1;
    int col = (int)(selStart - SendMessageW(g_app.hwndEdit, EM_LINEINDEX, line - 1, 0)) + 1;
    int lines = (int)SendMessageW(g_app.hwndEdit, EM_GETLINECOUNT, 0, 0);

    WCHAR status[128];
    StringCchPrintfW(status, ARRAYSIZE(status), L"Ln %d, Col %d    Lines: %d", line, col, lines);
    SendMessageW(g_app.hwndStatus, SB_SETTEXT, 0, (LPARAM)status);
}

static void ShowFindDialog(HWND hwnd) {
    if (g_app.hFindDlg) {
        SetForegroundWindow(g_app.hFindDlg);
        return;
    }

    ZeroMemory(&g_app.find, sizeof(g_app.find));
    g_app.find.lStructSize = sizeof(FINDREPLACEW);
    g_app.find.hwndOwner = hwnd;
    g_app.find.lpstrFindWhat = g_app.findText;
    g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
    g_app.find.Flags = g_app.findFlags;

    g_app.hFindDlg = FindTextW(&g_app.find);
}

static void ShowReplaceDialog(HWND hwnd) {
    if (g_app.hReplaceDlg) {
        SetForegroundWindow(g_app.hReplaceDlg);
        return;
    }

    ZeroMemory(&g_app.find, sizeof(g_app.find));
    g_app.find.lStructSize = sizeof(FINDREPLACEW);
    g_app.find.hwndOwner = hwnd;
    g_app.find.lpstrFindWhat = g_app.findText;
    g_app.find.lpstrReplaceWith = g_app.replaceText;
    g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
    g_app.find.wReplaceWithLen = ARRAYSIZE(g_app.replaceText);
    g_app.find.Flags = g_app.findFlags;

    g_app.hReplaceDlg = ReplaceTextW(&g_app.find);
}

static BOOL DoFindNext(BOOL reverse) {
    if (g_app.findText[0] == L'\0') {
        ShowFindDialog(g_app.hwndMain);
        return FALSE;
    }

    DWORD start = 0, end = 0;
    SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    BOOL matchCase = (g_app.findFlags & FR_MATCHCASE) != 0;
    BOOL down = (g_app.findFlags & FR_DOWN) != 0;
    if (reverse) down = !down;
    DWORD searchStart = down ? end : start;
    DWORD outStart = 0, outEnd = 0;
    if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, searchStart, &outStart, &outEnd)) {
        SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
        SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
        return TRUE;
    }
    MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
    return FALSE;
}

static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemInt(dlg, IDC_GOTO_EDIT, 1, FALSE);
        HWND edit = GetDlgItem(dlg, IDC_GOTO_EDIT);
        SendMessageW(edit, EM_SETLIMITTEXT, 10, 0);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            BOOL ok = FALSE;
            UINT line = GetDlgItemInt(dlg, IDC_GOTO_EDIT, &ok, FALSE);
            if (!ok || line == 0) {
                MessageBoxW(dlg, L"Enter a valid line number.", APP_TITLE, MB_ICONWARNING);
                return TRUE;
            }
            int maxLine = (int)SendMessageW(g_app.hwndEdit, EM_GETLINECOUNT, 0, 0);
            if ((int)line > maxLine) line = (UINT)maxLine;
            int charIndex = (int)SendMessageW(g_app.hwndEdit, EM_LINEINDEX, line - 1, 0);
            if (charIndex >= 0) {
                SendMessageW(g_app.hwndEdit, EM_SETSEL, charIndex, charIndex);
                SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static BOOL SaveSetting(LPCWSTR label, const BYTE* data, DWORD size) {
    HKEY hKey;
    BOOL ret = FALSE;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (LPCWSTR)L"Software\\retropad\\Settings", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        if (RegSetValueExW(hKey, label, 0, REG_BINARY, data, size) == ERROR_SUCCESS) {
            ret = TRUE;
        }
        RegCloseKey(hKey);
    }
    return ret;
}

static BOOL LoadSetting(LPCWSTR label, BYTE *data, DWORD size) {
    HKEY hKey;
    BOOL ret = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (LPCWSTR)L"Software\\retropad\\Settings", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD valueSize = size;
        if (RegQueryValueExW(hKey, label, 0, NULL, data, &valueSize) == ERROR_SUCCESS) {
            if (valueSize == size) {
                ret = TRUE;
            }
        }
        RegCloseKey(hKey);
    }
    return ret;
}

static void DoSelectFont(HWND hwnd) {
    LOGFONTW lf = { 0 };
    if (g_app.hFont) {
        GetObjectW(g_app.hFont, sizeof(LOGFONTW), &lf);
    }
    else {
        SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(LOGFONTW), &lf, 0);
    }

    CHOOSEFONTW cf = { 0 };
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;

    if (ChooseFontW(&cf)) {
        if (SetSelectedFont(hwnd, &lf) == 0) {
            SaveSetting((LPCWSTR)L"Font", (const BYTE*)&lf, sizeof(LOGFONTW));
        }
    }
}

static void InitializeFont(HWND hwnd) {
    LOGFONTW lf;
    if (LoadSetting((LPCWSTR)L"Font", (LPBYTE)&lf, sizeof(LOGFONTW))) {
        SetSelectedFont(hwnd, &lf);
    }
}

static int SetSelectedFont(HWND hwnd, LOGFONTW* lf) {
    HFONT newFont = CreateFontIndirectW(lf);
    if (newFont) {
        if (g_app.hFont) DeleteObject(g_app.hFont);
        g_app.hFont = newFont;
        ApplyFontToEdit(g_app.hwndEdit, g_app.hFont);
        UpdateLayout(hwnd);
        return 0;
    }
    return 1;
}

static void InsertTimeDate(HWND hwnd) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR date[64], time[64], stamp[128];
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, ARRAYSIZE(date));
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time, ARRAYSIZE(time));
    StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%s %s", time, date);
    SendMessageW(g_app.hwndEdit, EM_REPLACESEL, TRUE, (LPARAM)stamp);
}

static void HandleFindReplace(LPFINDREPLACE lpfr) {
    if (lpfr->Flags & FR_DIALOGTERM) {
        g_app.hFindDlg = NULL;
        g_app.hReplaceDlg = NULL;
        return;
    }

    g_app.findFlags = lpfr->Flags;
    if (lpfr->lpstrFindWhat && lpfr->lpstrFindWhat[0]) {
        StringCchCopyW((STRSAFE_LPWSTR)g_app.findText, ARRAYSIZE(g_app.findText), (STRSAFE_LPCWSTR)lpfr->lpstrFindWhat);
    }
    if (lpfr->lpstrReplaceWith) {
        StringCchCopyW((STRSAFE_LPWSTR)g_app.replaceText, ARRAYSIZE(g_app.replaceText), (STRSAFE_LPCWSTR)lpfr->lpstrReplaceWith);
    }

    BOOL matchCase = (lpfr->Flags & FR_MATCHCASE) != 0;
    BOOL down = (lpfr->Flags & FR_DOWN) != 0;

    if (lpfr->Flags & FR_FINDNEXT) {
        DWORD start = 0, end = 0;
        SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        DWORD searchStart = down ? end : start;
        DWORD outStart = 0, outEnd = 0;
        if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, searchStart, &outStart, &outEnd)) {
            SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
            SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
        }
        else {
            MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
        }
    }
    else if (lpfr->Flags & FR_REPLACE) {
        DWORD start = 0, end = 0;
        SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        DWORD outStart = 0, outEnd = 0;
        if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, start, &outStart, &outEnd)) {
            SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
            SendMessageW(g_app.hwndEdit, EM_REPLACESEL, TRUE, (LPARAM)g_app.replaceText);
            SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
            g_app.modified = TRUE;
            UpdateTitle(g_app.hwndMain);
        }
        else {
            MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
        }
    }
    else if (lpfr->Flags & FR_REPLACEALL) {
        int replaced = ReplaceAllOccurrences(g_app.hwndEdit, g_app.findText, g_app.replaceText, matchCase);
        WCHAR msg[64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"Replaced %d occurrence%s.", replaced, replaced == 1 ? L"" : L"s");
        MessageBoxW(g_app.hwndMain, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
        if (g_app.hReplaceDlg) {
            SetForegroundWindow(g_app.hReplaceDlg);
        }
    }
}

static void UpdateMenuStates(HWND hwnd) {
    HMENU menu = GetMenu(hwnd);
    if (!menu) return;

    UINT wrapState = g_app.wordWrap ? MF_CHECKED : MF_UNCHECKED;
    UINT statusState = g_app.statusVisible ? MF_CHECKED : MF_UNCHECKED;
    UINT crlfState = g_app.convertCRLF ? MF_CHECKED : MF_UNCHECKED;
    CheckMenuItem(menu, IDM_FORMAT_WORD_WRAP, MF_BYCOMMAND | wrapState);
    CheckMenuItem(menu, IDM_OPTIONS_STATUS_BAR, MF_BYCOMMAND | statusState);
    CheckMenuItem(menu, IDM_OPTIONS_CONVERT_CRLF, MF_BYCOMMAND | crlfState);

    BOOL canGoTo = !g_app.wordWrap;
    EnableMenuItem(menu, IDM_EDIT_GOTO, MF_BYCOMMAND | (canGoTo ? MF_ENABLED : MF_GRAYED));
    if (g_app.wordWrap) {
        EnableMenuItem(menu, IDM_OPTIONS_STATUS_BAR, MF_BYCOMMAND | MF_GRAYED);
    } else {
        EnableMenuItem(menu, IDM_OPTIONS_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
    }

    BOOL modified = (SendMessageW(g_app.hwndEdit, EM_GETMODIFY, 0, 0) != 0);
    EnableMenuItem(menu, IDM_FILE_SAVE, MF_BYCOMMAND | (modified ? MF_ENABLED : MF_GRAYED));
}

static void HandleCommand(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    switch (LOWORD(wParam)) {
    case IDM_FILE_NEW:
        DoFileNew(hwnd);
        break;
    case IDM_FILE_OPEN:
        DoFileOpen(hwnd);
        break;
    case IDM_FILE_SAVE:
        DoFileSave(hwnd, FALSE);
        break;
    case IDM_FILE_SAVE_AS:
        DoFileSave(hwnd, TRUE);
        break;
    case IDM_FILE_PAGE_SETUP:
	DoPageSetup(hwnd);
	break;
    case IDM_FILE_PRINT:
	DoPrint(hwnd);
        break;
    case IDM_FILE_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        break;

    case IDM_EDIT_UNDO:
        SendMessageW(g_app.hwndEdit, EM_UNDO, 0, 0);
        break;
    case IDM_EDIT_CUT:
        SendMessageW(g_app.hwndEdit, WM_CUT, 0, 0);
        break;
    case IDM_EDIT_COPY:
        SendMessageW(g_app.hwndEdit, WM_COPY, 0, 0);
        break;
    case IDM_EDIT_PASTE:
        SendMessageW(g_app.hwndEdit, WM_PASTE, 0, 0);
        break;
    case IDM_EDIT_DELETE:
        SendMessageW(g_app.hwndEdit, WM_CLEAR, 0, 0);
        break;
    case IDM_EDIT_FIND:
        ShowFindDialog(hwnd);
        break;
    case IDM_EDIT_FIND_NEXT:
        DoFindNext(FALSE);
        break;
    case IDM_EDIT_REPLACE:
        ShowReplaceDialog(hwnd);
        break;
    case IDM_EDIT_GOTO:
        if (g_app.wordWrap) {
            MessageBoxW(hwnd, L"Go To is unavailable when Word Wrap is on.", APP_TITLE, MB_ICONINFORMATION);
        } else {
            DialogBoxW(g_hInst, (LPCWSTR)MAKEINTRESOURCE(IDD_GOTO), hwnd, GoToDlgProc);
        }
        break;
    case IDM_EDIT_SELECT_ALL:
        SendMessageW(g_app.hwndEdit, EM_SETSEL, 0, -1);
        break;
    case IDM_EDIT_TIME_DATE:
        InsertTimeDate(hwnd);
        break;

    case IDM_FORMAT_WORD_WRAP:
        SetWordWrap(hwnd, !g_app.wordWrap);
        break;
    case IDM_FORMAT_FONT:
        DoSelectFont(hwnd);
        break;

    case IDM_OPTIONS_STATUS_BAR:
        ToggleStatusBar(hwnd, !g_app.statusVisible);
        break;

    case IDM_OPTIONS_CONVERT_CRLF:
        ToggleCRLF(hwnd, !g_app.convertCRLF);
        break;

    case IDM_HELP_VIEW_HELP:
        MessageBoxW(hwnd, L"No help file is available for retropad.", APP_TITLE, MB_ICONINFORMATION);
        break;
    case IDM_HELP_ABOUT:
        DialogBoxW(g_hInst, (LPCWSTR)MAKEINTRESOURCE(IDD_ABOUT), hwnd, AboutDlgProc);
        break;
    }
}

static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    if (msg == g_findMsg) {
        HandleFindReplace((LPFINDREPLACE)lParam);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        CreateEditControl(hwnd);
        LoadSetting((LPCWSTR)L"StatusBarVisible", (LPBYTE)&g_app.statusVisible, sizeof(BOOL));
        LoadSetting((LPCWSTR)L"ConvertCRLF", (LPBYTE)&g_app.convertCRLF, sizeof(BOOL));
        ToggleStatusBar(hwnd, g_app.statusVisible);
        UpdateTitle(hwnd);
        UpdateStatusBar(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        InitializeFont(hwnd);
        return 0;
    }
    case WM_SETFOCUS:
        if (g_app.hwndEdit) SetFocus(g_app.hwndEdit);
        return 0;
    case WM_SIZE:
        UpdateLayout(hwnd);
        UpdateStatusBar(hwnd);
        return 0;
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        WCHAR path[MAX_PATH_BUFFER];
        if (DragQueryFileW(hDrop, 0, path, ARRAYSIZE(path))) {
            if (PromptSaveChanges(hwnd)) {
                LoadDocumentFromPath(hwnd, path);
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == g_app.hwndEdit) {
            g_app.modified = (SendMessageW(g_app.hwndEdit, EM_GETMODIFY, 0, 0) != 0);
            UpdateTitle(hwnd);
            UpdateStatusBar(hwnd);
            return 0;
        }
        else if (HIWORD(wParam) == EN_UPDATE && (HWND)lParam == g_app.hwndEdit) {
            UpdateStatusBar(hwnd);
            return 0;
        }
        HandleCommand(hwnd, wParam, lParam);
        return 0;
    case WM_INITMENUPOPUP:
        UpdateMenuStates(hwnd);
        return 0;
    case WM_CLOSE:
        if (PromptSaveChanges(hwnd)) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    g_hInst = hInstance;
    g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
    g_app.wordWrap = FALSE;
    g_app.statusVisible = TRUE;
    g_app.statusBeforeWrap = TRUE;
    g_app.encoding = ENC_UTF8;
    g_app.findFlags = FR_DOWN;

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, (LPCWSTR)MAKEINTRESOURCE(IDI_RETROPAD));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RETROPAD_WINDOW";
    wc.lpszMenuName = (LPCWSTR)MAKEINTRESOURCE(IDC_RETROPAD);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class.", APP_TITLE, MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, DEFAULT_WIDTH, DEFAULT_HEIGHT,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window.", APP_TITLE, MB_ICONERROR);
        return 0;
    }

    g_app.hwndMain = hwnd;

    // check for /register before showing the window
    if (lpCmdLine && wcslen(lpCmdLine) > 0 && !wcscmp(lpCmdLine, L"/register")) {
        DoRegisterExtension(hwnd);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (lpCmdLine && wcslen(lpCmdLine) > 0) {
        LoadDocumentFromPath(hwnd, lpCmdLine);
    }

    HACCEL accel = LoadAcceleratorsW(hInstance, (LPCWSTR)MAKEINTRESOURCE(IDC_RETROPAD));

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!accel || !TranslateAcceleratorW(hwnd, accel, &msg)) {
            if (g_app.hFindDlg && IsDialogMessageW(g_app.hFindDlg, &msg)) continue;
            if (g_app.hReplaceDlg && IsDialogMessageW(g_app.hReplaceDlg, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}

static void DoPageSetup(HWND hwnd) {
    PAGESETUPDLG psd = { 0 };
    psd.lStructSize = sizeof(PAGESETUPDLG);
    psd.hwndOwner = hwnd;
    psd.Flags = PSD_MARGINS | PSD_NONETWORKBUTTON;

    if (!LoadSetting((LPCWSTR)L"Margins", (BYTE*)&psd.rtMargin, sizeof(RECT))) {
        psd.rtMargin.top = DEFAULT_MARGINS;
        psd.rtMargin.left = DEFAULT_MARGINS;
        psd.rtMargin.right = DEFAULT_MARGINS;
        psd.rtMargin.bottom = DEFAULT_MARGINS;
    }

    HGLOBAL hDevMode = GlobalAlloc(GHND, sizeof(DEVMODEW));
    if (hDevMode) {
        DEVMODEW *pDevMode = (DEVMODEW *)GlobalLock(hDevMode);
        if (pDevMode) {
            if (LoadSetting((LPCWSTR)L"DevMode", (BYTE*)pDevMode, sizeof(DEVMODEW))) {
                psd.hDevMode = hDevMode;
            }
            GlobalUnlock(hDevMode);
        }
    }

    HGLOBAL hDevNames = GlobalAlloc(GHND, sizeof(DEVNAMES));
    if (hDevNames) {
        DEVNAMES* pDevNames = (DEVNAMES*)GlobalLock(hDevNames);
        if (pDevNames) {
            if (LoadSetting((LPCWSTR)L"DevNames", (BYTE*)pDevNames, sizeof(DEVNAMES))) {
                psd.hDevNames = hDevNames;
            }
            GlobalUnlock(hDevNames);
        }
    }

    if (PageSetupDlg(&psd)) {
        SaveSetting((LPCWSTR)L"Margins", (const BYTE*)&psd.rtMargin, sizeof(RECT));
        if (psd.hDevMode) {
            DEVMODEW* pDevMode = (DEVMODEW*)GlobalLock(psd.hDevMode);
            if (pDevMode) {
                SaveSetting((LPCWSTR)L"DevMode", (const BYTE*)pDevMode, sizeof(DEVMODEW));
                GlobalUnlock(psd.hDevMode);
            }
            GlobalFree(psd.hDevMode);
        }
        if (psd.hDevNames) {
            DEVNAMES* pDevNames = (DEVNAMES*)GlobalLock(psd.hDevNames);
            if (pDevNames) {
                SaveSetting((LPCWSTR)L"DevNames", (const BYTE*)pDevNames, sizeof(DEVNAMES));
                GlobalUnlock(psd.hDevNames);
            }
            GlobalFree(psd.hDevNames);
        }
    }

    if (hDevMode) {
        GlobalFree(hDevMode);
    }
    if (hDevNames) {
        GlobalFree(hDevNames);
    }
}

static BOOL AddFinalCRLF(WCHAR** buffer, int* len) {
    if (*len < 2 || !((*buffer)[*len - 2] == (WCHAR)'\r' && (*buffer)[*len - 1] == (WCHAR)'\n')) {
        *len += 2;
        *buffer = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (void*)*buffer, (*len + 1) * sizeof(WCHAR));
        if (*buffer) {
            (*buffer)[*len - 2] = (WCHAR)'\r';
            (*buffer)[*len - 1] = (WCHAR)'\n';
        }
        else {
            MessageBoxW(g_app.hwndMain, (LPCWSTR)L"Memory allocation failed.", (LPCWSTR)L"retropad", MB_ICONERROR);
            return FALSE;
        }
    }
    return TRUE;
}

static void DoPrint(HWND hwnd) {

    WCHAR* buffer = NULL;
    int len = 0;
    if (!GetEditText(g_app.hwndEdit, &buffer, &len)) return;
    if (!AddFinalCRLF(&buffer, &len)) return;

    PRINTDLGW pd = { 0 };
    pd.lStructSize = sizeof(PRINTDLGW);
    pd.hwndOwner = hwnd;
    pd.Flags = PD_RETURNDC;
    pd.hDevMode = NULL;
    pd.hDevNames = NULL;
    pd.nMinPage = 1;
    pd.nMaxPage = 100;
    pd.nCopies = 1;
    pd.nFromPage = 1;
    pd.nToPage = 100;

    RECT margin = { DEFAULT_MARGINS, DEFAULT_MARGINS, DEFAULT_MARGINS, DEFAULT_MARGINS };
    LoadSetting((LPCWSTR)L"Margins", (BYTE*)&margin, sizeof(RECT));

    HGLOBAL hDevMode = GlobalAlloc(GHND, sizeof(DEVMODEW));
    if (hDevMode) {
        DEVMODEW *pDevMode = (DEVMODEW*)GlobalLock(hDevMode);
        if (pDevMode) {
            if (LoadSetting((LPCWSTR)L"DevMode", (BYTE*)pDevMode, sizeof(DEVMODEW))) {
                pd.hDevMode = hDevMode;
            }
        }
        GlobalUnlock(hDevMode);
    }

    HGLOBAL hDevNames = GlobalAlloc(GHND, sizeof(DEVNAMES));
    if (hDevNames) {
        DEVNAMES *pDevNames = (DEVNAMES *)GlobalLock(hDevNames);
        if (pDevNames) {
            if (LoadSetting((LPCWSTR)L"DevNames", (BYTE*)pDevNames, sizeof(DEVNAMES))) {
                pd.hDevNames = hDevNames;
            }
        }
        GlobalUnlock(hDevNames);
    }

    if (PrintDlg(&pd)) {
        LOGFONTW lf = { 0 };
        HFONT oldFont = NULL;
        HFONT printerFont = NULL;
        if (LoadSetting((LPCWSTR)L"Font", (LPBYTE)&lf, sizeof(LOGFONTW))) {
            int printerDPI = GetDeviceCaps(pd.hDC, LOGPIXELSY);
            /* scale font from screen to printer DPI */
            lf.lfHeight = -MulDiv(-lf.lfHeight, printerDPI, 72);
            printerFont = CreateFontIndirectW(&lf);
            oldFont = SelectObject(pd.hDC, printerFont);
        }
        DOCINFO di = { 0 };
        di.cbSize = sizeof(DOCINFO);
        di.lpszDocName = L"retropad print";

        int pixelWidth = GetDeviceCaps(pd.hDC, HORZRES);
        int pixelHeight = GetDeviceCaps(pd.hDC, VERTRES);
        int xPixelsPerInch = GetDeviceCaps(pd.hDC, LOGPIXELSX);
        int yPixelsPerInch = GetDeviceCaps(pd.hDC, LOGPIXELSY);

        int leftMargin = MulDiv(margin.left, yPixelsPerInch, 1000);
        int topMargin = MulDiv(margin.top, xPixelsPerInch, 1000);
        int rightMargin = pixelWidth - MulDiv(margin.right, xPixelsPerInch, 1000);
        int bottomMargin = pixelHeight - MulDiv(margin.bottom, yPixelsPerInch, 1000);

        StartDoc(pd.hDC, &di);
        int xpos = leftMargin;
        int ypos = topMargin;
        TEXTMETRIC tm;
        GetTextMetrics(pd.hDC, &tm);
        WCHAR* lineStart = buffer;
        WCHAR* lineEnd;
        BOOL inPage = FALSE;
        while (*lineStart != 0) {
            if (!inPage) {
                StartPage(pd.hDC);
                inPage = TRUE;
                ypos = topMargin;
                ExcludeClipRect(pd.hDC, rightMargin, 0, pixelWidth, pixelHeight);
            }
            lineEnd = wcsstr(lineStart, L"\r\n");
            if (lineEnd) {
                *lineEnd = 0;
            }
            TextOut(pd.hDC, xpos, ypos, lineStart, lineEnd - lineStart);
            ypos += tm.tmHeight;
            if ((ypos + tm.tmHeight) >= bottomMargin) {
                if (inPage) {
                    EndPage(pd.hDC);
                    inPage = FALSE;
                }
            }
            if (lineEnd) {
                lineStart = lineEnd + 2;
            }
            else {
                break;
            }
        }
        if (inPage) {
            EndPage(pd.hDC);
        }
        EndDoc(pd.hDC);
        if (oldFont) {
            SelectObject(pd.hDC, oldFont);
        }
        if (printerFont) {
            DeleteObject(printerFont);
        }
    	DeleteDC(pd.hDC);
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    if (hDevMode) {
        GlobalFree(hDevMode);
    }
    if (hDevNames) {
        GlobalFree(hDevNames);
    }
}

static void DoConvertCRLF(HWND hwnd) {
    int i, j;

    int length = GetWindowTextLengthW(g_app.hwndEdit);
    if (length == 0) {
        return;
    }
    WCHAR* ibuf = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
    if (!ibuf) {
        MessageBoxW(hwnd, L"Memory allocation failed.", L"retropad", MB_ICONERROR);
        return;
    }
    GetWindowTextW(g_app.hwndEdit, ibuf, length + 1);

    /* count newlines */
    int lines = 0;
    for(i=0; i<length; i++) {
	    if(ibuf[i] == (WCHAR)'\n') {
	        ++lines;
	    }
    }

    WCHAR* obuf = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, (length + lines + 1) * sizeof(WCHAR));
    if (!obuf) {
        MessageBoxW(hwnd, L"Memory allocation failed.", L"retropad", MB_ICONERROR);
	    HeapFree(GetProcessHeap(), 0, ibuf);
        return;
    }
    WCHAR rune;
    WCHAR lastRune = 0;
    for(i=0,j=0; i<length; i++) {
	    rune = ibuf[i];
	    if (rune == (WCHAR)'\n' && lastRune != (WCHAR)'\r') {
	        obuf[j++] = (WCHAR)'\r';
	    }
	    obuf[j++] = rune;
        lastRune = rune;
    }
    obuf[j]=0;
    SetWindowTextW(g_app.hwndEdit, (LPCWSTR)obuf);

    HeapFree(GetProcessHeap(), 0, ibuf);
    HeapFree(GetProcessHeap(), 0, obuf);
}

static BOOL PruneRegistryKey(HWND hwnd, LPCWSTR path, BOOL quiet) {
    //MessageBox(hwnd, path, L"PruneRegistryKey", MB_OK);
    if (RegDeleteTreeW(HKEY_CURRENT_USER, path) != ERROR_SUCCESS) {
        if (!quiet) {
            MessageBoxW(hwnd, L"Failed to delete registry path", L"retropad", MB_ICONERROR);
        }
        return FALSE;
    }
    return TRUE;
}

static HKEY CreateRegistryKey(HWND hwnd, LPCWSTR path) {
    //MessageBox(hwnd, path, L"CreateRegistryKey", MB_OK);

    HKEY hKey;
    if (RegCreateKeyW(HKEY_CURRENT_USER, path, &hKey) != ERROR_SUCCESS) {
        MessageBoxW(hwnd, L"Failed to create or open registry key.", L"retropad", MB_ICONERROR);
        return NULL;
    }
    return hKey;
}

static BOOL SetRegistryValue(HWND hwnd, HKEY hKey, LPCWSTR name, DWORD type, const BYTE* data, DWORD len) {
    //MessageBox(hwnd, name, L"SetRegistryValue", MB_OK);
    if (RegSetValueExW(hKey, name, 0, type, data, len) != ERROR_SUCCESS) {
        MessageBoxW(hwnd, L"Failed to set registry value", L"retropad", MB_ICONERROR);
        return FALSE;
    }
    return TRUE;
}

static BOOL CloseRegistryKey(HWND hwnd, HKEY hKey) {
    //MessageBox(hwnd, L"closing", L"CloseRegistryKey", MB_OK);
    if (RegCloseKey(hKey) != ERROR_SUCCESS) {
        MessageBoxW(hwnd, L"Failed to close registry key", L"retropad", MB_ICONERROR);
        return FALSE;
    }
    return TRUE;
}

static BOOL RegisterAppPath(HWND hwnd, LPCWSTR appPath) {
    BOOL ret = FALSE;
    LPCWSTR path = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\retropad.exe";
    PruneRegistryKey(hwnd, path, TRUE);
    HKEY hKey = CreateRegistryKey(hwnd, path);
    if (hKey) {
        ret = SetRegistryValue(hwnd, hKey, NULL, REG_SZ, (const BYTE*)appPath, (wcslen(appPath) + 1) * sizeof(WCHAR));
        CloseRegistryKey(hwnd, hKey);
    }
    return ret;
}

static BOOL RegisterFileType(HWND hwnd) {
    BOOL ret = FALSE;
    LPCWSTR path = L"Software\\Classes\\.txt";
    PruneRegistryKey(hwnd, path, TRUE);
    HKEY hKey = CreateRegistryKey(hwnd, path);
    if (!hKey) return FALSE;
    LPCWSTR progId = L"retropad.txt.1";
    ret = SetRegistryValue(hwnd, hKey, NULL, REG_SZ, (const BYTE *)progId, (wcslen(progId) + 1) * sizeof(WCHAR));
    if (!CloseRegistryKey(hwnd, hKey)) return FALSE;
    return ret;
}

static BOOL RegisterAppAssociation(HWND hwnd, LPCWSTR appPath) {
    BOOL ret = FALSE;
    PruneRegistryKey(hwnd, L"Software\\Classes\\retropad.txt.1", TRUE);
    HKEY hKey = CreateRegistryKey(hwnd, L"Software\\Classes\\retropad.txt.1\\shell\\open");
    if (!hKey) return FALSE;
    WCHAR command[MAX_PATH_BUFFER + 64];
    if (SUCCEEDED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" \"%%1\"", appPath))) {
        ret = TRUE;
    }
    if (ret) {
        ret = SetRegistryValue(hwnd, hKey, L"command", REG_SZ, (const BYTE *)command, (wcslen(command) + 1) * sizeof(WCHAR));
    }
    if (!CloseRegistryKey(hwnd, hKey)) return FALSE;
    return ret;
}

static void DoRegisterExtension(HWND hwnd) {

    if (!IsRunningAsAdmin()) {
        MessageBoxW(hwnd, L"Administrator rights are required for the /register command.", L"retropad", MB_ICONERROR);
        return;
    }

    WCHAR appPath[MAX_PATH_BUFFER];
    if (GetModuleFileNameW(NULL, appPath, ARRAYSIZE(appPath)) == 0) {
        MessageBoxW(hwnd, L"Failed reading module filename", L"retropad", MB_ICONERROR);
        return;
    }
    if (!RegisterAppPath(hwnd, appPath)) return;
    if (!RegisterFileType(hwnd)) return;
    if (!RegisterAppAssociation(hwnd, appPath)) return;

    // remove the explorer association
    PruneRegistryKey(hwnd, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.txt", TRUE);

    // inform the shell of changed file type associations
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSH, NULL, NULL);
}

BOOL IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    HANDLE hToken = NULL;
    TOKEN_ELEVATION elevation;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD size;
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
            isAdmin = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return isAdmin;
}

