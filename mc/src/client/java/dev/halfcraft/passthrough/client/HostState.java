package dev.halfcraft.passthrough.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import dev.halfcraft.passthrough.Passthrough;
import net.minecraft.client.Camera;

/**
 * The host's latest camera and player pose, already in Minecraft coordinates (the host converts); or, while the host
 * lets Minecraft drive ("own"), Minecraft's own camera for the frame being rendered.
 */
public final class HostState {
	/**
	 * @param hostFrame the host's frame number for this pose (echoed in the exported frame)
	 * @param x camera position
	 * @param yaw Minecraft yaw (0 = facing +Z), pitch (positive = down) and roll, in degrees
	 * @param fov vertical field of view, degrees
	 * @param firstPerson whether the host camera is first person (third person shows the player model)
	 * @param px the player's feet (third person)
	 * @param bodyYaw the player's body yaw (third person)
	 * @param drive Minecraft moves the player (elytra flight) and the host follows; lookYaw/lookPitch steer
	 * @param gun the player holds one of the host's guns (Steve aims it; his own item isn't drawn)
	 * @param own Minecraft drives player and camera from the host's forwarded input; the pose is Minecraft's camera
	 */
	public record Pose(
		long hostFrame, double x, double y, double z, float yaw, float pitch, float roll, float fov,
		boolean firstPerson, double px, double py, double pz, float bodyYaw, long receivedNanos,
		boolean drive, float lookYaw, float lookPitch, boolean gun, boolean own
	) {
	}

	private static final long TIMEOUT_NANOS = 2_000_000_000L;
	private static volatile Pose latest;
	/** The pose this frame renders with, taken once per frame so every hook agrees. Render thread only. */
	private static Pose frame;
	/** Half Sword's frame time, from its heartbeat ("own", one per frame): smoothed, nanoseconds (0: unknown). */
	private static volatile long hostFrameNanos;
	private static volatile long lastOwnNanos;
	/** Whether the last frame was in own mode (to notice the switch). Render thread only. */
	private static boolean wasOwn;

	private HostState() {
	}

	/** {"t":"cam","f":frame,"p":[x,y,z],"r":[yaw,pitch,roll],"fov":deg,"fp":bool,"pl":[x,y,z],"h":bodyYaw} */
	static void update(final JsonObject m) {
		JsonArray p = m.getAsJsonArray("p");
		JsonArray r = m.getAsJsonArray("r");
		JsonArray pl = m.has("pl") ? m.getAsJsonArray("pl") : p;
		float yaw = r.get(0).getAsFloat();
		latest = new Pose(
			m.has("f") ? m.get("f").getAsLong() : 0L,
			p.get(0).getAsDouble(), p.get(1).getAsDouble(), p.get(2).getAsDouble(),
			yaw, r.get(1).getAsFloat(), r.size() > 2 ? r.get(2).getAsFloat() : 0.0F,
			m.has("fov") ? m.get("fov").getAsFloat() : 70.0F,
			!m.has("fp") || m.get("fp").getAsBoolean(),
			pl.get(0).getAsDouble(), pl.get(1).getAsDouble(), pl.get(2).getAsDouble(),
			m.has("h") ? m.get("h").getAsFloat() : yaw,
			System.nanoTime(),
			m.has("drive") && m.get("drive").getAsBoolean(),
			m.has("look") ? m.getAsJsonArray("look").get(0).getAsFloat() : yaw,
			m.has("look") ? m.getAsJsonArray("look").get(1).getAsFloat() : r.get(1).getAsFloat(),
			m.has("gun") && m.get("gun").getAsBoolean(),
			false
		);
	}

	/** {"t":"own","f":frame}: the host's heartbeat while Minecraft drives (sent every host frame). */
	static void own(final JsonObject m) {
		long now = System.nanoTime();
		long gap = now - lastOwnNanos;
		lastOwnNanos = now;
		if (gap > 0L && gap < 200_000_000L) {
			long smoothed = hostFrameNanos;
			hostFrameNanos = smoothed == 0L ? gap : smoothed + (gap - smoothed) / 16;
		}

		latest = new Pose(m.has("f") ? m.get("f").getAsLong() : 0L, 0, 0, 0, 0, 0, 0, 70.0F, true, 0, 0, 0, 0, System.nanoTime(),
			false, 0, 0, false, true);
	}

	/** When Half Sword's last heartbeat came (System.nanoTime): it sends it as it starts a frame, its camera just set. */
	static long lastTickNanos() {
		return lastOwnNanos;
	}

	/** Half Sword's smoothed frame time in nanoseconds, or 0 when it isn't driving. */
	static long hostFrameNanos() {
		Pose p = latest;
		return p != null && p.own() && System.nanoTime() - lastOwnNanos < 200_000_000L ? hostFrameNanos : 0L;
	}

	/** The latest pose if the host is still sending, else null. */
	static Pose live() {
		Pose p = latest;
		return p != null && System.nanoTime() - p.receivedNanos() < TIMEOUT_NANOS ? p : null;
	}

	/** The pose for the frame being rendered, or null when no host is attached. */
	public static Pose frame() {
		return frame;
	}

	/** Whether Minecraft drives (the host's own heartbeat is live). */
	public static boolean own() {
		Pose p = frame;
		return p != null && p.own();
	}

	public static void beginFrame() {
		frame = live();
		Passthrough.active = frame != null;
		boolean own = frame != null && frame.own();
		Passthrough.own = own;
		if (own != wasOwn) {
			wasOwn = own;
			OwnMode.changed(own);
		}
	}

	/** End of the camera update in own mode: this frame renders from Minecraft's own camera; that is the pose exported. */
	public static void ownCamera(final Camera camera) {
		Pose h = frame;
		if (h == null || !h.own()) {
			return;
		}

		var pos = camera.position();
		frame = new Pose(h.hostFrame(), pos.x, pos.y, pos.z, camera.yRot(), camera.xRot(), 0.0F, camera.getFov(), !camera.isDetached(),
			pos.x, pos.y, pos.z, camera.yRot(), h.receivedNanos(), false, camera.yRot(), camera.xRot(), false, true);
	}
}
