// HSPassthrough.addon64: the Half Sword half of the Minecraft passthrough, loaded by ReShade (add-on support).
// ReShade loads and frees its add-ons several times while UE creates its D3D12 devices, so the module pins itself
// (it hooks the engine) and only its ReShade registration follows that lifecycle, through AddonInit/AddonUninit.
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include "MinHook.h"
#include "bridge.h"
#include "compositor.h"
#include "log.h"
#include "ue.h"

namespace
{
	HMODULE g_module = nullptr;
	DWORD g_gameThread = 0;
	std::atomic<bool> g_started{false};
	using ProcessEventFn = void (*)(const SDK::UObject *, SDK::UFunction *, void *);
	ProcessEventFn g_processEvent = nullptr;
	using ProcessInternalFn = void (*)(SDK::UObject *, void *, void *);
	ProcessInternalFn g_processInternal = nullptr;

	/// UE's game thread is the process's main thread: the oldest one.
	DWORD main_thread_id()
	{
		DWORD best = 0;
		ULONGLONG bestTime = ~0ull;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		THREADENTRY32 te = {sizeof(te)};
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
		{
			if (te.th32OwnerProcessID != GetCurrentProcessId())
				continue;
			if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID))
			{
				FILETIME created, exited, kernel, user;
				if (GetThreadTimes(h, &created, &exited, &kernel, &user))
				{
					const ULONGLONG t = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
					if (t < bestTime)
					{
						bestTime = t;
						best = te.th32ThreadID;
					}
				}
				CloseHandle(h);
			}
		}
		CloseHandle(snap);
		return best;
	}

	void hooked_process_event(const SDK::UObject *object, SDK::UFunction *function, void *parms)
	{
		if (GetCurrentThreadId() != g_gameThread)
		{
			g_processEvent(object, function, parms);
			return;
		}
		bridge::on_process_event();
		if (bridge::filter_event(object, function, parms))
			return;
		g_processEvent(object, function, parms);
	}

	/// UObject::ProcessInternal runs every Blueprint function, including BP-to-BP calls that never pass ProcessEvent.
	/// FFrame: FOutputDevice (0x10), then Node (UFunction*), Object, Code, Locals (the parameters).
	void hooked_process_internal(SDK::UObject *context, void *stack, void *result)
	{
		auto *function = *reinterpret_cast<SDK::UFunction **>(static_cast<uint8_t *>(stack) + 0x10);
		void *locals = *reinterpret_cast<void **>(static_cast<uint8_t *>(stack) + 0x28);
		const bool gameThread = GetCurrentThreadId() == g_gameThread;
		if (gameThread && bridge::block_script(context, function, locals))
			return; // (a void event: nothing to fill in)
		const bool watched = gameThread && bridge::before_script(context, function, locals);
		if (gameThread)
			bridge::enter_script(context);
		g_processInternal(context, stack, result);
		if (gameThread)
			bridge::leave_script();
		if (watched)
			bridge::after_script(context, function, locals);
	}

	DWORD WINAPI init_thread(LPVOID)
	{
		g_gameThread = main_thread_id();
		// wait for the engine: GObjects filled and a world up
		for (int i = 0; i < 600 && SDK::UWorld::GetWorld() == nullptr; ++i)
			Sleep(100);
		void *pe = reinterpret_cast<void *>(SDK::InSDKUtils::GetImageBase() + SDK::Offsets::ProcessEvent);
		if (MH_Initialize() != MH_OK || MH_CreateHook(pe, reinterpret_cast<void *>(&hooked_process_event), reinterpret_cast<void **>(&g_processEvent)) != MH_OK)
		{
			hslog::info("ProcessEvent hook failed");
			return 0;
		}
		// every Blueprint (non-native) function's ExecFunction is UObject::ProcessInternal: take it from one
		void *pi = nullptr;
		for (int tries = 0; tries < 600 && pi == nullptr; ++tries)
		{
			for (int i = 0; i < SDK::UObject::GObjects->Num() && pi == nullptr; ++i)
			{
				SDK::UObject *o = SDK::UObject::GObjects->GetByIndex(i);
				if (o == nullptr || !o->IsA(SDK::EClassCastFlags::Function))
					continue;
				auto *f = static_cast<SDK::UFunction *>(o);
				if (!(uint32_t(f->FunctionFlags) & 0x400) && f->ExecFunction != nullptr && o->Outer != nullptr &&
					o->Outer->Class->GetName() == "BlueprintGeneratedClass")
					pi = reinterpret_cast<void *>(f->ExecFunction);
			}
			if (pi == nullptr)
				Sleep(100);
		}
		if (pi == nullptr || MH_CreateHook(pi, reinterpret_cast<void *>(&hooked_process_internal), reinterpret_cast<void **>(&g_processInternal)) != MH_OK)
			hslog::info("ProcessInternal hook failed (fighters' hits on Steve won't reach Minecraft)");
		bridge::start();
		MH_EnableHook(MH_ALL_HOOKS);
		hslog::info("hooks: ProcessEvent %p, ProcessInternal %p, game thread %lu", pe, pi, g_gameThread);
		return 0;
	}
}

extern "C" __declspec(dllexport) const char *NAME = "Minecraft Passthrough";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Plays a running Minecraft (passthrough mod) inside Half Sword.";

/// ReShade, after loading this module (also again after each unload: the module stays, pinned).
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
	const bool ok = compositor::try_register(addon_module);
	hslog::info("ReShade add-on init: %s", ok ? "registered" : "failed");
	if (ok && !g_started.exchange(true))
		CloseHandle(CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr));
	return ok;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
	compositor::unregister(addon_module);
	hslog::info("ReShade add-on uninit");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = module;
		DisableThreadLibraryCalls(module);
		HMODULE pinned;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
		hslog::init(module);
		hslog::info("HSPassthrough loaded");
	}
	return TRUE;
}
