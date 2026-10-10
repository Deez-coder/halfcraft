package dev.halfcraft.passthrough.mixin;

import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;
import org.spongepowered.asm.mixin.gen.Invoker;

@Mixin(AbstractArrow.class)
public interface AbstractArrowAccessor {
	@Accessor("baseDamage")
	double passthrough$baseDamage();

	@Invoker("isInGround")
	boolean passthrough$isInGround();

	@Invoker("doPostHurtEffects")
	void passthrough$doPostHurtEffects(net.minecraft.world.entity.LivingEntity target);
}
