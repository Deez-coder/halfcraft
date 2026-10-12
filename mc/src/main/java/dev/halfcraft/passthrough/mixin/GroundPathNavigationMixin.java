package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.ai.navigation.GroundPathNavigation;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Where a path goes, for a target standing on the host's ground: vanilla goes down from it through air to the surface
 * (and up out of solid blocks). The host's ground is air to vanilla, so its solid cells are a block to that search, or
 * it went down into the void.
 */
@Mixin(GroundPathNavigation.class)
abstract class GroundPathNavigationMixin {
	@WrapOperation(method = "findSurfacePosition(Lnet/minecraft/world/level/chunk/LevelChunk;Lnet/minecraft/core/BlockPos;I)Lnet/minecraft/core/BlockPos;",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/chunk/LevelChunk;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
	private BlockState passthrough$hostGroundUnderTarget(final LevelChunk chunk, final BlockPos pos, final Operation<BlockState> original) {
		BlockState state = original.call(chunk, pos);
		return HostGround.solidAir(chunk.getLevel(), pos, state) ? Blocks.STONE.defaultBlockState() : state;
	}
}
