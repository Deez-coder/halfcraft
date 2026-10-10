package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.Passthrough;
import java.util.Locale;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.portal.TeleportTransition;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Minecraft moving the player (ender pearls, /tp) moves the host's player too; otherwise the host would pull it back. */
@Mixin(ServerPlayer.class)
abstract class ServerPlayerMixin {
	/** The fall protection a mace smash gave, from before the player's own Wind Burst went off. */
	@Unique
	private Vec3 passthrough$smashImpact;

	@Inject(method = "teleport(Lnet/minecraft/world/level/portal/TeleportTransition;)Lnet/minecraft/server/level/ServerPlayer;", at = @At("HEAD"))
	private void passthrough$teleported(final TeleportTransition transition, final CallbackInfoReturnable<ServerPlayer> cir) {
		if (Passthrough.active) {
			Vec3 p = transition.position();
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"pteleport\",\"pos\":[%.3f,%.3f,%.3f]}", p.x, p.y, p.z));
		}
	}

	@Inject(method = "onExplosionHit(Lnet/minecraft/world/entity/Entity;)V", at = @At("HEAD"))
	private void passthrough$beforeBurst(final Entity cause, final CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		this.passthrough$smashImpact = self.currentImpulseImpactPos;
		if (Passthrough.active) {
			Passthrough.LOG.info("player in an explosion of {} at {}", cause == null ? "nothing" : cause.getType().toShortString(), self.position());
		}
	}

	/** What hurts the player, and where he was (to tell a host fighter's blow from anything else). */
	@Inject(method = "hurtServer(Lnet/minecraft/server/level/ServerLevel;Lnet/minecraft/world/damagesource/DamageSource;F)Z", at = @At("HEAD"))
	private void passthrough$logHurt(final net.minecraft.server.level.ServerLevel level, final net.minecraft.world.damagesource.DamageSource source,
		final float amount, final CallbackInfoReturnable<Boolean> cir) {
		if (Passthrough.active) {
			ServerPlayer self = (ServerPlayer) (Object) this;
			Entity by = source.getEntity(), direct = source.getDirectEntity();
			Passthrough.LOG.info("player hurt: {} {} by {} ({}) at {}{}", source.type().msgId(), String.format(Locale.ROOT, "%.2f", amount),
				by == null ? "nothing" : by.getType().toShortString() + " at " + by.position(), direct == null ? "-" : direct.getType().toShortString(),
				self.position(), self.isInvulnerableTo(level, source) ? " (immune)" : "");
		}
	}

	/**
	 * The player's own Wind Burst (a smash that landed) ends the smash's fall protection's grace, and the player is
	 * usually on the ground right then (fighters' proxies don't stop a fall: the smash lands as he touches down), so
	 * the protection went at once and the launch's whole fall hurt. It stays: landing no lower than the smash is safe.
	 */
	@Inject(method = "onExplosionHit(Lnet/minecraft/world/entity/Entity;)V", at = @At("RETURN"))
	private void passthrough$keepSmashProtection(final Entity cause, final CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		if (Passthrough.active && cause == self && this.passthrough$smashImpact != null) {
			self.setIgnoreFallDamageFromCurrentImpulse(true, this.passthrough$smashImpact);
		}
		this.passthrough$smashImpact = null;
	}
}
