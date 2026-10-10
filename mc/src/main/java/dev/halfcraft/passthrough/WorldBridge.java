package dev.halfcraft.passthrough;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
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

/** Server-side half: the host's collision as invisible barrier blocks, commands, and events back to the host. */
public final class WorldBridge {
	private static volatile MinecraftServer server;
	/** Barriers we placed (so a reset only removes ours, never the player's builds). */
	private static final Set<BlockPos> barriers = ConcurrentHashMap.newKeySet();
	/** Block changes this tick (server thread only): true = now solid, false = gone. Sent at the end of the tick. */
	private static final Map<BlockPos, Boolean> changes = new LinkedHashMap<>();
	/** While placing or removing the host's own ground: those changes aren't news to the host (server thread only). */
	private static boolean placingGround;
	/** The save's leftover barriers were removed this session (WorldBridge.clearSolid). */
	private static boolean legacyCleaned;
	/** Ground barriers placed and removed since the last report (the log: are they churning?). */
	private static int barriersPlaced, barriersRemoved, reportTicks;
	/** The barrier set changed since it was last written to the world's record. */
	private static boolean recordDirty;

	private WorldBridge() {
	}

	static void attach(final MinecraftServer s) {
		server = s;
	}

	static void detach() {
		server = null;
		barriers.clear();
	}

	public static boolean ready() {
		return server != null;
	}

	static MinecraftServer server() {
		return server;
	}

	/** Columns of solid ground from the host: {x, z, yBottom, yTop, ...} in block coordinates (inclusive). Only air is replaced. */
	public static void solid(final int[] columns) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			BlockState barrier = Blocks.BARRIER.defaultBlockState();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			placingGround = true;
			for (int i = 0; i + 3 < columns.length; i += 4) {
				for (int y = columns[i + 2]; y <= columns[i + 3]; y++) {
					pos.set(columns[i], y, columns[i + 1]);
					if (level.isInWorldBounds(pos) && level.getBlockState(pos).isAir()) {
						level.setBlock(pos, barrier, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
						barriers.add(pos.immutable());
					}
				}
			}

			placingGround = false;
		});
	}

	/** Server thread: one of the host's full ground cells (HostGround) as a barrier (only into air), or not any more. */
	static void groundBarrier(final ServerLevel level, final BlockPos pos, final boolean on) {
		placingGround = true;
		if (on) {
			if (level.isInWorldBounds(pos) && level.getBlockState(pos).isAir() && !BarrierOverrides.suppressed(pos)) {
				level.setBlock(pos, Blocks.BARRIER.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
				barriers.add(pos.immutable());
				barriersPlaced++;
				recordDirty = true;
			}
		} else if (barriers.remove(pos) && level.getBlockState(pos).is(Blocks.BARRIER)) {
			level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
			barriersRemoved++;
			recordDirty = true;
		}

		placingGround = false;
	}

	/** The ground barriers the mod placed (server thread). */
	static Set<BlockPos> groundBarriers() {
		return barriers;
	}

	/** A ground barrier the player removed for good (BarrierOverrides): no longer the mod's. */
	static void forgetBarrier(final BlockPos pos) {
		barriers.remove(pos);
	}

	/** Remove every barrier we placed (e.g. when the host teleports somewhere else). */
	public static void clearSolid() {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			placingGround = true;
			for (BlockPos pos : barriers) {
				if (level.getBlockState(pos).is(Blocks.BARRIER)) {
					level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
				}
			}

			// once per session: barriers left in the save by the last session (its record: wherever they are), and by an
			// older version of the mod (around the player)
			if (!legacyCleaned) {
				int recorded = 0;
				for (long key : readRecord(s)) {
					BlockPos p = BlockPos.of(key);
					if (level.getBlockState(p).is(Blocks.BARRIER)) {
						level.setBlock(p, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
						recorded++;
					}
				}

				Passthrough.LOG.info("ground: removed {} barriers the last session left (its record)", recorded);
				legacyCleaned = true;
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
			}

			placingGround = false;
			barriers.clear();
			recordDirty = true;
		});
	}

	/** The world's record of the ground barriers placed (so the next session can remove them wherever they are). */
	private static java.nio.file.Path recordFile(final MinecraftServer s) {
		return s.getWorldPath(net.minecraft.world.level.storage.LevelResource.ROOT).resolve("passthrough-ground-barriers.txt");
	}

	private static java.util.List<Long> readRecord(final MinecraftServer s) {
		java.util.List<Long> out = new java.util.ArrayList<>();
		try {
			java.nio.file.Path f = recordFile(s);
			if (java.nio.file.Files.exists(f)) {
				for (String line : java.nio.file.Files.readAllLines(f)) {
					if (!line.isBlank()) {
						out.add(Long.parseLong(line.trim()));
					}
				}
			}
		} catch (Exception e) {
			Passthrough.LOG.warn("ground: couldn't read the barrier record: {}", e.toString());
		}

		return out;
	}

	/** Server thread: write the record (when it changed). */
	static void writeRecord(final MinecraftServer s) {
		if (!recordDirty) {
			return;
		}

		recordDirty = false;
		StringBuilder b = new StringBuilder();
		for (BlockPos p : barriers) {
			b.append(p.asLong()).append(System.lineSeparator());
		}

		try {
			java.nio.file.Files.writeString(recordFile(s), b);
		} catch (Exception e) {
			Passthrough.LOG.warn("ground: couldn't write the barrier record: {}", e.toString());
		}
	}

	/**
	 * /hs clean (server thread): every block within `radius` (horizontally) of `centre` that isn't the host's ground
	 * goes — the world is void, so that is everything built or left behind — and so do loose items, arrows, TNT and
	 * falling blocks. Then the ground comes back whole ({@link #restoreGround}). Returns the blocks removed.
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
								if (dx * dx + dz * dz <= radius * radius && !state.isAir() && !state.is(Blocks.BARRIER) && !Nether.isGround(p)) {
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
		restoreGround(level);
		return gone.size();
	}

	/**
	 * Barriers of the host's ground that were broken come back (server thread), and the host sends its ground again
	 * for the places that never had one (a block was there first). Returns the barriers put back.
	 */
	public static int restoreGround(final ServerLevel level) {
		int back = 0;
		placingGround = true;
		for (BlockPos pos : barriers) {
			if (level.getBlockState(pos).isAir() && !BarrierOverrides.suppressed(pos)) {
				level.setBlock(pos, Blocks.BARRIER.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
				back++;
			}
		}

		placingGround = false;
		Passthrough.events.accept("{\"t\":\"resample\"}");
		return back;
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
		});
	}

	private static boolean solidForHost(final ServerLevel level, final BlockPos pos, final BlockState state) {
		// the Nether's ground is the host's own ground turned: nothing to collide with that isn't there already
		return !state.isAir() && !state.is(Blocks.BARRIER) && !state.getCollisionShape(level, pos).isEmpty() && !Nether.isGround(pos);
	}

	/** Arrows the host already hit something with (they stay where they hit and aren't reported again). */
	private static final String HIT_TAG = "passthrough_hit";

	/** Every server tick: block changes, and projectiles in flight for the host to trace through its own world. */
	static void tick(final MinecraftServer s) {
		flush(s);
		if (++reportTicks >= 100) {
			reportTicks = 0;
			if (barriersPlaced > 0 || barriersRemoved > 0) {
				Passthrough.LOG.info("ground barriers: {} placed, {} removed in 5 s ({} now)", barriersPlaced, barriersRemoved, barriers.size());
				barriersPlaced = barriersRemoved = 0;
			}

			if (legacyCleaned) {
				writeRecord(s); // (every 5 s at most: a crash then leaves little behind)
			}
		}
		if (Passthrough.active) {
			reportProjectiles(s.overworld());
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
