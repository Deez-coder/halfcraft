package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.MobWar;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.LightningBolt;
import net.minecraft.world.entity.npc.villager.Villager;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** A fighter's proxy is a villager underneath: right-clicking it must not open a trading screen. */
@Mixin(Villager.class)
abstract class VillagerMixin {
	@Inject(method = "mobInteract", at = @At("HEAD"), cancellable = true)
	private void passthrough$noTrade(final Player player, final InteractionHand hand, final CallbackInfoReturnable<InteractionResult> cir) {
		if (MobWar.isProxy((Villager) (Object) this)) {
			cir.setReturnValue(InteractionResult.PASS);
		}
	}

	/** Lightning would make a proxy a witch (and the fighter nobody's): it strikes the real fighter (MobWar.onProxyLightning). */
	@Inject(method = "thunderHit", at = @At("HEAD"), cancellable = true)
	private void passthrough$struck(final ServerLevel level, final LightningBolt bolt, final CallbackInfo ci) {
		Villager self = (Villager) (Object) this;
		if (MobWar.isProxy(self)) {
			MobWar.onProxyLightning(self);
			ci.cancel();
		}
	}
}
