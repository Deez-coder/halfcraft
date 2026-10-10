package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.MobWar;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Wind and a mace's shockwave push a host fighter's proxy: the push goes to the real fighter (MobWar.onProxyPushed). */
@Mixin(Entity.class)
abstract class EntityPushMixin {
	@Inject(method = "push(DDD)V", at = @At("HEAD"), cancellable = true)
	private void passthrough$pushFighter(final double x, final double y, final double z, final CallbackInfo ci) {
		Entity self = (Entity) (Object) this;
		if (!self.level().isClientSide() && MobWar.isProxy(self) && MobWar.onProxyPushed(self, x, y, z)) {
			ci.cancel();
		}
	}
}
