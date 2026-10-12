package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import dev.halfcraft.passthrough.MobWar;
import dev.halfcraft.passthrough.Passthrough;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.Projectile;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Steve's arrows and rockets fly through proxies: the host traces them into its real people itself. */
@Mixin(Projectile.class)
abstract class ProjectileMixin {
	@Shadow
	public abstract Entity getOwner();

	/**
	 * A projectile ignores a block hit where the block is air. The host's ground is air to vanilla: arrows, pearls,
	 * wind charges and the rest found it, stopped on it and then never landed, creeping down through it into the void.
	 * Its cells are a block to that check.
	 */
	@WrapOperation(method = "hitTargetOrDeflectSelf(Lnet/minecraft/world/phys/HitResult;)Lnet/minecraft/world/entity/projectile/ProjectileDeflection;",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
	private BlockState passthrough$landsOnHostGround(final Level level, final BlockPos pos, final Operation<BlockState> original) {
		BlockState state = original.call(level, pos);
		return state.isAir() && HostGround.cellAt(pos) != null && HostGround.inHost(level) ? Blocks.STONE.defaultBlockState() : state;
	}

	@Inject(method = "canHitEntity(Lnet/minecraft/world/entity/Entity;)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$throughProxies(final Entity target, final CallbackInfoReturnable<Boolean> cir) {
		if (!Passthrough.own && MobWar.isProxy(target) && this.getOwner() instanceof Player) {
			cir.setReturnValue(false);
		}
	}
}
