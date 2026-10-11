package dev.halfcraft.passthrough;

import dev.halfcraft.passthrough.mixin.MobAccessor;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicReference;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.tags.DamageTypeTags;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.effect.MobEffectInstance;
import net.minecraft.world.effect.MobEffects;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.EntityReference;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.NeutralMob;
import net.minecraft.world.entity.TamableAnimal;
import net.minecraft.world.entity.ai.memory.MemoryModuleType;
import net.minecraft.world.entity.ai.attributes.AttributeInstance;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.entity.npc.villager.Villager;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.vehicle.boat.AbstractBoat;
import net.minecraft.world.entity.vehicle.minecart.AbstractMinecart;
import net.minecraft.world.phys.Vec3;

/**
 * Minecraft's mobs vs the host's people. The host lists its people ("peds"); each gets an invisible, AI-less
 * villager "proxy" here that follows it and that hostile mobs hunt. A mob's hit on a proxy goes back to the host
 * ("mobhit"), which hurts the real person. The other way round, the host keeps a stand-in for every mob ("mobs") that
 * its police shoot at, and sends the damage back ("mobdmg").
 */
public final class MobWar {
	public static final String PROXY_TAG = "gta_proxy";
	/** Mobs hunt proxies this far away (blocks). */
	private static final double FOLLOW_RANGE = 48.0;
	/** Host people further than this from the player aren't mirrored, nor mobs reported (blocks). */
	private static final double REPORT_RANGE = 96.0;

	/** The newest "peds" list from the host, flattened [handle, x, y, z, dead, ...] (WebSocket thread -> server tick). */
	private static final AtomicReference<double[]> peds = new AtomicReference<>();
	private static volatile long pedsNanos;
	/** Host damage to mobs, [entity id, amount] (WebSocket thread -> server tick). */
	private static final ConcurrentLinkedQueue<double[]> damage = new ConcurrentLinkedQueue<>();

	// server thread only
	private static final Map<Integer, Villager> proxies = new HashMap<>();
	private static final Map<Integer, Integer> handleOf = new HashMap<>();
	private static final Map<Integer, Integer> missingTicks = new HashMap<>();
	/** Host person handle -> their proxy's entity id (the client's crosshair reads it: same id on both sides). */
	private static final Map<Integer, Integer> proxyIds = new java.util.concurrent.ConcurrentHashMap<>();
	/**
	 * The host people's real bodies: points along each skeleton ([handle] -> x, y, z of head, neck, spine, pelvis,
	 * the arms and the legs), from "peds". Steve's crosshair (client) and his arrows (here) test against capsules
	 * between them: a proxy's upright box misses a head, and anyone lying down.
	 */
	public static final int[][] BODY_SEGMENTS = {
		{0, 1}, {1, 2}, {2, 3}, {1, 4}, {4, 5}, {5, 6}, {1, 7}, {7, 8}, {8, 9}, {3, 10}, {10, 11}, {11, 12}, {3, 13}, {13, 14}, {14, 15}
	};
	public static final double[] BODY_RADIUS = {0.15, 0.22, 0.22, 0.12, 0.1, 0.09, 0.12, 0.1, 0.09, 0.15, 0.13, 0.11, 0.15, 0.13, 0.11};
	public static final double HEAD_RADIUS = 0.16;
	public static final int BODY_POINTS = 16;
	private static volatile Map<Integer, double[]> bodies = Map.of();

	/** The newest bodies (WebSocket thread). */
	public static void bodies(final Map<Integer, double[]> next) {
		bodies = next;
	}

	/** Proxies (entity ids) of dead host people: Steve can still hit the body, mobs leave it be. */
	private static final java.util.Set<Integer> deadProxies = java.util.concurrent.ConcurrentHashMap.newKeySet();

	/** Whether this proxy stands for a dead host person. */
	public static boolean isDeadProxy(final Entity e) {
		return deadProxies.contains(e.getId());
	}
	private static boolean reportedMobs;

	private MobWar() {
	}

	/** Brain-driven hostiles: they pick targets from brain memories, not goals, so they'd never join the fight. */
	private static final java.util.Set<String> BRAIN_MOBS = java.util.Set.of("piglin", "piglin_brute", "hoglin", "zoglin", "breeze", "creaking", "warden");

	/**
	 * A host person's stand-in. The server knows its tag; the client only sees that it's an invisible villager (a
	 * permanent hidden invisibility effect keeps that flag set: Minecraft resets it on sync otherwise).
	 */
	public static boolean isProxy(final Entity e) {
		return e instanceof Villager && (e.isInvisible() || e.entityTags().contains(PROXY_TAG));
	}

	/** Mobs that fight back when hurt, and on sight with /hs hostile: Minecraft's neutral mobs, and a few more. */
	private static final java.util.Set<String> NEUTRAL_EXTRA = java.util.Set.of("panda", "dolphin", "llama", "trader_llama");
	/** Flyers the fighters can't reach: no body in Half Sword. */
	private static final java.util.Set<String> UNREACHABLE = java.util.Set.of("ghast", "happy_ghast", "phantom", "vex", "allay", "bat");
	/** Half Sword: at most this many mobs get a body there (the nearest to Steve). */
	private static final int MAX_BODIES = 12;
	/** /hs hostile: neutral mobs attack the fighters and Steve on sight. */
	public static volatile boolean forceHostile;

	private static String kindOf(final Entity e) {
		return BuiltInRegistries.ENTITY_TYPE.getKey(e.getType()).getPath();
	}

	/** Hostile mobs (neutral ones like endermen aren't, though they're monsters). */
	private static boolean hostile(final Entity e) {
		return e instanceof Mob && e instanceof Enemy && !(e instanceof NeutralMob) && !isProxy(e);
	}

	/** Neutral mobs: they fight when provoked. Tamed animals are Steve's. */
	private static boolean neutral(final Entity e) {
		return e instanceof Mob && !isProxy(e) && (e instanceof NeutralMob || NEUTRAL_EXTRA.contains(kindOf(e))) && !(e instanceof TamableAnimal t && t.isTame());
	}

	/** Hostile, goal-driven mobs: they hunt proxies with a target goal of ours. */
	private static boolean fighter(final Entity e) {
		return hostile(e) && !BRAIN_MOBS.contains(kindOf(e));
	}

	/** Mobs Half Sword's fighters fight: hostile and neutral ones they can reach. */
	private static boolean combatant(final Entity e) {
		return (hostile(e) || neutral(e)) && !UNREACHABLE.contains(kindOf(e));
	}

	public static void peds(final double[] flat) {
		peds.set(flat);
		pedsNanos = System.nanoTime();
	}

	public static void damage(final int id, final double amount) {
		damage.add(new double[] {id, amount});
	}

	/** New mobs (and mobs loaded again) hunt proxies; stale proxies from an earlier session are removed. */
	static void onEntityLoad(final Entity e, final ServerLevel level) {
		if (e instanceof Villager v && v.entityTags().contains(PROXY_TAG) && !proxies.containsValue(v)) {
			v.discard();
			return;
		}

		if (!fighter(e)) {
			return;
		}

		Mob mob = (Mob) e;
		GoalSelector targets = ((MobAccessor) mob).passthrough$targetSelector();
		if (targets.getAvailableGoals().stream().noneMatch(w -> w.getGoal() instanceof ProxyTargetGoal)) {
			targets.addGoal(2, new ProxyTargetGoal(mob));
		}

		AttributeInstance range = mob.getAttribute(Attributes.FOLLOW_RANGE);
		if (range != null && range.getBaseValue() < FOLLOW_RANGE) {
			range.setBaseValue(FOLLOW_RANGE);
		}
	}

	/** Mobs' hits on proxies, waiting for the proxy's next tick to go with the knockback they gave it (server thread). */
	private static final Map<Integer, String> pendingHits = new HashMap<>();

	/**
	 * A proxy was hurt (server thread, from LivingEntity.hurtServer; the damage itself is always cancelled). True when
	 * the hit should count as landed: a mob's, so that Minecraft applies its knockback to the proxy (measured and
	 * sent to the host with the hit).
	 */
	public static boolean onProxyHit(final LivingEntity proxy, final DamageSource source, final float amount) {
		Integer handle = handleOf.get(proxy.getId());
		Entity attacker = source.getEntity();
		if (source.getDirectEntity() instanceof Projectile projectile && !(attacker instanceof Player)) {
			projectile.discard(); // a skeleton's arrow ends in the person it hit
		}

		// explosions are the host's own already (every Minecraft explosion is mirrored)
		// (a dead body is only Steve's to cut)
		if (handle == null || source.is(DamageTypeTags.IS_EXPLOSION) || (isDeadProxy(proxy) && !(attacker instanceof Player))) {
			return false;
		}

		// magic: a harming potion (splash, lingering, a tipped arrow), poison and wither ticking: straight off the
		// fighter's health (poison never kills, as in Minecraft)
		if (source.is(net.minecraft.world.damagesource.DamageTypes.MAGIC) || source.is(net.minecraft.world.damagesource.DamageTypes.INDIRECT_MAGIC)
			|| source.is(net.minecraft.world.damagesource.DamageTypes.WITHER)) {
			if (Passthrough.own && !isDeadProxy(proxy)) {
				boolean wither = source.is(net.minecraft.world.damagesource.DamageTypes.WITHER);
				boolean poison = !wither && source.getDirectEntity() == null && proxy.hasEffect(MobEffects.POISON);
				Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"magic\",\"h\":%d,\"d\":%.2f,\"k\":\"%s\",\"nokill\":%b}", handle,
					amount * resistance(proxy), wither ? "wither" : poison ? "poison" : "harming", poison));
			}
			return false;
		}

		// a wind charge's own hit is a point of damage: its burst (the push, onProxyPushed) is what counts
		if (source.getDirectEntity() instanceof net.minecraft.world.entity.projectile.hurtingprojectile.windcharge.AbstractWindCharge) {
			return false;
		}

		if (Passthrough.own) {
			// Half Sword: Steve's sword, his arrows and the mobs all hit the real fighter. "dir" is the push direction
			// (the projectile's flight, or attacker -> proxy), "y" how high on the body (0 feet .. 1 head).
			Entity direct = source.getDirectEntity();
			double dx, dy, dz, hy;
			if (direct instanceof Projectile) {
				var v = direct.getDeltaMovement();
				dx = v.x; dy = v.y; dz = v.z;
				hy = (direct.getY() - proxy.getY()) / proxy.getBbHeight();
			} else if (attacker != null) {
				dx = proxy.getX() - attacker.getX(); dy = 0.15; dz = proxy.getZ() - attacker.getZ();
				hy = attacker instanceof Player p ? (p.getEyeY() - proxy.getY()) / proxy.getBbHeight() : 0.7;
			} else {
				return false;
			}

			// Steve's: his weapon, or an arrow (tridents too); anything else he throws by its kind (snowball, egg)
			boolean arrowLike = direct instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow;
			String kind = attacker instanceof Player ? (direct instanceof Projectile ? (arrowLike ? "arrow" : kindOf(direct)) : "player")
				: BuiltInRegistries.ENTITY_TYPE.getKey((direct != null ? direct : attacker).getType()).getPath();
			// a snowball or an egg only pushes: Minecraft's push, measured like a mob's blow
			boolean thrown = direct instanceof net.minecraft.world.entity.projectile.throwableitemprojectile.Snowball
				|| direct instanceof net.minecraft.world.entity.projectile.throwableitemprojectile.ThrownEgg;
			String item = attacker instanceof Player p ? BuiltInRegistries.ITEM.getKey(p.getMainHandItem().getItem()).getPath() : "";
			boolean crit = attacker instanceof Player p && !(direct instanceof Projectile) && p.fallDistance > 0.0 && !p.onGround();
			// where the strike comes from, for the host to trace it into the fighter's body: Steve's eyes along his
			// look (melee), the projectile's flight, or a mob's head towards the proxy
			double ox, oy, oz, rx, ry, rz;
			if (direct instanceof Projectile) {
				var v = direct.getDeltaMovement().normalize();
				ox = direct.getX() - v.x * 1.5; oy = direct.getY() - v.y * 1.5; oz = direct.getZ() - v.z * 1.5;
				rx = v.x; ry = v.y; rz = v.z;
			} else if (attacker instanceof Player p) {
				var look = p.getLookAngle();
				ox = p.getX(); oy = p.getEyeY(); oz = p.getZ();
				rx = look.x; ry = look.y; rz = look.z;
			} else {
				ox = attacker.getX(); oy = attacker.getEyeY(); oz = attacker.getZ();
				var to = proxy.position().add(0, proxy.getBbHeight() * 0.7, 0).subtract(ox, oy, oz).normalize();
				rx = to.x; ry = to.y; rz = to.z;
			}

			if (direct instanceof Projectile projectile && attacker instanceof Player) {
				// a tipped arrow's potion (a spectral arrow's glow): Minecraft gives it when a hit lands, and this one
				// never lands on the proxy itself
				if (projectile instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow arrow && !isDeadProxy(proxy)) {
					((dev.halfcraft.passthrough.mixin.AbstractArrowAccessor) arrow).passthrough$doPostHurtEffects(proxy);
				}
				projectile.discard(); // it ends in the fighter
			}

			// Steve's Knockback (sword) or Punch (the bow that fired the arrow): Minecraft's own enchantment bonus
			float kbl = 0.0F;
			if (attacker instanceof Player p && proxy.level() instanceof ServerLevel sl) {
				net.minecraft.world.item.ItemStack weapon = direct instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow arrow ? arrow.getWeaponItem()
					: p.getMainHandItem();
				if (weapon != null && !weapon.isEmpty()) {
					kbl = net.minecraft.world.item.enchantment.EnchantmentHelper.modifyKnockback(sl, weapon, proxy, source, 0.0F);
				}
			}

			String hit = String.format(Locale.ROOT,
				"{\"t\":\"hit\",\"h\":%d,\"d\":%.2f,\"dir\":[%.3f,%.3f,%.3f],\"y\":%.2f,\"k\":\"%s\",\"item\":\"%s\",\"crit\":%b,"
					+ "\"from\":[%.3f,%.3f,%.3f],\"ray\":[%.4f,%.4f,%.4f],\"kbl\":%.2f}",
				handle, amount * resistance(proxy) * HsConfig.weapon(item.isEmpty() ? kind : item), dx, dy, dz, Math.clamp(hy, 0.0, 1.0), kind, item, crit,
				ox, oy, oz, rx, ry, rz, kbl);
			// a mace smash (Steve falling onto him): its bonus (Minecraft's, for the fall, Density too) goes with the hit,
			// and the hit lands in Minecraft too, so the mace does the rest itself (Steve bounces, no fall damage, the
			// shockwave around, Wind Burst)
			if (attacker instanceof Player p && direct == p && p.getMainHandItem().getItem() instanceof net.minecraft.world.item.MaceItem mace
				&& net.minecraft.world.item.MaceItem.canSmashAttack(p)) {
				float bonus = mace.getAttackDamageBonus(proxy, amount, source);
				smashTick = proxy.level().getGameTime();
				Passthrough.events.accept(String.format(Locale.ROOT, "%s,\"smash\":%.2f,\"fall\":%.2f}", hit.substring(0, hit.length() - 1), bonus,
					p.fallDistance));
				return true;
			}

			if (attacker instanceof Mob || thrown) {
				// Minecraft's knockback for this hit, on the proxy: the usual push away from the attacker (hurtServer's,
				// which is cancelled here), then whatever the mob's attack adds once it lands (an iron golem's fling up,
				// a ravager's shove). It goes with the hit when the proxy next ticks.
				if (!pendingHits.containsKey(proxy.getId())) {
					proxy.setDeltaMovement(Vec3.ZERO);
				}

				proxy.dealDefaultKnockback(source, amount, false);
				pendingHits.put(proxy.getId(), hit);
				return true;
			}

			Passthrough.events.accept(hit);
			return false;
		}

		if (attacker == null || attacker instanceof Player) {
			return false;
		}

		String kind = BuiltInRegistries.ENTITY_TYPE.getKey(attacker.getType()).getPath();
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"mobhit\",\"h\":%d,\"d\":%.2f,\"from\":[%.3f,%.3f,%.3f],\"k\":\"%s\"}",
			handle, amount, attacker.getX(), attacker.getY(), attacker.getZ(), kind));
		return false;
	}

	/**
	 * A proxy's tick, before it moves (server thread): a mob's hit since its last tick goes to the host with the
	 * knockback it gave the proxy (blocks per tick, "kb"); then it stands still again (it follows its person).
	 */
	public static void proxyTick(final LivingEntity proxy) {
		// just under full health: Minecraft's regeneration only heals the hurt (and the healing goes to the host)
		if (proxy.getHealth() >= proxy.getMaxHealth()) {
			proxy.setHealth(proxy.getMaxHealth() - 1.0F);
		}

		String hit = pendingHits.remove(proxy.getId());
		Vec3 v = proxy.getDeltaMovement();
		if (hit != null) {
			Passthrough.events.accept(String.format(Locale.ROOT, "%s,\"kb\":[%.4f,%.4f,%.4f]}", hit.substring(0, hit.length() - 1), v.x, v.y, v.z));
		}

		if (v.lengthSqr() > 0.0) {
			proxy.setDeltaMovement(Vec3.ZERO);
		}
	}

	/** Minecraft's Resistance on a proxy: what's left of a blow (-20% a level). */
	private static float resistance(final LivingEntity e) {
		MobEffectInstance r = e.getEffect(MobEffects.RESISTANCE);
		return r == null ? 1.0F : Math.max(0.0F, 1.0F - 0.2F * (r.getAmplifier() + 1));
	}

	/** Minecraft heals a proxy (server thread, LivingEntity.heal): instant health, regeneration -> the fighter. */
	public static void onProxyHealed(final LivingEntity proxy, final float amount) {
		Integer handle = handleOf.get(proxy.getId());
		if (handle != null && Passthrough.own && !isDeadProxy(proxy) && amount > 0.0F) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"heal\",\"h\":%d,\"d\":%.2f}", handle, amount));
		}
	}

	/** Proxies (entity ids) made invisible by a potion, until (game time): they're always invisible themselves. */
	private static final Map<Integer, Long> invisibleUntil = new HashMap<>();

	/**
	 * An effect on a proxy (server thread, LivingEntity.addEffect). A potion's invisibility is only noted (the proxy
	 * keeps its own, endless one); true when handled so.
	 */
	public static boolean onProxyEffect(final LivingEntity proxy, final MobEffectInstance effect) {
		if (effect.getEffect().value() != MobEffects.INVISIBILITY.value() || effect.isInfiniteDuration()) {
			return false;
		}

		invisibleUntil.merge(proxy.getId(), proxy.level().getGameTime() + effect.getDuration(), Math::max);
		return true;
	}

	private static int fxTicks;
	private static boolean fxSent;

	private static void level(final StringBuilder b, final int handle, final MobEffectInstance e) {
		if (e != null) {
			b.append(b.isEmpty() ? "" : ",").append(handle).append(',').append(e.getAmplifier() + 1);
		}
	}

	/**
	 * The effects on host people's proxies that change how they fight: {"t":"fx","slow":[h,level,..],"speed":[..],
	 * "str":[..],"weak":[..],"invis":[h,..]} every half second while any have one (and once more, empty, after).
	 */
	private static void reportEffects(final ServerLevel level) {
		if (++fxTicks < 10 || !Passthrough.own) {
			return;
		}

		fxTicks = 0;
		long now = level.getGameTime();
		invisibleUntil.values().removeIf(t -> t <= now);
		StringBuilder slow = new StringBuilder(), speed = new StringBuilder(), str = new StringBuilder(), weak = new StringBuilder(),
			invis = new StringBuilder();
		for (Map.Entry<Integer, Villager> e : proxies.entrySet()) {
			Villager v = e.getValue();
			if (v.isRemoved() || deadProxies.contains(v.getId())) {
				continue;
			}

			level(slow, e.getKey(), v.getEffect(MobEffects.SLOWNESS));
			level(speed, e.getKey(), v.getEffect(MobEffects.SPEED));
			level(str, e.getKey(), v.getEffect(MobEffects.STRENGTH));
			level(weak, e.getKey(), v.getEffect(MobEffects.WEAKNESS));
			if (invisibleUntil.containsKey(v.getId())) {
				invis.append(invis.isEmpty() ? "" : ",").append(e.getKey());
			}
		}

		boolean any = !(slow.isEmpty() && speed.isEmpty() && str.isEmpty() && weak.isEmpty() && invis.isEmpty());
		if (any || fxSent) {
			Passthrough.events.accept("{\"t\":\"fx\",\"slow\":[" + slow + "],\"speed\":[" + speed + "],\"str\":[" + str + "],\"weak\":[" + weak
				+ "],\"invis\":[" + invis + "]}");
		}

		fxSent = any;
	}

	/** The game time of Steve's last mace smash: Minecraft's pushes on proxies in that tick are its shockwave. */
	private static long smashTick = -1;
	/** The wind explosion going off right now (server thread), e.g. "wind_charge", "wind_burst"; null if none. */
	public static String wind;

	/**
	 * Minecraft pushes a proxy (server thread, Entity.push): a wind burst or a mace's shockwave goes to the host's
	 * fighter, blocks per tick (the proxy itself only follows him). True when handled (the push is cancelled).
	 */
	public static boolean onProxyPushed(final Entity proxy, final double x, final double y, final double z) {
		Integer handle = handleOf.get(proxy.getId());
		String kind = wind != null ? wind : proxy.level().getGameTime() == smashTick ? "mace" : null;
		if (handle == null || !Passthrough.own || kind == null) {
			return false;
		}

		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"push\",\"h\":%d,\"v\":[%.4f,%.4f,%.4f],\"k\":\"%s\"}",
			handle, x, y, z, kind));
		return true;
	}

	/**
	 * Steve reels in a rod hooked into a proxy (server thread, FishingHook.pullEntity): Minecraft's own pull, a tenth
	 * of the way from the hook to him, goes to the host's fighter as a push (the proxy only follows him).
	 */
	public static void onProxyPulled(final Entity proxy, final Entity hook, final Entity owner) {
		Integer handle = handleOf.get(proxy.getId());
		if (handle == null || !Passthrough.own) {
			return;
		}

		Vec3 v = new Vec3(owner.getX() - hook.getX(), owner.getY() - hook.getY(), owner.getZ() - hook.getZ()).scale(0.1);
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"push\",\"h\":%d,\"v\":[%.4f,%.4f,%.4f],\"k\":\"fishing_rod\"}",
			handle, v.x, v.y, v.z));
	}

	/** A fighter's hits on mobs' bodies in the host: [mob entity id, damage, fighter handle] (WebSocket thread -> tick). */
	private static final ConcurrentLinkedQueue<double[]> fighterHits = new ConcurrentLinkedQueue<>();

	/** {"t":"mobhurt","id":entity,"d":damage,"by":fighter handle} (WebSocket thread). */
	public static void fighterHit(final int id, final double amount, final int handle) {
		fighterHits.add(new double[] {id, amount, handle});
	}

	/** The fighters' hits on mobs: from the fighter's proxy, so Minecraft pushes the mob away from him and it turns on him. */
	private static void applyFighterHits(final ServerLevel level) {
		for (double[] d; (d = fighterHits.poll()) != null;) {
			if (!(level.getEntity((int) d[0]) instanceof Mob mob) || !mob.isAlive()) {
				continue;
			}

			Villager by = proxies.get((int) d[2]);
			LivingEntity from = by != null && !by.isRemoved() ? by : nearestProxy(level, mob);
			DamageSource source = from != null ? level.damageSources().mobAttack(from) : level.damageSources().generic();
			// Minecraft's hurt cooldown merges a blade touching for several frames into one hit, as with Steve
			mob.hurtServer(level, source, (float) d[1]);
		}
	}

	private static int aimTicks;
	/** Neutral mobs /hs hostile set on someone (to calm them again when it's turned off). */
	private static final java.util.Set<java.util.UUID> angered = new java.util.HashSet<>();

	/**
	 * Every half second: brain-driven hostiles without a target go for the nearest fighter (their brains never look
	 * for one), and with /hs hostile, neutral mobs without a target go for the nearest fighter or Steve.
	 */
	private static void aimMobs(final MinecraftServer s, final ServerLevel level) {
		if (++aimTicks < 10) {
			return;
		}

		aimTicks = 0;
		ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
		if (player == null) {
			return;
		}

		for (Entity e : level.getEntities((Entity) null, player.getBoundingBox().inflate(REPORT_RANGE), x -> x instanceof Mob && x.isAlive())) {
			Mob mob = (Mob) e;
			if (hostile(mob) && BRAIN_MOBS.contains(kindOf(mob))) {
				// (a fighter who died is dropped: the next living one nearest)
				if (mob.getBrain().getMemory(MemoryModuleType.ATTACK_TARGET).filter(MobWar::isDeadProxy).isPresent()) {
					mob.getBrain().eraseMemory(MemoryModuleType.ATTACK_TARGET);
				}

				if (mob instanceof net.minecraft.world.entity.monster.warden.Warden warden) {
					// a warden stays angry at whoever it fought, and goes back to the angriest
					for (Villager v : proxies.values()) {
						if (isDeadProxy(v)) {
							warden.clearAnger(v);
						}
					}
				}

				if (Passthrough.own && !mob.getBrain().hasMemoryValue(MemoryModuleType.ATTACK_TARGET)) {
					LivingEntity t = nearestLivingProxy(mob);
					if (t != null && t.distanceTo(mob) < 32.0F) {
						mob.getBrain().setMemory(MemoryModuleType.ATTACK_TARGET, t);
					}
				}
			} else if (neutral(mob)) {
				if (forceHostile) {
					LivingEntity current = mob.getTarget();
					if (current == null || !current.isAlive() || current.isRemoved() || (isProxy(current) && isDeadProxy(current))) {
						LivingEntity t = nearestTarget(level, mob, player);
						if (t != null) {
							if (mob instanceof NeutralMob n) {
								n.setPersistentAngerTarget(EntityReference.of(t));
								n.startPersistentAngerTimer();
							}

							mob.setTarget(t);
							angered.add(mob.getUUID());
						}
					}
				} else if (angered.remove(mob.getUUID())) {
					if (mob instanceof NeutralMob n) {
						n.stopBeingAngry();
					}

					mob.setTarget(null);
				}
			}
		}

		if (!forceHostile) {
			angered.clear(); // (the rest calm down when their anger runs out)
		}
	}

	/** The nearest living fighter's proxy, within 48 blocks of the mob. */
	private static LivingEntity nearestLivingProxy(final Mob mob) {
		LivingEntity best = null;
		double bestD = 48.0 * 48.0;
		for (Villager v : proxies.values()) {
			double dd = v.distanceToSqr(mob);
			if (!v.isRemoved() && !isDeadProxy(v) && dd < bestD) {
				best = v;
				bestD = dd;
			}
		}

		return best;
	}

	/** The nearest standing fighter's proxy or Steve (in survival), within 32 blocks of the mob. */
	private static LivingEntity nearestTarget(final ServerLevel level, final Mob mob, final ServerPlayer player) {
		LivingEntity best = null;
		double bestD = 32.0 * 32.0;
		for (Villager v : proxies.values()) {
			double dd = v.distanceToSqr(mob);
			if (!v.isRemoved() && !isDeadProxy(v) && dd < bestD) {
				best = v;
				bestD = dd;
			}
		}

		if (player.isAlive() && !player.isCreative() && !player.isSpectator() && player.distanceToSqr(mob) < bestD) {
			best = player;
		}

		return best;
	}

	/** Every server tick (server thread). */
	static void tick(final MinecraftServer s) {
		ServerLevel level = s.overworld();
		syncProxies(level);
		ride(level);
		applyDamage(level);
		applyFighterHits(level);
		aimMobs(s, level);
		reportBurning(level);
		reportEffects(level);
		pendingHits.keySet().removeIf(id -> !(level.getEntity(id) instanceof LivingEntity p) || p.isRemoved()); // (proxies gone)
		if (Passthrough.own) {
			arrowsIntoBodies(level);
		}
		if (Passthrough.active) {
			reportMobs(s, level);
		}
	}

	private static void syncProxies(final ServerLevel level) {
		if (!proxies.isEmpty() && System.nanoTime() - pedsNanos > 1_500_000_000L) {
			clearProxies(); // the host stopped sending: nobody to hunt
			return;
		}

		double[] p = peds.getAndSet(null);
		if (p == null) {
			return;
		}

		Map<Integer, Boolean> seen = new HashMap<>();
		for (int i = 0; i + 4 < p.length; i += 5) {
			int handle = (int) p[i];
			double x = p[i + 1], y = p[i + 2], z = p[i + 3];
			boolean dead = p[i + 4] != 0.0;
			seen.put(handle, true);
			Villager v = proxies.get(handle);
			if (v == null || v.isRemoved()) {
				v = EntityTypes.VILLAGER.create(level, EntitySpawnReason.COMMAND);
				if (v == null) {
					continue;
				}

				v.setInvisible(true);
				v.addEffect(new MobEffectInstance(MobEffects.INVISIBILITY, MobEffectInstance.INFINITE_DURATION, 0, false, false), null);
				v.setNoAi(true);
				v.setNoGravity(true);
				v.setSilent(true);
				v.addTag(PROXY_TAG);
				v.snapTo(x, y, z, 0.0F, 0.0F);
				proxies.put(handle, v); // before adding: the load event must not take it for a stale one
				if (!level.addFreshEntity(v)) {
					proxies.remove(handle);
					continue;
				}

				handleOf.put(v.getId(), handle);
				proxyIds.put(handle, v.getId());
			} else if (!v.isPassenger()) {
				v.setPos(x, y, z); // (a rider sits where his boat or cart puts him)
			}

			if (dead) {
				deadProxies.add(v.getId());
			} else {
				deadProxies.remove(v.getId());
			}

			missingTicks.remove(handle);
		}

		// hysteresis: a person missing from a few lists in a row (out of range, dead, despawned) loses their proxy
		for (Iterator<Map.Entry<Integer, Villager>> it = proxies.entrySet().iterator(); it.hasNext();) {
			Map.Entry<Integer, Villager> e = it.next();
			if (seen.containsKey(e.getKey())) {
				continue;
			}

			int missing = missingTicks.merge(e.getKey(), 1, Integer::sum);
			if (missing > 6 || e.getValue().isRemoved()) {
				handleOf.remove(e.getValue().getId());
				proxyIds.remove(e.getKey());
				e.getValue().discard();
				missingTicks.remove(e.getKey());
				it.remove();
			}
		}
	}

	/** Server ticks a fighter stays out of boats and carts after getting out (he'd climb straight back in). */
	private static final int REBOARD_TICKS = 60;
	/** Fighter handle -> the game time he may get in again. */
	private static final Map<Integer, Long> boardAfter = new HashMap<>();
	private static boolean hadRiders;
	/**
	 * The riders for the client (it draws their vehicles a few ticks behind the server: the seat it reports each frame
	 * is where the vehicle is seen): handle -> {vehicle entity id, the seat's offset from the vehicle x, z}.
	 */
	public static volatile Map<Integer, double[]> riders = Map.of();
	/** Fighters the host says fell out of their boat or cart (WebSocket thread -> server tick). */
	private static final ConcurrentLinkedQueue<Integer> fellOut = new ConcurrentLinkedQueue<>();

	/** {"t":"unride","h":handle}: the fighter's body is nowhere near his seat any more (WebSocket thread). */
	public static void fellOut(final int handle) {
		fellOut.add(handle);
	}

	/** Boats (rafts too) and minecarts that carry people. */
	private static boolean carrier(final Entity e) {
		return !e.isRemoved() && (e instanceof AbstractBoat || (e instanceof AbstractMinecart m && m.isRideable()));
	}

	/**
	 * Fighters in boats and minecarts (server thread, after syncProxies). A living fighter whose proxy touches one gets
	 * in, as Minecraft's mobs do, and stays in until it breaks (or an activator rail, water over the boat...). A dead
	 * one gets out. The host hears every rider's seat each tick: {"t":"ride","r":[[handle, x, y, z, kind],...]}, y the
	 * vehicle's bottom, kind 0 boat, 1 minecart; then once an empty list when the last one is out.
	 */
	private static void ride(final ServerLevel level) {
		long now = level.getGameTime();
		for (Integer h; (h = fellOut.poll()) != null;) {
			Villager v = proxies.get(h);
			if (v != null && v.isPassenger()) {
				v.stopRiding();
				Passthrough.LOG.info("fighter {} fell out of his vehicle", h);
			}

			boardAfter.put(h, now + REBOARD_TICKS);
		}

		StringBuilder b = null;
		Map<Integer, double[]> seats = new HashMap<>();
		for (Map.Entry<Integer, Villager> e : proxies.entrySet()) {
			Villager v = e.getValue();
			int handle = e.getKey();
			if (v.isRemoved()) {
				continue;
			}

			boolean dead = isDeadProxy(v);
			if (v.isPassenger()) {
				Entity vehicle = v.getVehicle();
				if (dead || vehicle == null || !carrier(vehicle)) {
					v.stopRiding();
					boardAfter.put(handle, now + REBOARD_TICKS);
					Passthrough.LOG.info("fighter {} got out ({})", handle, dead ? "dead" : "no vehicle");
					continue;
				}

				seats.put(handle, new double[] {vehicle.getId(), v.getX() - vehicle.getX(), v.getZ() - vehicle.getZ()});
				b = b == null ? new StringBuilder("{\"t\":\"ride\",\"r\":[") : b.append(',');
				b.append(String.format(Locale.ROOT, "[%d,%.3f,%.3f,%.3f,%d]", handle, v.getX(), vehicle.getY(), v.getZ(),
					vehicle instanceof AbstractMinecart ? 1 : 0));
				continue;
			}

			if (boardAfter.containsKey(handle)) {
				if (now < boardAfter.get(handle)) {
					continue;
				}

				boardAfter.remove(handle);
			}

			if (dead || !Passthrough.own) {
				continue;
			}

			for (Entity vehicle : level.getEntities(v, v.getBoundingBox().inflate(0.2), MobWar::carrier)) {
				if (v.startRiding(vehicle)) {
					Passthrough.LOG.info("fighter {} got in a {}", handle, kindOf(vehicle));
					break;
				}
			}
		}

		if (b != null || hadRiders) {
			Passthrough.events.accept(b == null ? "{\"t\":\"ride\",\"r\":[]}" : b.append("]}").toString());
		}

		hadRiders = b != null;
		riders = seats.isEmpty() ? Map.of() : seats;
	}

	private static void applyDamage(final ServerLevel level) {
		for (double[] d; (d = damage.poll()) != null;) {
			if (!(level.getEntity((int) d[0]) instanceof Mob mob) || !mob.isAlive()) {
				continue;
			}

			// from the nearest person (a cop, most likely), so the mob turns on them
			LivingEntity from = nearestProxy(level, mob);
			DamageSource source = from != null ? level.damageSources().mobAttack(from) : level.damageSources().generic();
			mob.damageCooldownTime = 0; // automatic fire lands several hits inside the usual 10-tick cooldown
			mob.hurtServer(level, source, (float) d[1]);
		}
	}

	/** The entity id of a host person's proxy, or -1 (any thread). */
	public static int proxyId(final int handle) {
		return proxyIds.getOrDefault(handle, -1);
	}

	private static int burnTicks;

	/**
	 * Steve's arrows in flight against the host people's real bodies, along the way each went this tick: one that
	 * passes through a head or limb hits that person (as a hit on their proxy, from where the arrow met the body).
	 */
	/** Steve's arrows in flight last tick (one that's in the ground now landed this tick). */
	private static java.util.Set<Integer> flyingArrows = new java.util.HashSet<>();

	private static void arrowsIntoBodies(final ServerLevel level) {
		Map<Integer, double[]> all = bodies;
		if (all.isEmpty()) {
			return;
		}

		// arrows in flight, and arrows that landed this tick: a fighter lying down has his legs and head on the ground
		// (the host's barriers), so an arrow at them meets the ground in the same tick it passes through them
		List<net.minecraft.world.entity.projectile.arrow.AbstractArrow> arrows = new ArrayList<>();
		java.util.Set<Integer> nowFlying = new java.util.HashSet<>();
		for (Entity e : level.getAllEntities()) {
			if (e instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow a && a.getOwner() instanceof Player && !a.isRemoved()) {
				if (!((dev.halfcraft.passthrough.mixin.AbstractArrowAccessor) a).passthrough$isInGround()) {
					nowFlying.add(a.getId());
					arrows.add(a);
				} else if (flyingArrows.contains(a.getId())) {
					arrows.add(a);
				}
			}
		}

		java.util.Set<Integer> landedNow = new java.util.HashSet<>(flyingArrows);
		flyingArrows = nowFlying;
		for (net.minecraft.world.entity.projectile.arrow.AbstractArrow a : arrows) {
			double ax = a.xo, ay = a.yo, az = a.zo, bx = a.getX(), by = a.getY(), bz = a.getZ();
			double len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay) + (bz - az) * (bz - az);
			if (len2 < 1.0E-6) {
				continue;
			}

			boolean landed = landedNow.contains(a.getId()) && !nowFlying.contains(a.getId());
			// the last tick's travel: a landed arrow's speed (it has none left)
			Vec3 travel = new Vec3(bx - ax, by - ay, bz - az);
			if (landed) {
				// stopped at the ground's surface: on a little into it, where a lying body sinks
				double len = Math.sqrt(len2), more = 0.6 / len;
				bx += (bx - ax) * more;
				by += (by - ay) * more;
				bz += (bz - az) * more;
			}

			int bestHandle = -1;
			double bestT = 2.0;
			for (Map.Entry<Integer, double[]> e : all.entrySet()) {
				double[] p = e.getValue();
				for (int s = 0; s <= BODY_SEGMENTS.length; s++) {
					double[] seg;
					double r;
					if (s == BODY_SEGMENTS.length) {
						// the head: from the head bone on, as far again as the neck is below it
						seg = new double[] {p[0], p[1], p[2], 2 * p[0] - p[3], 2 * p[1] - p[4], 2 * p[2] - p[5]};
						r = HEAD_RADIUS;
					} else {
						int i = BODY_SEGMENTS[s][0] * 3, j = BODY_SEGMENTS[s][1] * 3;
						seg = new double[] {p[i], p[i + 1], p[i + 2], p[j], p[j + 1], p[j + 2]};
						r = BODY_RADIUS[s];
					}

					double t = segmentsMeet(ax, ay, az, bx, by, bz, seg, r + 0.05);
					if (t >= 0.0 && t < bestT) {
						bestT = t;
						bestHandle = e.getKey();
					}
				}
			}

			Villager proxy = bestHandle < 0 ? null : proxies.get(bestHandle);
			if (proxy == null || proxy.isRemoved()) {
				continue;
			}

			// Minecraft's own arrow damage: speed times its base damage (Power raises it), more for a crit
			if (landed) {
				a.setDeltaMovement(travel); // (the hit's direction and force come from it)
			}

			var v = a.getDeltaMovement();
			int damage = (int) Math.ceil(Math.clamp(v.length() * ((dev.halfcraft.passthrough.mixin.AbstractArrowAccessor) a).passthrough$baseDamage(), 0.0,
				Integer.MAX_VALUE));
			if (a.isCritArrow()) {
				damage += ThreadLocalRandom.current().nextInt(damage / 2 + 2);
			}

			// where it met the body: the hit is traced from there into the real body
			a.setPos(ax + (bx - ax) * bestT, ay + (by - ay) * bestT, az + (bz - az) * bestT);
			proxy.hurtServer(level, level.damageSources().arrow(a, a.getOwner()), damage);
			if (!a.isRemoved()) {
				a.discard();
			}
		}
	}

	/**
	 * Where (0..1 along a-b) segment a-b first comes within `r` of segment `seg` (x, y, z, x, y, z), or -1. The two
	 * segments' closest points, near enough for thin limbs and fast arrows.
	 */
	private static double segmentsMeet(final double ax, final double ay, final double az, final double bx, final double by, final double bz,
		final double[] seg, final double r) {
		double dx = bx - ax, dy = by - ay, dz = bz - az;
		double ex = seg[3] - seg[0], ey = seg[4] - seg[1], ez = seg[5] - seg[2];
		double wx = ax - seg[0], wy = ay - seg[1], wz = az - seg[2];
		double a = dx * dx + dy * dy + dz * dz, b = dx * ex + dy * ey + dz * ez, c = ex * ex + ey * ey + ez * ez;
		double d = dx * wx + dy * wy + dz * wz, e = ex * wx + ey * wy + ez * wz;
		double den = a * c - b * b;
		double s = den > 1.0E-9 ? Math.clamp((b * e - c * d) / den, 0.0, 1.0) : 0.0;
		double t = c > 1.0E-9 ? Math.clamp((b * s + e) / c, 0.0, 1.0) : 0.0;
		// t clamped: the arrow's point nearest that body point
		s = a > 1.0E-9 ? Math.clamp((b * t - d) / a, 0.0, 1.0) : 0.0;
		double px = ax + dx * s - (seg[0] + ex * t), py = ay + dy * s - (seg[1] + ey * t), pz = az + dz * s - (seg[2] + ez * t);
		double dist2 = px * px + py * py + pz * pz;
		if (dist2 > r * r) {
			return -1.0;
		}

		// back up from the closest approach to where it enters
		return Math.max(0.0, s - Math.sqrt(r * r - dist2) / Math.sqrt(a));
	}

	/**
	 * Host people whose proxies are on fire (Minecraft's own fire: flint and steel, fire aspect, lava, a burning
	 * arrow): {"t":"burn","h":[handles]} every few ticks while any burn. The host makes them panic.
	 */
	private static void reportBurning(final ServerLevel level) {
		if (++burnTicks < 4 || !Passthrough.own) {
			return;
		}

		burnTicks = 0;
		StringBuilder b = null;
		for (Map.Entry<Integer, Villager> e : proxies.entrySet()) {
			if (!e.getValue().isRemoved() && e.getValue().isOnFire() && !deadProxies.contains(e.getValue().getId())
				&& !e.getValue().hasEffect(MobEffects.FIRE_RESISTANCE)) {
				b = b == null ? new StringBuilder("{\"t\":\"burn\",\"h\":[") : b.append(',');
				b.append(e.getKey());
			}
		}

		if (b != null) {
			Passthrough.events.accept(b.append("]}").toString());
		}
	}

	/** The proxy nearest (x, y, z), within 4 blocks (server thread). */
	public static LivingEntity proxyNear(final ServerLevel level, final double x, final double y, final double z) {
		LivingEntity best = null;
		double bestD = 4.0 * 4.0;
		for (Villager v : proxies.values()) {
			double dd = v.distanceToSqr(x, y, z);
			if (!v.isRemoved() && dd < bestD) {
				best = v;
				bestD = dd;
			}
		}

		return best;
	}

	private static LivingEntity nearestProxy(final ServerLevel level, final Mob mob) {
		LivingEntity best = null;
		double bestD = 48.0 * 48.0;
		for (Villager v : proxies.values()) {
			double dd = v.distanceToSqr(mob);
			if (!v.isRemoved() && dd < bestD) {
				best = v;
				bestD = dd;
			}
		}

		return best;
	}

	/**
	 * {"t":"mobs","m":[[id,"zombie",x,y,z,height,yaw],...]}: the hostile and neutral mobs near the player (the nearest
	 * MAX_BODIES), every tick while there are any. Half Sword gives each a body its fighters fight.
	 */
	private static void reportMobs(final MinecraftServer s, final ServerLevel level) {
		ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
		if (player == null) {
			return;
		}

		List<Entity> near = new ArrayList<>(level.getEntities((Entity) null, player.getBoundingBox().inflate(REPORT_RANGE), MobWar::combatant));
		near.sort(java.util.Comparator.comparingDouble(e -> e.distanceToSqr(player)));
		if (near.isEmpty() && !reportedMobs) {
			return;
		}

		StringBuilder b = new StringBuilder("{\"t\":\"mobs\",\"m\":[");
		int n = 0;
		for (Entity e : near) {
			if (!e.isAlive()) {
				continue;
			}

			if (n >= MAX_BODIES) {
				break;
			}

			b.append(n++ == 0 ? "" : ",").append(String.format(Locale.ROOT, "[%d,\"%s\",%.3f,%.3f,%.3f,%.2f,%.1f]",
				e.getId(), kindOf(e), e.getX(), e.getY(), e.getZ(), e.getBbHeight(), e.getYRot()));
		}

		Passthrough.events.accept(b.append("]}").toString());
		reportedMobs = n > 0;
	}

	/**
	 * Spawn `count` mobs of `kind` (e.g. "zombie") on the ground `minR`..`maxR` blocks from the player, within `arc`
	 * degrees either side of where the player faces (or of `yawOffset` from it); or, given `at` ({x, y, z}: a fixed
	 * spot, y near its ground), all round that spot instead, wherever the player is.
	 */
	public static void spawn(final String kind, final int count, final double minR, final double maxR, final double arc, final double yawOffset,
		final double[] at) {
		MinecraftServer s = serverOrNull();
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			Optional<EntityType<?>> type = BuiltInRegistries.ENTITY_TYPE.getOptional(Identifier.withDefaultNamespace(kind));
			if (player == null || type.isEmpty()) {
				Passthrough.LOG.warn("spawnmobs: no player or unknown mob {}", kind);
				return;
			}

			ThreadLocalRandom random = ThreadLocalRandom.current();
			double cx = at != null ? at[0] : player.getX(), cy = at != null ? at[1] : player.getY(), cz = at != null ? at[2] : player.getZ();
			double baseYaw = at != null ? 0.0 : player.getYRot() + yawOffset, spread = at != null ? 180.0 : arc;
			int spawned = 0;
			for (int i = 0; i < count * 4 && spawned < count; i++) {
				double yaw = Math.toRadians(baseYaw + (random.nextDouble() * 2.0 - 1.0) * spread);
				double r = minR + random.nextDouble() * (maxR - minR);
				int x = (int) Math.floor(cx - Math.sin(yaw) * r), z = (int) Math.floor(cz + Math.cos(yaw) * r);
				BlockPos ground = groundAt(level, x, (int) Math.floor(cy), z);
				if (ground == null) {
					continue; // no host ground there (yet)
				}

				Entity e = type.get().spawn(level, ground, EntitySpawnReason.COMMAND);
				if (e instanceof Mob mob) {
					mob.setPersistenceRequired();
					spawned++;
				}
			}

			Passthrough.LOG.info("spawnmobs: {} x {}", spawned, kind);
		});
	}

	/** The free block above the host's ground near height y in column (x, z), or null. */
	private static BlockPos groundAt(final ServerLevel level, final int x, final int y, final int z) {
		BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
		for (int dy = 6; dy >= -12; dy--) {
			p.set(x, y + dy, z);
			if (!level.getBlockState(p).isAir() && level.getBlockState(p.above()).isAir() && level.getBlockState(p.above(2)).isAir()) {
				return p.above().immutable();
			}
		}

		return null;
	}

	/** Remove every fighting mob (not the player's other things). */
	public static void clearMobs() {
		MinecraftServer s = serverOrNull();
		if (s != null) {
			s.execute(() -> discardFighters(s.overworld()));
		}
	}

	/** Server thread. */
	static void discardFighters(final ServerLevel level) {
		List<Entity> all = new ArrayList<>();
		level.getAllEntities().forEach(all::add);
		all.stream().filter(e -> fighter(e) || isProxy(e)).forEach(Entity::discard);
	}

	/** The host went away: nobody left to hunt. */
	public static void hostGone() {
		MinecraftServer s = serverOrNull();
		if (s != null) {
			s.execute(MobWar::clearProxies);
		}
	}

	private static void clearProxies() {
		pendingHits.clear();
		proxies.values().forEach(Entity::discard);
		proxies.clear();
		proxyIds.clear();
		handleOf.clear();
		missingTicks.clear();
		boardAfter.clear();
	}

	static void detach(final MinecraftServer s) {
		// the world is saved next: none of the fight is kept in it
		discardFighters(s.overworld());
		proxies.clear();
		proxyIds.clear();
		handleOf.clear();
		missingTicks.clear();
		peds.set(null);
		damage.clear();
		fighterHits.clear();
		pendingHits.clear();
		angered.clear();
		reportedMobs = false;
	}

	private static MinecraftServer serverOrNull() {
		return WorldBridge.server();
	}
}
