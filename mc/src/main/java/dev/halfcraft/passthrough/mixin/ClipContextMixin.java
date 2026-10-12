package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Rays (the player's aim, line of sight, projectiles) stop at the host's partial ground cells (HostGround) too, not
 * only at blocks: the crosshair finds the host's floor where it isn't a whole barrier block, so blocks can be placed
 * there (the cell is air, so the block goes into it).
 */
@Mixin(ClipContext.class)
abstract class ClipContextMixin {
	@Shadow
	@Final
	private ClipContext.Block block;

	@Inject(method = "getBlockShape(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
		at = @At("RETURN"), cancellable = true)
	private void passthrough$hostGround(final BlockState state, final BlockGetter level, final BlockPos pos, final CallbackInfoReturnable<VoxelShape> cir) {
		if (this.block == ClipContext.Block.FALLDAMAGE_RESETTING) {
			return;
		}

		HostGround.Cell cell = HostGround.cellAt(pos);
		if (cell == null || (level instanceof Level l && l.dimension() != Level.OVERWORLD)) {
			return;
		}

		VoxelShape shape = cir.getReturnValue();
		if (shape != Shapes.block()) {
			cir.setReturnValue(shape.isEmpty() ? cell.shape() : Shapes.or(shape, cell.shape()));
		}
	}
}
