package dev.halfcraft.passthrough.client.mixin;

import dev.halfcraft.passthrough.client.OwnMode;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * While Minecraft drives from the host's window, the system cursor is over the host, not over Minecraft: draw the
 * virtual cursor on top of every screen (inventory, chat, menus) so it shows in the composited overlay.
 */
@Mixin(Screen.class)
abstract class ScreenMixin {
	@Inject(method = "extractRenderStateWithTooltipAndSubtitles", at = @At("TAIL"))
	private void passthrough$cursor(final GuiGraphicsExtractor graphics, final int mouseX, final int mouseY, final float a, final CallbackInfo ci) {
		double[] c = OwnMode.cursor();
		if (c == null) {
			return;
		}

		int scale = Minecraft.getInstance().getWindow().getGuiScale();
		int x = (int) (c[0] / scale), y = (int) (c[1] / scale);
		graphics.nextStratum();
		// a small arrow: black outline, white fill
		for (int i = 0; i < 9; i++) {
			graphics.fill(x, y + i, x + Math.min(i, 6) + 2, y + i + 1, 0xFF000000);
		}

		for (int i = 1; i < 8; i++) {
			graphics.fill(x + 1, y + i, x + Math.min(i, 6) + 1, y + i + 1, 0xFFFFFFFF);
		}
	}
}
