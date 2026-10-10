#include "input.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include "log.h"

namespace
{
	HWND g_hwnd = nullptr;
	WNDPROC g_original = nullptr;
	std::atomic<bool> g_route{false};
	std::atomic<bool> g_mcScreen{false};
	std::atomic<unsigned> g_hotkeys{0}; // bit per F-key: F7 = 1 << 7
	std::atomic<bool> g_escape{false};
	bool g_hsKeys[256] = {};    // keys Half Sword saw go down and not up (it had the input then)
	bool g_hsButtons[3] = {};   // and mouse buttons (left, right, middle)
	std::atomic<bool> g_shareG{false}; // G goes to Half Sword as well (its win screen: "hold G to continue")
	bool g_gInHalfSword = false;       // Half Sword has G as down from us
	bool g_held[512] = {};    // scancodes Minecraft has as down
	bool g_buttons[6] = {};   // mouse buttons Minecraft has as down
	std::recursive_mutex g_lock; // the events, the motion, and what Minecraft has as held (game thread and input thread)
	std::string g_events;
	long g_dx = 0, g_dy = 0;
	void (*g_send)(const std::string &) = nullptr;

	DWORD g_nextRegister = 0;
	// The raw mouse goes to a window of our own thread, so motion and clicks reach Minecraft the moment they happen
	// (Half Sword's window only reads its messages once per frame, and the bridge sent them later still, at its tick).
	HWND g_sink = nullptr;
	constexpr UINT kRegisterSink = WM_APP + 1;

	// SDL_Keymod
	constexpr int kModLShift = 0x0001, kModRShift = 0x0002, kModLCtrl = 0x0040, kModRCtrl = 0x0080, kModLAlt = 0x0100,
				  kModRAlt = 0x0200, kModLGui = 0x0400, kModRGui = 0x0800, kModCaps = 0x2000;
	constexpr int kScancodeMask = 1 << 30; // SDLK_SCANCODE_MASK

	void push(const char *event)
	{
		std::lock_guard<std::recursive_mutex> lock(g_lock);
		if (!g_events.empty())
			g_events += ',';
		g_events += event;
	}

	/// PC/AT set 1 scan code (with the E0 prefix as 0x100) -> USB HID usage = SDL_Scancode.
	int hid_from_set1(int sc)
	{
		static const unsigned char base[0x59] = {
			0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,   // 00: -, Esc, 1..0, -, =, Backspace, Tab
			20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,   // 10: Q W E R T Y U I O P [ ] Enter LCtrl A S
			7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,    // 20: D F G H J K L ; ' ` LShift \ Z X C V
			5, 17, 16, 54, 55, 56, 229, 85, 226, 44, 57, 58, 59, 60, 61, 62, // 30: B N M , . / RShift KP* LAlt Space Caps F1..F5
			63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,  // 40: F6..F10 NumLock ScrollLock KP7 KP8 KP9 KP- KP4 KP5 KP6 KP+ KP1
			90, 91, 98, 99, 0, 0, 100, 68, 69};                              // 50: KP2 KP3 KP0 KP. - - NonUS\ F11 F12
		if (sc < 0x100)
			return sc < 0x59 ? base[sc] : 0;
		switch (sc & 0xFF)
		{
		case 0x1C: return 88;  // KP Enter
		case 0x1D: return 228; // RCtrl
		case 0x35: return 84;  // KP /
		case 0x38: return 230; // RAlt
		case 0x47: return 74;  // Home
		case 0x48: return 82;  // Up
		case 0x49: return 75;  // PageUp
		case 0x4B: return 80;  // Left
		case 0x4D: return 79;  // Right
		case 0x4F: return 77;  // End
		case 0x50: return 81;  // Down
		case 0x51: return 78;  // PageDown
		case 0x52: return 73;  // Insert
		case 0x53: return 76;  // Delete
		case 0x5B: return 227; // LGui
		case 0x5C: return 231; // RGui
		case 0x5D: return 101; // Menu
		default: return 0;
		}
	}

	/// The SDL keycode for a scancode (US layout for the printable keys, which is what Minecraft's bindings use).
	int sdl_keycode(int hid, UINT vk)
	{
		if (hid >= 4 && hid <= 29)
			return 'a' + (hid - 4);
		if (hid >= 30 && hid <= 38)
			return '1' + (hid - 30);
		switch (hid)
		{
		case 39: return '0';
		case 40: return '\r';
		case 41: return 27;
		case 42: return '\b';
		case 43: return '\t';
		case 44: return ' ';
		case 45: return '-';
		case 46: return '=';
		case 47: return '[';
		case 48: return ']';
		case 49: return '\\';
		case 51: return ';';
		case 52: return '\'';
		case 53: return '`';
		case 54: return ',';
		case 55: return '.';
		case 56: return '/';
		case 76: return 127; // Delete
		default: return hid != 0 ? (hid | kScancodeMask) : 0;
		}
	}

	int sdl_mods()
	{
		int m = 0;
		auto down = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
		if (down(VK_LSHIFT)) m |= kModLShift;
		if (down(VK_RSHIFT)) m |= kModRShift;
		if (down(VK_LCONTROL)) m |= kModLCtrl;
		if (down(VK_RCONTROL)) m |= kModRCtrl;
		if (down(VK_LMENU)) m |= kModLAlt;
		if (down(VK_RMENU)) m |= kModRAlt;
		if (down(VK_LWIN)) m |= kModLGui;
		if (down(VK_RWIN)) m |= kModRGui;
		if (GetKeyState(VK_CAPITAL) & 1) m |= kModCaps;
		return m;
	}

	void key_event(WPARAM vk, LPARAM lp, bool down)
	{
		int sc = (lp >> 16) & 0xFF;
		if (lp & (1 << 24))
			sc |= 0x100;
		if (vk == VK_SHIFT && sc == 0x36)
			sc = 0x36; // right shift has no E0 prefix
		const int hid = hid_from_set1(sc);
		if (hid == 0)
			return;
		const int action = !down ? 0 : (lp & (1 << 30)) ? -1 : 1; // previous-state bit: auto-repeat
		if (!down && !g_held[hid])
			return; // released after the input moved here: Minecraft never saw it go down
		g_held[hid] = down;
		char e[96];
		snprintf(e, sizeof(e), "[\"k\",%d,%d,%d,%d]", hid, sdl_keycode(hid, UINT(vk)), sdl_mods(), action);
		push(e);
	}

	void text_event(wchar_t c)
	{
		static wchar_t high = 0;
		if (c >= 0xD800 && c <= 0xDBFF)
		{
			high = c;
			return;
		}
		wchar_t w[3] = {0};
		if (c >= 0xDC00 && c <= 0xDFFF && high != 0)
		{
			w[0] = high;
			w[1] = c;
		}
		else
			w[0] = c;
		high = 0;
		if (w[0] < 32 || w[0] == 127)
			return; // control characters arrive as keys
		char utf8[16] = {0};
		WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof(utf8), nullptr, nullptr);
		std::string escaped;
		for (const char *p = utf8; *p; ++p)
		{
			if (*p == '"' || *p == '\\')
				escaped += '\\';
			escaped += *p;
		}
		push(("[\"c\",\"" + escaped + "\"]").c_str());
	}

	/// Send what's queued: the motion first ({"t":"mv"}: Minecraft applies it just before it turns the player), then
	/// the events in order. Any thread; the lock keeps the order.
	void flush()
	{
		std::lock_guard<std::recursive_mutex> lock(g_lock);
		if (g_send == nullptr)
			return;
		if (g_dx != 0 || g_dy != 0)
		{
			char e[64];
			snprintf(e, sizeof(e), "{\"t\":\"mv\",\"d\":[%ld,%ld]}", g_dx, g_dy);
			g_dx = g_dy = 0;
			g_send(e);
		}
		if (!g_events.empty())
		{
			g_send("{\"t\":\"ev\",\"e\":[" + g_events + "]}");
			g_events.clear();
		}
	}

	void raw_mouse(const RAWMOUSE &m)
	{
		std::lock_guard<std::recursive_mutex> lock(g_lock);
		if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE))
		{
			g_dx += m.lLastX;
			g_dy += m.lLastY;
		}
		const int mods = sdl_mods();
		struct { USHORT down, up; int button; } buttons[] = {
			{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1},
			{RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2},
			{RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 3},
			{RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4},
			{RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5},
		};
		char e[64];
		for (const auto &b : buttons)
		{
			if (m.usButtonFlags & b.down)
			{
				g_buttons[b.button] = true;
				snprintf(e, sizeof(e), "[\"b\",%d,1,%d]", b.button, mods);
				push(e);
			}
			if ((m.usButtonFlags & b.up) && g_buttons[b.button])
			{
				g_buttons[b.button] = false;
				snprintf(e, sizeof(e), "[\"b\",%d,0,%d]", b.button, mods);
				push(e);
			}
		}
		if (m.usButtonFlags & RI_MOUSE_WHEEL)
		{
			snprintf(e, sizeof(e), "[\"w\",0,%.3f]", short(m.usButtonData) / 120.0);
			push(e);
		}
		if (m.usButtonFlags & RI_MOUSE_HWHEEL)
		{
			snprintf(e, sizeof(e), "[\"w\",%.3f,0]", short(m.usButtonData) / 120.0);
			push(e);
		}
	}

	/// Everything Minecraft has as held goes up (focus lost: the key-ups would go to another window).
	void release_all()
	{
		std::lock_guard<std::recursive_mutex> lock(g_lock);
		for (int hid = 0; hid < 512; ++hid)
			if (g_held[hid])
			{
				g_held[hid] = false;
				char e[96];
				snprintf(e, sizeof(e), "[\"k\",%d,%d,0,0]", hid, sdl_keycode(hid, 0));
				push(e);
			}
		for (int b = 1; b < 6; ++b)
			if (g_buttons[b])
			{
				g_buttons[b] = false;
				char e[64];
				snprintf(e, sizeof(e), "[\"b\",%d,0,0]", b);
				push(e);
			}
	}

	LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_ACTIVATEAPP || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || msg == WM_KILLFOCUS)
		{
			// the Windows key, Alt+Tab...: whatever was held is let go, and the raw mouse is ours again on return
			release_all();
			flush();
			g_nextRegister = 0;
		}
		if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) && (wp == VK_F7 || wp == VK_F8 || wp == VK_F9 || wp == VK_F10))
		{
			if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)))
				g_hotkeys |= 1u << (wp - VK_F1 + 1);
			return 0;
		}
		if (msg == WM_KEYDOWN && wp == VK_ESCAPE && !(lp & (1 << 30)) && !(g_route && g_mcScreen))
			g_escape = true; // this one goes to Half Sword
		if (!g_route)
		{
			// what Half Sword holds: let go of in Half Sword when the input goes back to Minecraft (else a key held then,
			// G above all, stays down in Half Sword for good: holding G in a fight gives up)
			if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) && wp < 256)
				g_hsKeys[wp] = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
			else if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
				g_hsButtons[0] = msg == WM_LBUTTONDOWN;
			else if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
				g_hsButtons[1] = msg == WM_RBUTTONDOWN;
			else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)
				g_hsButtons[2] = msg == WM_MBUTTONDOWN;
			return CallWindowProcW(g_original, hwnd, msg, wp, lp);
		}
		switch (msg)
		{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYUP:
		{
			const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
			if (msg == WM_SYSKEYDOWN && wp == VK_F4)
				break; // Alt+F4 still closes the game
			if (wp == VK_ESCAPE && !g_mcScreen)
				break; // no Minecraft screen open: Escape is Half Sword's menu
			if (wp == VK_LWIN || wp == VK_RWIN)
				break; // the Windows key is Windows': Minecraft never sees it (its key-up would go to the Start menu)
			key_event(wp, lp, down);
			flush();
			if (wp == 'G' && ((g_shareG && !g_mcScreen && down) || (!down && g_gInHalfSword)))
			{
				// the match is over: Half Sword's "hold G to continue" gets it too (Minecraft has no use for G)
				g_gInHalfSword = down;
				return CallWindowProcW(g_original, hwnd, msg, wp, lp);
			}
			return 0;
		}
		case WM_CHAR:
		case WM_SYSCHAR:
			if (wp == 27 && !g_mcScreen)
				break;
			text_event(wchar_t(wp));
			flush();
			return 0;
		case WM_INPUT:
		{
			RAWINPUT raw;
			UINT size = sizeof(raw);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
				raw.header.dwType == RIM_TYPEMOUSE)
			{
				raw_mouse(raw.data.mouse);
				flush();
				return DefWindowProcW(hwnd, msg, wp, lp); // cleanup only: Half Sword never sees it
			}
			break;
		}
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
			return 0; // buttons, wheel and motion come from raw input
		default:
			break;
		}
		return CallWindowProcW(g_original, hwnd, msg, wp, lp);
	}

	/// Which window (and program) has the focus instead of Half Sword's: logged now and then while Minecraft has the
	/// input, so a recorder's or an overlay's window that takes it (the input stops) shows up in the log.
	void note_foreground(HWND fg)
	{
		static ULONGLONG said = 0;
		if (GetTickCount64() - said < 3000)
			return;
		said = GetTickCount64();
		wchar_t cls[128] = L"", title[128] = L"", exe[MAX_PATH] = L"?";
		DWORD pid = 0;
		if (fg != nullptr)
		{
			GetClassNameW(fg, cls, 128);
			GetWindowTextW(fg, title, 128);
			GetWindowThreadProcessId(fg, &pid);
			if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); p != nullptr)
			{
				DWORD n = MAX_PATH;
				QueryFullProcessImageNameW(p, 0, exe, &n);
				CloseHandle(p);
			}
		}
		const wchar_t *name = wcsrchr(exe, L'\\');
		// (as UTF-8 here: the log's printf can't take every character of a window title as %ls, and writes nothing)
		auto utf8 = [](const wchar_t *w) {
			char out[256] = "";
			WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out) - 1, nullptr, nullptr);
			return std::string(out);
		};
		hslog::info("input: Minecraft has the input but %s (class %s, \"%s\", pid %lu%s) has the focus: the mouse and keys don't reach the game",
			utf8(name != nullptr ? name + 1 : exe).c_str(), utf8(cls).c_str(), utf8(title).c_str(), pid,
			pid == GetCurrentProcessId() ? ", Half Sword's own" : "");
	}

	/// The input thread's window: the raw mouse while Minecraft has the input.
	LRESULT CALLBACK sink_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == kRegisterSink)
		{
			// (a window is registered for raw input from its own thread)
			RAWINPUTDEVICE rid = {0x01, 0x02, RIDEV_INPUTSINK, hwnd};
			if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
				hslog::info("input: raw mouse to the input thread failed (%lu)", GetLastError());
			return 0;
		}
		if (msg == WM_INPUT)
		{
			RAWINPUT raw;
			UINT size = sizeof(raw);
			if (g_route && g_hwnd != nullptr)
				if (HWND fg = GetForegroundWindow(); fg != g_hwnd)
					note_foreground(fg);
			// (an input sink hears the mouse whatever window has the focus: only Half Sword's counts)
			if (g_route && g_hwnd != nullptr && GetForegroundWindow() == g_hwnd &&
				GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
				raw.header.dwType == RIM_TYPEMOUSE)
			{
				raw_mouse(raw.data.mouse);
				// a gaming mouse reports up to 1000 times a second: one message for all that is already waiting
				if (!((GetQueueStatus(QS_RAWINPUT) >> 16) & QS_RAWINPUT))
					flush();
			}
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	void sink_thread()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		WNDCLASSEXW wc = {sizeof(wc)};
		wc.lpfnWndProc = sink_proc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"HSPassthroughInput";
		RegisterClassExW(&wc);
		HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
		if (hwnd == nullptr)
		{
			hslog::info("input: no input thread window (%lu), the mouse goes through Half Sword's window", GetLastError());
			return;
		}
		g_sink = hwnd;
		hslog::info("input: input thread ready");
		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0)
			DispatchMessageW(&msg);
	}

	BOOL CALLBACK find_window(HWND hwnd, LPARAM out)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		wchar_t cls[64];
		if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"UnrealWindow") == 0)
		{
			*reinterpret_cast<HWND *>(out) = hwnd;
			return FALSE;
		}
		return TRUE;
	}
}

namespace input
{
	void attach()
	{
		if (g_hwnd == nullptr)
		{
			HWND hwnd = nullptr;
			EnumWindows(find_window, reinterpret_cast<LPARAM>(&hwnd));
			if (hwnd == nullptr)
				return;
			g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
			g_hwnd = hwnd;
			hslog::info("input: window %p subclassed", hwnd);
		}
		static bool started = false;
		if (!started)
		{
			started = true;
			std::thread(sink_thread).detach();
		}
		// the raw mouse to the input thread (Half Sword may register its own again when it shows a cursor: every second)
		if (g_route && GetTickCount() >= g_nextRegister)
		{
			g_nextRegister = GetTickCount() + 1000;
			if (g_sink != nullptr)
				PostMessageW(g_sink, kRegisterSink, 0, 0);
			else
			{
				RAWINPUTDEVICE rid = {0x01, 0x02, 0, g_hwnd};
				RegisterRawInputDevices(&rid, 1, sizeof(rid));
			}
		}
	}

	void set_sender(void (*send)(const std::string &))
	{
		std::lock_guard<std::recursive_mutex> lock(g_lock);
		g_send = send;
	}

	void flush()
	{
		::flush();
	}

	/// Everything Half Sword saw held when the input goes to Minecraft is let go of there (it never hears the key-ups).
	void release_in_half_sword()
	{
		if (g_hwnd == nullptr || g_original == nullptr)
			return;
		int released = 0;
		for (int vk = 1; vk < 256; ++vk)
		{
			if (!g_hsKeys[vk])
				continue;
			g_hsKeys[vk] = false;
			const UINT sc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
			CallWindowProcW(g_original, g_hwnd, WM_KEYUP, WPARAM(vk), LPARAM(1 | (sc << 16) | (1u << 30) | (1u << 31)));
			++released;
		}
		static const UINT ups[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
		for (int b = 0; b < 3; ++b)
			if (g_hsButtons[b])
			{
				g_hsButtons[b] = false;
				CallWindowProcW(g_original, g_hwnd, ups[b], 0, 0);
				++released;
			}
		if (released > 0)
			hslog::info("input: %d keys/buttons let go of in Half Sword", released);
	}

	void route_to_minecraft(bool on)
	{
		if (on != g_route.exchange(on))
		{
			hslog::info("input -> %s", on ? "Minecraft" : "Half Sword");
			if (on)
			{
				g_nextRegister = 0;
				release_in_half_sword();
			}
			else
			{
				// keys and buttons held when the input moved back to Half Sword: release them in Minecraft
				release_all();
				{
					std::lock_guard<std::recursive_mutex> lock(g_lock);
					g_dx = g_dy = 0;
				}
				::flush();
				// and the raw mouse is Half Sword's window's again
				if (g_hwnd != nullptr)
				{
					RAWINPUTDEVICE rid = {0x01, 0x02, 0, g_hwnd};
					RegisterRawInputDevices(&rid, 1, sizeof(rid));
				}
			}
		}
	}

	void share_continue_key(bool on)
	{
		g_shareG = on;
		if (!on && g_gInHalfSword && g_hwnd != nullptr && g_original != nullptr)
		{
			// held when the match started again: let go of it in Half Sword (holding G in a fight gives up)
			g_gInHalfSword = false;
			const UINT sc = MapVirtualKeyW('G', MAPVK_VK_TO_VSC);
			CallWindowProcW(g_original, g_hwnd, WM_KEYUP, 'G', LPARAM(1 | (sc << 16) | (1u << 30) | (1u << 31)));
		}
	}

	void set_minecraft_screen(bool open)
	{
		g_mcScreen = open;
	}

	bool take_hotkey(int vk)
	{
		const unsigned bit = 1u << (vk - VK_F1 + 1);
		return (g_hotkeys.fetch_and(~bit) & bit) != 0;
	}

	bool routed_to_minecraft()
	{
		return g_route;
	}

	bool take_escape_to_half_sword()
	{
		return g_escape.exchange(false);
	}
}
