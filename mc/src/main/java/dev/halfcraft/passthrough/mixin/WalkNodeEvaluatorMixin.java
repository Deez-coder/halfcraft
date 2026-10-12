package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.pathfinder.PathfindingContext;
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
	/**
	 * Where a path starts, for a mob off the ground (a step, a hop on a slope): vanilla goes down from it through air
	 * to the first block. The host's ground is air to vanilla: that went down into the void, no path was found and the
	 * mob wandered off. Its solid cells are a block to that search.
	 */
	@WrapOperation(method = "getStart()Lnet/minecraft/world/level/pathfinder/Node;", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/world/level/pathfinder/PathfindingContext;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
	private BlockState passthrough$hostGroundUnderMob(final PathfindingContext context, final BlockPos pos, final Operation<BlockState> original) {
		BlockState state = original.call(context, pos);
		return HostGround.solidAir(context.level(), pos, state) ? Blocks.STONE.defaultBlockState() : state;
	}

	@Inject(method = "getPathTypeFromState(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/pathfinder/PathType;",
		at = @At("HEAD"), cancellable = true)
	private static void passthrough$hostGroundType(final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<PathType> cir) {
		HostGround.Cell cell = HostGround.cellAt(pos);
		if (cell == null || !HostGround.inHost(level)) {
			return;
		}

		// a real block in the cell: vanilla's. The cell itself is air to vanilla (no block there): its type is ours
		if (!level.getBlockState(pos).isAir()) {
			return;
		}

		if (cell.whole()) {
			cir.setReturnValue(PathType.BLOCKED);
		} else if (cell.support()) {
			// the ground's surface on a slope: walked in, its floor where the slope is (getFloorLevel below). As a solid
			// step (BLOCKED, the node on top) a low slope's next node stood more than a step above the mob: it jumped
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

		// the cell below: its top (a fence's counts 1.5 high, out of a mob's step and jump); the ground's surface on a
		// slope is walked in, so its own top rather than a block's
		BlockPos below = pos.below();
		HostGround.Cell under = HostGround.cellAt(below);
		if (under != null && !under.whole()) {
			double top = below.getY() + (under.kind() == HostGround.Kind.FENCE ? 1.5 : under.top());
			floor = under.support() && level.getBlockState(below).isAir() ? Math.max(top, own != null && own.kind() == HostGround.Kind.LOW
				? pos.getY() + own.top() : top) : Math.max(floor, top);
		}

		if (floor != cir.getReturnValueD()) {
			cir.setReturnValue(floor);
		}
	}
}
