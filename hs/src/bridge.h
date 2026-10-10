// The Half Sword half of the passthrough. Minecraft drives: the player's input goes to Minecraft (input.cpp), Half
// Sword's camera follows the camera of Minecraft's exported frames, and the Half Sword fighter becomes an invisible
// stand-in for Steve that the other fighters attack. The other fighters get proxies in Minecraft; hits on those come
// back as Half Sword damage, and hits on the stand-in go to Minecraft as damage to Steve.
#pragma once

namespace bridge
{
	void start();
	/// ProcessEvent hook, game thread, before the call: runs the tick at most once per rendered frame.
	void on_process_event();

	/// After a game-thread ProcessEvent has run: undoes anything that moved Steve's stand-in.
	void after_event(const void *object);
	/// ProcessEvent hook, game thread, before the call: true to skip it (hit events on Steve's stand-in).
	bool filter_event(const void *object, const void *function, void *parms);
	/// ProcessInternal hook (every Blueprint function), game thread: true to get after_script for this call.
	bool before_script(void *context, const void *function, void *locals);

	/// Game thread, before a Blueprint function or event runs: true to skip it (Half Sword ending the fight as Steve's
	/// loss while Steve is alive).
	/// Game thread: true to skip this Blueprint call (`parms`: its parameters, or null).
	bool block_script(const void *context, const void *function, const void *parms);
	/// Game thread, around each Blueprint call: which object's code is running (for block_script).
	void enter_script(const void *context);
	void leave_script();
	void after_script(void *context, const void *function, void *locals);
}
