package dev.halfcraft.passthrough.client;

import dev.halfcraft.passthrough.Passthrough;

/**
 * Minecraft's side of the GPU balance (/hs balance): no faster than a little above the rate Half Sword shows frames
 * at, since it shows one Minecraft frame per frame of its own and the rest would only take GPU time from it. Half
 * Sword caps itself the same way from Minecraft's rate, so the two meet. Sleeps at the end of a frame, before the
 * next one reads the input.
 */
public final class Pacer {
	/** How far above Half Sword's rate Minecraft may run (Half Sword uses the same margin). */
	private static final double MARGIN = 1.15;
	/** The last stretch is spun (the timer wakes a little late at times). */
	private static final long SPIN_NANOS = 500_000L;
	private static long next;
	private static SharedMemory.Timer timer;
	private static boolean noTimer;

	private Pacer() {
	}

	/** Render thread, end of each frame. */
	public static void endFrame() {
		long interval = HostState.hostFrameNanos();
		if (!Passthrough.own || !Passthrough.balance || interval <= 0L) {
			next = 0L;
			return;
		}

		long now = System.nanoTime();
		if (next != 0L && now < next) {
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

		long step = (long)(interval / MARGIN);
		// keep the cadence, but never bank time from a slow frame
		next = next != 0L && now - next < step ? next + step : now + step;
	}
}
