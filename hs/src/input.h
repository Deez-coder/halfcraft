// The player's keyboard and mouse, taken from Half Sword's window while Minecraft drives: each Windows message becomes
// the SDL event Minecraft would have seen (USB HID scancode, SDL keycode and modifiers; raw mouse motion), and Half
// Sword's own input never sees it.
#pragma once
#include <string>

namespace input
{
	/// Subclass the game window (game thread, once it exists). Safe to call every tick.
	void attach();
	/// Whether input goes to Minecraft (set each tick by the bridge).
	void route_to_minecraft(bool on);
	/// Whether G also goes to Half Sword while the input is Minecraft's (its win screen's "hold G to continue").
	void share_continue_key(bool on);
	/// Whether Minecraft has a screen open (Escape then closes it instead of opening Half Sword's menu).
	void set_minecraft_screen(bool open);
	/// Where the input goes, as JSON messages ({"t":"mv"} motion, {"t":"ev"} events): sent the moment it happens.
	void set_sender(void (*send)(const std::string &));
	/// Send anything still queued (the bridge, each tick).
	void flush();
	/// Whether a hotkey (F7, F8, F9, F10: never passed on) was pressed since the last call.
	bool take_hotkey(int vk);
	/// Whether Escape went to Half Sword (its menu opened or closed) since the last call.
	bool take_escape_to_half_sword();

	/// Whether the input goes to Minecraft now.
	bool routed_to_minecraft();
}
