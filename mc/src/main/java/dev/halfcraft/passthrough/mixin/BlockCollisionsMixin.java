package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockCollisions;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * The host's partial ground cells (HostGround) join every block-collision query: entity movement, step-up, onGround,
 * fall damage and "does this fit" checks then run unchanged against them (players, mobs, items alike).
 */
@Mixin(BlockCollisions.class)
abstract class BlockCollisionsMixin {
	@WrapOperation(method = "computeNext", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/world/phys/shapes/CollisionContext;getCollisionShape(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/CollisionGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"))
	private VoxelShape passthrough$hostGround(final CollisionContext context, final BlockState state, final CollisionGetter level, final BlockPos pos,
		final Operation<VoxelShape> original) {
		VoxelShape shape = original.call(context, state, level, pos);
		HostGround.Cell cell = HostGround.cellAt(pos);
		if (cell == null || (level instanceof Level l && l.dimension() != Level.OVERWORLD)) {
			return shape;
		}

		if (HostGround.exactInstead(state, cell)) {
			return cell.shape(); // our barrier in a partial cell (support for torches...): the slope's own shape
		}

		if (shape == Shapes.block()) {
			return shape; // (a whole block already: the barrier itself, usually)
		}

		return shape.isEmpty() ? cell.shape() : Shapes.or(shape, cell.shape());
	}
}
