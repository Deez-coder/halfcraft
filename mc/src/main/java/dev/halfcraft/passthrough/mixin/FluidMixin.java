package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.material.Fluid;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Water and lava don't flow into the host's ground: a solid cell holds no fluid (it rests on it, spreads along it). */
@Mixin(FlowingFluid.class)
abstract class FluidMixin {
	@Inject(method = "canHoldFluid(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/Fluid;)Z",
		at = @At("HEAD"), cancellable = true)
	private static void passthrough$notIntoHostGround(final BlockGetter level, final BlockPos pos, final BlockState state, final Fluid fluid,
		final CallbackInfoReturnable<Boolean> cir) {
		if (HostGround.solidAir(level, pos, state)) {
			cir.setReturnValue(false);
		}
	}
}
