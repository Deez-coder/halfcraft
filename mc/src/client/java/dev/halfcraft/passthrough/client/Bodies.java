package dev.halfcraft.passthrough.client;

import com.google.gson.JsonArray;
import dev.halfcraft.passthrough.MobWar;
import java.util.HashMap;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * The Half Sword fighters' real bodies (points along each skeleton, from "peds"), for the crosshair: a proxy's box
 * stands upright at the fighter's feet, but a fighter on the ground lies beside it, on (or a little into) the host's
 * barrier ground. Steve's look is tested against capsules between the points; the body it meets first, if nearer than
 * what Minecraft found, is what he hits (as that fighter's proxy, so the attack goes the usual way).
 */
public final class Bodies {
	/** Point pairs (indices into the host's points: head, neck, spine, pelvis, arms l/r, legs l/r) and their radius. */
	private static final int[][] SEGMENTS = MobWar.BODY_SEGMENTS;
	private static final double[] RADIUS = MobWar.BODY_RADIUS;
	private static final double HEAD_RADIUS = MobWar.HEAD_RADIUS;
	private static final int POINTS = MobWar.BODY_POINTS;
	/** How far into the host's ground a lying body may be and still be hit. */
	private static final double THROUGH_GROUND = 0.6;

	/** handle -> points (x, y, z each). Replaced whole by the WebSocket thread. */
	private static volatile Map<Integer, double[]> bodies = Map.of();

	private Bodies() {
	}

	/** {"t":"peds","p":[[handle, feet x,y,z, then the body points], ...]} (WebSocket thread). */
	static void update(final JsonArray list) {
		Map<Integer, double[]> next = new HashMap<>();
		for (int i = 0; i < list.size(); i++) {
			JsonArray e = list.get(i).getAsJsonArray();
			if (e.size() < 4 + POINTS * 3) {
				continue;
			}

			double[] p = new double[POINTS * 3];
			for (int k = 0; k < p.length; k++) {
				p[k] = e.get(4 + k).getAsDouble();
			}

			next.put(e.get(0).getAsInt(), p);
		}

		bodies = next;
		MobWar.bodies(next); // (Steve's arrows, on the server)
	}

	/** Render thread, after Minecraft's own pick: a fighter's body under the crosshair, nearer than what it found. */
	public static void pick(final Minecraft minecraft, final float partialTick) {
		LocalPlayer player = minecraft.player;
		Map<Integer, double[]> all = bodies;
		if (player == null || minecraft.level == null || all.isEmpty()) {
			return;
		}

		Vec3 eye = player.getEyePosition(partialTick);
		Vec3 dir = player.getViewVector(partialTick);
		// the weapon's reach (a spear's is longer, and has a minimum)
		net.minecraft.world.item.component.AttackRange range = player.getAttackRangeWith(player.getMainHandItem());
		double reach = range.effectiveMaxRange(player), min = range.effectiveMinRange(player);
		// what Minecraft found, and how far: a body counts if it's nearer (a lying one a little into the ground, too)
		HitResult found = minecraft.hitResult;
		double limit = reach;
		if (found != null && found.getType() != HitResult.Type.MISS) {
			limit = Math.min(limit, found.getLocation().distanceTo(eye));
			if (found instanceof BlockHitResult block && dev.halfcraft.passthrough.HostGround.cellAt(block.getBlockPos()) != null) {
				limit = Math.min(reach, limit + THROUGH_GROUND);
			}
		}

		int bestHandle = -1;
		double bestT = limit;
		for (Map.Entry<Integer, double[]> e : all.entrySet()) {
			double[] p = e.getValue();
			for (int s = 0; s <= SEGMENTS.length; s++) {
				double t;
				if (s == SEGMENTS.length) {
					// the head: from the head bone on, as far again as the neck is below it
					int h = 0, n = 3;
					t = rayCapsule(eye, dir, p[h], p[h + 1], p[h + 2], 2 * p[h] - p[n], 2 * p[h + 1] - p[n + 1], 2 * p[h + 2] - p[n + 2], HEAD_RADIUS);
				} else {
					int a = SEGMENTS[s][0] * 3, b = SEGMENTS[s][1] * 3;
					t = rayCapsule(eye, dir, p[a], p[a + 1], p[a + 2], p[b], p[b + 1], p[b + 2], RADIUS[s]);
				}

				if (t >= min && t < bestT) {
					bestT = t;
					bestHandle = e.getKey();
				}
			}
		}

		if (bestHandle < 0) {
			return;
		}

		Entity proxy = minecraft.level.getEntity(MobWar.proxyId(bestHandle));
		if (proxy == null || (found instanceof EntityHitResult hit && hit.getEntity() == proxy)) {
			return;
		}

		minecraft.hitResult = new EntityHitResult(proxy, eye.add(dir.scale(bestT)));
		minecraft.crosshairPickEntity = proxy;
	}

	/**
	 * Distance along the ray (eye + dir * t, dir unit) to where it comes within `r` of segment a-b, or -1. The
	 * closest approach of the two lines, good enough for limbs this thin.
	 */
	private static double rayCapsule(final Vec3 eye, final Vec3 dir, final double ax, final double ay, final double az, final double bx,
		final double by, final double bz, final double r) {
		double ux = bx - ax, uy = by - ay, uz = bz - az;
		double wx = eye.x - ax, wy = eye.y - ay, wz = eye.z - az;
		double a = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
		double b = dir.x * ux + dir.y * uy + dir.z * uz;
		double c = ux * ux + uy * uy + uz * uz;
		double d = dir.x * wx + dir.y * wy + dir.z * wz;
		double e = ux * wx + uy * wy + uz * wz;
		double den = a * c - b * b;
		double s; // along the segment, 0..1
		double t; // along the ray
		if (c < 1.0E-9 || den < 1.0E-9) {
			s = 0.0; // a point, or parallel: from the segment's start
		} else {
			s = Math.clamp((a * e - b * d) / den, 0.0, 1.0);
		}

		// the ray's nearest point to that segment point, then the segment point nearest it
		t = Math.max(0.0, (dir.x * (ax + ux * s - eye.x) + dir.y * (ay + uy * s - eye.y) + dir.z * (az + uz * s - eye.z)) / a);
		if (c >= 1.0E-9) {
			s = Math.clamp(((eye.x + dir.x * t - ax) * ux + (eye.y + dir.y * t - ay) * uy + (eye.z + dir.z * t - az) * uz) / c, 0.0, 1.0);
		}

		double qx = ax + ux * s, qy = ay + uy * s, qz = az + uz * s;
		double px = eye.x + dir.x * t, py = eye.y + dir.y * t, pz = eye.z + dir.z * t;
		double dist2 = (px - qx) * (px - qx) + (py - qy) * (py - qy) + (pz - qz) * (pz - qz);
		if (dist2 > r * r) {
			return -1.0;
		}

		// back up from the closest approach to where the ray enters the capsule
		return Math.max(0.0, t - Math.sqrt(r * r - dist2));
	}
}
