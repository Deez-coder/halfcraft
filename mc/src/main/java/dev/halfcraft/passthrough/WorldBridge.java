package dev.halfcraft.passthrough;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.FireworkRocketEntity;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

/**
 * Server-side half: commands, block changes and events back to the host. The host's ground is no blocks (HostGround);
 * older versions of the mod placed barrier blocks for it, and those left in a save go at the first fight of a session.
 */
public final class WorldBridge {
	private static volatile MinecraftServer server;
	/** Block changes this tick (server thread only): true = now solid, false = gone. Sent at the end of the tick. */
	private static final Map<BlockPos, Boolean> changes = new LinkedHashMap<>();
	/** While the Nether turns the host's ground: those changes aren't news to the host (server thread only). */
	private static boolean placingGround;
	/** The barriers an older version left in the save are gone (once a session). */
	private static boolean legacyCleaned;

	private WorldBridge() {
	}

	static void attach(final MinecraftServer s) {
		server = s;
	}

	static void detach() {
		server = null;
	}


	public static boolean ready() {
		return server != null;
	}

	static MinecraftServer server() {
		return server;
	}

	/** The host's ground was cleared (a new fight, a new map): the player hovers until it's in. */
	public static void clearSolid() {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			startHold(s);
			if (!legacyCleaned) {
				legacyCleaned = true;
				removeOldBarriers(s);
			}
		});
	}

	/**
	 * The barriers older versions of the mod placed for the host's ground: those the last of them recorded (wherever
	 * they are; the record goes too), and any in the loaded chunks around the player. Server thread.
	 */
	private static void removeOldBarriers(final MinecraftServer s) {
		ServerLevel level = s.overworld();
		placingGround = true;
		java.nio.file.Path record = s.getWorldPath(net.minecraft.world.level.storage.LevelResource.ROOT).resolve("passthrough-ground-barriers.txt");
		int recorded = 0;
		try {
			if (java.nio.file.Files.exists(record)) {
				for (String line : java.nio.file.Files.readAllLines(record)) {
					BlockPos p = line.isBlank() ? null : BlockPos.of(Long.parseLong(line.trim()));
					if (p != null && level.getBlockState(p).is(Blocks.BARRIER)) {
						level.setBlock(p, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
						recorded++;
					}
				}

				java.nio.file.Files.delete(record);
			}
		} catch (Exception e) {
			Passthrough.LOG.warn("ground: couldn't read the old barrier record: {}", e.toString());
		}

		Passthrough.LOG.info("ground: removed {} barriers an older version recorded", recorded);

		ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
		if (player != null) {
			// every loaded chunk within 16 of the player, every height; sections without a barrier are skipped
			// whole (the world is void: nearly all of them)
			BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
			int pcx = player.getBlockX() >> 4, pcz = player.getBlockZ() >> 4, removed = 0;
			for (int cx = pcx - 16; cx <= pcx + 16; cx++) {
				for (int cz = pcz - 16; cz <= pcz + 16; cz++) {
					net.minecraft.world.level.chunk.LevelChunk chunk = level.getChunkSource().getChunkNow(cx, cz);
					if (chunk == null) {
						continue;
					}

					net.minecraft.world.level.chunk.LevelChunkSection[] sections = chunk.getSections();
					for (int si = 0; si < sections.length; si++) {
						net.minecraft.world.level.chunk.LevelChunkSection section = sections[si];
						if (section.hasOnlyAir() || !section.getStates().maybeHas(st -> st.is(Blocks.BARRIER))) {
							continue;
						}

						int baseY = chunk.getSectionYFromSectionIndex(si) << 4;
						for (int x = 0; x < 16; x++) {
							for (int y = 0; y < 16; y++) {
								for (int z = 0; z < 16; z++) {
									if (section.getBlockState(x, y, z).is(Blocks.BARRIER)) {
										p.set((cx << 4) + x, baseY + y, (cz << 4) + z);
										level.setBlock(p, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
										removed++;
									}
								}
							}
						}
					}
				}
			}

			Passthrough.LOG.info("ground: removed {} other barriers left in the save (loaded chunks around the player)", removed);
		}
		placingGround = false;
	}

	/**
	 * /hs clean (server thread): every block within `radius` (horizontally) of `centre` that isn't the host's ground
	 * goes — the world is void, so that is everything built or left behind — and so do loose items, arrows, TNT and
	 * falling blocks. Then the host sends its ground again ({@link #restoreGround}). Returns the blocks removed.
	 */
	public static int clean(final ServerLevel level, final BlockPos centre, final int radius) {
		java.util.List<BlockPos> gone = new java.util.ArrayList<>();
		BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
		for (int cx = (centre.getX() - radius) >> 4; cx <= (centre.getX() + radius) >> 4; cx++) {
			for (int cz = (centre.getZ() - radius) >> 4; cz <= (centre.getZ() + radius) >> 4; cz++) {
				net.minecraft.world.level.chunk.LevelChunk chunk = level.getChunkSource().getChunkNow(cx, cz);
				if (chunk == null) {
					continue;
				}

				net.minecraft.world.level.chunk.LevelChunkSection[] sections = chunk.getSections();
				for (int i = 0; i < sections.length; i++) {
					if (sections[i].hasOnlyAir()) {
						continue;
					}

					int baseY = chunk.getSectionYFromSectionIndex(i) << 4;
					for (int y = 0; y < 16; y++) {
						for (int z = 0; z < 16; z++) {
							for (int x = 0; x < 16; x++) {
								p.set((cx << 4) + x, baseY + y, (cz << 4) + z);
								int dx = p.getX() - centre.getX(), dz = p.getZ() - centre.getZ();
								BlockState state = sections[i].getBlockState(x, y, z);
								if (dx * dx + dz * dz <= radius * radius && !state.isAir() && !Nether.isGround(p)) {
									gone.add(p.immutable());
								}
							}
						}
					}
				}
			}
		}

		for (BlockPos pos : gone) {
			level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
		}

		net.minecraft.world.phys.AABB box = new net.minecraft.world.phys.AABB(centre).inflate(radius, 512, radius);
		level.getEntities((Entity) null, box, e -> e instanceof net.minecraft.world.entity.item.ItemEntity || e instanceof AbstractArrow
			|| e instanceof net.minecraft.world.entity.item.PrimedTnt || e instanceof net.minecraft.world.entity.item.FallingBlockEntity
			|| e instanceof net.minecraft.world.entity.ExperienceOrb).forEach(Entity::discard);
		restoreGround();
		return gone.size();
	}

	/** Blocks the Willies are wearing down: how far each is gone (0..1) and the game time it was last hit. Server thread. */
	private static final Map<BlockPos, double[]> mining = new java.util.HashMap<>();

	/**
	 * {"t":"mine","c":[x,y,z],"v":m/s}: a Willie's weapon, fist or foot hit this block that hard. It wears down by that
	 * over its hardness - a good blow breaks dirt or glass, stone and planks take a few, obsidian dozens, bedrock never -
	 * with the cracks showing, and breaks with its drops. The wear goes after 5 s without a hit.
	 */
	public static void mine(final int x, final int y, final int z, final double speed) {
		MinecraftServer s = server;
		double strength = HsConfig.mining();
		if (s == null || strength <= 0.0) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			long now = level.getGameTime();
			mining.entrySet().removeIf(e -> {
				boolean old = now - (long) e.getValue()[1] > 100;
				if (old) {
					level.destroyBlockProgress(crackId(e.getKey()), e.getKey(), -1);
				}

				return old;
			});
			BlockPos pos = new BlockPos(x, y, z);
			BlockState state = level.getBlockState(pos);
			float hardness = state.getDestroySpeed(level, pos);
			if (state.isAir() || hardness < 0.0F || state.getBlock() instanceof net.minecraft.world.level.block.LiquidBlock) {
				return;
			}

			double[] wear = mining.computeIfAbsent(pos.immutable(), p -> new double[2]);
			wear[0] += speed * strength / (hardness * 8.0 + 1.0);
			wear[1] = now;
			if (wear[0] >= 1.0) {
				mining.remove(pos);
				level.destroyBlockProgress(crackId(pos), pos, -1);
				level.destroyBlock(pos, true);
			} else {
				level.destroyBlockProgress(crackId(pos), pos, (int) (wear[0] * 10.0));
			}
		});
	}

	/** The cracks' "breaker" for a block the Willies wear (never an entity's id: those are positive). */
	private static int crackId(final BlockPos pos) {
		return -1 - (pos.hashCode() & 0x3FFFFFFF);
	}

	/** The host scans its ground and sends it all again (/hs fixground, /hs clean). */
	public static void restoreGround() {
		Passthrough.events.accept("{\"t\":\"resample\"}");
	}

	/** Run a command as the server (op). Results go to the log, not to chat (send_command_feedback is off). */
	public static void command(final String command) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			Passthrough.LOG.info("command: {}", command);
			s.getCommands().performPrefixedCommand(s.createCommandSourceStack(), command);
			if (command.startsWith("tp @a")) {
				endHold(s, "put at the start"); // (the host's ground is there now)
			}
		});
	}

	/** Ticks left holding the player while the host's ground streams in (0: not holding). */
	private static int holdTicks;
	private static final int HOLD_MAX_TICKS = 400;

	/**
	 * The host's ground was cleared (a new fight, a new map): until the host puts the player at the start, he hovers
	 * where he is. He used to fall through the void meanwhile, and land at the start with all that fall in him (dead).
	 * Server thread.
	 */
	private static void startHold(final MinecraftServer s) {
		if (s.getPlayerList().getPlayers().isEmpty()) {
			return;
		}

		holdTicks = HOLD_MAX_TICKS;
		Passthrough.LOG.info("holding the player until the host's ground is in");
	}

	/** {"t":"hover","on":..}: nothing under the player in the host's world yet (its map loading there): held up till there is. */
	public static void hover(final boolean on) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			if (on) {
				startHold(s);
			} else {
				endHold(s, "the host's ground is under him now");
			}
		});
	}

	private static void endHold(final MinecraftServer s, final String why) {
		if (holdTicks <= 0) {
			return;
		}

		holdTicks = 0;
		for (ServerPlayer p : s.getPlayerList().getPlayers()) {
			p.resetFallDistance();
			p.gameMode.getGameModeForPlayer().updatePlayerAbilities(p.getAbilities()); // (flying as its game mode has it)
			p.getAbilities().flying = p.getAbilities().flying && p.getAbilities().mayfly;
			p.onUpdateAbilities();
		}

		Passthrough.LOG.info("player let go: {}", why);
	}

	/** Each tick while holding: hovering, no fall building up (game mode commands reset flight: set again). */
	private static void hold(final MinecraftServer s) {
		if (holdTicks <= 0) {
			return;
		}

		if (--holdTicks == 0) {
			holdTicks = 1;
			endHold(s, "the host never put him at the start");
			return;
		}

		for (ServerPlayer p : s.getPlayerList().getPlayers()) {
			p.resetFallDistance();
			if (!p.getAbilities().flying || !p.getAbilities().mayfly) {
				p.getAbilities().mayfly = true;
				p.getAbilities().flying = true;
				p.onUpdateAbilities();
			}
		}
	}

	private static boolean solidForHost(final ServerLevel level, final BlockPos pos, final BlockState state) {
		// the Nether's ground is the host's own ground turned: nothing to collide with that isn't there already
		return !state.isAir() && !state.getCollisionShape(level, pos).isEmpty() && !Nether.isGround(pos);
	}

	/** Arrows the host already hit something with (they stay where they hit and aren't reported again). */
	private static final String HIT_TAG = "passthrough_hit";

	/** Every server tick: block changes, and projectiles in flight for the host to trace through its own world. */
	static void tick(final MinecraftServer s) {
		flush(s);
		hold(s);
		if (Passthrough.active) {
			reportProjectiles(s.overworld());
			s.getPlayerList().getPlayers().forEach(HostGround::unstuck);
		}

		MobWar.tick(s);
		Nether.tick(s);
	}

	/**
	 * Steve's arrows (bow, crossbow) and crossbow fireworks in flight: {"t":"proj","p":[[id,kind,x,y,z],...]}. Mobs'
	 * arrows aren't traced by the host: they hit its people's proxies here.
	 */
	private static void reportProjectiles(final ServerLevel level) {
		StringBuilder b = null;
		for (Entity e : level.getAllEntities()) {
			String kind = null;
			if (!(e instanceof Projectile projectile) || !(projectile.getOwner() instanceof Player)) {
				continue;
			}

			if (e instanceof AbstractArrow arrow && !arrow.entityTags().contains(HIT_TAG) && arrow.getDeltaMovement().lengthSqr() > 1.0E-4) {
				kind = "arrow";
			} else if (e instanceof FireworkRocketEntity rocket && rocket.isShotAtAngle()) {
				kind = "firework"; // not the ones boosting an elytra flight
			}

			if (kind != null) {
				b = b == null ? new StringBuilder("{\"t\":\"proj\",\"p\":[") : b.append(',');
				b.append(String.format(Locale.ROOT, "[%d,\"%s\",%.3f,%.3f,%.3f]", e.getId(), kind, e.getX(), e.getY(), e.getZ()));
			}
		}

		if (b != null) {
			Passthrough.events.accept(b.append("]}").toString());
		}
	}

	/**
	 * The host traced a projectile into something of its own: a firework bursts there; an arrow goes into a person
	 * or car (gone) or sticks where it hit a wall.
	 */
	public static void projectileHit(final int id, final double x, final double y, final double z, final boolean stick) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			Entity e = level.getEntity(id);
			if (e instanceof FireworkRocketEntity) {
				e.setPos(x, y, z);
				level.broadcastEntityEvent(e, (byte) 17);
				e.discard();
			} else if (e instanceof AbstractArrow) {
				if (stick) {
					e.setPos(x, y, z);
					e.setDeltaMovement(Vec3.ZERO);
					e.setNoGravity(true);
					e.addTag(HIT_TAG);
				} else {
					e.discard();
				}
			}
		});
	}

	/** Server thread, from Level.setBlock: remember the change; flushed once per tick. */
	public static void onBlockChanged(final ServerLevel level, final BlockPos pos, final BlockState state) {
		if (!Passthrough.active || level != level.getServer().overworld()) {
			return;
		}

		Nether.onBlockChanged(pos, state);
		if (!placingGround) {
			changes.put(pos.immutable(), solidForHost(level, pos, state));
		}
	}

	/** While on, block changes are the host's own ground being edited (not reported as blocks to collide with). */
	static void quietGround(final boolean on) {
		placingGround = on;
	}

	/** End of each server tick: {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]}. */
	static void flush(final MinecraftServer s) {
		if (changes.isEmpty()) {
			return;
		}

		StringBuilder set = new StringBuilder();
		StringBuilder clear = new StringBuilder();
		for (Map.Entry<BlockPos, Boolean> e : changes.entrySet()) {
			StringBuilder b = e.getValue() ? set : clear;
			BlockPos p = e.getKey();
			b.append(b.isEmpty() ? "" : ",").append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ());
		}

		changes.clear();
		Passthrough.events.accept("{\"t\":\"blocks\",\"set\":[" + set + "],\"clear\":[" + clear + "]}");
	}

	/** Every solid block within `radius` of the player, as one "blocks" message (the host's props start from this). */
	public static void sync(final int radius) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			if (player == null) {
				return;
			}

			BlockPos c = player.blockPosition();
			BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
			int found = 0;
			for (int x = -radius; x <= radius && found < 3000; x++) {
				for (int z = -radius; z <= radius && found < 3000; z++) {
					for (int y = -24; y <= 40; y++) {
						p.set(c.getX() + x, c.getY() + y, c.getZ() + z);
						if (!level.isInWorldBounds(p) || !level.isLoaded(p)) {
							continue;
						}

						BlockState state = level.getBlockState(p);
						if (solidForHost(level, p, state)) {
							changes.put(p.immutable(), true);
							found++;
						}
					}
				}
			}

			flush(s);
		});
	}

	/** Elytra on and gliding (creative flight off), launched forward along the look; or back to creative flight. */
	public static void glide(final boolean start, final double speed) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			if (s.getPlayerList().getPlayers().isEmpty()) {
				return;
			}

			ServerPlayer player = s.getPlayerList().getPlayers().get(0);
			if (start) {
				player.setItemSlot(EquipmentSlot.CHEST, new ItemStack(Items.ELYTRA));
				player.getAbilities().flying = false;
				player.onUpdateAbilities();
				player.setOnGround(false);
				player.startFallFlying();
				player.setDeltaMovement(player.getLookAngle().scale(speed));
				player.needsSync = true;
			} else {
				player.stopFallFlying();
				player.setItemSlot(EquipmentSlot.CHEST, ItemStack.EMPTY);
				player.getAbilities().flying = true;
				player.onUpdateAbilities();
			}
		});
	}

	/**
	 * A Half Sword fighter hit the player (the host's hidden stand-in for Steve): Minecraft damage, from the fighter's
	 * direction (`from`, or null), so armour, the shield, knockback and death all work as Minecraft's own.
	 */
	public static void hurtPlayer(final float amount, final double[] from) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			if (s.getPlayerList().getPlayers().isEmpty()) {
				return;
			}

			ServerPlayer player = s.getPlayerList().getPlayers().get(0);
			ServerLevel level = player.level();
			LivingEntity attacker = from == null ? null : MobWar.proxyNear(level, from[0], from[1], from[2]);
			DamageSource source = attacker != null ? level.damageSources().mobAttack(attacker) : level.damageSources().generic();
			// Minecraft's own hurt cooldown applies: a blade touching Steve for several frames is one hit (only a harder
			// contact within it adds the difference), with one knockback
			player.hurtServer(level, source, amount);
		});
	}

	/** `source`: what exploded or was blown up, e.g. "tnt", "creeper", "fireball" (a ghast's). */
	public static void onExplosion(final Vec3 center, final float radius, final String source) {
		if (Passthrough.active) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"explosion\",\"pos\":[%.3f,%.3f,%.3f],\"r\":%.2f,\"src\":\"%s\"}",
				center.x, center.y, center.z, radius, source));
		}
	}
}
