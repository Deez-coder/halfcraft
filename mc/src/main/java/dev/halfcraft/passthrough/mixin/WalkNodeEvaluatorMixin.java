package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Mobs' pathfinding reads block states, not collision: the host's partial ground cells (HostGround) get the path type
 * of their vanilla counterpart - low floor bits stay open (walked on, like a carpet), dense ones are blocked with their
 * top as the floor above (like a slab or stairs), tall sparse ones (rails, posts) are fences (never stood on: their
 * floor counts 1.5 high, as a fence's collision does). (Full cells are real barrier blocks already.)
 */
@Mixin(WalkNodeEvaluator.class)
abstract class WalkNodeEvaluatorMixin {
	@Inject(method = "getPathTypeFromState(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/pathfinder/PathType;",
		at = @At("HEAD"), cancellable = true)
	private static void passthrough$hostGroundType(final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<PathType> cir) {
		HostGround.Cell cell = HostGround.cellAt(pos);
		BlockState state = level.getBlockState(pos);
		boolean barrier = state.is(Blocks.BARRIER) && !(level instanceof Level l && l.dimension() != Level.OVERWORLD);
		if (cell == null) {
			// a barrier in the open (the player's, for support) isn't in anyone's way: it doesn't collide
			if (barrier) {
				cir.setReturnValue(PathType.OPEN);
			}

			return;
		}

		// a real block in the cell: vanilla's. Our barriers can't be left to vanilla: it reads "is this a full block"
		// from a per-block-type cache built at startup, when a barrier's shape (BlockStateSuffocationMixin: the
		// host cell's, none without one) was empty - every barrier was OPEN, the whole floor was air to mobs and
		// those standing on it found no path at all
		if (!state.isAir() && !barrier) {
			return;
		}

		if (cell.whole()) {
			cir.setReturnValue(PathType.BLOCKED);
		} else if (barrier) {
			// a slope's support barrier: walked in, its floor where the slope is (getFloorLevel below). As a solid step
			// (BLOCKED, the node on top) a low slope's next node stood more than a step above the mob: it jumped
			cir.setReturnValue(PathType.OPEN);
		} else {
			cir.setReturnValue(switch (cell.kind()) {
				case LOW -> PathType.OPEN;
				case FENCE -> PathType.FENCE;
				default -> PathType.BLOCKED;
			});
		}
	}

	@Inject(method = "getFloorLevel(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)D", at = @At("RETURN"), cancellable = true)
	private static void passthrough$hostGroundFloor(final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<Double> cir) {
		double floor = cir.getReturnValueD();
		// a low floor bit in this very cell: standing on it
		HostGround.Cell own = HostGround.cellAt(pos);
		if (own != null && own.kind() == HostGround.Kind.LOW) {
			floor = Math.max(floor, pos.getY() + own.top());
		}

		// the cell below: its top (a fence's counts 1.5 high, out of a mob's step and jump); with one of our barriers in
		// it, its own top rather than the barrier's
		BlockPos below = pos.below();
		HostGround.Cell under = HostGround.cellAt(below);
		if (under != null && !under.whole()) {
			double top = below.getY() + (under.kind() == HostGround.Kind.FENCE ? 1.5 : under.top());
			floor = HostGround.exactInstead(level.getBlockState(below), under) ? Math.max(top, own != null && own.kind() == HostGround.Kind.LOW
				? pos.getY() + own.top() : top) : Math.max(floor, top);
		}

		if (floor != cir.getReturnValueD()) {
			cir.setReturnValue(floor);
		}
	}
}
