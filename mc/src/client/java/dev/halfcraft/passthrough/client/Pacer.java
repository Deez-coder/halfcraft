package dev.halfcraft.passthrough.client;

import dev.halfcraft.passthrough.Passthrough;

/**
 * Minecraft makes one frame for each of Half Sword's, timed to be handed over just before Half Sword's next frame
 * takes the newest one (its camera follows the newest Minecraft frame as it starts a frame: its heartbeat, "own").
 * Run free, the frame Half Sword took was on average half a frame old already, up to a whole one; locked to it, about
 * a millisecond. How long Minecraft takes from the start of a frame to handing it over is measured as it goes (with
 * its jitter, so a frame rarely misses). Any faster would only take GPU time from Half Sword (Minecraft's GPU work goes
 * first: GpuPriority). Sleeps at the end of a frame, before the next one reads the input.
 */
public final class Pacer {
	/** Handed over this long before Half Sword's frame starts, on top of the measured jitter. */
	private static final long LEAD_NANOS = 500_000L;
	/** The last stretch is spun (the timer wakes a little late at times). */
	private static final long SPIN_NANOS = 500_000L;
	private static SharedMemory.Timer timer;
	private static boolean noTimer;
	/** When this frame started (the pacer let it go), and the host frame it's for. */
	private static long frameStart;
	private static long target;
	/** Start of a frame to its handing over (smoothed), and how much that varies. */
	private static double work = 8_000_000.0;
	private static double jitter = 1_000_000.0;

	private Pacer() {
	}

	/** Render thread: this frame was handed over (sync export). */
	static void published() {
		if (frameStart != 0L) {
			double d = System.nanoTime() - frameStart;
			jitter += (Math.abs(d - work) - jitter) / 16.0;
			work += (d - work) / 16.0;
		}
	}

	/** Render thread, end of each frame. */
	public static void endFrame() {
		long interval = HostState.hostFrameNanos();
		if (!Passthrough.own || interval <= 0L) {
			target = 0L;
			frameStart = 0L;
			return;
		}

		long now = System.nanoTime();
		long lead = (long)(work + jitter) + LEAD_NANOS;
		// the start for the next frame of Half Sword's this one can still make (one only just missed: go at once, it
		// may still), and never two for the same one
		long next = HostState.lastTickNanos() + interval - lead;
		while (next < now - lead / 2) {
			next += interval;
		}

		if (target != 0L && next - target < interval / 2) {
			next += interval;
		}

		target = next;
		if (now < next) {
			// a high-resolution timer for most of it (plain sleeps round up to 15.6 ms steps here), spin the rest
			if (timer == null && !noTimer) {
				try {
					timer = new SharedMemory.Timer();
				} catch (Throwable t) {
					noTimer = true;
					Passthrough.LOG.warn("frame pacing: no high-resolution timer ({}), spinning", t.toString());
				}
			}

			if (timer != null && next - now > SPIN_NANOS) {
				try {
					timer.sleep(next - now - SPIN_NANOS);
				} catch (Throwable t) {
					timer = null;
					noTimer = true;
				}

				now = System.nanoTime();
			}

			while (now < next) {
				Thread.onSpinWait();
				now = System.nanoTime();
			}
		}

		frameStart = now;
	}
}
