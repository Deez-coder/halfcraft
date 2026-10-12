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
 * Clicking one of the host's ground cells (HostGround: air to Minecraft, so a block would go into it): the block goes
 * where it sits closest to the surface clicked. That surface can be anywhere in its cell (a floor low in it, a wall at
 * its back): with more than half the cell free in front of it, the block goes into that cell (sunk into the surface at
 * most half a block), else next to the face clicked, as on a real block (at most half a block off it). A torch on a
 * wall at the back of its cell hung a whole block out in the air before.
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

		// how much of the cell is free in front of the surface clicked, along its face
		Direction face = hit.getDirection();
		double at = Math.clamp(hit.getLocation().get(face.getAxis()) - clicked.get(face.getAxis()), 0.0, 1.0);
		double free = face.getAxisDirection() == Direction.AxisDirection.POSITIVE ? 1.0 - at : at;
		if (free < 0.5) {
			this.replaceClicked = false; // (getClickedPos is then relativePos: the cell next to the face)
		}
	}
}
