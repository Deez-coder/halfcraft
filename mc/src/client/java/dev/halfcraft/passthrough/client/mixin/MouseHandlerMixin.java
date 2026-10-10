package dev.halfcraft.passthrough.client.mixin;

import dev.halfcraft.passthrough.Passthrough;
import dev.halfcraft.passthrough.client.OwnMode;
import net.minecraft.client.Minecraft;
import net.minecraft.client.MouseHandler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The host's mouse motion, as late as possible: just before Minecraft turns the player with it this frame. */
@Mixin(MouseHandler.class)
abstract class MouseHandlerMixin {
	@Inject(method = "handleAccumulatedMovement", at = @At("HEAD"))
	private void passthrough$hostMotion(final CallbackInfo ci) {
		if (Passthrough.own) {
			OwnMode.applyMotion(Minecraft.getInstance());
		}
	}
}
