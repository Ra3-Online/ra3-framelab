// gui_res.h -- resource ids and shared constants for Ra3FpsTest.exe
//
// 2026-09-17 / portable-GUI session (Claude)
//
// Kept separate from the .cpp so the .rc and the code cannot drift apart: the .rc
// #includes this file, so both sides see the same numbers.
//
// ASCII ONLY in this header and in the .rc -- link.exe/rc.exe read .def/.rc in the ANSI
// codepage and silently drop the line after a non-ASCII comment (that accident once shipped
// a decorated export name; see tools/re/check_exports.py). Comments here are English on purpose.

#ifndef GUI_RES_H
#define GUI_RES_H

// The patcher DLL, stored as a raw resource so the shipped tool is ONE self-contained file.
#define IDR_FRAMELAB_DLL   101

// Controls. Three of them are user-facing (see the layout comment in ra3fps_gui.cpp):
//   IDC_FPS_COMBO   -- which frame rate to test
//   IDC_DIR_EDIT / IDC_DIR_BROWSE -- the game folder (one logical control, shown as path + browse)
//   IDC_START       -- start the game and patch it
// IDC_LOG is output only, not a control the user operates.
#define IDC_FPS_COMBO      1001
#define IDC_DIR_EDIT       1002
#define IDC_DIR_BROWSE     1003
#define IDC_START          1004
#define IDC_LOG            1005

// Custom messages: the worker thread posts these to the window so all UI work happens on
// the UI thread. lParam of WM_APP_LOG is a heap wchar_t* that the UI thread frees.
#define WM_APP_LOG         (WM_APP + 1)
#define WM_APP_DONE        (WM_APP + 2)

#endif // GUI_RES_H
