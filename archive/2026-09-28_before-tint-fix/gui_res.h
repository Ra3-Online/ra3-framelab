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

// 2026-09-18 batch 11 -- the black box. The user tests on a machine that has ONLY the game
// (no tools, no scripts), so a recorder reachable only from flctl.exe would not be reachable at
// all. These two buttons are the whole reason the recorder is usable in the field.
//   IDC_BB_DUMP -- "save the scene now": dumps the flight-recorder ring to a file.
//                  The user is told to press this the moment the engine pops its DESYNC dialog,
//                  because that dialog freezes the sim and the 512 recorded frames are the only
//                  record of what led up to it.
//   IDC_BB_TEST -- "self-test": walks the real vectored-exception path and reports whether a
//                  report actually landed on disk. A recorder nobody ever tested is worse than
//                  none, because it is believed.
#define IDC_BB_DUMP        1006
#define IDC_BB_TEST        1007

// 2026-09-21 chassis-suspension session -- the vehicle body bounce fix as a visible checkbox.
//   IDC_CHASSIS_FIX -- checked (the default) = shipped groups + 0x380000 (chassis probe + 30 Hz
//                      cadence gate + pose interpolation); unchecked = the shipped groups only,
//                      byte-for-byte the old behaviour, which is what an A/B needs.
//   Why a checkbox and not just the RA3FL_GROUPS environment variable: measured on the user's
//   own test run, the fix was OFF (mask 0x2CFFF in both logs) because a double-clicked exe never
//   sees a variable set in some console. A switch nobody can reach is not a switch.
#define IDC_CHASSIS_FIX    1008
//   IDC_SCROLL_FIX  -- user-selectable (default UNCHECKED): keep keyboard / screen-edge scrolling at
//                      the vanilla distance per second instead of letting it scale with the frame
//                      rate (2x at 60 fps, 3x at 90 fps). Adds group 0x800000. A matter of feel,
//                      not of correctness, hence a switch. Right-button drag scrolling is not covered.
#define IDC_SCROLL_FIX     1009
//   IDC_FX_FIX      -- user-selectable (default UNCHECKED): the BUNDLE of "visual effects that run
//                      2x/3x fast at 60/90 fps" fixes. Groups 0x400000 (the 4th frame-rate derived
//                      constant 0xCDBD34 = seconds per display frame) | 0x1000000 (tracer-stream
//                      30 Hz cadence gate -- the "fires like a machine gun" symptom) | 0x2000000
//                      (status-icon ping-pong divisor) | 0x4000000 (particle-system manager 30 Hz
//                      cadence gate, added 2026-09-23 -- the power-plant / refinery glow) | 0x8000000
//                      (scripted-camera timing, added 2026-09-26 -- campaign cutscene camera). One checkbox
//                      rather than four because they
//                      are one class of bug; flctl still has each bit separately for bisection.
#define IDC_FX_FIX         1010

// Custom messages: the worker thread posts these to the window so all UI work happens on
// the UI thread. lParam of WM_APP_LOG is a heap wchar_t* that the UI thread frees.
#define WM_APP_LOG         (WM_APP + 1)
#define WM_APP_DONE        (WM_APP + 2)
// 2026-09-18 batch 11: the worker thread has just learned the game pid, so the two black-box
// buttons can become usable. EnableWindow must happen on the UI thread.
#define WM_APP_BB          (WM_APP + 3)

#endif // GUI_RES_H
