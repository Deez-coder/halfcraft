package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Barriers never collide by their own block: what's there is the host's ground shape (HostGround) - whole in a whole
 * cell, a slope's exact shape in a partial one, nothing in the open (one the player placed for support). That holds
 * wherever Minecraft asks a block for its collision shape directly, not only in block-collision queries: mobs'
 * movement (a mob jumps when its feet are under the top of the block in its own cell), floor levels, suffocation.
 * They stay sturdy for torches, redstone and water (that reads a per-block-type cache) and can still be aimed at (by
 * their outline). (The host's world is in the overworld; other dimensions keep vanilla barriers.)
 */
@Mixin(BlockBehaviour.BlockStateBase.class)
abstract class BlockStateSuffocationMixin {
	@Inject(method = "isSuffocating(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$slopeBarrierBreathes(final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<Boolean> cir) {
		if (hostBarrier(level)) {
			HostGround.Cell cell = HostGround.cellAt(pos);
			if (cell == null || !cell.whole()) {
				cir.setReturnValue(false);
			}
		}
	}

	/** A barrier in the host's world (the overworld; a getter that isn't a level, like a pathfinding region, counts). */
	private boolean hostBarrier(final BlockGetter level) {
		return ((BlockState) (Object) this).is(Blocks.BARRIER) && !(level instanceof Level l && l.dimension() != Level.OVERWORLD);
	}

	/**
	 * The two-argument form (a per-state cache, not the three-argument one) is what single-block checks use: a mob
	 * jumps when its feet are under the top of the block in its own cell, which on a slope's cell is always so. To
	 * those, our slope barriers are empty, as the cell was before it had one (movement collides with the exact shape
	 * through block-collision queries; the mob floor level comes from HostGround).
	 */
	@Inject(method = "getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$slopeBarrierShapeShort(final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<VoxelShape> cir) {
		if (hostBarrier(level)) {
			HostGround.Cell cell = HostGround.cellAt(pos);
			if (cell == null || !cell.whole()) {
				cir.setReturnValue(Shapes.empty());
			}
		}
	}

	@Inject(method = "getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/phys/shapes/CollisionContext;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$slopeBarrierShape(final BlockGetter level, final BlockPos pos, final CollisionContext context,
		final CallbackInfoReturnable<VoxelShape> cir) {
		if (hostBarrier(level)) {
			HostGround.Cell cell = HostGround.cellAt(pos);
			cir.setReturnValue(cell == null ? Shapes.empty() : cell.shape());
		}
	}
}
