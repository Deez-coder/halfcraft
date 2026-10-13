package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.item.FireChargeItem;
import net.minecraft.world.item.FlintAndSteelItem;
import net.minecraft.world.item.context.UseOnContext;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Flint and steel and fire charges light the cell next to the face clicked, not as blocks are placed: on the host's
 * ground (a bump low in its cell) the fire floated a block over it. It goes where a block would (HostGround.intoCell).
 */
@Mixin({FlintAndSteelItem.class, FireChargeItem.class})
abstract class FireStarterMixin {
	@WrapOperation(method = "useOn(Lnet/minecraft/world/item/context/UseOnContext;)Lnet/minecraft/world/InteractionResult;",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/core/BlockPos;relative(Lnet/minecraft/core/Direction;)Lnet/minecraft/core/BlockPos;"))
	private BlockPos passthrough$onHostGround(final BlockPos clicked, final Direction face, final Operation<BlockPos> original, final UseOnContext context) {
		return HostGround.intoCell(context.getLevel(), clicked, face, context.getClickLocation()) ? clicked : original.call(clicked, face);
	}
}
