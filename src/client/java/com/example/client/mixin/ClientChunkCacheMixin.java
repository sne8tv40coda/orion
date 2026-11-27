package com.example.client.mixin;

import com.example.client.lod.OrionLodManager;
import com.example.vulkan.NativeVulkan;

import net.minecraft.client.multiplayer.ClientChunkCache;
import net.minecraft.network.protocol.game.ClientboundLevelChunkPacketData;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.LevelChunk;

import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;


@Mixin(ClientChunkCache.class)
public class ClientChunkCacheMixin {

    @Inject(
            method = "drop",
            at = @At("HEAD")
    )
    private void onDropChunk(
            ChunkPos pos,
            CallbackInfo ci
    ) {
        if (pos == null) {
            return;
        }


        try {
            NativeVulkan.removeLightChunk(
                    pos.x(),
                    pos.z()
            );
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }

    @Inject(
            method = "replaceWithPacketData",
            at = @At("RETURN")
    )
    private void onReplaceChunkWithPacketData(
            int x,
            int z,
            ClientboundLevelChunkPacketData data,
            CallbackInfoReturnable<LevelChunk> cir
    ) {
        if (cir.getReturnValue() != null) {
            OrionLodManager.onChunkLoaded(x, z);
        }
    }
}