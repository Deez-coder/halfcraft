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
 * <li>/hs show; /hs changed: only the settings that aren't at their default; /hs reset: every setting back to its default</li>
 * <li>/hs clean [radius]: remove every placed block (default within 64), loose items, arrows, TNT; ground restored</li>
 * <li>/hs fixground: Half Sword scans its ground again and resends it</li>
 * <li>/hs fps &lt;n&gt;: Half Sword's frame cap (0 = none): what it leaves of the GPU goes to Minecraft</li>
 * <li>/hs kit save|give|reset|auto on|off: your inventory as the kit you get at the start of every Half Sword fight</li>
 * <li>/hs hostile on|off: neutral mobs (wolves, golems, bees, endermen...) attack the fighters and Steve on sight</li>
 * <li>/hs priority on|off: Minecraft's GPU work goes first (its picture half as old on screen, Half Sword ~20% slower)</li>
 * <li>/hs balance on|off: each game no faster than a little above the other (the GPU goes to frames that are shown)</li>
 * <li>/hs mining on|off, /hs mining &lt;x&gt;: whether the fighters break Minecraft's blocks by hitting them, and how fast</li>
 * <li>/hs current &lt;x&gt;: how hard water's and lava's currents push the fighters</li>
 * <li>/hs explosions &lt;x&gt;: how hard explosions (TNT, creepers...) throw the fighters, Steve and mobs (not how far they reach)</li>
 * <li>/hs mace &lt;x&gt;: the mace's damage to the fighters (its blows and smashes)</li>
 * <li>/hs props &lt;x&gt;: how hard Steve's hits push Half Sword's loose things (candle stands, chains, weapons)</li>
 * <li>/hs ground, /hs why: for bug reports - how the ground under you was scanned (Half Sword's log), what the nearest
 * mob's pathfinding makes of where it stands</li>
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
		double props = 1.0;
		boolean mining = true;
		double current = 1.0;
		double miningStrength = 1.0;
		double explosions = 1.0;
		double mace = 1.0;
		Map<String, Double> parts = new LinkedHashMap<>();
		Map<String, Double> weapons = new LinkedHashMap<>();
		int fps = 0;
		boolean syncExport = true;
		boolean balance = false;
		boolean gpuPriority = true;
		boolean hostile = false;
	}

	private static Data data = load();

	static {
		Passthrough.syncExport = data.syncExport;
		Passthrough.balance = data.balance;
		Passthrough.gpuPriority = data.gpuPriority;
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

	/** How hard the fighters' blows wear Minecraft's blocks down (0: they don't). */
	public static double mining() {
		return data.mining ? data.miningStrength : 0.0;
	}

	/** How hard explosions throw what they hit. */
	public static double explosions() {
		return data.explosions;
	}

	/** The multiplier for one Minecraft item's hits (1 if none set). */
	public static double weapon(final String item) {
		return data.weapons.getOrDefault(item, 1.0);
	}

	/** /hs help: every command, one line each. */
	private static final String[][] HELP = {
		{"/hs show", "see all your settings"},
		{"/hs changed", "see only the settings you changed, and what they are now"},
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
		{"/hs balance on|off", "keeps both games' fps matched: can be smoother with less lag on some PCs (off by default)"},
		{"/hs priority on|off", "Minecraft's frames go first on the GPU: much less lag, Half Sword a bit slower (on by default)"},
		{"/hs mining on|off", "fighters break your blocks by hitting them (on by default)"},
		{"/hs mining <x>", "how fast fighters break blocks (1 = normal, 2 = twice as fast)"},
		{"/hs current <x>", "how hard water and lava currents push fighters (1 = normal, 0 = not at all)"},
		{"/hs explosions <x>", "how hard explosions throw fighters, you and mobs (1 = normal, 2 = twice as hard)"},
		{"/hs mace <x>", "how much damage the mace does to fighters (1 = normal)"},
		{"/hs props <x>", "how hard your hits push loose things in Half Sword (1 = normal, 0 = off)"},
		{"/hs fps <n>", "cap Half Sword's fps (0 = no cap)"},
		{"/hs clean [radius]", "remove everything you built nearby (blocks, items, arrows, TNT)"},
		{"/hs fixground", "scan the invisible ground again if you fall through it"},
		{"/hs export sync|async", "how Minecraft's picture is sent (sync = less lag, the default)"},
		{"/hs ground", "for bug reports: writes how the ground under you was scanned to HSPassthrough.log"},
		{"/hs why", "for bug reports: what the nearest mob's pathfinding makes of where it stands"},
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
		m.addProperty("props", data.props);
		m.addProperty("current", data.current);
		m.addProperty("explosions", data.explosions);
		m.addProperty("mace", data.mace);
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
			.then(Commands.literal("why").executes(MobDebug::why))
			.then(Commands.literal("ground").executes(ctx -> {
				// diagnostic: Half Sword logs how its ground scan sees the column you stand in
				Passthrough.events.accept("{\"t\":\"groundwhy\"}");
				ctx.getSource().sendSystemMessage(Component.literal("Half Sword: the ground under you is written to HSPassthrough.log"));
				return 1;
			}))
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
			.then(Commands.literal("mining")
				.then(Commands.literal("on").executes(ctx -> {
					data.mining = true;
					return changed(ctx, "fighters break blocks by hitting them");
				}))
				.then(Commands.literal("off").executes(ctx -> {
					data.mining = false;
					return changed(ctx, "fighters can't break blocks");
				}))
				.then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
					data.miningStrength = DoubleArgumentType.getDouble(ctx, "multiplier");
					return changed(ctx, String.format(Locale.ROOT, "fighters break blocks x%.2f", data.miningStrength));
				})))
			.then(Commands.literal("current").then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
				data.current = DoubleArgumentType.getDouble(ctx, "multiplier");
				return changed(ctx, String.format(Locale.ROOT, "currents push fighters x%.2f", data.current));
			})))
			.then(Commands.literal("explosions").then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
				data.explosions = DoubleArgumentType.getDouble(ctx, "multiplier");
				return changed(ctx, String.format(Locale.ROOT, "explosions throw x%.2f as hard", data.explosions));
			})))
			.then(Commands.literal("mace").then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
				data.mace = DoubleArgumentType.getDouble(ctx, "multiplier");
				return changed(ctx, String.format(Locale.ROOT, "mace damage x%.2f", data.mace));
			})))
			.then(Commands.literal("props").then(Commands.argument("multiplier", DoubleArgumentType.doubleArg(0.0, 100.0)).executes(ctx -> {
				data.props = DoubleArgumentType.getDouble(ctx, "multiplier");
				return changed(ctx, String.format(Locale.ROOT, "hits push loose things x%.2f", data.props));
			})))
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
			.then(Commands.literal("priority")
				.then(Commands.literal("on").executes(ctx -> {
					data.gpuPriority = Passthrough.gpuPriority = true;
					return changed(ctx, "Minecraft's frames go first on the GPU (less lag)");
				}))
				.then(Commands.literal("off").executes(ctx -> {
					data.gpuPriority = Passthrough.gpuPriority = false;
					return changed(ctx, "Minecraft and Half Sword share the GPU evenly (Half Sword faster, more lag)");
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
				StringBuilder b = new StringBuilder(String.format(Locale.ROOT, "dealt x%.2f, taken x%.2f, potions x%.2f, props x%.2f, current x%.2f, explosions x%.2f, mace x%.2f, mining %s, frame cap %d, balance %s, priority %s, export %s, hostile %s",
					data.dealt, data.taken, data.potions, data.props, data.current, data.explosions, data.mace, data.mining ? String.format(Locale.ROOT, "x%.2f", data.miningStrength) : "off", data.fps, data.balance ? "on" : "off", data.gpuPriority ? "on" : "off", data.syncExport ? "sync" : "async", data.hostile ? "on" : "off"));
				for (String p : PARTS) {
					b.append(String.format(Locale.ROOT, ", %s x%.2f", p, data.parts.getOrDefault(p, 1.0)));
				}

				data.weapons.forEach((k, v) -> b.append(String.format(Locale.ROOT, ", %s x%.2f", k, v)));
				ctx.getSource().sendSystemMessage(Component.literal("Half Sword: " + b));
				return 1;
			}))
			.then(Commands.literal("changed").executes(HsConfig::changedSettings))
			.then(Commands.literal("reset").executes(ctx -> {
				data = new Data();
				Passthrough.syncExport = data.syncExport;
				Passthrough.balance = data.balance;
				Passthrough.gpuPriority = data.gpuPriority;
				MobWar.forceHostile = data.hostile;
				return changed(ctx, "every setting back to its default (damage x1, no frame cap, balance off, export sync, hostile off)");
			}))
			.then(Commands.literal("clean")
				.executes(ctx -> clean(ctx, 64))
				.then(Commands.argument("radius", com.mojang.brigadier.arguments.IntegerArgumentType.integer(1, 256))
					.executes(ctx -> clean(ctx, com.mojang.brigadier.arguments.IntegerArgumentType.getInteger(ctx, "radius")))))
			.then(Commands.literal("fixground").executes(ctx -> {
				WorldBridge.restoreGround();
				ctx.getSource().sendSystemMessage(Component.literal("Half Sword: the ground is scanned again"));
				return 1;
			})));
	}

	/** What each setting is called in /hs changed (the config file's names otherwise). */
	private static final Map<String, String> NAMES = Map.ofEntries(Map.entry("dealt", "damage dealt"), Map.entry("taken", "damage taken"),
		Map.entry("potions", "potion damage"), Map.entry("props", "props push"), Map.entry("mining", "fighters break blocks"),
		Map.entry("miningStrength", "mining speed"), Map.entry("current", "currents push"), Map.entry("explosions", "explosion knockback"),
		Map.entry("mace", "mace damage"), Map.entry("fps", "Half Sword frame cap"), Map.entry("syncExport", "sync export"),
		Map.entry("balance", "balance"), Map.entry("gpuPriority", "GPU priority"), Map.entry("hostile", "hostile mobs"));

	/** /hs changed: every setting that isn't at its default, with its value now (the kit's too). */
	private static int changedSettings(final CommandContext<CommandSourceStack> ctx) {
		JsonObject now = GSON.toJsonTree(data).getAsJsonObject();
		JsonObject defaults = GSON.toJsonTree(new Data()).getAsJsonObject();
		List<String> lines = new java.util.ArrayList<>();
		for (Map.Entry<String, com.google.gson.JsonElement> e : now.entrySet()) {
			if (e.getValue().isJsonObject()) {
				// parts and weapons: each one set (at 1 it's no change)
				for (Map.Entry<String, com.google.gson.JsonElement> x : e.getValue().getAsJsonObject().entrySet()) {
					if (x.getValue().getAsDouble() != 1.0) {
						lines.add(String.format(Locale.ROOT, "%s %s: x%.2f", e.getKey().equals("parts") ? "hits on" : "item", x.getKey(), x.getValue().getAsDouble()));
					}
				}
			} else if (!e.getValue().equals(defaults.get(e.getKey()))) {
				com.google.gson.JsonPrimitive v = e.getValue().getAsJsonPrimitive();
				String value = v.isBoolean() ? (v.getAsBoolean() ? "on" : "off") : e.getKey().equals("fps") ? v.getAsString()
					: String.format(Locale.ROOT, "x%.2f", v.getAsDouble());
				lines.add(NAMES.getOrDefault(e.getKey(), e.getKey()) + ": " + value);
			}
		}

		if (!Kit.auto) {
			lines.add("kit every fight: off");
		}

		if (Kit.custom()) {
			lines.add("kit: your own (saved)");
		}

		if (lines.isEmpty()) {
			ctx.getSource().sendSystemMessage(Component.literal("Half Sword: every setting is at its default"));
			return 1;
		}

		ctx.getSource().sendSystemMessage(Component.literal("Settings you changed:").withStyle(net.minecraft.ChatFormatting.YELLOW));
		for (String line : lines) {
			ctx.getSource().sendSystemMessage(Component.literal(line).withStyle(net.minecraft.ChatFormatting.GRAY));
		}

		return 1;
	}

	/** /hs clean [radius]: every placed block around the player goes, loose items too, and the ground is whole again. */
	private static int clean(final CommandContext<CommandSourceStack> ctx, final int radius) {
		CommandSourceStack source = ctx.getSource();
		int gone = WorldBridge.clean(source.getLevel(), net.minecraft.core.BlockPos.containing(source.getPosition()), radius);
		source.sendSystemMessage(Component.literal(String.format(Locale.ROOT, "Half Sword: cleaned up %d blocks within %d, ground restored", gone, radius)));
		return 1;
	}
}
