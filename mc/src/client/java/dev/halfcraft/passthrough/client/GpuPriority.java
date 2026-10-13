package dev.halfcraft.passthrough.client;

import com.sun.jna.Library;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import dev.halfcraft.passthrough.Passthrough;

/**
 * Minecraft's GPU work goes first (Windows' GPU scheduling priority for this process) while Half Sword drives it and
 * /hs priority is on. Its frames are small, but queued behind Half Sword's they waited ~28 ms each: Minecraft held at
 * ~30 fps and its picture ~45 ms old on screen. Going first, they wait ~4 ms (Minecraft as fast as Half Sword's real
 * frames, its picture ~22 ms old), for ~20% of Half Sword's frame rate. Realtime if Windows allows it (high barely
 * helps: tried), else high.
 */
final class GpuPriority {
	private interface Gdi32 extends Library {
		int D3DKMTSetProcessSchedulingPriorityClass(Pointer process, int priorityClass);
	}

	/** D3DKMT_SCHEDULINGPRIORITYCLASS. */
	private static final int NORMAL = 2, HIGH = 4, REALTIME = 5;
	/** GetCurrentProcess()'s pseudo handle. */
	private static final Pointer SELF = new Pointer(-1L);
	private static Gdi32 gdi;
	private static Boolean applied;
	private static boolean broken;

	private GpuPriority() {
	}

	/** Render thread, each frame: as /hs priority and own mode want it. */
	static void update() {
		boolean want = Passthrough.own && Passthrough.gpuPriority;
		if (broken || (applied != null && applied == want)) {
			return;
		}

		applied = want;
		try {
			if (gdi == null) {
				gdi = Native.load("gdi32", Gdi32.class);
			}

			if (!want) {
				gdi.D3DKMTSetProcessSchedulingPriorityClass(SELF, NORMAL);
				Passthrough.LOG.info("GPU priority: normal");
				return;
			}

			int realtime = gdi.D3DKMTSetProcessSchedulingPriorityClass(SELF, REALTIME);
			int high = realtime == 0 ? 0 : gdi.D3DKMTSetProcessSchedulingPriorityClass(SELF, HIGH);
			Passthrough.LOG.info("GPU priority: {}", realtime == 0 ? "realtime" : high == 0 ? "high" : "unchanged (" + Integer.toHexString(high) + ")");
		} catch (Throwable t) {
			broken = true;
			Passthrough.LOG.warn("GPU priority: couldn't set it: {}", t.toString());
		}
	}
}
