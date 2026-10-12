package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Sand, gravel, concrete powder and anvils placed on the host's ground stay there (it's air to vanilla: they'd fall). */
@Mixin(FallingBlock.class)
abstract class FallingBlockMixin {
	@WrapOperation(method = "tick(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/server/level/ServerLevel;Lnet/minecraft/core/BlockPos;Lnet/minecraft/util/RandomSource;)V",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/block/FallingBlock;isFree(Lnet/minecraft/world/level/block/state/BlockState;)Z"))
	private boolean passthrough$hostGroundHolds(final BlockState below, final Operation<Boolean> original, final BlockState state, final ServerLevel level,
		final BlockPos pos, final RandomSource random) {
		return original.call(below) && !HostGround.solidAir(level, pos.below(), below);
	}
}
