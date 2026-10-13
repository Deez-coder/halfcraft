package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HsConfig;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.level.SimpleExplosionDamageCalculator;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** /hs explosions: how hard explosions (TNT, creepers, beds...) throw what they hit; wind isn't one. Their reach stays. */
@Mixin(ServerExplosion.class)
abstract class ExplosionStrengthMixin {
	@WrapOperation(method = "hurtEntities",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/ExplosionDamageCalculator;getKnockbackMultiplier(Lnet/minecraft/world/entity/Entity;)F"))
	private float passthrough$strength(final ExplosionDamageCalculator calc, final Entity entity, final Operation<Float> original) {
		float k = original.call(calc, entity);
		return calc instanceof SimpleExplosionDamageCalculator ? k : (float) (k * HsConfig.explosions());
	}
}
