package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The host's ground has no blocks in Minecraft: where its cells are, the world is air. To whatever asks a single spot
 * whether it is a block to stand on or hang from, a "solid" cell (whole, or the ground's surface on a slope) is one:
 * torches, redstone, rails, doors, buttons, pressure plates and the like stay on it; and a wall that covers at least
 * half of a cell's side (an uneven one, not reaching the cell's edge everywhere) is one to hang a torch on there.
 * Its collision for the one-spot checks (a mob's step, a snow layer's footing) is a whole block only in a whole cell,
 * as the barriers' was: a slope's
 * exact shape reaches movement through block-collision queries (BlockCollisionsMixin), and its floor level reaches
 * mobs through WalkNodeEvaluatorMixin.
 */
@Mixin(BlockBehaviour.BlockStateBase.class)
abstract class HostGroundStateMixin {
	@Inject(method = "isFaceSturdy(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/core/Direction;Lnet/minecraft/world/level/block/SupportType;)Z",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$hostGroundSturdy(final BlockGetter level, final BlockPos pos, final Direction direction, final SupportType type,
		final CallbackInfoReturnable<Boolean> cir) {
		if (HostGround.sturdyAir(level, pos, (BlockState) (Object) this, direction)) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "isFaceSturdy(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/core/Direction;)Z",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$hostGroundSturdyFull(final BlockGetter level, final BlockPos pos, final Direction direction, final CallbackInfoReturnable<Boolean> cir) {
		if (HostGround.sturdyAir(level, pos, (BlockState) (Object) this, direction)) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "getBlockSupportShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$hostGroundSupport(final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<VoxelShape> cir) {
		if (HostGround.solidAir(level, pos, (BlockState) (Object) this)) {
			cir.setReturnValue(Shapes.block());
		}
	}

	/** The two-argument form (a per-state cache, not the three-argument one): what single-spot checks use. */
	@Inject(method = "getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$hostGroundShapeShort(final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<VoxelShape> cir) {
		BlockState state = (BlockState) (Object) this;
		if (state.isAir() && HostGround.inHost(level)) {
			HostGround.Cell cell = HostGround.cellAt(pos);
			if (cell != null && cell.whole()) {
				cir.setReturnValue(Shapes.block());
			}
		}
	}
}
