package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.MobWar;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.monster.warden.Warden;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * A warden asks its own canTargetEntity (canAttack too), never canBeSeenAsEnemy: a dead fighter's proxy is no target
 * of its either. Its brain then drops the corpse and its anger at it.
 */
@Mixin(Warden.class)
abstract class WardenMixin {
	@Inject(method = "canTargetEntity(Lnet/minecraft/world/entity/Entity;)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$deadProxyNoTarget(final Entity entity, final CallbackInfoReturnable<Boolean> cir) {
		if (entity != null && MobWar.isDeadProxy(entity)) {
			cir.setReturnValue(false);
		}
	}
}
