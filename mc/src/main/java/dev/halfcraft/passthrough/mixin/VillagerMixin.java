package dev.halfcraft.passthrough.mixin;

import dev.halfcraft.passthrough.MobWar;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.npc.villager.Villager;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
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
}
