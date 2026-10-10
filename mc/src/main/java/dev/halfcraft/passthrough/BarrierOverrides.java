package dev.halfcraft.passthrough;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;

/**
 * The player's own changes to the host's ground barriers, per map (/hs barriers): barriers they broke stay gone (the
 * ground keeps its collision: HostGround), barriers they placed are put back whenever that map is played. Spots are kept
 * in the map's own heights (Minecraft y minus the host's offset for that fight), so they land in the same place whatever
 * the ground's levelling came out as. Saved in config/passthrough-barriers.json: {map: {"add": [[x, y, z], ...],
 * "remove": [...]}}.
 */
public final class BarrierOverrides {
	private static final Path FILE = FabricLoader.getInstance().getConfigDir().resolve("passthrough-barriers.json");
	private static final int SCAN_RADIUS = 64;
	private static final int SCAN_BELOW = 24;
	private static final int SCAN_ABOVE = 24;

	private static JsonObject data;
	/** The map being played and its offset (server thread). */
	private static String map;
	private static double offset;
	/** This map's "never place a barrier here", as BlockPos.asLong at the current offset (read on the server thread). */
	private static final Set<Long> removed = new HashSet<>();

	private BarrierOverrides() {
	}

	/** {"t":"map","name":..,"off":..} (WebSocket thread; comes right after the ground's "clear"). */
	public static void onMap(final String name, final double off) {
		MinecraftServer s = WorldBridge.server();
		if (s == null) {
			return;
		}

		s.execute(() -> {
			map = name;
			offset = off;
			load();
			removed.clear();
			JsonObject entry = entry(false);
			if (entry != null) {
				for (BlockPos p : spots(entry, "remove")) {
					removed.add(p.asLong());
				}
			}

			int placed = placeAdded(s.overworld());
			Passthrough.LOG.info("barriers: map {} (offset {}): {} of your barriers placed, {} spots kept clear", name, off, placed, removed.size());
		});
	}

	/** Server thread: the ground must not get a barrier here (the player removed it on this map). */
	public static boolean suppressed(final BlockPos pos) {
		return !removed.isEmpty() && removed.contains(pos.asLong());
	}

	public static String map() {
		return map;
	}

	/** The saved changes for the current map: {added, removed}. */
	public static int[] counts() {
		JsonObject entry = map == null ? null : entry(false);
		return entry == null ? new int[] {0, 0} : new int[] {entry.getAsJsonArray("add").size(), entry.getAsJsonArray("remove").size()};
	}

	/**
	 * /hs barriers save (server thread): barriers the mod placed that are gone now are remembered as removed, barriers
	 * that the mod didn't place are remembered as added (around the player). Returns {added, removed} newly saved, or
	 * null when there is no map yet.
	 */
	public static int[] save(final ServerLevel level, final BlockPos around) {
		if (map == null) {
			return null;
		}

		load();
		JsonObject entry = entry(true);
		List<BlockPos> add = spots(entry, "add");
		List<BlockPos> remove = spots(entry, "remove");
		int newAdd = 0, newRemove = 0;
		// saved changes the player undid since
		add.removeIf(p -> !level.getBlockState(p).is(Blocks.BARRIER));
		remove.removeIf(p -> {
			if (level.getBlockState(p).is(Blocks.BARRIER)) {
				removed.remove(p.asLong());
				return true;
			}

			return false;
		});
		// which barriers are the mod's: the host's cells say (a whole cell, or the ground's surface as support) - not
		// what this session happened to place (a game restarted on its own left that list behind)
		final int[] gone = {0};
		final List<BlockPos> remove2 = remove;
		HostGround.forEachNear(around, SCAN_RADIUS, SCAN_BELOW, SCAN_ABOVE, (pos, cell) -> {
			if (cell.barrier() && !level.getBlockState(pos).is(Blocks.BARRIER) && level.getBlockState(pos).isAir() && !remove2.contains(pos)) {
				remove2.add(pos);
				removed.add(pos.asLong());
				WorldBridge.forgetBarrier(pos);
				gone[0]++;
			}
		});
		newRemove = gone[0];

		// barriers around the player where the mod puts none
		Set<BlockPos> already = new HashSet<>(add);
		int seen = 0;
		BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
		for (int x = around.getX() - SCAN_RADIUS; x <= around.getX() + SCAN_RADIUS; x++) {
			for (int z = around.getZ() - SCAN_RADIUS; z <= around.getZ() + SCAN_RADIUS; z++) {
				for (int y = around.getY() - SCAN_BELOW; y <= around.getY() + SCAN_ABOVE; y++) {
					p.set(x, y, z);
					if (!level.getBlockState(p).is(Blocks.BARRIER)) {
						continue;
					}

					seen++;
					HostGround.Cell cell = HostGround.cellAt(p);
					boolean mods = (cell != null && cell.barrier()) || WorldBridge.groundBarriers().contains(p);
					if (!mods && !already.contains(p)) {
						BlockPos at = p.immutable();
						add.add(at);
						already.add(at);
						newAdd++;
					}
				}
			}
		}

		Passthrough.LOG.info("barriers: save on {}: {} barriers around, {} host cells known; {} new added, {} new removed", map, seen, HostGround.shapes(), newAdd,
			newRemove);
		entry.add("add", toJson(add));
		entry.add("remove", toJson(remove));
		write();
		return new int[] {newAdd, newRemove};
	}

	/**
	 * /hs barriers reset (server thread): this map's changes are forgotten; the barriers the player added go, and the
	 * host sends the ground again (its barriers come back). Returns the barriers removed, -1 when there is no map yet.
	 */
	public static int reset(final ServerLevel level) {
		if (map == null) {
			return -1;
		}

		load();
		JsonObject entry = entry(false);
		int gone = 0;
		if (entry != null) {
			for (BlockPos p : spots(entry, "add")) {
				if (level.getBlockState(p).is(Blocks.BARRIER)) {
					level.setBlock(p, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
					gone++;
				}
			}

			data.remove(map);
			write();
		}

		removed.clear();
		WorldBridge.restoreGround(level); // (puts the mod's barriers back and asks the host for the ground again)
		return gone;
	}

	/** The player's added barriers of this map into the world (only into air). */
	private static int placeAdded(final ServerLevel level) {
		JsonObject entry = entry(false);
		if (entry == null) {
			return 0;
		}

		int placed = 0;
		for (BlockPos p : spots(entry, "add")) {
			if (level.isInWorldBounds(p) && level.getBlockState(p).isAir()) {
				level.setBlock(p, Blocks.BARRIER.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
				placed++;
			}
		}

		return placed;
	}

	private static JsonObject entry(final boolean create) {
		if (data == null || map == null) {
			return null;
		}

		if (!data.has(map)) {
			if (!create) {
				return null;
			}

			JsonObject e = new JsonObject();
			e.add("add", new JsonArray());
			e.add("remove", new JsonArray());
			data.add(map, e);
		}

		return data.getAsJsonObject(map);
	}

	/** Spots in the map's heights -> Minecraft cells at the current offset. */
	private static List<BlockPos> spots(final JsonObject entry, final String key) {
		List<BlockPos> out = new ArrayList<>();
		if (!entry.has(key)) {
			return out;
		}

		for (JsonElement e : entry.getAsJsonArray(key)) {
			JsonArray a = e.getAsJsonArray();
			out.add(new BlockPos(a.get(0).getAsInt(), (int) Math.floor(a.get(1).getAsDouble() + offset), a.get(2).getAsInt()));
		}

		return out;
	}

	/** Minecraft cells -> spots in the map's heights (the cell's middle, minus the current offset). */
	private static JsonArray toJson(final List<BlockPos> list) {
		JsonArray out = new JsonArray();
		for (BlockPos p : list) {
			JsonArray a = new JsonArray();
			a.add(p.getX());
			a.add(Math.round((p.getY() + 0.5 - offset) * 1000.0) / 1000.0);
			a.add(p.getZ());
			out.add(a);
		}

		return out;
	}

	private static void load() {
		if (data != null) {
			return;
		}

		data = new JsonObject();
		try {
			if (Files.exists(FILE)) {
				data = JsonParser.parseString(Files.readString(FILE)).getAsJsonObject();
			}
		} catch (Exception e) {
			Passthrough.LOG.warn("barriers: couldn't read {}: {}", FILE, e.toString());
		}
	}

	private static void write() {
		try {
			Files.createDirectories(FILE.getParent());
			Files.writeString(FILE, new GsonBuilder().setPrettyPrinting().create().toJson(data));
		} catch (Exception e) {
			Passthrough.LOG.warn("barriers: couldn't save {}: {}", FILE, e.toString());
		}
	}
}
