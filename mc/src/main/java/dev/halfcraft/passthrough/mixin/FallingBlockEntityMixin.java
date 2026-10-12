package dev.halfcraft.passthrough.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.halfcraft.passthrough.HostGround;
import net.minecraft.world.entity.item.FallingBlockEntity;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** A falling block landing on the host's ground becomes a block there (vanilla saw air under it: it dropped as an item). */
@Mixin(FallingBlockEntity.class)
abstract class FallingBlockEntityMixin {
	@WrapOperation(method = "tick()V",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/block/FallingBlock;isFree(Lnet/minecraft/world/level/block/state/BlockState;)Z"))
	private boolean passthrough$landsOnHostGround(final BlockState below, final Operation<Boolean> original) {
		FallingBlockEntity self = (FallingBlockEntity) (Object) this;
		return original.call(below) && !HostGround.solidAir(self.level(), self.blockPosition().below(), below);
	}
}
