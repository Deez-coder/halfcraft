package dev.halfcraft.passthrough.client.mixin;

import dev.halfcraft.passthrough.Passthrough;
import dev.halfcraft.passthrough.client.Bodies;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * While Minecraft drives, the host's window has the focus but the player is playing Minecraft through it: Minecraft
 * behaves as if it had the focus (mouse look, grabbing the mouse, no inactivity frame cap).
 */
@Mixin(Minecraft.class)
abstract class MinecraftMixin {
	@Inject(method = "isWindowActive", at = @At("HEAD"), cancellable = true)
	private void passthrough$hostFocus(final CallbackInfoReturnable<Boolean> cir) {
		if (Passthrough.own) {
			cir.setReturnValue(true);
		}
	}

	/** Half Sword's GPU balance: Minecraft no faster than a little above the rate Half Sword shows its frames. */
	@Inject(method = "renderFrame", at = @At("TAIL"))
	private void passthrough$pace(final boolean advanceGameTime, final CallbackInfo ci) {
		dev.halfcraft.passthrough.client.Pacer.endFrame();
	}

	/**
	 * Half Sword: a swing at nobody in particular can still hit one of its loose things (a candle stand, a chain, a
	 * weapon on the floor): where Steve looks, as far as he reaches, with how charged the blow is. The host pushes it.
	 */
	@Inject(method = "startAttack()Z", at = @At("HEAD"))
	private void passthrough$swingAtProps(final CallbackInfoReturnable<Boolean> cir) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (Passthrough.own && minecraft.player != null && !minecraft.player.isSpectator() && minecraft.gui.screen() == null
			&& (minecraft.hitResult == null || minecraft.hitResult.getType() != net.minecraft.world.phys.HitResult.Type.ENTITY)) {
			dev.halfcraft.passthrough.client.OwnMode.swing(minecraft.player);
		}
	}

	/** Half Sword: the fighters' real bodies under the crosshair (a fighter on the ground lies outside his proxy's box). */
	@Inject(method = "pick(F)V", at = @At("TAIL"))
	private void passthrough$pickBodies(final float partialTick, final CallbackInfo ci) {
		if (Passthrough.own) {
			Bodies.pick((Minecraft) (Object) this, partialTick);
		}
	}
}
