package dev.halfcraft.passthrough;

import com.mojang.brigadier.context.CommandContext;
import java.util.Locale;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.core.BlockPos;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.level.pathfinder.Node;
import net.minecraft.world.level.pathfinder.Path;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import net.minecraft.world.phys.Vec3;

/**
 * /hs why: what the nearest mob's pathfinding makes of where it stands (its path, its target, the path types and floor
 * heights of the cells around its feet, and a test path 4 blocks each way). Printed in chat and the log.
 */
final class MobDebug {
	private MobDebug() {
	}

	static int why(final CommandContext<CommandSourceStack> ctx) {
		CommandSourceStack src = ctx.getSource();
		ServerLevel level = src.getLevel();
		Vec3 at = src.getPosition();
		Mob mob = null;
		double best = 32.0 * 32.0;
		for (Mob m : level.getEntitiesOfClass(Mob.class, new net.minecraft.world.phys.AABB(at, at).inflate(32.0), m -> m.isAlive() && !MobWar.isProxy(m))) {
			if (m.distanceToSqr(at) < best) {
				best = m.distanceToSqr(at);
				mob = m;
			}
		}

		if (mob == null) {
			say(src, "no mob within 32 blocks");
			return 0;
		}

		say(src, String.format(Locale.ROOT, "%s at %.3f %.3f %.3f, on ground %b, nav done %b, stuck %b", mob.getType().toShortString(), mob.getX(), mob.getY(),
			mob.getZ(), mob.onGround(), mob.getNavigation().isDone(), mob.getNavigation().isStuck()));
		LivingEntity target = mob.getTarget();
		say(src, target == null ? "no target" : String.format(Locale.ROOT, "target %s at %.3f %.3f %.3f (block %s), proxy %b, dead proxy %b",
			target.getType().toShortString(), target.getX(), target.getY(), target.getZ(), target.blockPosition().toShortString(), MobWar.isProxy(target),
			MobWar.isDeadProxy(target)));
		say(src, "current " + describe(mob.getNavigation().getPath()));
		if (target != null) {
			say(src, "to target: " + describe(mob.getNavigation().createPath(target, 0)));
		}

		BlockPos feet = mob.blockPosition();
		for (int dy = 1; dy >= -2; dy--) {
			BlockPos p = feet.offset(0, dy, 0);
			HostGround.Cell cell = HostGround.cellAt(p);
			say(src, String.format(Locale.ROOT, "  y%+d %s: %s, type %s, floor %.3f, cell %s", dy, p.toShortString(),
				level.getBlockState(p).getBlock().getName().getString(), WalkNodeEvaluator.getPathTypeStatic(mob, p),
				WalkNodeEvaluator.getFloorLevel(level, p),
				cell == null ? "-" : String.format(Locale.ROOT, "%s top %.3f whole %b support %b", cell.kind(), cell.top(), cell.whole(), cell.support())));
		}

		for (int[] d : new int[][] {{4, 0}, {-4, 0}, {0, 4}, {0, -4}}) {
			say(src, String.format("test path %d,%d: ", d[0], d[1]) + describe(mob.getNavigation().createPath(feet.offset(d[0], 0, d[1]), 0)));
		}

		return 1;
	}

	private static String describe(final Path path) {
		if (path == null) {
			return "path: none";
		}

		Node end = path.getEndNode();
		return String.format(Locale.ROOT, "path: %d nodes, next %d, end %s, reaches %b, %.2f from target %s", path.getNodeCount(), path.getNextNodeIndex(),
			end == null ? "-" : end.x + " " + end.y + " " + end.z, path.canReach(), path.getDistToTarget(), path.getTarget().toShortString());
	}

	private static void say(final CommandSourceStack src, final String text) {
		Passthrough.LOG.info("why: {}", text);
		src.sendSystemMessage(Component.literal(text));
	}
}
