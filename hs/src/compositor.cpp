#include "compositor.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <d3d12.h>
#include <reshade.hpp>
#include "log.h"

using namespace reshade::api;

namespace
{
	constexpr const wchar_t *kMappingName = L"Local\\MCPassthroughFrame";
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
	constexpr int kHeader = 4096;
	constexpr int kSlotDesc = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr const char *kEffect = "MCPassthrough.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<ULONGLONG> g_activeAt{0}; // the host's last tick: no ticks (paused, loading, crashed hook) hides Minecraft
	std::atomic<float> g_hostNear{0.001f};
	std::atomic<float> g_hostFar{1.0e7f};
	std::atomic<uint32_t> g_bbWidth{0}, g_bbHeight{0};
	std::atomic<bool> g_cameraLocked{false};
	std::atomic<float> g_lookLight{-1.0f}, g_lookBias{-1.0f}, g_lookSlope{-1.0f};
	std::atomic<float> g_shakeX{0.0f}, g_shakeY{0.0f}, g_shakeRoll{0.0f}, g_portalWarp{0.0f};
	float g_savedLight = -1.0f, g_savedBias = -1.0f, g_savedSlope = -1.0f;

	struct Pose
	{
		float yaw = 0, pitch = 0, roll = 0, fov = 70;
		double x = 0, y = 0, z = 0;
		bool valid = false;
		int64_t mcFrame = -1; // host poses: the Minecraft frame the host's camera copied
		int64_t qpc = 0;      // host poses: when the bridge set it (its tick)
	};
	std::mutex g_poseLock;
	// GTA's recent camera poses, newest last. The script reads the camera GTA is about to render (the next frame),
	// while the picture being presented was rendered with the one before: re-project to that (g_poseLag back).
	Pose g_hostPoses[8];
	unsigned g_hostPoseCount = 0;
	std::atomic<int> g_poseLag{0}; // Half Sword runs with r.OneFrameThreadLag 0 + r.GTSyncType 2: the frame presented is the last one ticked
	Pose g_mcPose;

	HANDLE g_mapping = nullptr;
	const uint8_t *g_view = nullptr;
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	int64_t g_shownFrame = -1; // Minecraft frame number now in the textures
	// stats, logged every 2 s: presents, uploads, chosen frame's pose error (deg + 50 * m), exact matches
	struct Stats
	{
		ULONGLONG since = 0;
		int presents = 0, uploads = 0, exact = 0, withHost = 0, missed = 0, sameFrame = 0, torn = 0, staged = 0, waited = 0, direct = 0;
		int zeroCopy = 0, gpuLate = 0; // frames copied from the shared textures; frames there but not ready (or not ours)
		double uploadMs = 0, uploadMaxMs = 0;
		double ageMs = 0, publishMs = 0; // the shown frame: capture (Minecraft) to compose (here), and capture to publish
		double tickMs = 0;               // the bridge's tick (camera set) to the compose of that frame
		float errSum = 0, errMax = 0;
		int64_t publishedAt = -1;
	} g_stats;
	float g_lastError = 0;
	DWORD g_nextOpenAttempt = 0;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_world, g_depth, g_overlay;
	// Upload heap buffers Minecraft's frames are written into (persistently mapped), then copied to the textures on the
	// GPU timeline. A worker thread fills them the moment Minecraft publishes a frame (its event), so by the time Half
	// Sword composes, the frame its camera copied is normally staged already and the render thread only records the copy.
	// A stage the GPU may still be reading (used by one of the last kGpuFrames presents) is never rewritten.
	constexpr int kStaging = 8;
	constexpr uint64_t kGpuFrames = 3;
	struct Stage
	{
		resource buf = {0};
		uint8_t *mapped = nullptr;
		int64_t frame = -1;    // the Minecraft frame in it (-1: none, or being written)
		uint64_t usedAt = 0;   // the present that last copied from it (0: never)
		bool writing = false;
		uint8_t desc[kSlotDescBytes] = {}; // the frame's slot descriptor (pose, planes...), as it was staged
	};
	Stage g_stages[kStaging];
	std::mutex g_stageLock;            // the stages' frame/usedAt/writing
	std::shared_mutex g_stagesLife;    // copying into the stages (shared) vs creating/destroying them (exclusive)
	uint32_t g_stageW = 0, g_stageH = 0; // the frame size the stages are for (0: none)
	uint32_t g_stagingPitch = 0;   // bytes per row (D3D12: a multiple of 256)
	uint64_t g_stagingLayer = 0;   // bytes per layer (a multiple of 512)
	std::atomic<uint64_t> g_presents{0};
	std::atomic<int64_t> g_workerFrame{-1}; // the frame the worker is staging right now
	std::atomic<const uint8_t *> g_viewShared{nullptr};
	LARGE_INTEGER g_qpcFreq = {};
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;
	float g_mcNear = 0.05f, g_mcFar = 2048.0f;
	int32_t g_mcFlags = 7;

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	// ---- zero copy (GpuShare.java): textures shared by name with Minecraft, which draws its layers straight into
	// them (GPU to GPU) instead of ~25 MB a frame through the mapping and the upload buffers ----
	constexpr const wchar_t *kGpuMappingName = L"Local\\MCPassthroughGpu";
	constexpr uint32_t kGpuMagic = 0x5047434D; // "MCGP"
	constexpr int kGpuSlots = 6;               // Minecraft's slots: a frame's textures are its slot's
	constexpr int32_t kFlagGpu = 8;            // (slot flags) the frame is in the slot's shared textures, not in the mapping

	struct GpuShare
	{
		uint8_t *view = nullptr; // "Local\MCPassthroughGpu": what Minecraft opens (layout in GpuShare.java)
		ID3D12Fence *fence = nullptr;
		HANDLE fenceHandle = nullptr;
		ID3D12Resource *tex[kGpuSlots][3] = {};
		HANDLE handles[kGpuSlots][3] = {}; // (a name lives as long as a handle to it)
		uint32_t w = 0, h = 0;
		int32_t gen = 0;
		bool taken = false; // Minecraft said it opened them
		bool off = false;   // they couldn't be made, or Minecraft couldn't open them: the old way only
	} g_gpu;

	template <typename T>
	void write(uint8_t *p, T v)
	{
		std::memcpy(p, &v, sizeof(T));
	}

	uint8_t *gpu_view()
	{
		if (g_gpu.view == nullptr)
			if (HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, kGpuMappingName))
				g_gpu.view = static_cast<uint8_t *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 4096));
		return g_gpu.view;
	}

	/// (only once the GPU is done with them: a wait for idle, or never used)
	void destroy_gpu()
	{
		if (g_gpu.view != nullptr)
			write<int32_t>(g_gpu.view + 28, 0);
		for (int s = 0; s < kGpuSlots; ++s)
			for (int l = 0; l < 3; ++l)
			{
				if (g_gpu.tex[s][l] != nullptr)
					g_gpu.tex[s][l]->Release();
				if (g_gpu.handles[s][l] != nullptr)
					CloseHandle(g_gpu.handles[s][l]);
				g_gpu.tex[s][l] = nullptr;
				g_gpu.handles[s][l] = nullptr;
			}
		if (g_gpu.fence != nullptr)
			g_gpu.fence->Release();
		if (g_gpu.fenceHandle != nullptr)
			CloseHandle(g_gpu.fenceHandle);
		g_gpu.fence = nullptr;
		g_gpu.fenceHandle = nullptr;
		g_gpu.w = g_gpu.h = 0;
		g_gpu.taken = false;
	}

	/// Shared textures for Minecraft's w x h frames (each slot: world, depth R32F, overlay) and the fence it signals,
	/// under a new generation of names.
	void ensure_gpu(device *dev, uint32_t w, uint32_t h)
	{
		uint8_t *v = gpu_view();
		if (g_gpu.off || v == nullptr || (g_gpu.w == w && g_gpu.h == h))
			return;
		destroy_gpu();
		if (dev->get_api() != device_api::d3d12)
		{
			g_gpu.off = true;
			return;
		}
		auto *d3d = reinterpret_cast<ID3D12Device *>(dev->get_native());
		const int32_t gen = std::max(read<int32_t>(v + 8), g_gpu.gen) + 1;
		const DWORD pid = GetCurrentProcessId();
		wchar_t name[96];
		swprintf_s(name, L"Local\\MCPT_%lu_%d_f", pid, gen);
		bool ok = SUCCEEDED(d3d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_gpu.fence))) &&
			SUCCEEDED(d3d->CreateSharedHandle(g_gpu.fence, nullptr, GENERIC_ALL, name, &g_gpu.fenceHandle));
		const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM};
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		uint64_t sizes[3] = {};
		for (int l = 0; l < 3 && ok; ++l)
		{
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			rd.Width = w;
			rd.Height = h;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.Format = formats[l];
			rd.SampleDesc.Count = 1;
			rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			sizes[l] = d3d->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
			for (int s = 0; s < kGpuSlots && ok; ++s)
			{
				swprintf_s(name, L"Local\\MCPT_%lu_%d_%d_%d", pid, gen, s, l);
				ok = SUCCEEDED(d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
						 IID_PPV_ARGS(&g_gpu.tex[s][l]))) &&
					SUCCEEDED(d3d->CreateSharedHandle(g_gpu.tex[s][l], nullptr, GENERIC_ALL, name, &g_gpu.handles[s][l]));
			}
		}
		if (!ok)
		{
			hslog::info("compositor: couldn't make the shared textures: Minecraft's frames come the old way");
			destroy_gpu();
			g_gpu.off = true;
			return;
		}
		g_gpu.w = w;
		g_gpu.h = h;
		g_gpu.gen = gen;
		write<uint32_t>(v, kGpuMagic);
		write<int32_t>(v + 4, 1);
		write<int32_t>(v + 8, gen);
		write<int32_t>(v + 12, int32_t(w));
		write<int32_t>(v + 16, int32_t(h));
		write<int32_t>(v + 20, kGpuSlots);
		write<int32_t>(v + 24, int32_t(pid));
		for (int l = 0; l < 3; ++l)
			write<int64_t>(v + 32 + 8 * l, int64_t(sizes[l]));
		MemoryBarrier();
		write<int32_t>(v + 28, 1);
		hslog::info("compositor: shared textures for Minecraft's %ux%u frames (generation %d)", w, h, gen);
	}

	/// What Minecraft made of the textures: taken (logged once), or not (back to the old way for good).
	void check_gpu()
	{
		if (g_gpu.gen == 0 || g_gpu.off || g_gpu.view == nullptr)
			return;
		if (!g_gpu.taken && read<int32_t>(g_gpu.view + 64) == g_gpu.gen)
		{
			g_gpu.taken = true;
			hslog::info("compositor: Minecraft draws straight into the shared textures (zero copy)");
		}
		if (read<int32_t>(g_gpu.view + 68) == g_gpu.gen)
		{
			hslog::info("compositor: Minecraft couldn't open the shared textures: its frames come the old way");
			destroy_gpu(); // (never used: Minecraft wrote nothing into them)
			g_gpu.off = true;
		}
	}

	/// Record the copy of the slot's shared textures into the effect's: the frame Minecraft drew there. False if it
	/// isn't there (an older generation, the fence not passed yet).
	bool copy_shared(command_list *cmd, int32_t slot, const uint8_t *desc, const resource (&textures)[3])
	{
		const int64_t value = read<int64_t>(desc + 104);
		if (read<int32_t>(desc + 112) != g_gpu.gen || slot >= kGpuSlots || g_gpu.tex[slot][0] == nullptr ||
			(value != 0 && g_gpu.fence->GetCompletedValue() < uint64_t(value)))
			return false;
		auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
		D3D12_RESOURCE_BARRIER b[3] = {};
		for (int l = 0; l < 3; ++l)
		{
			b[l].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b[l].Transition.pResource = g_gpu.tex[slot][l];
			b[l].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b[l].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			b[l].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		}
		list->ResourceBarrier(3, b);
		for (int l = 0; l < 3; ++l)
			list->CopyResource(reinterpret_cast<ID3D12Resource *>(textures[l].handle), g_gpu.tex[slot][l]);
		for (D3D12_RESOURCE_BARRIER &x : b)
			std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
		list->ResourceBarrier(3, b);
		return true;
	}

	bool open_mapping()
	{
		if (g_view != nullptr)
			return true;
		if (GetTickCount() < g_nextOpenAttempt)
			return false;
		g_nextOpenAttempt = GetTickCount() + 1000;
		g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
		if (g_mapping == nullptr)
			return false;
		const auto *header = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, kHeader));
		if (header == nullptr || read<uint32_t>(header) != kMagic)
		{
			if (header != nullptr)
				UnmapViewOfFile(header);
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		g_slots = read<int32_t>(header + 12);
		g_stride = read<int64_t>(header + 16);
		UnmapViewOfFile(header);
		g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(kHeader + g_stride * g_slots)));
		if (g_view == nullptr)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		reshade::log::message(reshade::log::level::info, "MCPassthrough: connected to Minecraft's frame export");
		g_viewShared = g_view;
		return true;
	}

	void destroy_staging(device *dev)
	{
		std::unique_lock<std::shared_mutex> life(g_stagesLife);
		std::lock_guard<std::mutex> lock(g_stageLock);
		for (Stage &st : g_stages)
		{
			if (st.mapped != nullptr)
				dev->unmap_buffer_region(st.buf);
			if (st.buf.handle != 0)
				dev->destroy_resource(st.buf);
			st = Stage();
		}
		g_stageW = g_stageH = 0;
	}

	bool ensure_staging(device *dev, uint32_t w, uint32_t h)
	{
		if (g_stageW == w && g_stageH == h)
			return true;
		destroy_staging(dev);
		const uint32_t pitch = (w * 4 + 255) & ~255u;
		const uint64_t layer = (uint64_t(pitch) * h + 511) & ~511ull;
		std::unique_lock<std::shared_mutex> life(g_stagesLife);
		for (Stage &st : g_stages)
		{
			void *mapped = nullptr;
			if (!dev->create_resource(resource_desc(layer * 3, memory_heap::upload, resource_usage::copy_source), nullptr, resource_usage::cpu_access, &st.buf) ||
				!dev->map_buffer_region(st.buf, 0, layer * 3, map_access::write_only, &mapped))
			{
				life.unlock();
				destroy_staging(dev);
				return false;
			}
			st.mapped = static_cast<uint8_t *>(mapped); // (upload heaps may stay mapped)
		}
		g_stagingPitch = pitch;
		g_stagingLayer = layer;
		g_stageW = w;
		g_stageH = h;
		return true;
	}

	void destroy_layers(device *dev)
	{
		destroy_staging(dev);
		destroy_gpu();
		for (Layer *layer : {&g_world, &g_depth, &g_overlay})
		{
			if (layer->srv.handle != 0)
				dev->destroy_resource_view(layer->srv);
			if (layer->tex.handle != 0)
				dev->destroy_resource(layer->tex);
			*layer = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	/// The Minecraft frame Half Sword's camera copied last (it's composed a frame or two later: keep it staged).
	int64_t wanted_frame()
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		return g_hostPoseCount > 0 ? g_hostPoses[(g_hostPoseCount - 1) & 7].mcFrame : -1;
	}

	/// A stage to write a frame into (marked as being written), or -1. Never one the GPU may still be reading, nor
	/// the frame Half Sword is about to show; older frames first (the camera only ever copies newer ones), then the
	/// oldest of the newer.
	int take_stage()
	{
		const int64_t wanted = wanted_frame();
		std::lock_guard<std::mutex> lock(g_stageLock);
		const uint64_t presents = g_presents.load();
		int best = -1;
		auto rank = [&](const Stage &st) { return st.frame < wanted ? 0 : 1; }; // (empty stages: -1, oldest)
		for (int i = 0; i < kStaging; ++i)
		{
			const Stage &st = g_stages[i];
			if (st.buf.handle == 0 || st.writing || (st.usedAt != 0 && st.usedAt + kGpuFrames > presents) || (wanted >= 0 && st.frame == wanted))
				continue;
			if (best < 0 || rank(st) < rank(g_stages[best]) || (rank(st) == rank(g_stages[best]) && st.frame < g_stages[best].frame))
				best = i;
		}
		if (best >= 0)
		{
			g_stages[best].writing = true;
			g_stages[best].frame = -1;
		}
		return best;
	}

	/// Copy Minecraft's slot into stage i (taken with take_stage; the caller holds g_stagesLife shared). Rows to the
	/// buffer's pitch, the three layers at once. False if Minecraft rewrote the slot meanwhile.
	bool fill_stage(int i, const uint8_t *view, int32_t slot)
	{
		Stage &st = g_stages[i];
		const uint8_t *desc = view + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		const int64_t frame = read<int64_t>(desc + 8);
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		bool ok = !(seq & 1) && w == g_stageW && h == g_stageH;
		if (ok)
		{
			std::memcpy(st.desc, desc, kSlotDescBytes);
			const uint8_t *base = view + kHeader + g_stride * slot;
			const size_t layer = size_t(w) * h * 4;
			auto copy = [&](int l) {
				const uint8_t *src = base + l * layer;
				uint8_t *dst = st.mapped + l * g_stagingLayer;
				if (g_stagingPitch == w * 4)
					std::memcpy(dst, src, layer);
				else
					for (uint32_t row = 0; row < h; ++row)
						std::memcpy(dst + size_t(row) * g_stagingPitch, src + size_t(row) * w * 4, size_t(w) * 4);
			};
			std::thread depth(copy, 1), overlay(copy, 2);
			copy(0);
			depth.join();
			overlay.join();
			ok = read<int64_t>(desc) == seq;
		}
		std::lock_guard<std::mutex> lock(g_stageLock);
		st.writing = false;
		st.frame = ok ? frame : -1;
		return ok;
	}

	/// The worker: stage each frame Minecraft publishes, as it's published.
	void stage_worker()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		HANDLE published = CreateEventW(nullptr, FALSE, FALSE, L"Local\\MCPassthroughFramePublished");
		int64_t lastPublish = -1;
		for (;;)
		{
			// (Minecraft sets the event on each publish; without it, a look every millisecond)
			WaitForSingleObject(published, published != nullptr ? 20 : 1);
			const uint8_t *view = g_viewShared.load();
			if (view == nullptr || !g_active)
				continue;
			const int64_t publish = read<int64_t>(view + 32);
			if (publish == lastPublish)
				continue;
			lastPublish = publish;
			const int32_t slot = read<int32_t>(view + 40);
			if (slot < 0 || slot >= g_slots)
				continue;
			if (read<int32_t>(view + kSlotDesc + kSlotDescBytes * slot + 44) & kFlagGpu)
				continue; // (in the shared textures: nothing to stage)
			const int64_t frame = read<int64_t>(view + kSlotDesc + kSlotDescBytes * slot + 8);
			std::shared_lock<std::shared_mutex> life(g_stagesLife);
			if (g_stageW == 0)
				continue; // the render thread makes the stages (and resizes them) first
			{
				std::lock_guard<std::mutex> lock(g_stageLock);
				bool have = false;
				for (const Stage &st : g_stages)
					have = have || st.frame == frame;
				if (have)
					continue;
			}
			const int i = take_stage();
			if (i < 0)
				continue;
			g_workerFrame = frame;
			fill_stage(i, view, slot);
			g_workerFrame = -1;
		}
	}

	int find_stage(int64_t frame)
	{
		std::lock_guard<std::mutex> lock(g_stageLock);
		for (int i = 0; i < kStaging; ++i)
			if (g_stages[i].frame == frame && !g_stages[i].writing)
				return i;
		return -1;
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::shader_resource, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void bind(effect_runtime *runtime)
	{
		runtime->update_texture_bindings("MCWORLD", g_world.srv, g_world.srv);
		runtime->update_texture_bindings("MCDEPTH", g_depth.srv, g_depth.srv);
		runtime->update_texture_bindings("MCOVERLAY", g_overlay.srv, g_overlay.srv);
	}

	float pose_error(const Pose &a, const uint8_t *desc)
	{
		auto delta = [](float x, float y) { return std::fabs(std::remainder(x - y, 360.0f)); }; // (Minecraft's yaw isn't wrapped)
		const double dx = read<double>(desc + 48) - a.x, dy = read<double>(desc + 56) - a.y, dz = read<double>(desc + 64) - a.z;
		return delta(read<float>(desc + 72), a.yaw) + delta(read<float>(desc + 76), a.pitch) + float(std::sqrt(dx * dx + dy * dy + dz * dz)) * 50.0f;
	}

	/// Upload the Minecraft frame to show: the one rendered with (nearly) the camera this host frame was rendered with
	/// (the host's camera copies Minecraft's frames, so normally that exact frame is still in a slot: no warp at all);
	/// without a host pose, the newest.
	void upload(effect_runtime *runtime, command_list *cmd, const Pose &host)
	{
		int32_t slot = read<int32_t>(g_view + 40);
		if (slot < 0 || slot >= g_slots)
			return;
		bool byFrame = false;
		if (host.valid && host.mcFrame >= 0)
		{
			// the frame the host's camera copied: shown as is (a camera shake on top is the only warp left)
			for (int32_t s = 0; s < g_slots; ++s)
			{
				const uint8_t *d = g_view + kSlotDesc + kSlotDescBytes * s;
				if (!(read<int64_t>(d) & 1) && read<int64_t>(d + 8) == host.mcFrame)
				{
					slot = s;
					byFrame = true;
					g_lastError = pose_error(host, d);
					break;
				}
			}
			if (!byFrame)
				++g_stats.missed;
		}
		if (host.valid && !byFrame)
		{
			float best = 1e30f;
			int64_t bestFrame = -1;
			for (int32_t s = 0; s < g_slots; ++s)
			{
				const uint8_t *d = g_view + kSlotDesc + kSlotDescBytes * s;
				if ((read<int64_t>(d) & 1) || read<uint32_t>(d + 24) == 0)
					continue;
				const float e = pose_error(host, d);
				const int64_t frame = read<int64_t>(d + 8);
				if (e < best - 1e-4f || (e < best + 1e-4f && frame > bestFrame))
				{
					best = e;
					bestFrame = frame;
					slot = s;
				}
			}
			g_lastError = best;
		}
		const uint8_t *desc = g_view + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const int64_t mcFrame = read<int64_t>(desc + 8);
		if (mcFrame == g_shownFrame)
		{
			++g_stats.sameFrame;
			return;
		}
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		const int64_t published = read<int64_t>(g_view + 32);
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		if (w == 0 || h == 0)
			return;
		device *dev = runtime->get_device();
		if (w != g_width || h != g_height)
		{
			// frames still in flight on the GPU use the old textures and upload buffers: they must be done first
			// (freeing them under the GPU removed the device)
			runtime->get_command_queue()->wait_idle();
			destroy_layers(dev);
			if (!create_layer(dev, g_world, w, h, format::r8g8b8a8_unorm) ||
				!create_layer(dev, g_depth, w, h, format::r32_float) ||
				!create_layer(dev, g_overlay, w, h, format::r8g8b8a8_unorm))
			{
				destroy_layers(dev);
				return;
			}
			ensure_gpu(dev, w, h);
			g_width = w;
			g_height = h;
			bind(runtime);
		}
		const resource textures[3] = {g_world.tex, g_depth.tex, g_overlay.tex};
		const resource_usage readable[3] = {resource_usage::shader_resource, resource_usage::shader_resource, resource_usage::shader_resource};
		const resource_usage writable[3] = {resource_usage::copy_dest, resource_usage::copy_dest, resource_usage::copy_dest};
		uint8_t shared[kSlotDescBytes];
		const uint8_t *sd = nullptr;
		if (read<int32_t>(desc + 44) & kFlagGpu)
		{
			// zero copy: the frame is in the slot's shared textures, a copy on the GPU away
			std::memcpy(shared, desc, kSlotDescBytes);
			if (read<int64_t>(shared) != seq)
				return;
			cmd->barrier(3, textures, readable, writable);
			const bool ok = copy_shared(cmd, slot, shared, textures);
			cmd->barrier(3, textures, writable, readable);
			if (!ok)
			{
				++g_stats.gpuLate;
				return;
			}
			++g_stats.zeroCopy;
			sd = shared;
		}
		else
		{
			// (the upload buffers, ~200 MB, only once a frame comes this way: with zero copy, none does)
			if (!ensure_staging(dev, w, h))
				return;
			// the frame, staged by the worker (normally done long before: Minecraft published it before the camera copied it)
			int stage = find_stage(mcFrame);
			if (stage >= 0)
				++g_stats.staged;
			else if (g_workerFrame.load() == mcFrame)
			{
				const ULONGLONG until = GetTickCount64() + 12;
				while (stage < 0 && g_workerFrame.load() == mcFrame && GetTickCount64() < until)
				{
					YieldProcessor();
					stage = find_stage(mcFrame);
				}
				stage = find_stage(mcFrame);
				if (stage >= 0)
					++g_stats.waited;
			}
			if (stage < 0)
			{
				// not staged (just resized, or no event from Minecraft): stage it here
				std::shared_lock<std::shared_mutex> life(g_stagesLife);
				stage = take_stage();
				if (stage < 0)
					return;
				if (!fill_stage(stage, g_view, slot))
				{
					++g_stats.torn;
					return; // Minecraft rewrote the slot mid-copy: show the next one instead
				}
				++g_stats.direct;
			}
			{
				std::lock_guard<std::mutex> lock(g_stageLock);
				g_stages[stage].usedAt = g_presents.load();
			}
			const Stage &staged = g_stages[stage];
			sd = staged.desc;
			cmd->barrier(3, textures, readable, writable);
			for (int l = 0; l < 3; ++l)
				cmd->copy_buffer_to_texture(staged.buf, l * g_stagingLayer, g_stagingPitch / 4, h, textures[l], 0);
			cmd->barrier(3, textures, writable, readable);
		}
		QueryPerformanceCounter(&t1);
		const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
		g_stats.uploadMs += ms;
		g_stats.uploadMaxMs = std::max(g_stats.uploadMaxMs, ms);
		// Minecraft's System.nanoTime() is the same clock as QueryPerformanceCounter, in nanoseconds
		const double nowNs = double(t1.QuadPart) / double(g_qpcFreq.QuadPart) * 1.0e9;
		g_stats.ageMs += (nowNs - double(read<int64_t>(sd + 88))) / 1.0e6;
		g_stats.publishMs += double(read<int64_t>(sd + 96) - read<int64_t>(sd + 88)) / 1.0e6;
		g_lastPublish = published;
		g_shownFrame = read<int64_t>(sd + 8);
		++g_stats.uploads;
		g_mcNear = read<float>(sd + 32);
		g_mcFar = read<float>(sd + 36);
		g_mcFlags = read<int32_t>(sd + 44);
		g_mcPose.fov = read<float>(sd + 40);
		g_mcPose.yaw = read<float>(sd + 72);
		g_mcPose.pitch = read<float>(sd + 76);
		g_mcPose.roll = read<float>(sd + 80);
		g_mcPose.x = read<double>(sd + 48);
		g_mcPose.y = read<double>(sd + 56);
		g_mcPose.z = read<double>(sd + 64);
		g_mcPose.valid = true;
		g_hasFrame = true;
	}

	/// Camera-to-world rotation, as Minecraft builds it: rotationYXZ(pi - yaw, -pitch, roll) (camera looks down -z).
	void camera_rotation(const Pose &p, float m[3][3])
	{
		const float d2r = 3.14159265f / 180.0f;
		const float a = 3.14159265f - p.yaw * d2r, b = -p.pitch * d2r, c = p.roll * d2r;
		const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
		// Ry(a) * Rx(b) * Rz(c)
		const float ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
		const float rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
		const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
		float t[3][3] = {};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				for (int k = 0; k < 3; ++k)
					t[i][j] += ry[i][k] * rx[k][j];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				m[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					m[i][j] += t[i][k] * rz[k][j];
			}
	}

	/// Rows of R_mc^T * R_host: turns a ray in GTA's camera space into Minecraft's camera space.
	void warp_matrix(const Pose &host, const Pose &mc, float out[3][3])
	{
		float rh[3][3], rm[3][3];
		camera_rotation(host, rh);
		camera_rotation(mc, rm);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					out[i][j] += rm[k][i] * rh[k][j];
			}
	}

	/// Reload MCPassthrough.fx when the file changes (ReShade doesn't watch it), so tweaks don't need a GTA restart.
	void watch_effect_file(effect_runtime *runtime)
	{
		static DWORD next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount() < next)
			return;
		next = GetTickCount() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"reshade-shaders\\Shaders\\MCPassthrough.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(kEffect);
		last = info.ftLastWriteTime;
	}

	void on_present(effect_runtime *runtime)
	{
		watch_effect_file(runtime); // every frame, even when the effect failed to compile
	}

	void on_begin_effects(effect_runtime *runtime, command_list *cmd, resource_view, resource_view)
	{
		uint32_t bw = 0, bh = 0;
		runtime->get_screenshot_width_and_height(&bw, &bh);
		g_bbWidth = bw;
		g_bbHeight = bh;
		++g_presents;
		bool on = g_active && GetTickCount64() - g_activeAt.load() < 300 && open_mapping();
		// the camera this host frame was rendered with (the pose history, the render pipeline's depth back)
		Pose host, prev;
		{
			std::lock_guard<std::mutex> lock(g_poseLock);
			const unsigned lag = unsigned(std::clamp(g_poseLag.load(), 0, 5));
			if (g_hostPoseCount > lag)
				host = g_hostPoses[(g_hostPoseCount - 1 - lag) & 7];
			if (g_hostPoseCount > lag + 1)
				prev = g_hostPoses[(g_hostPoseCount - 2 - lag) & 7];
		}
		if (on)
		{
			check_gpu();
			upload(runtime, cmd, host);
			Stats &s = g_stats;
			if (host.valid && host.qpc != 0)
			{
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				s.tickMs += double(now.QuadPart - host.qpc) * 1000.0 / double(g_qpcFreq.QuadPart);
			}
			const ULONGLONG now = GetTickCount64();
			if (s.since == 0)
			{
				s.since = now;
				s.publishedAt = read<int64_t>(g_view + 32);
			}
			++s.presents;
			if (host.valid)
			{
				++s.withHost;
				s.errSum += g_lastError;
				s.errMax = std::max(s.errMax, g_lastError);
				if (g_lastError < 0.05f)
					++s.exact;
			}
			if (now - s.since >= 2000)
			{
				const int64_t pub = read<int64_t>(g_view + 32);
				const float secs = (now - s.since) / 1000.0f;
				hslog::info("compositor: host %.0f fps, Minecraft %.0f fps, uploads %d (%.2f ms avg, %.2f max; staged %d, waited %d, here %d), "
					"shown frame age %.1f ms (publish %.1f, tick to compose %.1f), same frame again %d, torn %d, pose error avg %.2f max %.2f, exact %d/%d, copied frame gone %d, "
					"zero copy %d (not ready %d)",
					s.presents / secs, (pub - s.publishedAt) / secs, s.uploads, s.uploads ? s.uploadMs / s.uploads : 0.0, s.uploadMaxMs, s.staged, s.waited,
					s.direct, s.uploads ? s.ageMs / s.uploads : 0.0, s.uploads ? s.publishMs / s.uploads : 0.0,
					s.withHost ? s.tickMs / s.withHost : 0.0, s.sameFrame, s.torn,
					s.withHost ? s.errSum / s.withHost : 0.0f, s.errMax, s.exact, s.withHost, s.missed, s.zeroCopy, s.gpuLate);
				s = Stats{};
				s.since = now;
				s.publishedAt = pub;
			}
		}
		on = on && g_hasFrame;
		// The technique stays enabled (preset); McActive gates it, so GTA passes through untouched until a
		// Minecraft frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, on);
		if (!on)
			return;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_mcNear, g_mcFar, float(g_mcFlags));
		// a scene's look overrides the preset's light matching and depth bias; the preset's values come back after
		auto look = [&](const char *name, float want, float &saved) {
			const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name);
			if (v.handle == 0)
				return;
			if (want >= 0.0f)
			{
				if (saved < 0.0f)
					runtime->get_uniform_value_float(v, &saved, 1);
				runtime->set_uniform_value_float(v, want);
			}
			else if (saved >= 0.0f)
			{
				runtime->set_uniform_value_float(v, saved);
				saved = -1.0f;
			}
		};
		look("LightMatch", g_lookLight.load(), g_savedLight);
		look("DepthBias", g_lookBias.load(), g_savedBias);
		look("SlopeBias", g_lookSlope.load(), g_savedSlope);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "Shake"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_shakeX.load(), g_shakeY.load(), g_shakeRoll.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PortalWarp"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_portalWarp.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());

		// Re-projection from Minecraft's pose to the host's (extrapolated by the effect's PosePrediction frames).
		float predict = 0.0f;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PosePrediction"); v.handle != 0)
			runtime->get_uniform_value_float(v, &predict, 1);
		const bool warp = host.valid && g_mcPose.valid && !g_cameraLocked;
		if (warp && prev.valid && predict != 0.0f)
		{
			auto delta = [](float a, float b) { float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f; return d; };
			host.yaw += delta(host.yaw, prev.yaw) * predict;
			host.pitch += (host.pitch - prev.pitch) * predict;
			host.roll += (host.roll - prev.roll) * predict;
			host.x += (host.x - prev.x) * predict;
			host.y += (host.y - prev.y) * predict;
			host.z += (host.z - prev.z) * predict;
		}
		float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float t[3] = {0, 0, 0};
		if (warp)
		{
			warp_matrix(host, g_mcPose, m);
			// T = R_mc^T (host position - Minecraft's camera position), in Minecraft's camera space
			float rm[3][3];
			camera_rotation(g_mcPose, rm);
			const float d[3] = {float(host.x - g_mcPose.x), float(host.y - g_mcPose.y), float(host.z - g_mcPose.z)};
			for (int i = 0; i < 3; ++i)
				t[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
		}
		const char *rows[3] = {"WarpRow0", "WarpRow1", "WarpRow2"};
		for (int i = 0; i < 3; ++i)
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, rows[i]); v.handle != 0)
				runtime->set_uniform_value_float(v, m[i][0], m[i][1], m[i][2]);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpT"); v.handle != 0)
			runtime->set_uniform_value_float(v, t[0], t[1], t[2]);
		const float d2r = 3.14159265f / 180.0f;
		const float tanHost = std::tan((warp ? host.fov : g_mcPose.fov) * d2r * 0.5f), tanMc = std::tan(g_mcPose.fov * d2r * 0.5f);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpTan"); v.handle != 0)
			runtime->set_uniform_value_float(v, tanHost, tanMc, float(g_width) / float(g_height));
	}


	void on_reloaded_effects(effect_runtime *runtime)
	{
		if (g_width != 0)
			bind(runtime);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		destroy_layers(runtime->get_device());
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered)
			return true;
		if (!reshade::register_addon(module))
			return false;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		g_registered = true;
		QueryPerformanceFrequency(&g_qpcFreq);
		static bool workerStarted = false; // (the module is pinned: the worker lives as long as the game)
		if (!workerStarted)
		{
			workerStarted = true;
			std::thread(stage_worker).detach();
		}
		reshade::log::message(reshade::log::level::info, "MCPassthrough: registered");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false))
			reshade::unregister_addon(static_cast<HMODULE>(module));
	}

	void set_active(bool active)
	{
		g_active = active;
		g_activeAt = GetTickCount64();
	}

	void set_host_planes(float near_clip, float far_clip)
	{
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	void backbuffer_size(int &width, int &height)
	{
		width = int(g_bbWidth.load());
		height = int(g_bbHeight.load());
	}

	void set_camera_locked(bool locked)
	{
		g_cameraLocked = locked;
	}

	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp)
	{
		g_shakeX = shake_x;
		g_shakeY = shake_y;
		g_shakeRoll = shake_roll;
		g_portalWarp = portal_warp;
	}

	void set_look(float light_match, float depth_bias, float slope_bias)
	{
		g_lookLight = light_match;
		g_lookBias = depth_bias;
		g_lookSlope = slope_bias;
	}

	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z, int64_t mcFrame)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		g_hostPoses[g_hostPoseCount & 7] = {yaw, pitch, roll, fov, x, y, z, true, mcFrame, now.QuadPart};
		++g_hostPoseCount;
	}

	uint64_t presents()
	{
		return g_presents.load();
	}

	void set_pose_lag(int frames)
	{
		g_poseLag = frames;
	}

	int pose_lag()
	{
		return g_poseLag;
	}
}
