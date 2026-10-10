package dev.halfcraft.passthrough;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.mojang.serialization.JsonOps;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.resources.RegistryOps;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.item.ItemStack;

/**
 * Steve's kit in Half Sword: the built-in one, or the inventory the player saved (/hs kit save), kept in
 * config/passthrough-kit.json. Given the first time Minecraft drives, and (/hs kit auto, on by default) at the start
 * of every Half Sword fight ("match" from the host: a new fight gives the player a new body there), with full health
 * and food.
 */
public final class Kit {
	private static final Path FILE = FabricLoader.getInstance().getConfigDir().resolve("passthrough-kit.json");
	/** The built-in kit: commands run as the player. */
	private static final List<String> DEFAULT = List.of(
		"item replace entity @s hotbar.0 with minecraft:diamond_sword",
		"item replace entity @s hotbar.1 with minecraft:bow[enchantments={power:5,infinity:1}]",
		"item replace entity @s hotbar.2 with minecraft:crossbow[enchantments={multishot:1,quick_charge:3}]",
		"item replace entity @s hotbar.3 with minecraft:tnt 64",
		"item replace entity @s hotbar.4 with minecraft:flint_and_steel",
		"item replace entity @s hotbar.5 with minecraft:creeper_spawn_egg 64",
		"item replace entity @s hotbar.6 with minecraft:zombie_spawn_egg 64",
		"item replace entity @s hotbar.7 with minecraft:cobblestone 64",
		"item replace entity @s hotbar.8 with minecraft:cooked_beef 64",
		"item replace entity @s weapon.offhand with minecraft:shield",
		"item replace entity @s armor.head with minecraft:diamond_helmet",
		"item replace entity @s armor.chest with minecraft:diamond_chestplate",
		"item replace entity @s armor.legs with minecraft:diamond_leggings",
		"item replace entity @s armor.feet with minecraft:diamond_boots",
		"give @s minecraft:arrow 64",
		"give @s minecraft:oak_planks 64",
		"give @s minecraft:glass 64",
		"give @s minecraft:firework_rocket[fireworks={flight_duration:3,explosions:[{shape:\"large_ball\",colors:[I;16733525,16755200],has_trail:true}]}] 64"
	);
	private static final String GIVEN_TAG = "hs_kit";

	/** The saved kit: [{"slot":n,"item":{...}}, ...], or null for the built-in one. */
	private static JsonArray saved;
	/** Give the kit at the start of every Half Sword fight. */
	public static volatile boolean auto = true;

	static {
		try {
			if (Files.exists(FILE)) {
				JsonObject o = JsonParser.parseString(Files.readString(FILE)).getAsJsonObject();
				saved = o.has("items") ? o.getAsJsonArray("items") : null;
				auto = !o.has("auto") || o.get("auto").getAsBoolean();
			}
		} catch (IOException | RuntimeException e) {
			Passthrough.LOG.warn("couldn't read {}: {}", FILE, e.toString());
		}
	}

	private Kit() {
	}

	private static void write() {
		JsonObject o = new JsonObject();
		o.addProperty("auto", auto);
		if (saved != null) {
			o.add("items", saved);
		}

		try {
			Files.createDirectories(FILE.getParent());
			Files.writeString(FILE, new GsonBuilder().setPrettyPrinting().create().toJson(o));
		} catch (IOException e) {
			Passthrough.LOG.warn("couldn't write {}: {}", FILE, e.toString());
		}
	}

	/** The player's whole inventory (hotbar, bag, armor, offhand) becomes the kit. Returns the number of stacks. */
	public static int save(final ServerPlayer player) {
		RegistryOps<JsonElement> ops = player.registryAccess().createSerializationContext(JsonOps.INSTANCE);
		Inventory inventory = player.getInventory();
		JsonArray items = new JsonArray();
		for (int i = 0; i < inventory.getContainerSize(); i++) {
			ItemStack stack = inventory.getItem(i);
			if (stack.isEmpty()) {
				continue;
			}

			int slot = i;
			ItemStack.CODEC.encodeStart(ops, stack).result().ifPresent(json -> {
				JsonObject e = new JsonObject();
				e.addProperty("slot", slot);
				e.add("item", json);
				items.add(e);
			});
		}

		saved = items;
		write();
		return items.size();
	}

	/** Back to the built-in kit. */
	public static void reset() {
		saved = null;
		write();
	}

	public static void setAuto(final boolean on) {
		auto = on;
		write();
	}

	public static boolean custom() {
		return saved != null;
	}

	/** The kit, in place of whatever the player carries (server thread). */
	public static void give(final MinecraftServer s, final ServerPlayer player) {
		Inventory inventory = player.getInventory();
		inventory.clearContent();
		if (saved != null) {
			RegistryOps<JsonElement> ops = player.registryAccess().createSerializationContext(JsonOps.INSTANCE);
			for (JsonElement el : saved) {
				JsonObject e = el.getAsJsonObject();
				int slot = e.get("slot").getAsInt();
				if (slot < 0 || slot >= inventory.getContainerSize()) {
					continue;
				}

				ItemStack.CODEC.parse(ops, e.get("item")).result().ifPresent(stack -> inventory.setItem(slot, stack));
			}
		} else {
			for (String c : DEFAULT) {
				s.getCommands().performPrefixedCommand(s.createCommandSourceStack().withSuppressedOutput(), "execute as " + player.getUUID() + " run " + c);
			}
		}

		player.addTag(GIVEN_TAG);
		player.inventoryMenu.broadcastChanges();
	}

	/** Minecraft starts driving: a player who never had the kit gets it. */
	public static void giveFirst() {
		MinecraftServer s = WorldBridge.server();
		if (s != null) {
			s.execute(() -> s.getPlayerList().getPlayers().stream().filter(p -> !p.entityTags().contains(GIVEN_TAG)).forEach(p -> give(s, p)));
		}
	}

	/** {"t":"match"}: a Half Sword fight starts. The kit again (if auto), with full health and food. */
	public static void newMatch() {
		MinecraftServer s = WorldBridge.server();
		if (s == null || !auto) {
			return;
		}

		s.execute(() -> {
			for (ServerPlayer p : s.getPlayerList().getPlayers()) {
				if (!p.isAlive()) {
					continue;
				}

				give(s, p);
				p.setHealth(p.getMaxHealth());
				p.getFoodData().setFoodLevel(20);
				p.getFoodData().setSaturation(5.0F);
				Passthrough.LOG.info("new fight: kit given ({})", saved != null ? "saved" : "built-in");
			}
		});
	}
}
