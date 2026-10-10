package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.WorldBridge;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;
import dev.halfcraft.passthrough.MobWar;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.SimpleExplosionDamageCalculator;
import org.spongepowered.asm.mixin.Final;

/** Every server explosion (TNT, creepers, beds, ...) is reported to the host so it can set off its own. */
@Mixin(ServerExplosion.class)
abstract class ServerExplosionMixin {
	@Shadow
	public abstract Vec3 center();

	@Shadow
	public abstract float radius();

	@Shadow
	public abstract Entity getDirectSourceEntity();

	@Shadow
	@Final
	private ExplosionDamageCalculator damageCalculator;

	@Inject(method = "explode", at = @At("HEAD"))
	private void passthrough$report(final CallbackInfoReturnable<Integer> cir) {
		Entity source = this.getDirectSourceEntity();
		String kind = source == null ? "" : BuiltInRegistries.ENTITY_TYPE.getKey(source.getType()).getPath();
		// wind (a wind charge, a breeze's, Wind Burst on the player who smashed): no damage, only its push, which the
		// proxies pass on themselves (MobWar.onProxyPushed)
		if (this.damageCalculator instanceof SimpleExplosionDamageCalculator calc && !calc.shouldDamageEntity((ServerExplosion) (Object) this, source)) {
			MobWar.wind = kind.contains("wind_charge") ? kind : "wind_burst";
			return;
		}

		WorldBridge.onExplosion(this.center(), this.radius(), kind);
	}

	@Inject(method = "explode", at = @At("RETURN"))
	private void passthrough$windDone(final CallbackInfoReturnable<Integer> cir) {
		MobWar.wind = null;
	}
}
