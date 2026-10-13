package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.block.BannerBlock;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.CakeBlock;
import net.minecraft.world.level.block.CandleCakeBlock;
import net.minecraft.world.level.block.StandingSignBlock;
import net.minecraft.world.level.block.WallBannerBlock;
import net.minecraft.world.level.block.WallSignBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Cakes, signs and banners stand on (or hang from) a block that isSolid(), which the host's ground (air to vanilla)
 * never was: they couldn't be placed on it. Its cells are stone to that check, as to torches' (HostGroundStateMixin).
 */
@Mixin({CakeBlock.class, CandleCakeBlock.class, StandingSignBlock.class, WallSignBlock.class, BannerBlock.class, WallBannerBlock.class})
abstract class SolidBelowMixin {
	@WrapOperation(method = "canSurvive(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/LevelReader;Lnet/minecraft/core/BlockPos;)Z",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/LevelReader;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
	private BlockState passthrough$onHostGround(final LevelReader level, final BlockPos on, final Operation<BlockState> original,
		final BlockState self, final LevelReader l, final BlockPos at) {
		BlockState state = original.call(level, on);
		// (only the side's axis counts: Cell.wallFacing)
		Direction side = on.getY() != at.getY() ? Direction.UP : on.getX() != at.getX() ? Direction.EAST : Direction.SOUTH;
		return HostGround.sturdyAir(level, on, state, side) ? Blocks.STONE.defaultBlockState() : state;
	}
}
