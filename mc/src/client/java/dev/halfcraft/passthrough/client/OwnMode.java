package dev.halfcraft.passthrough.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import dev.halfcraft.passthrough.Passthrough;
import dev.halfcraft.passthrough.WorldBridge;
import java.util.List;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonInfo;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.phys.Vec3;

/**
 * Minecraft drives (Half Sword host): the host forwards the player's raw keyboard and mouse, which are replayed into
 * Minecraft's own input handlers exactly as SDL would deliver them, so everything works as if Minecraft had the focus
 * (movement, F5, inventory, chat, F3...). Minecraft reports back where the player is every frame ("me"); the host's
 * camera follows the exported frames' camera.
 */
public final class OwnMode {
	/** Run once on the server when Minecraft starts driving (the kit too, once per player: Kit). */
	private static final List<String> START = List.of(
		"gamemode survival @a",
		"gamerule keep_inventory true",
		"gamerule fall_damage true",
		"gamerule immediate_respawn true", // Steve's death ends Half Sword's match: its menu, not Minecraft's, comes next
		"difficulty normal",
		"effect clear @a"
	);

	/** The virtual cursor (window pixels) for screens; with the mouse grabbed only the relative motion counts. */
	private static double cursorX = -1, cursorY;
	/** Mouse motion the host sent ("mv", straight from its input thread), not yet given to Minecraft. */
	private static final Object MOTION = new Object();
	private static double motionX, motionY;

	/** The server (world) START last ran on. */
	private static Object startedFor;

	private OwnMode() {
	}

	/** Render thread, from HostState.beginFrame: Minecraft starts or stops driving. */
	static void changed(final boolean own) {
		Passthrough.LOG.info("own mode {}", own ? "on" : "off");
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		if (own) {
			dev.halfcraft.passthrough.HsConfig.send();
			// once per world: own mode also ends while Half Sword is tabbed out, and coming back put the player in
			// survival again (and cleared his effects)
			Object server = WorldBridge.server();
			if (server == null || server == startedFor) {
				return;
			}

			startedFor = server;
			START.forEach(WorldBridge::command);
			dev.halfcraft.passthrough.Kit.giveFirst();
			// frame generation in Half Sword: Minecraft only follows its real frames, and the balance's cap counts the
			// made-up ones too (it held real frames at half once)
			minecraft.gui.hud.getChat().addClientSystemMessage(net.minecraft.network.chat.Component.literal(
				"§e[Half Sword] If you get low fps, stutters or input lag, try /hs priority on|off and /hs balance on|off, "
					+ "and turning frame generation off in Half Sword's graphics settings."));
			if (player != null) {
				player.getAbilities().flying = false;
				player.onUpdateAbilities();
			}
		} else if (minecraft.mouseHandler.isMouseGrabbed() && !minecraft.isWindowActive()) {
			minecraft.mouseHandler.releaseMouse();
		}
	}

	private static long lastSwing;

	/**
	 * Steve swings (client, Minecraft.startAttack): {"t":"swing","o":[eyes],"d":[look],"r":reach,"s":charge}. The host
	 * pushes the first of its loose things on that line (/hs props sets how hard).
	 */
	public static void swing(final LocalPlayer player) {
		long now = System.currentTimeMillis();
		if (now - lastSwing < 120L) {
			return; // (one blow, however many clicks it took)
		}

		lastSwing = now;
		Vec3 eye = player.getEyePosition();
		Vec3 look = player.getViewVector(1.0F);
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"swing\",\"o\":[%.4f,%.4f,%.4f],\"d\":[%.4f,%.4f,%.4f],\"r\":%.2f,\"s\":%.2f}",
			eye.x, eye.y, eye.z, look.x, look.y, look.z, player.entityInteractionRange() + 0.5, player.getAttackStrengthScale(0.5F)));
	}

	/** Render thread, end of the camera update: keep the mouse grabbed for play, and tell the host where the player is. */
	public static void frame(final float partialTick) {
		GpuPriority.update();
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		if (player == null) {
			return;
		}

		if (minecraft.gui.screen() == null && !minecraft.mouseHandler.isMouseGrabbed()) {
			minecraft.mouseHandler.grabMouse();
		}

		Vec3 at = player.getPosition(partialTick);
		// night vision lights Half Sword's picture too (0..1, Minecraft's own: it flickers as it runs out)
		float nv = player.hasEffect(net.minecraft.world.effect.MobEffects.NIGHT_VISION)
			? net.minecraft.client.renderer.GameRenderer.nightVisionScale(player, partialTick) : 0.0F;
		Passthrough.events.accept(String.format(Locale.ROOT,
			"{\"t\":\"me\",\"p\":[%.4f,%.4f,%.4f],\"y\":%.2f,\"hp\":%.2f,\"max\":%.2f,\"scr\":%b,\"dead\":%b,\"sneak\":%b,\"nv\":%.3f}",
			at.x, at.y, at.z, player.getYRot(), player.getHealth(), player.getMaxHealth(), minecraft.gui.screen() != null,
			player.isDeadOrDying(), player.isShiftKeyDown(), nv));
		// fighters in boats and carts: their seats where the vehicles are drawn this frame
		// ({"t":"seats","s":[[handle, x, vehicle y, z],...]})
		java.util.Map<Integer, double[]> riders = dev.halfcraft.passthrough.MobWar.riders;
		if (!riders.isEmpty() && minecraft.level != null) {
			StringBuilder b = null;
			for (java.util.Map.Entry<Integer, double[]> e : riders.entrySet()) {
				net.minecraft.world.entity.Entity vehicle = minecraft.level.getEntity((int) e.getValue()[0]);
				if (vehicle == null) {
					continue;
				}

				Vec3 v = vehicle.getPosition(partialTick);
				b = b == null ? new StringBuilder("{\"t\":\"seats\",\"s\":[") : b.append(',');
				b.append(String.format(Locale.ROOT, "[%d,%.4f,%.4f,%.4f]", e.getKey(), v.x + e.getValue()[1], v.y, v.z + e.getValue()[2]));
			}

			if (b != null) {
				Passthrough.events.accept(b.append("]}").toString());
			}
		}
	}

	/**
	 * {"t":"ev","e":[...]} from the host, in order:
	 * ["k", scancode, sdl keycode, sdl mods, action 1/0/-1], ["c", "text"], ["b", button 1-5, action 1/0, mods],
	 * ["w", dx, dy], ["m", dx, dy] (raw mouse counts).
	 */
	static void events(final Minecraft minecraft, final JsonObject m) {
		long window = minecraft.getWindow().handle();
		for (JsonElement el : m.getAsJsonArray("e")) {
			JsonArray e = el.getAsJsonArray();
			switch (e.get(0).getAsString()) {
				case "k" -> minecraft.keyboardHandler.keyPress(window, e.get(4).getAsInt(),
					new KeyEvent(e.get(1).getAsInt(), e.get(2).getAsInt(), e.get(3).getAsInt()));
				case "c" -> minecraft.keyboardHandler.textInput(window, e.get(1).getAsString());
				case "b" -> {
					applyMotion(minecraft); // (a click lands where the cursor has moved to)
					minecraft.mouseHandler.onButton(window, new MouseButtonInfo(e.get(1).getAsInt(), e.get(3).getAsInt()), e.get(2).getAsInt());
				}
				case "w" -> minecraft.mouseHandler.onScroll(window, e.get(1).getAsDouble(), e.get(2).getAsDouble());
				case "m" -> move(minecraft, window, e.get(1).getAsDouble(), e.get(2).getAsDouble());
				default -> {
				}
			}
		}
	}

	/** {"t":"mv","d":[dx,dy]} (WebSocket thread): kept until Minecraft next reads the mouse. */
	static void addMotion(final double dx, final double dy) {
		synchronized (MOTION) {
			motionX += dx;
			motionY += dy;
		}
	}

	/** Render thread: the host's motion so far goes to Minecraft now. */
	public static void applyMotion(final Minecraft minecraft) {
		double dx, dy;
		synchronized (MOTION) {
			dx = motionX;
			dy = motionY;
			motionX = motionY = 0.0;
		}

		if (dx != 0.0 || dy != 0.0) {
			move(minecraft, minecraft.getWindow().handle(), dx, dy);
		}
	}

	private static void move(final Minecraft minecraft, final long window, final double dx, final double dy) {
		int w = minecraft.getWindow().getScreenWidth(), h = minecraft.getWindow().getScreenHeight();
		if (cursorX < 0 || minecraft.mouseHandler.isMouseGrabbed()) {
			// grabbed: Minecraft keeps the cursor centred; a screen opening then starts it there
			cursorX = w / 2.0;
			cursorY = h / 2.0;
		} else {
			cursorX = Math.clamp(cursorX + dx, 0.0, w - 1.0);
			cursorY = Math.clamp(cursorY + dy, 0.0, h - 1.0);
		}

		minecraft.mouseHandler.onMove(window, cursorX, cursorY, dx, dy);
	}

	/** Where the virtual cursor is (window pixels), or null while the mouse is grabbed. */
	public static double[] cursor() {
		Minecraft minecraft = Minecraft.getInstance();
		return cursorX < 0 || minecraft.mouseHandler.isMouseGrabbed() ? null : new double[] {cursorX, cursorY};
	}
}
