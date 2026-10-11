package dev.halfcraft.passthrough;

import com.google.gson.JsonArray;
import java.util.Base64;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.phys.shapes.BitSetDiscreteVoxelShape;
import net.minecraft.world.phys.shapes.CubeVoxelShape;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.VoxelShape;

/**
 * The host's ground and walls as Minecraft collision, at 1/8 of a block. The host sends cells (blocks): a cell wholly
 * inside its geometry becomes a barrier block (WorldBridge: a real block, so building on it, redstone and pathfinding
 * work as usual); a cell partly inside (the top of a slope, a step, a railing, a ledge) becomes a shape of 1/8-block
 * cubes, merged into every block-collision query (BlockCollisionsMixin) and into mob pathfinding the way vanilla
 * treats slabs (WalkNodeEvaluatorMixin). Uneven ground no longer turns into 1-block steps, and floors over floors work.
 * The idea (shapes merged into block collision) comes from SkyCraft (github.com/chasmlol/SkyCraft, MIT).
 */
public final class HostGround {
	/** What a partial cell is to mobs' pathfinding (vanilla's counterparts in brackets). */
	public enum Kind {
		/** All of it in the lower half: floor to walk on (a carpet, a snow layer). */
		LOW,
		/** Covers most of the cell up high: stood on (a slab, stairs). */
		SOLID,
		/** Tall but sparse (rails, posts, a wall's edge): never stood or walked on (a fence). */
		FENCE
	}

	/** A cell: its shape, its height (0..1 of the cell) and kind. */
	public record Cell(VoxelShape shape, double top, Kind kind, boolean support) {
		/** A whole cell (its barrier is exactly its shape). */
		public boolean whole() {
			return this.shape == Shapes.block();
		}

		/** The mod puts a barrier here: a whole cell, or the ground's surface as support. */
		public boolean barrier() {
			return this.support || this.whole();
		}

		Cell withSupport(final boolean on) {
			return on == this.support ? this : new Cell(this.shape, this.top, this.kind, on);
		}
	}

	/** Server thread: the cells around a spot (BarrierOverrides: which barriers are the mod's). */
	public static void forEachNear(final BlockPos around, final int radius, final int below, final int above,
		final java.util.function.BiConsumer<BlockPos, Cell> action) {
		for (Map.Entry<Long, Cell> e : CELLS.entrySet()) {
			BlockPos p = BlockPos.of(e.getKey());
			if (Math.abs(p.getX() - around.getX()) <= radius && Math.abs(p.getZ() - around.getZ()) <= radius && p.getY() >= around.getY() - below
				&& p.getY() <= around.getY() + above) {
				action.accept(p, e.getValue());
			}
		}
	}

	/**
	 * A partial cell with one of our barriers in it (support for torches, redstone, water on a slope): collision and
	 * aim use the cell's exact shape instead of the barrier's whole block (BlockCollisionsMixin, ClipContextMixin).
	 */
	public static boolean exactInstead(final BlockState state, final Cell cell) {
		return cell != null && !cell.whole() && state.is(Blocks.BARRIER);
	}

	/**
	 * A whole cell: a barrier block, and this shape behind it, so the ground stays solid where the barrier is broken
	 * (creative) or replaced.
	 */
	private static final Cell WHOLE = new Cell(Shapes.block(), 1.0, Kind.SOLID, false);

	/** Cells by BlockPos.asLong (read by collision on any thread; written on the server thread). */
	private static final Map<Long, Cell> CELLS = new ConcurrentHashMap<>();

	private HostGround() {
	}

	/** The host's shape in this cell, or null. */
	public static VoxelShape shapeAt(final BlockPos pos) {
		Cell c = cellAt(pos);
		return c == null ? null : c.shape();
	}

	/** The host's partial cell here, or null. */
	public static Cell cellAt(final BlockPos pos) {
		return CELLS.isEmpty() ? null : CELLS.get(pos.asLong());
	}

	/** Ticks the player has been inside the host's ground in a row. */
	private static int stuckTicks;

	/**
	 * Server tick: a player inside the host's ground (put there before it streamed in, or it was scanned anew around
	 * him) is lifted to the nearest free spot above, up to 3 blocks. Movement can't take him out: collision only
	 * keeps things from going in.
	 */
	public static void unstuck(final net.minecraft.server.level.ServerPlayer player) {
		if (CELLS.isEmpty() || player.isSpectator() || player.isPassenger() || player.isSleeping() || player.level().dimension() != net.minecraft.world.level.Level.OVERWORLD) {
			stuckTicks = 0;
			return;
		}

		net.minecraft.world.phys.AABB box = player.getBoundingBox().deflate(0.02);
		if (!insideGround(box)) {
			stuckTicks = 0;
			return;
		}

		if (++stuckTicks < 3) {
			return;
		}

		stuckTicks = 0;
		for (double up = 0.125; up <= 3.0; up += 0.125) {
			net.minecraft.world.phys.AABB moved = box.move(0.0, up, 0.0);
			if (!insideGround(moved) && player.level().noCollision(player, moved)) {
				Passthrough.LOG.info("player was inside the host's ground at {}: lifted {} blocks", player.position(), up);
				player.teleportTo(player.getX(), player.getY() + up + 0.02, player.getZ());
				return;
			}
		}
	}

	private static boolean insideGround(final net.minecraft.world.phys.AABB box) {
		for (BlockPos pos : BlockPos.betweenClosed(BlockPos.containing(box.minX, box.minY, box.minZ), BlockPos.containing(box.maxX, box.maxY, box.maxZ))) {
			Cell cell = cellAt(pos);
			if (cell != null) {
				for (net.minecraft.world.phys.AABB a : cell.shape().move(pos).toAabbs()) {
					if (a.intersects(box)) {
						return true;
					}
				}
			}
		}

		return false;
	}

	public static int shapes() {
		return CELLS.size();
	}

	/**
	 * {"t":"vox","c":[[x,y,z,mask(,1)],...]} (WebSocket thread): mask "F" = full (a barrier), "" = nothing, else base64 of
	 * 64 bytes: 8 layers bottom-up, in each a byte per row (z) and a bit per column (x).
	 */
	public static void cells(final JsonArray list) {
		int n = list.size();
		int[] xyz = new int[n * 3];
		Cell[] shapes = new Cell[n]; // null with full false: nothing there
		boolean[] full = new boolean[n];
		boolean[] support = new boolean[n]; // a partial cell at the host's ground surface: a barrier too
		for (int i = 0; i < n; i++) {
			JsonArray c = list.get(i).getAsJsonArray();
			xyz[i * 3] = c.get(0).getAsInt();
			xyz[i * 3 + 1] = c.get(1).getAsInt();
			xyz[i * 3 + 2] = c.get(2).getAsInt();
			String mask = c.get(3).getAsString();
			support[i] = c.size() > 4 && c.get(4).getAsInt() == 1;
			if (mask.equals("F")) {
				full[i] = true;
			} else if (!mask.isEmpty()) {
				shapes[i] = cell(Base64.getDecoder().decode(mask));
				full[i] = shapes[i] == null && isFull(mask);
			}
		}

		MinecraftServer s = WorldBridge.server();
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			for (int i = 0; i < n; i++) {
				pos.set(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]);
				long key = pos.asLong();
				if (full[i]) {
					CELLS.put(key, WHOLE);
					WorldBridge.groundBarrier(level, pos, true);
				} else if (shapes[i] != null) {
					CELLS.put(key, shapes[i].withSupport(support[i])); // (first: collision takes the exact shape over the barrier from now on)
					// the ground's surface (a slope's cell, a step): a barrier too, so torches, redstone and water have a real
					// block to stand on there (the host decides: props on the ground, like carriages, get none)
					WorldBridge.groundBarrier(level, pos, support[i]);
				} else {
					WorldBridge.groundBarrier(level, pos, false);
					CELLS.remove(key);
				}

				level.getPathTypeCache().invalidate(pos); // (mobs' pathfinding caches what each cell is)
			}
		});
	}

	/** {"t":"clear"} (the barriers go in WorldBridge.clearSolid): every shape goes. */
	public static void clear() {
		MinecraftServer s = WorldBridge.server();
		if (s == null) {
			CELLS.clear();
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			for (Long key : CELLS.keySet()) {
				level.getPathTypeCache().invalidate(BlockPos.of(key));
			}

			CELLS.clear();
		});
	}

	/** The cell of a mask: null when it is empty or completely full (a full cell is a barrier instead). */
	private static Cell cell(final byte[] bits) {
		if (bits.length != 64) {
			return null;
		}

		BitSetDiscreteVoxelShape cubes = new BitSetDiscreteVoxelShape(8, 8, 8);
		int count = 0;
		int topLayer = -1;
		int upperColumns = 0; // of the 8x8 columns, how many have something in the upper half
		for (int y = 0; y < 8; y++) {
			for (int z = 0; z < 8; z++) {
				int row = bits[y * 8 + z] & 0xFF;
				for (int x = 0; x < 8; x++) {
					if ((row & (1 << x)) != 0) {
						cubes.fill(x, y, z);
						count++;
						topLayer = y;
					}
				}
			}
		}

		if (count == 0 || count == 512) {
			return null;
		}

		for (int z = 0; z < 8; z++) {
			int any = 0;
			for (int y = 4; y < 8; y++) {
				any |= bits[y * 8 + z] & 0xFF;
			}

			upperColumns += Integer.bitCount(any);
		}

		double top = (topLayer + 1) / 8.0;
		Kind kind = top <= 0.5 ? Kind.LOW : upperColumns >= 32 ? Kind.SOLID : Kind.FENCE;
		return new Cell(new CubeVoxelShape(cubes), top, kind, false);
	}

	private static boolean isFull(final String mask) {
		byte[] bits = Base64.getDecoder().decode(mask);
		if (bits.length != 64) {
			return false;
		}

		for (byte b : bits) {
			if (b != (byte) 0xFF) {
				return false;
			}
		}

		return true;
	}
}
