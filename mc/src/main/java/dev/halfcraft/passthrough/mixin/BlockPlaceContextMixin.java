package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.context.BlockPlaceContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Clicking one of the host's partial ground cells (HostGround: air to Minecraft, so a block would go into it): the
 * block goes next to the face clicked, as on a real block - except on top of a floor in the lower half of its cell,
 * where it stays in that cell (on the floor, sunk at most half a block, rather than floating over it).
 */
@Mixin(BlockPlaceContext.class)
abstract class BlockPlaceContextMixin {
	@Shadow
	@Final
	private BlockPos relativePos;

	@Shadow
	protected boolean replaceClicked;

	@Inject(method = "<init>(Lnet/minecraft/world/level/Level;Lnet/minecraft/world/entity/player/Player;Lnet/minecraft/world/InteractionHand;Lnet/minecraft/world/item/ItemStack;Lnet/minecraft/world/phys/BlockHitResult;)V",
		at = @At("RETURN"))
	private void passthrough$besideHostGround(final Level level, final Player player, final InteractionHand hand, final ItemStack stack, final BlockHitResult hit,
		final CallbackInfo ci) {
		if (!this.replaceClicked || level == null || level.dimension() != Level.OVERWORLD) {
			return;
		}

		BlockPos clicked = hit.getBlockPos();
		VoxelShape host = HostGround.shapeAt(clicked);
		if (host == null || !level.getBlockState(clicked).isAir()) {
			return;
		}

		boolean lowFloor = hit.getDirection() == Direction.UP && host.max(Direction.Axis.Y) <= 0.5;
		if (!lowFloor) {
			this.replaceClicked = false; // (getClickedPos is then relativePos: the cell next to the face)
		}
	}
}
