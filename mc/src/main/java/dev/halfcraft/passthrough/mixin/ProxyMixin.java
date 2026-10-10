package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.MobWar;
import dev.halfcraft.passthrough.Passthrough;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Host people's proxies: hits on them go to the host instead of doing damage; they neither push nor get pushed
 * (they follow their person every tick); and Steve's crosshair ignores them (no trading screens, no blocked clicks).
 */
@Mixin(LivingEntity.class)
abstract class ProxyMixin {
	@Inject(method = "hurtServer(Lnet/minecraft/server/level/ServerLevel;Lnet/minecraft/world/damagesource/DamageSource;F)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyHurt(final ServerLevel level, final DamageSource source, final float amount, final CallbackInfoReturnable<Boolean> cir) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (MobWar.isProxy(self)) {
			cir.setReturnValue(MobWar.onProxyHit(self, source, amount));
		}
	}

	/**
	 * A dead fighter's proxy is nobody's enemy: mobs drop it at once and never pick it again (target goals, brains and
	 * zombies' own villager hunting all ask this through canAttack). Steve can still hit the body.
	 */
	@Inject(method = "canBeSeenAsEnemy()Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$deadProxyNoEnemy(final CallbackInfoReturnable<Boolean> cir) {
		if (MobWar.isDeadProxy((LivingEntity) (Object) this)) {
			cir.setReturnValue(false);
		}
	}

	/** Server: the knockback a mob's hit gave the proxy goes to the host with that hit, before the proxy moves. */
	@Inject(method = "tick()V", at = @At("HEAD"))
	private void passthrough$proxyTick(final CallbackInfo ci) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (!self.level().isClientSide() && MobWar.isProxy(self)) {
			MobWar.proxyTick(self);
		}
	}

	/** The player knocked back (server): by what strength, from where. */
	@Inject(method = "knockback(DDDLnet/minecraft/world/damagesource/DamageSource;FZ)V", at = @At("HEAD"))
	private void passthrough$logPlayerKnockback(final double strength, final double x, final double z, final DamageSource source, final float f,
		final boolean b, final CallbackInfo ci) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (Passthrough.active && self instanceof net.minecraft.server.level.ServerPlayer) {
			Passthrough.LOG.info("player knocked back: strength {} towards {}, {} by {} at {}", String.format(java.util.Locale.ROOT, "%.2f", strength),
				String.format(java.util.Locale.ROOT, "%.2f", -x), String.format(java.util.Locale.ROOT, "%.2f", -z),
				source == null ? "-" : source.type().msgId(), self.position());
		}
	}

	@Inject(method = "causeFallDamage(DFLnet/minecraft/world/damagesource/DamageSource;)Z", at = @At("HEAD"))
	private void passthrough$logFall(final double distance, final float multiplier, final DamageSource source, final CallbackInfoReturnable<Boolean> cir) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (Passthrough.active && distance > 3.0 && self instanceof net.minecraft.server.level.ServerPlayer) {
			Passthrough.LOG.info("player landed after falling {} blocks at {}", String.format(java.util.Locale.ROOT, "%.1f", distance), self.position());
		}
	}

	/** Server: healing a proxy (instant health, regeneration) heals its person in the host instead. */
	@Inject(method = "heal(F)V", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyHeal(final float amount, final CallbackInfo ci) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (!self.level().isClientSide() && MobWar.isProxy(self)) {
			MobWar.onProxyHealed(self, amount);
			ci.cancel();
		}
	}

	/** Server: a potion's invisibility on a proxy is noted for its person (the proxy keeps its own endless one). */
	@Inject(method = "addEffect(Lnet/minecraft/world/effect/MobEffectInstance;Lnet/minecraft/world/entity/Entity;)Z", at = @At("HEAD"),
		cancellable = true)
	private void passthrough$proxyEffect(final net.minecraft.world.effect.MobEffectInstance effect, final net.minecraft.world.entity.Entity source,
		final CallbackInfoReturnable<Boolean> cir) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (!self.level().isClientSide() && MobWar.isProxy(self) && MobWar.onProxyEffect(self, effect)) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "isPushable()Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyNotPushable(final CallbackInfoReturnable<Boolean> cir) {
		if (MobWar.isProxy((LivingEntity) (Object) this)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "pushEntities()V", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyNoPush(final CallbackInfo ci) {
		if (MobWar.isProxy((LivingEntity) (Object) this)) {
			ci.cancel();
		}
	}

	@Inject(method = "isPickable()Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$proxyNotPickable(final CallbackInfoReturnable<Boolean> cir) {
		LivingEntity self = (LivingEntity) (Object) this;
		// client only: on the server this is also what lets projectiles hit, and mobs' arrows should hit proxies
		if (self.level().isClientSide() && !Passthrough.own && MobWar.isProxy(self)) {
			cir.setReturnValue(false);
		}
	}
}
