package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.block.FireBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * No fire inside the host's ground (a whole cell is air to Minecraft): lightning lights the spots around it, fire
 * spreads, and both found room there, burning under the map. A bump's cell (not whole) still takes it.
 */
@Mixin(FireBlock.class)
abstract class FireBlockMixin {
	@Inject(method = "canSurvive(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/LevelReader;Lnet/minecraft/core/BlockPos;)Z",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$notInHostGround(final BlockState state, final LevelReader level, final BlockPos pos, final CallbackInfoReturnable<Boolean> cir) {
		HostGround.Cell cell = HostGround.cellAt(pos);
		if (cell != null && cell.whole() && HostGround.inHost(level)) {
			cir.setReturnValue(false);
		}
	}
}
