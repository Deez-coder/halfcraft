package dev.halfcraft.passthrough;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;
import com.mojang.brigadier.CommandDispatcher;
import com.mojang.brigadier.arguments.DoubleArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.context.CommandContext;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.commands.SharedSuggestionProvider;
import net.minecraft.network.chat.Component;

/**
 * Half Sword damage tuning, changed in game with /hs and kept in config/passthrough-hs.json:
 * <ul>
 * <li>/hs dealt &lt;x&gt;: everything Steve (and his mobs, TNT...) does to the fighters</li>
 * <li>/hs taken &lt;x&gt;: everything the fighters do to Steve</li>
 * <li>/hs potions &lt;x&gt;: what potions (harming, poison, wither) do to the fighters; dealt doesn't apply to them</li>
 * <li>/hs part &lt;head|neck|torso|arms|legs&gt; &lt;x&gt;: hits on that body part, both ways</li>
 * <li>/hs weapon &lt;item id&gt; &lt;x&gt;: one Minecraft item's hits (e.g. diamond_sword)</li>
 * <li>/hs help (or just /hs): what every command does</li>
 * <li>/hs show; /hs reset: every setting back to its default</li>
 * <li>/hs clean [radius]: remove every placed block (default within 64), loose items, arrows, TNT; ground restored</li>
 * <li>/hs fixground: put back broken barriers of the host's ground</li>
 * <li>/hs fps &lt;n&gt;: Half Sword's frame cap (0 = none): what it leaves of the GPU goes to Minecraft</li>
 * <li>/hs kit save|give|reset|auto on|off: your inventory as the kit you get at the start of every Half Sword fight</li>
 * <li>/hs hostile on|off: neutral mobs (wolves, golems, bees, endermen...) attack the fighters and Steve on sight</li>
 * <li>/hs balance on|off: each game no faster than a little above the other (the GPU goes to frames that are shown)</li>
 * <li>/hs export sync|async: hand Minecraft's frames over as soon as the GPU has them (sync), or Minecraft's own way</li>
 * </ul>
 * The host applies dealt/taken/part (it knows where a hit lands); weapon multipliers apply here, to the hit's damage.
 */
public final class HsConfig {
	public static final List<String> PARTS = List.of("head", "neck", "torso", "arms", "legs");
	private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();
	private static final Path FILE = FabricLoader.getInstance().getConfigDir().resolve("passthrough-hs.json");

	private static final class Data {
		double dealt = 1.0;
		double taken = 1.0;
		double potions = 1.0;
		Map<String, Double> parts = new LinkedHashMap<>();
		Map<String, Double> weapons = new LinkedHashMap<>();
		int fps = 0;
		boolean syncExport = true;
		boolean balance = true;
		boolean hostile = false;
	}

	private static Data data = load();

	static {
		Passthrough.syncExport = data.syncExport;
		Passthrough.balance = data.balance;
		MobWar.forceHostile = data.hostile;
	}

	private HsConfig() {
	}

	private static Data load() {
		try {
			if (Files.exists(FILE)) {
				Data d = GSON.fromJson(Files.readString(FILE), Data.class);
				if (d != null) {
					if (d.parts == null) {
						d.parts = new LinkedHashMap<>();
					}

					if (d.weapons == null) {
						d.weapons = new LinkedHashMap<>();
					}

					return d;
				}
			}
		} catch (IOException | RuntimeException e) {
			Passthrough.LOG.warn("couldn't read {}: {}", FILE, e.toString());
		}

		return new Data();
	}

	private static void save() {
		try {
			Files.createDirectories(FILE.getParent());
			Files.writeString(FILE, GSON.toJson(data));
		} catch (IOException e) {
			Passthrough.LOG.warn("couldn't write {}: {}", FILE, e.toString());
		}
	}

	/** The multiplier for one Minecraft item's hits (1 if none set). */
	public static double weapon(final String item) {
		return data.weapons.getOrDefault(item, 1.0);
	}

	/** /hs help: every command, one line each. */
	private static final String[][] HELP = {
		{"/hs show", "see all your settings"},
		{"/hs reset", "put every setting back to default"},
		{"/hs dealt <x>", "how much damage you do to fighters (1 = normal, 2 = double)"},
		{"/hs taken <x>", "how much damage fighters do to you"},
		{"/hs potions <x>", "how much damage harming, poison and wither do to fighters"},
		{"/hs part <head|neck|torso|arms|legs> <x>", "damage to one body part, both ways"},
		{"/hs weapon <item> <x>", "damage of one item, e.g. /hs weapon diamond_sword 1.5"},
		{"/hs kit save", "your current inventory becomes the kit you get every fight"},
		{"/hs kit give", "get your kit now"},
		{"/hs kit reset", "go back to the built-in kit"},
		{"/hs kit auto on|off", "get the kit at the start of every fight, or keep what you carry"},
		{"/hs hostile on|off", "wolves, golems, bees and other neutral mobs attack on sight"},
		{"/hs balance on|off", "keeps both games' fps matched: smoother, less lag (on by default)"},
		{"/hs fps <n>", "cap Half Sword's fps (0 = no cap)"},
		{"/hs clean [radius]", "remove everything you built nearby (blocks, items, arrows, TNT)"},
		{"/hs fixground", "put back the invisible ground if you broke it"},
		{"/hs barriers", "which map this is and how many barrier changes you saved for it"},
		{"/hs barriers save", "remember the barriers you broke or placed around you, for this map, every time"},
		{"/hs barriers reset", "forget this map's barrier changes: the barriers go back to what the mod makes"},
		{"/hs export sync|async", "how Minecraft's picture is sent (sync = less lag, the default)"},
	};

	private static int help(final CommandContext<CommandSourceStack> ctx) {
		ctx.getSource().sendSystemMessage(Component.literal("HalfCraft commands:").withStyle(net.minecraft.ChatFormatting.YELLOW));
		for (String[] line : HELP) {
			ctx.getSource().sendSystemMessage(Component.literal(line[0]).withStyle(net.minecraft.ChatFormatting.GOLD)
				.append(Component.literal(" - " + line[1]).withStyle(net.minecraft.ChatFormatting.GRAY)));
		}
		return 1;
	}

	/** Tell the host the multipliers it applies: {"t":"cfg","dealt":x,"taken":x,"parts":{...}}. */
	public static void send() {
		JsonObject m = new JsonObject();
		m.addProperty("t", "cfg");
		m.addProperty("dealt", data.dealt);
		m.addProperty("taken", data.taken);
		m.addProperty("potions", data.potions);
		for (String p : PARTS) {
			m.addProperty(p, data.parts.getOrDefault(p, 1.0));
		}

		m.addProperty("fps", data.fps);
		m.addProperty("balance", data.balance ? 1 : 0);
		Passthrough.events.accept(m.toString());
	}

	private static int changed(final CommandContext<CommandSourceStack> ctx, final String what) {
		save();
		send();
		ctx.getSource().sendSystemMessage(Component.literal("Half Sword: " + what));
		return 1;
	}

	public static void register(final CommandDispatcher<CommandSourceStack> dispatcher) {
		dispatcher.register(Commands.literal("hs")
			.executes(HsConfig::help)
			.then(Commands.literal("help").executes(HsConfig::help))
			.then(Commands.literal("dealt").then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
				data.dealt = DoubleArgumentType.getDouble(ctx, "multiplier");
				return changed(ctx, String.format(Locale.ROOT, "damage dealt x%.2f", data.dealt));
			})))
			.then(Commands.literal("potions").then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
				data.potions = DoubleArgumentType.getDouble(ctx, "multiplier");
				return changed(ctx, String.format(Locale.ROOT, "potion damage x%.2f", data.potions));
			})))
			.then(Commands.literal("taken").then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
				data.taken = DoubleArgumentType.getDouble(ctx, "multiplier");
				return changed(ctx, String.format(Locale.ROOT, "damage taken x%.2f", data.taken));
			})))
			.then(Commands.literal("part").then(Commands.argument("part", StringArgumentType.word())
				.suggests((ctx, b) -> SharedSuggestionProvider.suggest(PARTS, b))
				.then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
					String part = StringArgumentType.getString(ctx, "part");
					if (!PARTS.contains(part)) {
						ctx.getSource().sendFailure(Component.literal("parts: " + String.join(", ", PARTS)));
						return 0;
					}

					data.parts.put(part, DoubleArgumentType.getDouble(ctx, "multiplier"));
					return changed(ctx, String.format(Locale.ROOT, "%s hits x%.2f", part, data.parts.get(part)));
				}))))
			.then(Commands.literal("weapon").then(Commands.argument("item", StringArgumentType.word())
				.then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
					String item = StringArgumentType.getString(ctx, "item").replace("minecraft:", "");
					data.weapons.put(item, DoubleArgumentType.getDouble(ctx, "multiplier"));
					return changed(ctx, String.format(Locale.ROOT, "%s hits x%.2f", item, data.weapons.get(item)));
				}))))
			.then(Commands.literal("fps").then(Commands.argument("cap", com.mojang.brigadier.arguments.IntegerArgumentType.integer(0, 300)).executes(ctx -> {
				data.fps = com.mojang.brigadier.arguments.IntegerArgumentType.getInteger(ctx, "cap");
				return changed(ctx, data.fps == 0 ? "no frame cap" : "frame cap " + data.fps + " fps");
			})))
			.then(Commands.literal("kit")
				.then(Commands.literal("save").executes(ctx -> {
					int n = Kit.save(ctx.getSource().getPlayerOrException());
					ctx.getSource().sendSystemMessage(Component.literal("Half Sword: your inventory (" + n + " stacks) is the kit now"
						+ (Kit.auto ? ", given at the start of every fight" : " (/hs kit auto on to get it every fight)")));
					return 1;
				}))
				.then(Commands.literal("give").executes(ctx -> {
					Kit.give(ctx.getSource().getServer(), ctx.getSource().getPlayerOrException());
					ctx.getSource().sendSystemMessage(Component.literal("Half Sword: kit given (" + (Kit.custom() ? "yours" : "built-in") + ")"));
					return 1;
				}))
				.then(Commands.literal("reset").executes(ctx -> {
					Kit.reset();
					ctx.getSource().sendSystemMessage(Component.literal("Half Sword: back to the built-in kit"));
					return 1;
				}))
				.then(Commands.literal("auto")
					.then(Commands.literal("on").executes(ctx -> {
						Kit.setAuto(true);
						ctx.getSource().sendSystemMessage(Component.literal("Half Sword: the kit at the start of every fight"));
						return 1;
					}))
					.then(Commands.literal("off").executes(ctx -> {
						Kit.setAuto(false);
						ctx.getSource().sendSystemMessage(Component.literal("Half Sword: you keep what you carry between fights"));
						return 1;
					}))))
			.then(Commands.literal("hostile")
				.then(Commands.literal("on").executes(ctx -> {
					data.hostile = MobWar.forceHostile = true;
					return changed(ctx, "neutral mobs attack on sight");
				}))
				.then(Commands.literal("off").executes(ctx -> {
					data.hostile = MobWar.forceHostile = false;
					return changed(ctx, "neutral mobs only fight back");
				})))
			.then(Commands.literal("balance")
				.then(Commands.literal("on").executes(ctx -> {
					data.balance = Passthrough.balance = true;
					return changed(ctx, "frame rates balanced between the two games");
				}))
				.then(Commands.literal("off").executes(ctx -> {
					data.balance = Passthrough.balance = false;
					return changed(ctx, "frame rates not balanced");
				})))
			.then(Commands.literal("export")
				.then(Commands.literal("sync").executes(ctx -> {
					data.syncExport = Passthrough.syncExport = true;
					return changed(ctx, "Minecraft's frames handed over as soon as they're drawn");
				}))
				.then(Commands.literal("async").executes(ctx -> {
					data.syncExport = Passthrough.syncExport = false;
					return changed(ctx, "Minecraft's frames handed over the old way");
				})))
			.then(Commands.literal("show").executes(ctx -> {
				StringBuilder b = new StringBuilder(String.format(Locale.ROOT, "dealt x%.2f, taken x%.2f, potions x%.2f, frame cap %d, balance %s, export %s, hostile %s",
					data.dealt, data.taken, data.potions, data.fps, data.balance ? "on" : "off", data.syncExport ? "sync" : "async", data.hostile ? "on" : "off"));
				for (String p : PARTS) {
					b.append(String.format(Locale.ROOT, ", %s x%.2f", p, data.parts.getOrDefault(p, 1.0)));
				}

				data.weapons.forEach((k, v) -> b.append(String.format(Locale.ROOT, ", %s x%.2f", k, v)));
				ctx.getSource().sendSystemMessage(Component.literal("Half Sword: " + b));
				return 1;
			}))
			.then(Commands.literal("reset").executes(ctx -> {
				data = new Data();
				Passthrough.syncExport = data.syncExport;
				Passthrough.balance = data.balance;
				MobWar.forceHostile = data.hostile;
				return changed(ctx, "every setting back to its default (damage x1, no frame cap, balance on, export sync, hostile off)");
			}))
			.then(Commands.literal("clean")
				.executes(ctx -> clean(ctx, 64))
				.then(Commands.argument("radius", com.mojang.brigadier.arguments.IntegerArgumentType.integer(1, 256))
					.executes(ctx -> clean(ctx, com.mojang.brigadier.arguments.IntegerArgumentType.getInteger(ctx, "radius")))))
			.then(Commands.literal("barriers")
				.executes(ctx -> {
					String map = BarrierOverrides.map();
					if (map == null) {
						ctx.getSource().sendSystemMessage(Component.literal("Half Sword: no map yet (start a fight)"));
						return 0;
					}

					int[] n = BarrierOverrides.counts();
					ctx.getSource().sendSystemMessage(Component.literal("Half Sword: map " + map + ": " + n[0] + " barriers you added, " + n[1]
						+ " you removed (/hs barriers save after changing more, /hs barriers reset to undo them all)"));
					return 1;
				})
				.then(Commands.literal("save").executes(ctx -> {
					int[] n = BarrierOverrides.save(ctx.getSource().getLevel(), ctx.getSource().getPlayerOrException().blockPosition());
					if (n == null) {
						ctx.getSource().sendSystemMessage(Component.literal("Half Sword: no map yet (start a fight)"));
						return 0;
					}

					int[] all = BarrierOverrides.counts();
					ctx.getSource().sendSystemMessage(Component.literal("Half Sword: saved for " + BarrierOverrides.map() + ": " + n[0] + " new barriers added, "
						+ n[1] + " removed (in all " + all[0] + " added, " + all[1] + " removed on this map)"));
					return 1;
				}))
				.then(Commands.literal("reset").executes(ctx -> {
					int gone = BarrierOverrides.reset(ctx.getSource().getLevel());
					if (gone < 0) {
						ctx.getSource().sendSystemMessage(Component.literal("Half Sword: no map yet (start a fight)"));
						return 0;
					}

					ctx.getSource().sendSystemMessage(Component.literal("Half Sword: " + BarrierOverrides.map() + "'s barriers are back to normal (" + gone
						+ " of yours removed, the mod's put back)"));
					return 1;
				})))
			.then(Commands.literal("fixground").executes(ctx -> {
				int back = WorldBridge.restoreGround(ctx.getSource().getLevel());
				ctx.getSource().sendSystemMessage(Component.literal("Half Sword: ground restored (" + back + " barriers put back)"));
				return 1;
			})));
	}

	/** /hs clean [radius]: every placed block around the player goes, loose items too, and the ground is whole again. */
	private static int clean(final CommandContext<CommandSourceStack> ctx, final int radius) {
		CommandSourceStack source = ctx.getSource();
		int gone = WorldBridge.clean(source.getLevel(), net.minecraft.core.BlockPos.containing(source.getPosition()), radius);
		source.sendSystemMessage(Component.literal(String.format(Locale.ROOT, "Half Sword: cleaned up %d blocks within %d, ground restored", gone, radius)));
		return 1;
	}
}
