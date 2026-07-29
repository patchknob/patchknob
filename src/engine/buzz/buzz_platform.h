//----------------------------------------------------------------------------
//  PatchKnob -- portable Win32 shim for Buzz machine source.
//
//  Buzz machines were written for Win32 (windows.h + commctrl for their config
//  dialogs, and LoadLibrary/GetProcAddress for the "dock" pattern editor).  This
//  header replaces that dependency with platform-independent definitions so the
//  machine's DSP compiles + runs on ANY platform: the Win32 types become plain
//  portable typedefs and the GUI calls become inert stubs (this app supplies its
//  own SDL editor, so the machine's own dialog is not needed).
//
//  Include this INSTEAD of <windows.h>/<commctrl.h>/<windef.h> in a ported
//  machine, and make sure the real windows.h is not pulled in for that TU.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_BUZZ_BUZZ_PLATFORM_H
#define PATCHKNOB_ENGINE_BUZZ_BUZZ_PLATFORM_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>

// ---- calling-convention / attribute keywords ------------------------------
// __cdecl/__stdcall are GCC keywords on Windows (leave them); off Windows make
// them no-ops.  CALLBACK/WINAPI/APIENTRY are windows.h macros -- always define
// them here since we deliberately do NOT include windows.h.
#ifndef _WIN32
  #define __cdecl
  #define __stdcall
#endif
#ifndef CALLBACK
  #define CALLBACK
#endif
#ifndef WINAPI
  #define WINAPI
#endif
#ifndef APIENTRY
  #define APIENTRY
#endif

// ---- basic Win32 types -----------------------------------------------------
typedef unsigned long   DWORD;
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef int             BOOL;
typedef unsigned int    UINT;
typedef unsigned int    UINT_PTR;
typedef intptr_t        INT_PTR;
typedef uintptr_t       WPARAM;
typedef intptr_t        LPARAM;
typedef intptr_t        LRESULT;
typedef char*           LPSTR;
typedef const char*     LPCSTR;
typedef char*           LPTSTR;
typedef const char*     LPCTSTR;
typedef void*           LPVOID;
typedef long            LONG;
typedef unsigned long   ULONG;
typedef void*           HANDLE;
typedef void*           HWND;
typedef void*           HMODULE;
typedef void*           HINSTANCE;
typedef void*           HGLOBAL;
typedef void*           HKEY;
typedef HKEY*           PHKEY;
typedef unsigned int    REGSAM;
typedef void*           FARPROC;
typedef INT_PTR (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
struct GUID { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; };
struct NMHDR { HWND hwndFrom; UINT_PTR idFrom; UINT code; };
struct NMUPDOWN { NMHDR hdr; int iPos; int iDelta; };

// DllMain reason codes (DllMain is defined but never called in a static build).
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define DLL_PROCESS_DETACH 0
// trackbar (slider) messages -- only used through the stubbed SendMessage.
#define WM_USER       0x0400
#define TBM_SETPOS    (WM_USER+5)
#define TBM_SETRANGE  (WM_USER+6)
#define UDN_DELTAPOS  (-722)   // up-down control notify (compared in a stub handler)

#ifndef MAX_PATH
  #define MAX_PATH 260
#endif

#ifndef TRUE
  #define TRUE  1
#endif
#ifndef FALSE
  #define FALSE 0
#endif
#ifndef NULL
  #define NULL  0
#endif

// ---- word/long packing macros ----------------------------------------------
#ifndef LOWORD
  #define LOWORD(l)  ((WORD)((DWORD)(l) & 0xffff))
#endif
#ifndef HIWORD
  #define HIWORD(l)  ((WORD)(((DWORD)(l) >> 16) & 0xffff))
#endif
#ifndef LOBYTE
  #define LOBYTE(w)  ((BYTE)((w) & 0xff))
#endif
#ifndef HIBYTE
  #define HIBYTE(w)  ((BYTE)(((w) >> 8) & 0xff))
#endif
#ifndef MAKELONG
  #define MAKELONG(a,b) ((long)(((WORD)(a)) | (((DWORD)((WORD)(b))) << 16)))
#endif
#ifndef MAKEWORD
  #define MAKEWORD(a,b) ((WORD)(((BYTE)(a)) | (((WORD)((BYTE)(b))) << 8)))
#endif
#ifndef MAKELPARAM
  #define MAKELPARAM(l,h) ((LPARAM)MAKELONG(l,h))
#endif

// ---- window / dialog messages (Win32 values; only used in the dead DialogProc)
#define WM_INITDIALOG   0x0110
#define WM_COMMAND      0x0111
#define WM_CLOSE        0x0010
#define WM_DESTROY      0x0002
#define WM_NOTIFY       0x004E
#define WM_SHOWWINDOW   0x0018
#define WM_TIMER        0x0113
#define WM_HSCROLL      0x0114
#define WM_VSCROLL      0x0115

// button / listbox / combobox / edit control messages
#define BM_SETCHECK     0x00F1
#define BM_GETCHECK     0x00F0
#define LB_ADDSTRING       0x0180
#define LB_DELETESTRING    0x0182
#define LB_RESETCONTENT    0x0184
#define LB_GETCOUNT        0x018B
#define LB_GETCARETINDEX   0x019F
#define LB_GETSELITEMS     0x0191
#define LB_GETSELCOUNT     0x0190
#define LB_SETCURSEL       0x0186
#define LB_GETCURSEL       0x0188
#define CB_ADDSTRING       0x0143
#define CB_RESETCONTENT    0x014B
#define CB_SETCURSEL       0x014E
#define CB_GETCURSEL       0x0147

#define SW_HIDE        0
#define SW_SHOWNORMAL  1
#define SW_SHOW        5
#define BST_CHECKED   1
#define BST_UNCHECKED 0

// control notification codes (WM_COMMAND HIWORD)
#define BN_CLICKED     0
#define EN_CHANGE      0x0300
#define LBN_ERRSPACE   (-2)
#define LBN_SELCHANGE  1
#define LBN_DBLCLK     2
#define LBN_SELCANCEL  3
#define LBN_SETFOCUS   4
#define LBN_KILLFOCUS  5

// dialog IDs + MessageBox flags + SetWindowPos flags
#define IDOK      1
#define IDCANCEL  2
#define MB_OK           0x0000
#define MB_ICONERROR    0x0010
#define MB_SYSTEMMODAL  0x1000
#define SWP_NOSIZE      0x0001
#define SWP_NOMOVE      0x0002
#define SWP_NOZORDER    0x0004
#define SWP_NOACTIVATE  0x0010
#define HWND_TOP        ((HWND)0)

// ---- inert GUI stubs -------------------------------------------------------
// The machine has no window here; these keep its config-dialog code compiling
// and harmless.  MessageBox routes to stderr so machine warnings are still seen.
inline int   MessageBox(HWND, const char* text, const char* cap, UINT) {
    std::fprintf(stderr, "[buzz] %s: %s\n", cap ? cap : "msg", text ? text : ""); return 0;
}
inline INT_PTR DialogBoxParam(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM) { return 0; }  // no dialog
inline void  EndDialog(HWND, INT_PTR) {}
inline HWND  GetDlgItem(HWND, int) { return NULL; }
inline BOOL  SetDlgItemText(HWND, int, LPCSTR) { return TRUE; }
inline BOOL  SetDlgItemInt(HWND, int, UINT, BOOL) { return TRUE; }
inline UINT  GetDlgItemInt(HWND, int, BOOL* ok, BOOL) { if (ok) *ok = FALSE; return 0; }
inline UINT  GetDlgItemText(HWND, int, LPSTR buf, int cap) { if (buf && cap) buf[0] = 0; return 0; }
inline BOOL  CheckDlgButton(HWND, int, UINT) { return TRUE; }
inline UINT  IsDlgButtonChecked(HWND, int) { return 0; }
inline LRESULT SendMessage(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline LRESULT SendDlgItemMessage(HWND, int, UINT, WPARAM, LPARAM) { return 0; }
inline BOOL  ShowWindow(HWND, int) { return TRUE; }
inline BOOL  EnableWindow(HWND, BOOL) { return TRUE; }
inline BOOL  SetFocus(HWND) { return TRUE; }
inline BOOL  InvalidateRect(HWND, const void*, BOOL) { return TRUE; }

inline BOOL  SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
inline void  InitCommonControls() {}
inline HWND  GetDesktopWindow() { return NULL; }
inline HWND  GetForegroundWindow() { return NULL; }
inline HWND  CreateDialog(HINSTANCE, LPCSTR, HWND, DLGPROC) { return NULL; }
inline BOOL  DestroyWindow(HWND) { return TRUE; }
inline long  GetDialogBaseUnits() { return 0; }
#ifndef MAKEINTRESOURCE
  #define MAKEINTRESOURCE(i) ((LPCSTR)(uintptr_t)((WORD)(i)))
#endif

// dock (companion pattern-editor DLL) -- disabled: the app has its own tracker.
inline HMODULE LoadLibrary(LPCSTR) { return NULL; }
inline FARPROC GetProcAddress(HMODULE, LPCSTR) { return NULL; }
inline BOOL    FreeLibrary(HMODULE) { return TRUE; }

// registry (used only to locate the dock DLL -> always "not found" so it skips).
#define HKEY_CURRENT_USER ((HKEY)0)
#define KEY_QUERY_VALUE   0x0001
#define ERROR_SUCCESS     0L
inline long RegOpenKeyEx(HKEY, LPCSTR, DWORD, REGSAM, PHKEY) { return 1; }   // != ERROR_SUCCESS
inline long RegQueryValueEx(HKEY, LPCSTR, DWORD*, DWORD*, BYTE*, DWORD*) { return 1; }
inline long RegCloseKey(HKEY) { return 0; }

#endif // PATCHKNOB_ENGINE_BUZZ_BUZZ_PLATFORM_H
