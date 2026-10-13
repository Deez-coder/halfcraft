package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.context.BlockPlaceContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Clicking one of the host's ground cells (HostGround: air to Minecraft, so a block would go into it): the block goes
 * where it sits closest to the surface clicked (HostGround.intoCell): into that cell (sunk into the surface at most
 * half a block), else next to the face clicked, as on a real block (at most half a block off it).
 */
@Mixin(BlockPlaceContext.class)
abstract class BlockPlaceContextMixin {
	@Shadow
	protected boolean replaceClicked;

	@Inject(method = "<init>(Lnet/minecraft/world/level/Level;Lnet/minecraft/world/entity/player/Player;Lnet/minecraft/world/InteractionHand;Lnet/minecraft/world/item/ItemStack;Lnet/minecraft/world/phys/BlockHitResult;)V",
		at = @At("RETURN"))
	private void passthrough$besideHostGround(final Level level, final Player player, final InteractionHand hand, final ItemStack stack, final BlockHitResult hit,
		final CallbackInfo ci) {
		BlockPos clicked = hit.getBlockPos();
		if (this.replaceClicked && level != null && level.dimension() == Level.OVERWORLD && HostGround.shapeAt(clicked) != null && level.getBlockState(clicked).isAir()
			&& !HostGround.intoCell(level, clicked, hit.getDirection(), hit.getLocation())) {
			this.replaceClicked = false; // (getClickedPos is then relativePos: the cell next to the face)
		}
	}
}
