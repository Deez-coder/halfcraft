package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.MobWar;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.projectile.FishingHook;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Reeling in a rod hooked into a host fighter's proxy pulls the real fighter (the proxy only follows him). */
@Mixin(FishingHook.class)
abstract class FishingHookMixin {
	@Inject(method = "pullEntity(Lnet/minecraft/world/entity/Entity;)V", at = @At("HEAD"), cancellable = true)
	private void passthrough$pullFighter(final Entity entity, final CallbackInfo ci) {
		FishingHook self = (FishingHook) (Object) this;
		if (MobWar.isProxy(entity) && self.getOwner() != null && !self.level().isClientSide()) {
			MobWar.onProxyPulled(entity, self, self.getOwner());
			ci.cancel();
		}
	}
}
