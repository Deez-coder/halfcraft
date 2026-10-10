package dev.halfcraft.passthrough.client.mixin;

import dev.halfcraft.passthrough.Passthrough;
import dev.halfcraft.passthrough.client.FrameExporter;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * While the host shows Minecraft's frames, nobody sees Minecraft's own window: the frame isn't drawn into it or
 * swapped (a full-screen blit and a present every frame, on the GPU the host needs). Only the OpenGL work is skipped;
 * the surface's bookkeeping above it runs as usual.
 */
@Mixin(targets = "com.mojang.renderpearl.backend.opengl.GlSurface")
abstract class GlSurfaceMixin {
	@Inject(method = "blitFromTexture", at = @At("HEAD"), cancellable = true)
	private void passthrough$skipBlit(final CallbackInfo ci) {
		if (Passthrough.own && FrameExporter.exporting()) {
			ci.cancel();
		}
	}

	@Inject(method = "present", at = @At("HEAD"), cancellable = true)
	private void passthrough$skipPresent(final CallbackInfo ci) {
		if (Passthrough.own && FrameExporter.exporting()) {
			ci.cancel();
		}
	}
}
