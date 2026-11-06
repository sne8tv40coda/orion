package com.example.client.mixin;

import java.util.Map;
import net.minecraft.client.renderer.rendertype.RenderSetup;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(RenderSetup.class)
public interface RenderSetupAccessor {
    @Accessor("textures") Map<String, Object> rtx$getTextures();

    /**
     * 2026-11-04：旗帜图案层的识别需要 pipeline 的 location
     * （{@code "minecraft:pipeline/banner_pattern"}）。RenderSetup 的这个字段是包私有的，
     * 只能靠 accessor 读。用户：判断"这是不是旗帜图案层"。
     */
    @Accessor("pipeline")
    com.mojang.renderpearl.api.pipeline.RenderPipeline rtx$getPipeline();
}
