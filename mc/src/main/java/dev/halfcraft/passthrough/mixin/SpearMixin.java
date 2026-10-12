package dev.halfcraft.passthrough.mixin;

import com.mojang.datafixers.util.Either;
import dev.halfcraft.passthrough.HostGround;
import dev.halfcraft.passthrough.MobWar;
import dev.halfcraft.passthrough.Passthrough;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.function.Predicate;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.ProjectileUtil;
import net.minecraft.world.item.component.AttackRange;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Spears (their jab and their charge) take what they hit along Steve's look by entity boxes. A Half Sword fighter's
 * proxy box stands upright where he stands; his real body (lying, leaning, a limb out) is elsewhere. The fighters whose
 * real bodies are on that line, within the spear's reach, are hit too (Steve's arrows and crosshair go by them the same
 * way); the host traces each hit into the body from Steve's eyes.
 */
@Mixin(ProjectileUtil.class)
abstract class SpearMixin {
	/** How far into the host's ground a lying body may be and still be hit (as the crosshair's). */
	private static final double THROUGH_GROUND = 0.6;

	@Inject(method = "getHitEntitiesAlong(Lnet/minecraft/world/entity/Entity;Lnet/minecraft/world/item/component/AttackRange;Ljava/util/function/Predicate;Lnet/minecraft/world/level/ClipContext$Block;)Lcom/mojang/datafixers/util/Either;",
		at = @At("RETURN"), cancellable = true)
	private static void passthrough$fightersBodies(final Entity attacker, final AttackRange range, final Predicate<Entity> canHit, final ClipContext.Block blocks,
		final CallbackInfoReturnable<Either<BlockHitResult, Collection<EntityHitResult>>> cir) {
		if (!Passthrough.own || !(attacker instanceof Player) || attacker.level().isClientSide()) {
			return;
		}

		Vec3 eye = attacker.getEyePosition();
		Vec3 look = attacker.getViewVector(1.0F);
		double min = range.effectiveMinRange(attacker), max = range.effectiveMaxRange(attacker);
		Vec3 end = eye.add(look.scale(max));
		// nothing through walls: as far as the first block on the line (a little into the host's ground: lying bodies)
		BlockHitResult wall = attacker.level().clip(new ClipContext(eye, end, blocks, ClipContext.Fluid.NONE, attacker));
		double limit = max;
		if (wall.getType() == HitResult.Type.BLOCK) {
			limit = Math.min(max, wall.getLocation().distanceTo(eye) + (HostGround.cellAt(wall.getBlockPos()) != null ? THROUGH_GROUND : 0.0));
		}

		Map<LivingEntity, Double> found = MobWar.bodiesAlong(eye, end, range.hitboxMargin());
		List<EntityHitResult> hits = new ArrayList<>();
		cir.getReturnValue().ifRight(hits::addAll);
		boolean added = false;
		for (Map.Entry<LivingEntity, Double> e : found.entrySet()) {
			double d = e.getValue() * max;
			LivingEntity proxy = e.getKey();
			if (d >= min && d <= limit && canHit.test(proxy) && hits.stream().noneMatch(h -> h.getEntity() == proxy)) {
				hits.add(new EntityHitResult(proxy, eye.add(look.scale(d))));
				added = true;
			}
		}

		if (added) {
			cir.setReturnValue(Either.right(hits));
		}
	}
}
