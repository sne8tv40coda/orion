package com.example.client.mixin;

import com.example.vulkan.NativeVulkan;
import com.example.client.DynamicLightCapture;
import com.example.client.EntityRtCapture;
import com.example.client.ParticleRtCapture;import com.example.client.compat.FarFieldGate;
import com.example.client.compat.VoxyLodManager;
import com.example.client.compat.dh.DhBridge;
import com.example.client.compat.dh.DhLodMeshSink;
import com.example.client.lod.OrionLodManager;
import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.ViewArea;
import net.minecraft.client.renderer.entity.EntityRenderDispatcher;
import net.minecraft.client.renderer.entity.player.AvatarRenderer;
import net.minecraft.client.model.player.PlayerModel;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import net.minecraft.client.renderer.entity.state.EntityRenderState;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.client.renderer.state.level.LevelRenderState;
import net.minecraft.client.renderer.texture.OverlayTexture;
import com.mojang.math.Axis;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.Pose;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;
import net.minecraft.world.phys.Vec3;

@Mixin(LevelRenderer.class)
public class LevelRendererMixin {
    @Shadow @Final
    private EntityRenderDispatcher entityRenderDispatcher;

    @Shadow
    private ViewArea viewArea;

    private static float rtx$partialTick;

    private static long rtx$memReportFrame;

    private static long rtx$nmtReportCount;

    private static final boolean RTX$NMT_DIAG_ENABLED =
            "1".equals(System.getProperty("orion.nmtDiag"))
                    || Boolean.parseBoolean(System.getProperty("orion.nmtDiag"))
                    || "1".equals(System.getenv("ORION_NMT_DIAG"));

    private static void rtx$logJavaMemory() {
        if ((rtx$memReportFrame++ % 3600L) != 0L) {
            return;
        }

        final Runtime runtime = Runtime.getRuntime();
        final long mb = 1024L * 1024L;

        System.out.println("[RT MEM JAVA]"
                + " heapUsedMB=" + ((runtime.totalMemory() - runtime.freeMemory()) / mb)
                + " heapTotalMB=" + (runtime.totalMemory() / mb)
                + " heapMaxMB=" + (runtime.maxMemory() / mb)
                + " | " + rtx$directMemoryStats()
                + " | dh=" + DhLodMeshSink.stats());

        if (RTX$NMT_DIAG_ENABLED && (rtx$nmtReportCount++ % 1L) == 0L) {
            for (final String line : rtx$nativeMemorySummary().split("\\R")) {
                if (!line.isBlank()) {
                    System.out.println("[RT MEM NMT]" + line);
                }
            }
        }
    }

    private static String rtx$directMemoryStats() {
        long directCount = -1L;
        long directUsed = -1L;
        long mappedUsed = -1L;

        String failure = null;

        try {
            final javax.management.MBeanServer server =
                    java.lang.management.ManagementFactory.getPlatformMBeanServer();

            final java.util.Set<javax.management.ObjectName> names =
                    server.queryNames(
                            new javax.management.ObjectName("*:type=BufferPool,*"),
                            null);

            for (final javax.management.ObjectName objectName : names) {
                final String pool = String.valueOf(
                        server.getAttribute(objectName, "Name"));
                final long used = ((Number) server
                        .getAttribute(objectName, "MemoryUsed")).longValue();

                if ("direct".equalsIgnoreCase(pool)) {
                    directUsed = used;
                    directCount = ((Number) server
                            .getAttribute(objectName, "Count")).longValue();
                } else if ("mapped".equalsIgnoreCase(pool)) {
                    mappedUsed = used;
                }
            }
        } catch (final Throwable t) {
            failure = t.getClass().getSimpleName() + ":" + t.getMessage();
        }

        final long mb = 1024L * 1024L;

        return "direct=" + directCount + "/" + (directUsed / mb) + "MB"
                + " mapped=" + (mappedUsed / mb) + "MB"
                + " threads=" + java.lang.management.ManagementFactory
                        .getThreadMXBean().getThreadCount()
                + (failure == null ? "" : " poolErr=" + failure);
    }

    private static String rtx$nativeMemorySummary() {
        try {
            final javax.management.MBeanServer server =
                    java.lang.management.ManagementFactory.getPlatformMBeanServer();

            final Object report = server.invoke(
                    new javax.management.ObjectName(
                            "com.sun.management:type=DiagnosticCommand"),
                    "vmNativeMemory",
                    new Object[]{new String[]{"summary"}},
                    new String[]{String[].class.getName()});

            return report == null ? "" : String.valueOf(report);
        } catch (final Throwable t) {
            return "unavailable (" + t.getClass().getSimpleName() + ")";
        }
    }

    private static int[] rtx$visibleSectionScratch = new int[0];

    private static void rtx$uploadVisibleSections(CameraRenderState cameraState) {
        if (cameraState != null && cameraState.isFrustumCaptured) {
            NativeVulkan.setVisibleSections(null, 0);
            return;
        }

        final LevelRenderer renderer = Minecraft.getInstance().levelRenderer;
        if (renderer == null) {
            NativeVulkan.setVisibleSections(null, 0);
            return;
        }

        final var sections = renderer.visibleSections();
        final int count = sections == null ? 0 : sections.size();
        if (count <= 0) {
            NativeVulkan.setVisibleSections(null, 0);
            return;
        }

        final int needed = count * 3;
        if (rtx$visibleSectionScratch.length < needed) {
            rtx$visibleSectionScratch = new int[Math.max(needed, 3 * 2048)];
        }

        final int[] out = rtx$visibleSectionScratch;
        int write = 0;
        for (int i = 0; i < count; i++) {
            final long node = sections.get(i).getSectionNode();
            out[write++] = SectionPos.x(node);
            out[write++] = SectionPos.y(node);
            out[write++] = SectionPos.z(node);
        }

        NativeVulkan.setVisibleSections(out, count);
    }

    @Inject(method = "render", at = @At("HEAD"))
    private void rtx$beginEntityRtFrame(
            com.mojang.blaze3d.resource.GraphicsResourceAllocator resourceAllocator,
            boolean renderOutline,
            CameraRenderState cameraState,
            com.mojang.renderpearl.api.buffers.GpuBufferSlice terrainFog,
            org.joml.Vector4f fogColor,
            boolean shouldRenderSky,
            boolean renderDebug,
            CallbackInfo ci) {
        // 2026-11-09：整段 level render 的墙钟起点（含本 HEAD 内的我们的 JNI）。
        com.example.client.FrameBudget.beginLevelRender();
        com.example.client.LevelPhaseProbe.beginFrame();
        com.example.client.FrameBudget.beginCaptureHead();
        rtx$partialTick = Minecraft.getInstance().getDeltaTracker().getGameTimeDeltaPartialTick(false);
        rtx$logJavaMemory();
        long rtx$tV0 = System.nanoTime();
        VoxyLodManager.tick();
        long rtx$t0 = System.nanoTime();
        DhBridge.tick();
        OrionLodManager.tick(cameraState != null ? cameraState.pos : null);
        long rtx$t1 = System.nanoTime();
        VoxyLodManager.perfAdd(VoxyLodManager.PERF_DH, rtx$t1 - rtx$t0);
        FarFieldGate.updateNearby(cameraState, viewArea);
        long rtx$t2 = System.nanoTime();
        VoxyLodManager.perfAdd(VoxyLodManager.PERF_FFG, rtx$t2 - rtx$t1);
        EntityRtCapture.endHandPhase();
        EntityRtCapture.beginEarlyFrame();
        ParticleRtCapture.beginFrame();
        DynamicLightCapture.submitFrame();
        long rtx$t3 = System.nanoTime();
        rtx$uploadVisibleSections(cameraState);
        long rtx$t4 = System.nanoTime();
        VoxyLodManager.perfAdd(VoxyLodManager.PERF_CAPTURE, rtx$t4 - rtx$t2);
        com.example.client.FrameBudget.recordCaptureHeadBreakdown(
                (rtx$t0 - rtx$tV0) / 1_000_000.0,
                (rtx$t1 - rtx$t0) / 1_000_000.0,
                (rtx$t2 - rtx$t1) / 1_000_000.0,
                (rtx$t3 - rtx$t2) / 1_000_000.0,
                (rtx$t4 - rtx$t3) / 1_000_000.0);
        com.example.client.FrameBudget.endCaptureHead();
    }

    @Inject(method = "submitEntities", at = @At("HEAD"))
    private void rtx$beginSubmitEntities(
            PoseStack poseStack,
            LevelRenderState levelRenderState,
            SubmitNodeCollector output,
            CallbackInfo ci) {
        // 2026-11-09：实体提交整段（含逐模型捕获的我们的 JNI）。
        com.example.client.FrameBudget.beginSubmitEntities();
    }

    @Inject(method = "submitEntities", at = @At("TAIL"))
    private void rtx$submitLocalPlayerProxy(
            PoseStack poseStack,
            LevelRenderState levelRenderState,
            SubmitNodeCollector output,
            CallbackInfo ci) {
        com.example.client.FrameBudget.endSubmitEntities();
        var player = Minecraft.getInstance().player;
        if (player == null) {
            EntityRtCapture.disarmLocalPlayerProxyFallback();
            return;
        }

        for (EntityRenderState state : levelRenderState.entityRenderStates) {
            if (state instanceof AvatarRenderState avatarState && avatarState.id == player.getId()) {
                EntityRtCapture.disarmLocalPlayerProxyFallback();
                return;
            }
        }

        EntityRenderState state = this.entityRenderDispatcher.extractEntity(player, rtx$partialTick);
        EntityRtCapture.markLocalPlayerProxyState(state);
        if (EntityRtCapture.isDiagnosticFrame()) {
            System.err.println("[RTX-PROXY] synthetic proxy submitted, state="
                    + (state == null ? "null" : state.getClass().getSimpleName()));
        }
        CameraRenderState cameraState = levelRenderState.cameraRenderState;
        var cameraPos = cameraState.pos;
        EntityRtCapture.beginSyntheticLocalPlayerSubmit();
        try {
            this.entityRenderDispatcher.submit(
                    state, cameraState,
                    state.x - cameraPos.x(), state.y - cameraPos.y(), state.z - cameraPos.z(),
                    poseStack, output
            );
        } finally {
            EntityRtCapture.endSyntheticLocalPlayerSubmit();
        }
    }

    @Inject(method = "render", at = @At("RETURN"))
    private void onRenderLevel(
            com.mojang.blaze3d.resource.GraphicsResourceAllocator resourceAllocator,
            boolean renderOutline,
            CameraRenderState cameraState,
            com.mojang.renderpearl.api.buffers.GpuBufferSlice terrainFog,
            org.joml.Vector4f fogColor,
            boolean shouldRenderSky,
            boolean renderDebug,
            CallbackInfo ci) {

        EntityRtCapture.endSyntheticLocalPlayerSubmit();
        EntityRtCapture.disarmLocalPlayerProxyFallback();

        // 2026-11-09：level render 墙钟终点（第一句，保证覆盖整段）。
        com.example.client.FrameBudget.endLevelRender();
        com.example.client.LevelPhaseProbe.endFrame();

        DhBridge.markFrameEnded();

        if (NativeVulkan.handlesPending) {
            System.err.println("[HEART PROBE] LevelRenderer Triggering Init!");
            NativeVulkan.initDeviceHandles(
                    NativeVulkan.pendingInstanceHandle,
                    NativeVulkan.pendingPhysicalDeviceHandle,
                    NativeVulkan.pendingDeviceHandle,
                    NativeVulkan.pendingQueueHandle,
                    NativeVulkan.pendingQueueFamilyIndex
            );
            NativeVulkan.handlesPending = false;
        }

        ParticleRtCapture.endFrame();
    }

    // ── 2026-11-11：level 段 15.5 ms 内部归因（ORION_LEVEL_PHASES=1 才有输出） ──
    // submitFeatures = 实体 / 方块实体 / 破坏动画 / 粒子 / gizmo 的提交（原版，纯浪费的一半嫌疑）
    @Inject(method = "submitFeatures", at = @At("HEAD"))
    private void rtx$phaseSubmitHead(
            LevelRenderState levelRenderState,
            SubmitNodeCollector output,
            boolean renderOutline,
            CallbackInfo ci) {
        com.example.client.LevelPhaseProbe.submitHead();
    }

    @Inject(method = "submitFeatures", at = @At("RETURN"))
    private void rtx$phaseSubmitTail(
            LevelRenderState levelRenderState,
            SubmitNodeCollector output,
            boolean renderOutline,
            CallbackInfo ci) {
        com.example.client.LevelPhaseProbe.submitTail();
    }

    // prepareChunkRenders* = 可见段遍历 + draw group 构建 + 动态 UBO 写入（差异分配的大头）
    @Inject(method = "prepareChunkRenders", at = @At("HEAD"))
    private void rtx$phasePrepareHead(org.joml.Matrix4fc matrix, boolean respectOrder, CallbackInfoReturnable<net.minecraft.client.renderer.chunk.ChunkSectionsToRender> cir) {
        com.example.client.LevelPhaseProbe.prepareHead();
    }

    @Inject(method = "prepareChunkRenders", at = @At("RETURN"))
    private void rtx$phasePrepareTail(org.joml.Matrix4fc matrix, boolean respectOrder, CallbackInfoReturnable<net.minecraft.client.renderer.chunk.ChunkSectionsToRender> cir) {
        com.example.client.LevelPhaseProbe.prepareTail();
    }

    @Inject(method = "prepareChunkRendersIndirect", at = @At("HEAD"))
    private void rtx$phasePrepareIndirectHead(org.joml.Matrix4fc matrix, boolean respectOrder, CallbackInfoReturnable<net.minecraft.client.renderer.chunk.ChunkSectionsToRender> cir) {
        com.example.client.LevelPhaseProbe.prepareHead();
    }

    @Inject(method = "prepareChunkRendersIndirect", at = @At("RETURN"))
    private void rtx$phasePrepareIndirectTail(org.joml.Matrix4fc matrix, boolean respectOrder, CallbackInfoReturnable<net.minecraft.client.renderer.chunk.ChunkSectionsToRender> cir) {
        com.example.client.LevelPhaseProbe.prepareTail();
    }

    /**
     * 2026-11-11 接管式渲染第一层，**变体 A**：跳过地形的 draw group 构建。
     *
     * <p>这是 DrawIndirect 与 DrawSeparate 两条路径的<b>共同上游</b>。返回 0 后
     * 每个 layer 的 draw group 为空、{@code sectionInfos} 为空、{@code largestIndexCount}=0，
     * 于是下游 {@code renderGroup} 的空判断自然把 {@code drawIndexedIndirect} /
     * {@code drawMultipleIndexed} 全部跳过 —— 地形不再进入命令缓冲，
     * 遍历与逐段分配也一起消失。RT 的深度是自己写的，不消费原版深度，
     * 详见 {@code TerrainTakeover} 的类注释。
     *
     * <p><b>默认不走这条</b>（{@code ORION_TAKEOVER_EXTRACT=1} 才开）。2026-11-11 实测
     * 变体 A 会让 {@code rebuildWorldGeometry} 的 {@code pCap}（等区块锁）从 2 ms 涨到
     * 65~200 ms；默认改用变体 B（在 {@code renderGroup} 取消录制），保留本方法全部簿记。
     */
    @Inject(method = "extractSectionDrawGroups", at = @At("HEAD"), cancellable = true)
    private void rtx$takeoverSkipTerrainDrawGroups(
            boolean respectTranslucentOrder,
            java.util.List<?> sectionInfos,
            java.util.Map<?, ?> drawGroups,
            CallbackInfoReturnable<Integer> cir) {
        if (com.example.client.TerrainTakeover.skipTerrainExtraction()) {
            cir.setReturnValue(0);
        }
    }
}
