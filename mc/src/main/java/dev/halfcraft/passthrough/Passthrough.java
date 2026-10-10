package dev.halfcraft.passthrough;

import java.util.function.Consumer;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class Passthrough implements ModInitializer {
	public static final String ID = "passthrough";
	public static final Logger LOG = LoggerFactory.getLogger(ID);
	/** True while a host game is driving the camera. The integrated server shares this JVM, so both sides read it. */
	public static volatile boolean active;
	/**
	 * True while the host lets Minecraft drive: the player moves with Minecraft's own physics from the host's
	 * forwarded input, and the host's camera follows Minecraft's (Half Sword).
	 */
	public static volatile boolean own;
	/**
	 * The frame export waits for the GPU at the end of each frame and hands the frame over right then, instead of
	 * at the start of a later frame (1-2 frames sooner on screen; /hs export sync|async).
	 */
	public static volatile boolean syncExport = true;
	/** Minecraft and Half Sword cap themselves a little above each other's frame rate (/hs balance on|off). */
	public static volatile boolean balance = true;
	/** Where events for the host go (JSON lines); the client's HostLink sets it. */
	public static volatile Consumer<String> events = message -> {};

	@Override
	public void onInitialize() {
		ServerLifecycleEvents.SERVER_STARTED.register(WorldBridge::attach);
		ServerLifecycleEvents.SERVER_STOPPING.register(server -> {
			// before the world is saved: the Nether goes back and the fight's mobs go, so none of it is kept
			Nether.detach(server);
			MobWar.detach(server);
			WorldBridge.writeRecord(server);
			WorldBridge.detach();
		});
		ServerEntityEvents.ENTITY_LOAD.register(MobWar::onEntityLoad);
		ServerTickEvents.END_SERVER_TICK.register(WorldBridge::tick);
		net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback.EVENT.register((dispatcher, registry, env) -> HsConfig.register(dispatcher));
		LOG.info("passthrough loaded");
	}
}
