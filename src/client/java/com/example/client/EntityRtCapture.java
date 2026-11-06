package com.example.client;

import com.example.client.mixin.RenderSetupAccessor;
import com.example.client.mixin.RenderTypeAccessor;
import com.example.client.mixin.TextureBindingAccessor;
import com.example.vulkan.NativeVulkan;
import com.mojang.renderpearl.api.textures.GpuSampler;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import com.mojang.math.Axis;
import com.example.client.config.RTXSettingsConfig;
import com.mojang.renderpearl.backend.vulkan.VulkanGpuSampler;
import com.mojang.renderpearl.backend.vulkan.VulkanGpuTextureView;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Locale;
import java.util.Set;
import net.minecraft.client.gui.Font;
import net.minecraft.client.gui.font.TextRenderable;
import net.minecraft.client.model.Model;
import net.minecraft.client.renderer.blockentity.BeaconRenderer;
import net.minecraft.client.renderer.blockentity.state.BeaconRenderState;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import net.minecraft.client.renderer.feature.ModelFeatureRenderer;
import net.minecraft.client.renderer.rendertype.RenderSetup;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.client.renderer.texture.AbstractTexture;
import net.minecraft.client.Minecraft;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.ResourceManager;
import net.minecraft.util.RandomSource;
import org.joml.Matrix4f;
import org.lwjgl.system.MemoryUtil;

/** Captures 26.2 entity-model batches and the explicit experience-orb quad. */
public final class EntityRtCapture {
    private static final int MAX_TEXTURE_SLOTS = 128;
    private static final int MAX_REGULAR_TEXTURE_SLOTS = MAX_TEXTURE_SLOTS - 1;
    private static final int BYTES_PER_VERTEX = 12 * Float.BYTES;
    private static final Identifier EXPERIENCE_ORB_TEXTURE =
            Identifier.withDefaultNamespace("textures/entity/experience/experience_orb.png");
    private static final Identifier LIGHTNING_TEXTURE =
            Identifier.fromNamespaceAndPath("orion", "textures/rt_lightning.png");
    private static final Identifier[] frameTextureSlots = new Identifier[MAX_TEXTURE_SLOTS];
    private static final String[] frameTextureLabels = new String[MAX_TEXTURE_SLOTS];
    private static int frameTextureSlotCount;
    private static final int[] slotLastSeenFrame = new int[MAX_TEXTURE_SLOTS];
    private static final float MATERIAL_CLASS_SCALE = 32.0f;
    private static final int MATERIAL_CLASS_DEFAULT = 0;
    private static final int MATERIAL_CLASS_IRON = 1;
    private static final int MATERIAL_CLASS_GOLD = 2;
    private static final int MATERIAL_CLASS_DIAMOND = 3;
    private static final int MATERIAL_CLASS_NETHERITE = 4;
    private static final int MATERIAL_CLASS_COPPER = 5;
    private static final int MATERIAL_CLASS_SOFT = 6;
    

    private static final int MATERIAL_CLASS_UNCUTOUT = 7;
    private static final int[] frameTextureClasses = new int[MAX_TEXTURE_SLOTS];
    private static int captureFrameSerial;
    private static final int SLOT_EVICTION_GRACE_FRAMES = 3;
    private static final float ENTITY_COPLANAR_LAYER_STEP = 0.002f;
    private static final float ENTITY_COPLANAR_LAYER_MAX = 0.02f;
    private static final long[] publishedEntityViews = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntitySamplers = new long[MAX_TEXTURE_SLOTS];
    private static final boolean[] slotTexturePublished = new boolean[MAX_TEXTURE_SLOTS];
    private static final java.util.HashMap<Identifier, Identifier[]> ENTITY_PBR_SIBLINGS =
            new java.util.HashMap<>();
    private static final long[] publishedEntityNormalViews = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntityNormalSamplers = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntitySpecularViews = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntitySpecularSamplers = new long[MAX_TEXTURE_SLOTS];
    private static final boolean[] entityPbrSettled = new boolean[MAX_TEXTURE_SLOTS];
    private static boolean entityPbrNativeAvailable = true;
    private static final ThreadLocal<ActiveBatch> activeBatch = new ThreadLocal<>();
    private static final ThreadLocal<ActiveBatch> reusableBatch =
            ThreadLocal.withInitial(ActiveBatch::new);
    private static final Set<Object> localPlayerProxyStates =
            Collections.newSetFromMap(new IdentityHashMap<>());
    private static boolean localPlayerProxyAvatarPending;
    private static Identifier localPlayerProxyBodyTexture;
    private static final ThreadLocal<ActiveBatch> textBatch = ThreadLocal.withInitial(ActiveBatch::new);
    private static int textBatchSlot;
    private static boolean captureFrameActive;

    private static RenderType lastGlyphRenderType;
    private static int lastGlyphSlot;

    private static boolean textPerfFrame;
    private static long textCaptureNanos;
    private static long textUploadNanos;
    private static long textLookupNanos;
    private static int textGlyphs;
    private static int textVertices;
    private static int textBytes;
    private static int textUploads;
    private static long textEmitNanos;
    private static long textGapNanos;
    private static long lastGlyphHeadNanos;
    private static long lastGlyphTailNanos;

    

    public static final boolean SKIP_VANILLA_WORLD_TEXT = resolveSkipVanillaWorldText();

    private static boolean resolveSkipVanillaWorldText() {
        String raw = System.getProperty("orion.skipVanillaWorldText");
        if (raw == null) {
            raw = System.getenv("ORION_SKIP_VANILLA_WORLD_TEXT");
        }
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    private static long textPathNanos;
    private static long lastGlyphEntryNanos;

    private static long evictionsInUseTotal;

    private static int textSlotChanges;
    private static int evictionsInUse;
    private static final java.util.HashSet<Identifier> textFrameTextures =
            new java.util.HashSet<>();

    private EntityRtCapture() {}

    private static boolean entityFrameOpen;

    /**
     * 每帧复位开窗标记，由 {@code GameRendererMixin} 在 {@code GameRenderer.render}
     * 的 HEAD 调用。它只清 Java 侧标记，不碰原生状态：窗口现在可能由手部提前打开、
     * 由 {@code LevelRenderer} 收帧，中间若因为关卡未渲染而缺了收帧那一步，标记会一直
     * 挂着 —— 那会导致之后手部再也不开窗（手又消失）。这里保证每帧重新开始。
     */
    public static void resetEntityFrameOpen() {
        entityFrameOpen = false;
    }

    /**
     * 打开实体帧窗口，**幂等**（每帧恰恰一次原生 {@code beginEntityFrame()}）。
     *
     * <p>⚠ 2026-10-25 修正（旧注释与 26.3 源码不符）：26.3 的实际顺序是
     * {@code GameRenderer.renderLevel()} → {@code levelRenderer.render(...)}（世界）
     * → {@code render3dHud(...)} → {@code renderItemInHand(...)}（第一人称手/手持物）。
     * 也就是说<b>手在世界之后提交</b>，而不是旧注释写的"手在世界之前"。</p>
     *
     * <p>由此得到的真实故障（2026-10-25 现场核对）：旧代码在
     * {@code LevelRenderer.render} 的 RETURN 就调用 {@code endEntityFrame()}，
     * 而手的 feature 批次要到之后 {@code render3dHud} 里才 prepareGroup ⇒
     * Java 侧照常捕获（slot、LabPBR 都解析过），但 {@code NativeVulkan.uploadEntityBatch}
     * 撞上原生门禁 {@code if(!g_entityFrameOpen) return;} 被<b>静默丢弃</b>。
     * 结论：<b>手是有的（有几何、有材质），只是完全不参与光追</b>。</p>
     *
     * <p>所以窗口必须跨"世界 + 手"两段：世界开始时在这里打开，手提交完由
     * {@link #finishEntityFrame()} 收帧（见 {@code GameRendererMixin} 的 prepareFrame
     * 注入点）；第三人称/无 HUD 等没有手的帧由 {@code renderLevel} 的 RETURN 兜底。</p>
     */
    public static void beginEarlyFrame() {
        if (entityFrameOpen) {
            return;
        }
        ++entityMotionFrame;
        if (--entityMotionLogCountdown <= 0) {
            entityMotionLogCountdown = 600;
            pruneEntityMotionTrackers();
        }
        beginFrame();
        NativeVulkan.beginEntityFrame();
    }

    private static boolean handPhase;

    /**
     * 进入手部阶段：开窗 + 标记"这一段提交的是手"。
     *
     * <p>⚠ 2026-10-25：<b>没有调用者，是死代码，不要再接回来。</b> 原来调用它的
     * {@code FirstPersonHandsAndItemsRendererMixin} 已经从仓库里删除（grep 可证）。
     * 更关键的是这个标记的两条用途现在<b>都是错的</b>：</p>
     * <ul>
     *   <li>顶点旋转（{@link #captureVertex} 里的 {@code handRotation}）：26.3 的
     *       {@code GameRenderer.renderItemInHand} 已经把
     *       {@code viewRotationMatrix.invert()} 乘进 PoseStack（GameRenderer.java:384），
     *       顶点已是"世界朝向 + 相机相对"，与 ENTITY 层同口径 ⇒ 再旋一次会把
     *       手转到侧面/背后；</li>
     *   <li>代理豁免（{@link #isLocalPlayerProxyState}）：26.3 第一人称手臂走
     *       {@code submitModelPart}，state 是 {@code Unit.INSTANCE}（不是
     *       {@code AvatarRenderState}），本来就不会命中 id 回退，不需要豁免。</li>
     * </ul>
     * <p>因此手的帧窗口问题改由 {@link #beginEarlyFrame()} + {@link #finishEntityFrame()}
     * 的时序解决，与 handPhase 无关。</p>
     */
    public static void beginHandPhase() {
        if (!handPhaseLogged) {
            handPhaseLogged = true;
            System.out.println("[RTX-HAND] first-person hand capture window opened");
        }
        handPhase = true;
        beginEarlyFrame();
    }

    private static boolean handPhaseLogged;
    private static boolean endPortalLogged;
    private static boolean endPortalBatchLogged;
    private static boolean endPortalSyntheticLogged;
    private static boolean endPortalSyntheticSlotLogged;
    private static boolean lightningSyntheticLogged;
    private static boolean lightningSyntheticSlotLogged;
    private static boolean beaconSyntheticLogged;
    private static boolean beaconSyntheticSlotLogged;

    /** 手部阶段结束（世界渲染开始，由 {@code LevelRendererMixin} 调用）。 */
    public static void endHandPhase() {
        handPhase = false;
    }

    public static void beginFrame() {
        com.example.client.cull.EntityVisibilityCuller.tick();
        if (activeBatch.get() != null) throw new IllegalStateException("Entity model capture leaked across frames");
        ++captureFrameSerial;
        entityFrameOpen = true;
        textBatchSlot = 0;
        captureFrameActive = true;

        textPerfFrame = isDiagnosticFrame();
        lastGlyphRenderType = null;
        lastGlyphSlot = 0;
        textCaptureNanos = 0L;
        textUploadNanos = 0L;
        textLookupNanos = 0L;
        textGlyphs = 0;
        textVertices = 0;
        textBytes = 0;
        textUploads = 0;
        textSlotChanges = 0;
        evictionsInUse = 0;
        textPathNanos = 0L;
        lastGlyphEntryNanos = 0L;
        textFrameTextures.clear();
    }

    /**
     * Closes the Java-side capture window (text run flush + counters). The text
     * run is the one batch that is not bounded by a single model submit, so it
     * is flushed here rather than in endModel's caller.
     *
     * <p>⚠ 2026-10-25：不要直接调用它来收帧 —— 必须走 {@link #finishEntityFrame()}，
     * 那个方法才会连同 {@code NativeVulkan.endEntityFrame()} 一起收（原生门禁是
     * {@code g_entityFrameOpen}，只关 Java 标记会让手/实体几何继续被静默丢弃）。</p>
     */
    public static void endFrame() {
        captureFrameActive = false;
        entityFrameOpen = false;
        flushTextBatch();

        if (textPerfFrame && (textGlyphs != 0 || textUploads != 0)) {
            System.out.println(
                    "[TEXT PERF]"
                            + " glyphs=" + textGlyphs
                            + " verts=" + textVertices
                            + " bytes=" + textBytes
                            + " uploads=" + textUploads
                            + " lookupUs=" + (textLookupNanos / 1000L)
                            + " captureUs=" + (textCaptureNanos / 1000L)
                            + " uploadUs=" + (textUploadNanos / 1000L)
                            + " textPathUs=" + (textPathNanos / 1000L)
                            + " tex=" + textFrameTextures.size()
                            + " changes=" + textSlotChanges
                            + " evictInUse=" + evictionsInUse
                            + " evictInUseTotal=" + evictionsInUseTotal
                            + " tableSize=" + frameTextureSlotCount
                            + " disabled=" + (TEXT_CAPTURE_ENABLED ? 0 : 1));
        }
    }

    /**
     * 2026-10-25：收实体帧 —— Java 标记 + 原生 {@code endEntityFrame()} 一起收，幂等。
     *
     * <p>调用时序（26.3）：</p>
     * <ol>
     *   <li>{@code LevelRenderer.render} HEAD → {@link #beginEarlyFrame()} 开窗（世界实体）；</li>
     *   <li>{@code GameRenderer.render3dHud → renderItemInHand}：手/手持物提交，
     *       {@code FeatureRenderDispatcher.prepareFrame} 返回后由 {@code GameRendererMixin}
     *       调用本方法 —— 此时手顶点已经进了实体批次，且<b>早于</b>
     *       {@code createRenderPass("Item in hand")}（RT 注入就发生在那个 render pass
     *       的 vkCmdBeginRendering 之前），所以同帧就能看到手，没有一帧延迟；</li>
     *   <li>第三人称 / 隐藏 HUD / 无手帧走不到上面那一步 ⇒
     *       {@code GameRenderer.renderLevel} 的 RETURN 兜底调用本方法。</li>
     * </ol>
     *
     * <p>为什么必须在这里收而不是在 {@code LevelRenderer.render} 的 RETURN：手晚于世界，
     * 提前收会把手的批次挡在原生门禁 {@code if(!g_entityFrameOpen) return;} 外面
     * —— 这正是 2026-10-25 之前"手完全不参与光追"的原因。</p>
     */
    public static void finishEntityFrame() {
        if (!entityFrameOpen) {
            return;
        }
        endFrame();
        NativeVulkan.endEntityFrame();
    }

    

    private static final boolean TEXT_CAPTURE_ENABLED = resolveTextCaptureEnabled();

    private static boolean resolveTextCaptureEnabled() {
        String raw = System.getProperty("orion.textCapture");
        if (raw == null) {
            raw = System.getenv("ORION_TEXT_CAPTURE");
        }
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    /**
     * Mirrors one glyph quad emitted by TextFeatureRenderer into the entity batch.
     *
     * @return true = 本字形**已经**镜像进实体批次，原版那一份可以被跳过（见
     *         {@link #SKIP_VANILLA_WORLD_TEXT}）；false = 没有镜像成功，原版必须照旧画。
     */
    public static boolean captureTextGlyph(
            TextRenderable glyph,
            Matrix4f pose,
            int lightCoords,
            Font.DisplayMode displayMode) {
        if (!captureFrameActive) return false;

        if (textPerfFrame) {
            final long glyphEntryNanos = System.nanoTime();
            if (lastGlyphEntryNanos != 0L) {
                textPathNanos += glyphEntryNanos - lastGlyphEntryNanos;
            }
            lastGlyphEntryNanos = glyphEntryNanos;
        }

        if (!TEXT_CAPTURE_ENABLED) {
            textGlyphs += 1;
            return false;
        }

        RenderType renderType = glyph.renderType(displayMode);
        if (renderType == null) return false;

        int textureSlot;

        if (renderType == lastGlyphRenderType) {
            textureSlot = lastGlyphSlot;
        } else {
            final long textLookupStartNanos = textPerfFrame ? System.nanoTime() : 0L;

            Identifier texture = resolveSampler0(renderType);
            if (texture == null) return false;

            textureSlot = slotFor(texture);
            if (textureSlot == 0) return false;

            if (textPerfFrame) {
                textLookupNanos += System.nanoTime() - textLookupStartNanos;
                textFrameTextures.add(texture);
            }

            lastGlyphRenderType = renderType;
            lastGlyphSlot = textureSlot;
        }

        ActiveBatch batch = textBatch.get();
        if (textBatchSlot != 0 && textureSlot != textBatchSlot) {
            if (textPerfFrame) ++textSlotChanges;
            flushTextBatch();
        }
        if (textBatchSlot == 0) {
            batch.reset(textureSlot, false, false);
            textBatchSlot = textureSlot;
        }

        final long textStartNanos = textPerfFrame ? System.nanoTime() : 0L;
        final int textVerticesBefore = batch.vertexCount;

        CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            glyph.render(pose, consumer, lightCoords, false);
        } finally {
            consumer.finish();
        }

        textGlyphs += 1;
        textVertices += batch.vertexCount - textVerticesBefore;

        if (textPerfFrame) {
            textCaptureNanos += System.nanoTime() - textStartNanos;
        }

        return true;
    }

    private static void flushTextBatch() {
        if (textBatchSlot == 0) return;
        textBatchSlot = 0;
        ActiveBatch batch = textBatch.get();
        if (batch.vertexCount != 0) {
            final long textUploadStartNanos = textPerfFrame ? System.nanoTime() : 0L;

            uploadBatch(batch);

            if (textPerfFrame) {
                textUploadNanos += System.nanoTime() - textUploadStartNanos;
            }

            textUploads += 1;
            textBytes += batch.vertexCount * BYTES_PER_VERTEX;
        }
    }

    public static boolean isDiagnosticFrame() {
        return (captureFrameSerial & 63) == 1;
    }

    /**
     * A resource reload destroys Vulkan texture views while this class keeps
     * lightweight handle de-duplication state. The numeric handle may be
     * reused by Vulkan, so comparing it across reloads is not valid.
     */
    public static void invalidateTextureCache() {
        frameTextureSlotCount = 0;
        Arrays.fill(frameTextureSlots, null);
        Arrays.fill(frameTextureLabels, null);
        Arrays.fill(slotLastSeenFrame, -1);
        Arrays.fill(publishedEntityViews, 0L);
        Arrays.fill(publishedEntitySamplers, 0L);
        Arrays.fill(slotTexturePublished, false);
        ENTITY_PBR_SIBLINGS.clear();
        Arrays.fill(publishedEntityNormalViews, 0L);
        Arrays.fill(publishedEntityNormalSamplers, 0L);
        Arrays.fill(publishedEntitySpecularViews, 0L);
        Arrays.fill(publishedEntitySpecularSamplers, 0L);
        Arrays.fill(entityPbrSettled, false);
    }

    public static void beginModel(ModelFeatureRenderer.Submit<?> submit) {
        beginModel(submit, false);
    }

    public static void markLocalPlayerProxyState(Object state) {
        if (localPlayerProxyStates.size() >= 8) localPlayerProxyStates.clear();
        localPlayerProxyStates.add(state);
        localPlayerProxyAvatarPending = state instanceof AvatarRenderState;
        localPlayerProxyBodyTexture = null;
        if (state instanceof AvatarRenderState avatarState && avatarState.skin != null) {
            localPlayerProxyBodyTexture = avatarState.skin.body().texturePath();
        }
    }

    public static void disarmLocalPlayerProxyFallback() {
        localPlayerProxyAvatarPending = false;
        localPlayerProxyBodyTexture = null;
    }

    public static boolean isLocalPlayerProxyState(ModelFeatureRenderer.Submit<?> submit) {
        if (handPhase) return false;
        if (localPlayerProxyStates.contains(submit.state())) return true;
        if (!localPlayerProxyAvatarPending) return false;
        var player = Minecraft.getInstance().player;
        boolean idHit = player != null
                && submit.state() instanceof AvatarRenderState avatarState
                && avatarState.id == player.getId();
        if (isDiagnosticFrame() && idHit) {
            System.err.println("[RTX-PROXY] proxy recognized via id fallback (state copy)");
        }
        return idHit;
    }

    public static boolean isLocalPlayerProxyState(Object renderState) {
        return localPlayerProxyStates.contains(renderState);
    }

    /** Captures the local-player body before it reaches SubmitNodeCollector. */
    public static <S> void captureLocalPlayerModel(
            Model<? super S> model,
            S state,
            PoseStack poseStack,
            RenderType renderType,
            int lightCoords,
            int overlayCoords,
            int tintedColor) {
        Identifier texture = resolveSampler0(renderType);
        if (texture == null) {
            if (isDiagnosticFrame()) {
                System.err.println("[RTX-PROXY] captureLocalPlayerModel sampler0 resolve FAILED");
            }
            return;
        }

        publishTexture(MAX_TEXTURE_SLOTS, texture);
        ActiveBatch batch = acquireBatch(MAX_TEXTURE_SLOTS, true, false);
        activeBatch.set(batch);
        CapturingVertexConsumer captureConsumer = batch.captureConsumer();
        try {
            model.setupAnim(state);
            model.renderToBuffer(poseStack, captureConsumer, lightCoords, overlayCoords, tintedColor);
        } finally {
            captureConsumer.finish();
            endModel();
        }
    }

    public static void captureLocalPlayerProxy(
            ModelFeatureRenderer.Submit<?> submit, PoseStack poseStack) {
        beginModel(submit, true);
        ActiveBatch batch = activeBatch.get();
        if (batch == null) return;

        CapturingVertexConsumer captureConsumer = batch.captureConsumer();
        try {
            renderModelToCaptureConsumer(submit, poseStack, captureConsumer);
        } finally {
            captureConsumer.finish();
            endModel();
        }
    }

    private static <S> void renderModelToCaptureConsumer(
            ModelFeatureRenderer.Submit<S> submit,
            PoseStack poseStack,
            VertexConsumer captureConsumer) {
        Model<? super S> model = submit.model();
        model.setupAnim(submit.state());
        model.renderToBuffer(
                poseStack,
                captureConsumer,
                submit.lightCoords(),
                submit.overlayCoords(),
                submit.tintedColor()
        );
    }

    private static final java.util.HashMap<
            net.minecraft.world.entity.EntityType<?>, java.util.ArrayList<double[]>>
            entityMotionTrackers = new java.util.HashMap<>();

    

    private static final boolean LIMB_MV_ENABLED = resolveLimbMvEnabled();

    private static boolean resolveLimbMvEnabled() {
        String raw = System.getProperty("orion.limbMv");
        if (raw == null) {
            raw = System.getenv("ORION_LIMB_MV");
        }
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    

    private static final java.util.HashMap<
            double[], java.util.HashMap<ModelBatchKey, VertexHistory>>
            entityVertexHistories = new java.util.HashMap<>();

    private static final int ENTITY_VERTEX_HISTORY_MAX_KEYS = 32;

    /** 一批模型几何的稳定身份：同一实体的同一提交逐帧对应。 */
    private record ModelBatchKey(Model<?> model, RenderType renderType) {}

    /** 一条 (模型, RenderType) 批次的上一帧顶点位置（相机相对，与顶点缓冲同序）。 */
    private static final class VertexHistory {
        private int frame = Integer.MIN_VALUE;
        private float[] positions;
    }

    private static int entityMotionFrame;

    private static final double ENTITY_MOTION_MATCH_RADIUS_SQ = 64.0;

    private static final int ENTITY_MOTION_MAX_AGE_FRAMES = 60;

    private static final int ENTITY_MOTION_MAX_TRACKERS_PER_TYPE = 192;

    private static int entityMotionLogCountdown;

    

    private static final int T_X = 0;
    private static final int T_Y = 1;
    private static final int T_Z = 2;
    private static final int T_FRAME = 3;
    private static final int T_MOTION = 4;
    private static final int T_DELTA = 7;
    private static final int T_POSE = 19;
    private static final int T_DELTA_VALID = 35;
    private static final int T_POSE_VALID = 36;
    private static final int T_LEN = 37;

    private static final ThreadLocal<float[]> poseScratch =
            ThreadLocal.withInitial(() -> new float[16]);

    private static double[] entityMotionTracker(double x, double y, double z) {
        final double[] record = new double[T_LEN];
        record[T_X] = x;
        record[T_Y] = y;
        record[T_Z] = z;
        record[T_FRAME] = entityMotionFrame;
        setIdentityDelta(record);
        entityVertexHistories.put(record, new java.util.HashMap<>());
        return record;
    }

    /** 把增量置成恒等，并标记"不可用"（消费端会退化成"这一批不动"）。 */
    private static void setIdentityDelta(double[] record) {
        record[T_DELTA + 0] = 1.0;
        record[T_DELTA + 1] = 0.0;
        record[T_DELTA + 2] = 0.0;
        record[T_DELTA + 3] = 0.0;
        record[T_DELTA + 4] = 0.0;
        record[T_DELTA + 5] = 1.0;
        record[T_DELTA + 6] = 0.0;
        record[T_DELTA + 7] = 0.0;
        record[T_DELTA + 8] = 0.0;
        record[T_DELTA + 9] = 0.0;
        record[T_DELTA + 10] = 1.0;
        record[T_DELTA + 11] = 0.0;
        record[T_DELTA_VALID] = 0.0;
    }

    /** 记录本帧根位姿，供下一帧算增量。 */
    private static void writePose(double[] record, Matrix4f pose) {
        if (pose == null) {
            record[T_POSE_VALID] = 0.0;
            return;
        }
        final float[] scratch = poseScratch.get();
        pose.get(scratch);
        for (int index = 0; index < 16; ++index) {
            record[T_POSE + index] = scratch[index];
        }
        record[T_POSE_VALID] = 1.0;
    }

    /**
     * delta = prevPose · curPose⁻¹，并写进 {@link #T_DELTA}。
     *
     * <p>为什么乘在左边的是 prevPose：顶点 {@code p} 现在是 {@code curPose · m}
     * （{@code m} = 模型局部坐标），它上一帧在 {@code prevPose · m}。
     * 所以 {@code p_prev = prevPose · curPose⁻¹ · p}，右乘 curPose 的逆把
     * "当前世界位置"先拉回模型局部，再用上一帧的位姿放回上一帧的世界。</p>
     *
     * <p>纯平移时这个式子退化成 {@code p + (t_prev − t_cur)}，与旧口径逐位同量。</p>
     */
    private static void writePoseDelta(double[] record, Matrix4f currentPose) {
        if (currentPose == null || record[T_POSE_VALID] == 0.0) {
            setIdentityDelta(record);
            return;
        }

        final float[] scratch = poseScratch.get();
        for (int index = 0; index < 16; ++index) {
            scratch[index] = (float) record[T_POSE + index];
        }

        final Matrix4f delta = new Matrix4f().set(scratch);
        delta.mul(new Matrix4f(currentPose).invert());

        // JOML 是列主序（mXY = 第 X 列、第 Y 行）⇒ 一行 r0 的三个线性分量是
        // m00 / m10 / m20，平移列是 m30 / m31 / m32。实测见交付文档「自证」。
        record[T_DELTA + 0] = delta.m00();
        record[T_DELTA + 1] = delta.m10();
        record[T_DELTA + 2] = delta.m20();
        record[T_DELTA + 3] = delta.m30();
        record[T_DELTA + 4] = delta.m01();
        record[T_DELTA + 5] = delta.m11();
        record[T_DELTA + 6] = delta.m21();
        record[T_DELTA + 7] = delta.m31();
        record[T_DELTA + 8] = delta.m02();
        record[T_DELTA + 9] = delta.m12();
        record[T_DELTA + 10] = delta.m22();
        record[T_DELTA + 11] = delta.m32();

        record[T_DELTA_VALID] = deltaIsIdentity(record) ? 0.0 : 1.0;
    }

    /**
     * 增量是否等价于恒等。判据刻意留 1e-6 的余量而不是严格相等：位姿矩阵每帧
     * 由 float 运算重建，静止实体也会剩下 1e-7 量级的抖动；不挡掉的话，
     * 每个静止实体每帧都会走"逐顶点算 MV"那条路（白花 CPU，且给 DLSS 喂噪声）。
     */
    private static boolean deltaIsIdentity(double[] record) {
        final double[] identity = {
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0
        };
        for (int index = 0; index < 12; ++index) {
            if (Math.abs(record[T_DELTA + index] - identity[index]) > 1.0e-6) {
                return false;
            }
        }
        return true;
    }

    /** 把记录里的增量搬到批次上（同一实体的多个部件共用第一次算出的结果）。 */
    private static void applyRecordedMotion(ActiveBatch batch, double[] record) {
        for (int index = 0; index < 12; ++index) {
            batch.poseDelta[index] = (float) record[T_DELTA + index];
        }
        batch.hasPoseDelta = record[T_DELTA_VALID] != 0.0;
        batch.motionX = (float) record[T_MOTION];
        batch.motionY = (float) record[T_MOTION + 1];
        batch.motionZ = (float) record[T_MOTION + 2];
    }

    private static double entityMotionDistanceSq(double[] record, double x, double y, double z) {
        final double dx = record[T_X] - x;
        final double dy = record[T_Y] - y;
        final double dz = record[T_Z] - z;
        return dx * dx + dy * dy + dz * dz;
    }

    /**
     * 算出这一批的**根位姿增量**并挂到批次上。
     *
     * <p>世界空间是对的：rgen 里两次投影都用**上一帧**相机，所以"世界位移"进去之后
     * 得到的正是 DLSS 需要的那一项（总位移里属于相机的那一份由 DLSS 自己加）。
     * 相机跟随的物体（本地玩家自己的身体）自动得到"屏幕上不动"的正确结果。</p>
     *
     * <p>2026-10-17：返回值从"一个平移三元组"换成"一个 3×4 仿射增量"，逐顶点在
     * {@code uploadBatch} 里应用 ⇒ 旋转（转身 / 俯仰 / 展示框摆动）终于有 MV 了。
     * 平移口径不变，所以纯平移实体的结果与旧实现**数值等价**。</p>
     */
    private static void resolveBatchMotion(
            ActiveBatch batch, ModelFeatureRenderer.Submit<?> submit) {
        final Object stateObject = submit.state();

        if (!(stateObject
                instanceof net.minecraft.client.renderer.entity.state.EntityRenderState renderState)) {
            return;
        }

        final double x = renderState.x;
        final double y = renderState.y;
        final double z = renderState.z;

        final com.mojang.blaze3d.vertex.PoseStack.Pose rootPose = submit.pose();
        final Matrix4f currentPose = rootPose != null ? rootPose.pose() : null;

        final java.util.ArrayList<double[]> trackers =
                entityMotionTrackers.computeIfAbsent(
                        renderState.entityType, key -> new java.util.ArrayList<>());

        double[] best = null;
        double bestDistance = Double.MAX_VALUE;

        for (int index = 0; index < trackers.size(); ++index) {
            final double[] record = trackers.get(index);
            if ((int)record[T_FRAME] != entityMotionFrame) {
                continue;
            }
            final double distance = entityMotionDistanceSq(record, x, y, z);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = record;
            }
        }

        if (best != null && bestDistance <= ENTITY_MOTION_MATCH_RADIUS_SQ) {
            applyRecordedMotion(batch, best);
            attachVertexHistory(batch, submit, best);
            return;
        }

        double[] candidate = null;
        double candidateDistance = Double.MAX_VALUE;

        for (int index = 0; index < trackers.size(); ++index) {
            final double[] record = trackers.get(index);
            if ((int)record[T_FRAME] == entityMotionFrame) {
                continue;
            }
            final double distance = entityMotionDistanceSq(record, x, y, z);
            if (distance < candidateDistance) {
                candidateDistance = distance;
                candidate = record;
            }
        }

        if (candidate == null) {
            // 这个类型第一次出现：没有上一帧可配对，增量恒等（首帧本来也没有历史）。
            final double[] record = entityMotionTracker(x, y, z);
            writePose(record, currentPose);
            trackers.add(record);
            // 仍然挂上历史：本帧 uploadBatch 会把顶点位置记进去，下一帧就有 p_prev 了。
            attachVertexHistory(batch, submit, record);
            return;
        }

        final int age = entityMotionFrame - (int)candidate[T_FRAME];
        final boolean paired =
                age <= ENTITY_MOTION_MAX_AGE_FRAMES
                        && candidateDistance <= ENTITY_MOTION_MATCH_RADIUS_SQ;

        final float motionX = paired ? (float)(candidate[T_X] - x) : 0.0F;
        final float motionY = paired ? (float)(candidate[T_Y] - y) : 0.0F;
        final float motionZ = paired ? (float)(candidate[T_Z] - z) : 0.0F;

        if (paired) {
            writePoseDelta(candidate, currentPose);
        } else {
            setIdentityDelta(candidate);
        }

        candidate[T_X] = x;
        candidate[T_Y] = y;
        candidate[T_Z] = z;
        candidate[T_FRAME] = entityMotionFrame;
        candidate[T_MOTION + 0] = motionX;
        candidate[T_MOTION + 1] = motionY;
        candidate[T_MOTION + 2] = motionZ;
        writePose(candidate, currentPose);

        applyRecordedMotion(batch, candidate);
        attachVertexHistory(batch, submit, candidate);

        if (trackers.size() > ENTITY_MOTION_MAX_TRACKERS_PER_TYPE) {
            pruneEntityMotionTrackers();
        }
    }

    /**
     * 把该记录所属的顶点位置历史挂到批次上。没有历史（开关关闭 / 记录刚被 prune）
     * 时保持 null ⇒ uploadBatch 退回根位姿增量。
     */
    private static void attachVertexHistory(
            ActiveBatch batch, ModelFeatureRenderer.Submit<?> submit, double[] record) {
        if (!LIMB_MV_ENABLED) {
            return;
        }
        final java.util.HashMap<ModelBatchKey, VertexHistory> history =
                entityVertexHistories.get(record);
        if (history == null) {
            return;
        }
        batch.vertexHistory = history;
        batch.vertexHistoryKey = new ModelBatchKey(submit.model(), submit.renderType());
    }

    /** 清掉太久没被匹配到的记录（以及随之变空的类型）。 */
    private static void pruneEntityMotionTrackers() {
        final java.util.Iterator<
                java.util.Map.Entry<
                        net.minecraft.world.entity.EntityType<?>,
                        java.util.ArrayList<double[]>>> types =
                entityMotionTrackers.entrySet().iterator();

        while (types.hasNext()) {
            final java.util.ArrayList<double[]> trackers = types.next().getValue();

            for (int index = trackers.size() - 1; index >= 0; --index) {
                final double[] record = trackers.get(index);
                if (entityMotionFrame - (int)record[T_FRAME] > ENTITY_MOTION_MAX_AGE_FRAMES) {
                    trackers.remove(index);
                    // 顶点位置历史与记录同生命周期，否则表会随实体消失一直涨。
                    entityVertexHistories.remove(record);
                }
            }

            if (trackers.isEmpty()) {
                types.remove();
            }
        }
    }

    private static void beginModel(
            ModelFeatureRenderer.Submit<?> submit,
            boolean localPlayerProxy) {
        if (activeBatch.get() != null) throw new IllegalStateException("Nested entity model capture");
        Identifier texture = resolveSampler0(submit.renderType());
        if (handPhase) {
            if (!handTextureSubstituted) {
                handTextureSubstituted = true;
                System.out.println("[RTX-HAND] texture probe: original="
                        + texture + " -> substituted with blocks atlas");
            }
            texture = BLOCKS_ATLAS_TEXTURE;
        }
        if (localPlayerProxy && isLocalPlayerBodyTexture(submit, texture)) {
            int textureSlot = MAX_TEXTURE_SLOTS;
            publishTexture(textureSlot, texture);
            ActiveBatch proxyBatch = acquireBatch(textureSlot, true, false);
            proxyBatch.diagHand = false;
            // 2026-11-04：Submit 这个 record 自带 uvMapping（ModelPart 0..1 → 图集矩形）。
            proxyBatch.uvMapping = submit.uvMapping();
            activeBatch.set(proxyBatch);
            resolveBatchMotion(proxyBatch, submit);
            handDiagLog("PROXY-PATH", submit, texture, textureSlot);
            return;
        }
        int textureSlot = slotFor(texture);
        // 第一人称手部（手臂 + 手持物）的 state 是 Unit.INSTANCE，不是 AvatarRenderState，
        // 所以这里不能按 state 类型过滤，只能按"第一人称"这个相机状态判断。
        final boolean handBatch = isFirstPersonHandState(submit);
        if (handBatch) {
            handDiagLog("SLOT-PATH", submit, texture, textureSlot);
        }
        if (isDiagnosticFrame()
                && submit.state() instanceof AvatarRenderState avatarState) {
            var player = Minecraft.getInstance().player;
            if (player != null && avatarState.id == player.getId()) {
                System.err.println("[RTX-PROXY] ordinary path saw local player, firstPerson="
                        + Minecraft.getInstance().options.getCameraType().isFirstPerson()
                        + ", textureNull=" + (texture == null) + ", slot=" + textureSlot);
            }
        }
        if (textureSlot != 0) {
            ActiveBatch batch = acquireBatch(textureSlot, localPlayerProxy, false);
            batch.diagHand = handBatch;
            // 2026-11-04：见 ActivaBatch.uvMapping 处的说明。
            batch.uvMapping = submit.uvMapping();
            // 2026-11-04：旗帜图案层沿法线外移固定两步。
            // 依据实测（[RTX-BANNER]）：batch 内 hits=0 ⇒ 没有重复质心，
            // 所以 rank 就等于这里给的 boost，不会被 seenBefore 干扰。
            final boolean bannerBoost = isBannerPattern(submit.renderType());
            batch.coplanarLayerBoost = bannerBoost ? 2 : 0;
            if (bannerBoost) {
                customDiagLog("banner_boost2",
                        renderTypeName(submit.renderType()), texture);
            }
            // 旗帜图案层整体外移一个 rank（见 bannerLayerBoost）。
            activeBatch.set(batch);
            resolveBatchMotion(batch, submit);
        }
    }

    private static boolean isLocalPlayerBodyTexture(
            ModelFeatureRenderer.Submit<?> submit, Identifier texture) {
        if (texture == null || !(submit.state() instanceof AvatarRenderState avatarState)) return false;
        var player = Minecraft.getInstance().player;
        return player != null
                && avatarState.id == player.getId()
                && avatarState.skin != null
                && texture.equals(avatarState.skin.body().texturePath());
    }

    public static void endModel() {
        ActiveBatch batch = activeBatch.get();
        activeBatch.remove();
        if (batch == null || batch.vertexCount == 0) {
            if (isDiagnosticFrame() && batch != null && batch.localPlayerProxy) {
                System.err.println("[RTX-PROXY] proxy batch EMPTY, slot=" + batch.textureSlot);
            }
            return;
        }
        uploadBatch(batch);
        if (batch.localPlayerProxy && isDiagnosticFrame()) {
            System.err.println("[RTX-PROXY] proxy upload slot=" + batch.textureSlot
                    + " vertices=" + batch.vertexCount);
        }
        if (handPhase && !handBatchLogged) {
            handBatchLogged = true;
            Minecraft minecraft = Minecraft.getInstance();
            final float cx = (batch.minX + batch.maxX) * 0.5f;
            final float cy = (batch.minY + batch.maxY) * 0.5f;
            final float cz = (batch.minZ + batch.maxZ) * 0.5f;
            double yaw = 0.0;
            double pitch = 0.0;
            if (minecraft.player != null) {
                yaw = Math.toRadians(minecraft.player.getYRot());
                pitch = Math.toRadians(minecraft.player.getXRot());
            }
            final double lx = -Math.sin(yaw) * Math.cos(pitch);
            final double ly = -Math.sin(pitch);
            final double lz = Math.cos(yaw) * Math.cos(pitch);
            final double dist = Math.sqrt(cx * cx + cy * cy + cz * cz);
            final double along = cx * lx + cy * ly + cz * lz;
            final double offAxis = dist > 1.0e-4
                    ? Math.toDegrees(Math.acos(Math.max(-1.0, Math.min(1.0, along / dist))))
                    : -1.0;
            final float rx = handRotation[0] * cx + handRotation[4] * cy + handRotation[8] * cz;
            final float ry = handRotation[1] * cx + handRotation[5] * cy + handRotation[9] * cz;
            final float rz = handRotation[2] * cx + handRotation[6] * cy + handRotation[10] * cz;
            final double alongM = rx * lx + ry * ly + rz * lz;
            final float sx = handRotation[0] * cx + handRotation[1] * cy + handRotation[2] * cz;
            final float sy = handRotation[4] * cx + handRotation[5] * cy + handRotation[6] * cz;
            final float sz = handRotation[8] * cx + handRotation[9] * cy + handRotation[10] * cz;
            final double alongT = sx * lx + sy * ly + sz * lz;
            handTransformMode = (alongM > along + 0.05 && alongM >= alongT)
                    ? 1
                    : (alongT > along + 0.05 ? 2 : 0);
            System.out.println("[RTX-HAND] batch slot=" + batch.textureSlot
                    + " verts=" + batch.vertexCount
                    + " center=[" + cx + "," + cy + "," + cz + "]"
                    + " dist=" + dist
                    + " alongRaw=" + along
                    + " alongM=" + alongM
                    + " alongMt=" + alongT
                    + " offAxisDeg=" + offAxis
                    + " transformMode=" + handTransformMode);
        }
    }

    private static boolean handBatchLogged;
    private static boolean vertexColorLogged;
    private static boolean handTextureSubstituted;

    // 2026-11-03 手部诊断：第一人称手臂 / 手持物到底走哪条上传路径。
    // 目的：区分"手臂走 proxy 路径（slot=128 皮肤纹理，mask 0x02 主射线不可见）"
    // 与"手持物走 slotFor 普通路径（自己的贴图，mask 0xff 可见）"。
    // 每 64 帧（isDiagnosticFrame）打印一次，一次运行就能定性。
    private static long handDiagFrame = -1L;
    private static int handDiagCount;

    private static void handDiagLog(
            String where,
            ModelFeatureRenderer.Submit<?> submit,
            Identifier texture,
            int slot) {
        if (!isDiagnosticFrame()) return;
        if (handDiagFrame != captureFrameSerial) {
            handDiagFrame = captureFrameSerial;
            handDiagCount = 0;
        }
        // 一帧最多打 12 条，够看清手部由哪几个批次组成，又不会刷屏。
        if (handDiagCount >= 12) return;
        ++handDiagCount;
        final Object state = submit.state();
        System.err.println("[RTX-HANDDIAG] " + where
                + " state=" + (state == null ? "null" : state.getClass().getSimpleName())
                + " texture=" + texture
                + " slot=" + slot
                + " renderType=" + submit.renderType()
                + " firstPerson="
                + Minecraft.getInstance().options.getCameraType().isFirstPerson());
    }

    /**
     * 2026-11-03：第一人称手部批次的判据。26.3 的手臂/手持物走
     * {@code submitModelPart}，state 是 {@code Unit.INSTANCE}（不是 AvatarRenderState），
     * 所以只能按类名判。
     */
    private static boolean isFirstPersonHandState(ModelFeatureRenderer.Submit<?> submit) {
        if (!Minecraft.getInstance().options.getCameraType().isFirstPerson()) return false;
        final Object state = submit.state();
        return state != null && "Unit".equals(state.getClass().getSimpleName());
    }

    /** 取小数部分并夹到 [0,1)：末地门合成 UV 用（顶点在整数角点上，见 captureVertex）。 */
    private static float fract01(float value) {
        final float f = value - (float) Math.floor(value);
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    private static final Identifier BLOCKS_ATLAS_TEXTURE =
            Identifier.withDefaultNamespace("textures/atlas/blocks.png");

    

    private static int handTransformMode = -1;

    

    private static final float[] handRotation = new float[] {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
    };

    // =====================================================
    // 2026-11-03 手部 MV：上一帧的相机状态。
    //
    // 手部几何是"相机相对"的（GameRenderer.renderItemInHand 已把
    // viewRotationMatrix.invert() 乘进 PoseStack），所以它每帧的世界位置变化
    // 完全由相机状态决定：
    //     p_prev = R_prev·R_cur⁻¹·p_cur + (pos_prev − R_prev·R_cur⁻¹·pos_cur)
    // 这两个 setter 每帧各调一次（VulkanCameraMixin:300-303），所以在写入本帧值
    // 之前把旧值存下来，就得到干净的上一帧。
    // =====================================================
    private static final float[] prevHandRotation = new float[] {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
    };
    private static final float[] handMotionMatrix = new float[9];
    private static float prevCameraWorldX;
    private static float prevCameraWorldY;
    private static float prevCameraWorldZ;
    private static boolean prevCameraRotationValid;
    private static boolean cameraRotationSeen;

    /** A/B 开关：-DORION_HAND_CAMERA_MV=0 关掉手部相机相对 MV。 */
    private static final boolean HAND_CAMERA_MV = resolveHandCameraMv();

    private static boolean resolveHandCameraMv() {
        String raw = System.getProperty("orion.handCameraMv");
        if (raw == null) raw = System.getenv("ORION_HAND_CAMERA_MV");
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    /**
     * 2026-11-04：UV 重映射**默认开启**（结论修正）。
     *
     * 实测三种状态的差别正好卡在"映射几次"上：
     *   关闭   ⇒ UV = ModelPart 0..1 ⇒ 落在图集透明区 ⇒ cutout 整片丢弃 ⇒ **完全没有图案**
     *             （用户观察：底色对、图案消失）
     *   一次   ⇒ 落在图案 sprite 上 ⇒ 正确
     *   两次   ⇒ 压到 sprite 一角 ⇒ 错乱
     * 所以 captureVertex 里那一次是**必要的**，上一轮"双重映射"的判断是错的。
     *
     * A/B：-DORION_ENTITY_UV_REMAP=0 关闭（复现"没有图案"）。
     */
    private static final boolean UV_REMAP_ENABLED = resolveUvRemapEnabled();

    private static boolean resolveUvRemapEnabled() {
        String raw = System.getProperty("orion.entityUvRemap");
        if (raw == null) raw = System.getenv("ORION_ENTITY_UV_REMAP");
        if (raw == null) return true;
        return !"0".equalsIgnoreCase(raw.trim());
    }

    public static void setHandRotation(float[] columnMajor) {
        if (columnMajor == null || columnMajor.length < 16) return;
        System.arraycopy(handRotation, 0, prevHandRotation, 0, 16);
        System.arraycopy(columnMajor, 0, handRotation, 0, 16);
        prevCameraRotationValid = cameraRotationSeen;
        cameraRotationSeen = true;
    }

    

    private static float cameraWorldX;
    private static float cameraWorldY;
    private static float cameraWorldZ;

    public static void setCameraOrigin(float x, float y, float z) {
        prevCameraWorldX = cameraWorldX;
        prevCameraWorldY = cameraWorldY;
        prevCameraWorldZ = cameraWorldZ;
        cameraWorldX = x;
        cameraWorldY = y;
        cameraWorldZ = z;
    }

    private static void uploadBatch(ActiveBatch batch) {
        batch.pendingQuadVertexCount = 0;
        batch.buffer.flip();
        if (!endPortalBatchLogged && batch.textureSlot >= 1
                && batch.textureSlot <= MAX_TEXTURE_SLOTS) {
            final String endPortalLabel = frameTextureLabels[batch.textureSlot - 1];
            if (endPortalLabel != null && endPortalLabel.contains("end_portal")) {
                endPortalBatchLogged = true;
                System.out.println("[RTX-ENTITY] end portal batch upload slot="
                        + batch.textureSlot + " vertices=" + batch.vertexCount
                        + " label=" + endPortalLabel);
            }
        }
        // =====================================================
        // 2026-11-03：第一人称手部（手臂 + 手持物）的运动矢量。
        //
        // resolveBatchMotion 对手臂直接 return（它的 state 是 Unit，不是
        // EntityRenderState），手持物走 custom 路径压根不调那个函数 ⇒ 两条路径
        // 都拿不到 motion ⇒ MV 恒为 0。
        //
        // 而手部是"相机相对"的：转视角/走动时它的世界位置每帧都在变。MV=0 等于告诉
        // DLSS"这个表面相对世界静止"，DLSS 于是按相机运动去重投影它 —— 但手明明
        // 没跟着动，重投影落到错误位置，表现为闪动 + 鬼影。
        //
        // 这里用相机矩阵直接构造 poseDelta（3×4 行主序，p_prev = M·p_cur + t）：
        //     M = R_prev·R_cur⁻¹      t = pos_prev − M·pos_cur
        // handRotation 存的是相机旋转的**逆**（column-major），所以 R = (R⁻¹)ᵀ，
        // 即 R_ij = inv[i*4+j]。
        // A/B：-DORION_HAND_CAMERA_MV=0 关闭。
        // =====================================================
        if (HAND_CAMERA_MV && batch.diagHand && !batch.hasPoseDelta
                && prevCameraRotationValid) {
            final float[] hp = prevHandRotation;
            final float[] hc = handRotation;
            final float[] m = handMotionMatrix;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    float sum = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        sum += hp[i * 4 + k] * hc[j * 4 + k];
                    }
                    m[i * 3 + j] = sum;
                }
            }
            final float tx = prevCameraWorldX
                    - (m[0] * cameraWorldX + m[1] * cameraWorldY + m[2] * cameraWorldZ);
            final float ty = prevCameraWorldY
                    - (m[3] * cameraWorldX + m[4] * cameraWorldY + m[5] * cameraWorldZ);
            final float tz = prevCameraWorldZ
                    - (m[6] * cameraWorldX + m[7] * cameraWorldY + m[8] * cameraWorldZ);
            batch.poseDelta[0] = m[0];
            batch.poseDelta[1] = m[1];
            batch.poseDelta[2] = m[2];
            batch.poseDelta[3] = tx;
            batch.poseDelta[4] = m[3];
            batch.poseDelta[5] = m[4];
            batch.poseDelta[6] = m[5];
            batch.poseDelta[7] = ty;
            batch.poseDelta[8] = m[6];
            batch.poseDelta[9] = m[7];
            batch.poseDelta[10] = m[8];
            batch.poseDelta[11] = tz;
            batch.hasPoseDelta = true;
        }

        long motionPointer = 0L;
        if (batch.vertexCount > 0) {
            final int motionFloats = batch.vertexCount * 3;
            final boolean useVertexHistory = batch.vertexHistoryIsPreviousFrame(motionFloats);
            final float[] positionStore = batch.vertexHistoryPositions(motionFloats);
            final boolean needMotion = useVertexHistory || batch.hasPoseDelta;
            if (needMotion || positionStore != null) {
                final ByteBuffer motion = needMotion ? batch.motionScratchFor(motionFloats) : null;
                final ByteBuffer positions = batch.buffer;
                final float[] delta = batch.poseDelta;
                boolean anyMotion = false;
                for (int vertex = 0; vertex < batch.vertexCount; ++vertex) {
                    final int offset = vertex * BYTES_PER_VERTEX;
                    final float px = positions.getFloat(offset);
                    final float py = positions.getFloat(offset + 4);
                    final float pz = positions.getFloat(offset + 8);
                    if (needMotion) {
                        final float mx;
                        final float my;
                        final float mz;
                        if (useVertexHistory) {
                            final int motionIndex = vertex * 3;
                            mx = positionStore[motionIndex] - px;
                            my = positionStore[motionIndex + 1] - py;
                            mz = positionStore[motionIndex + 2] - pz;
                        } else {
                            mx = delta[0] * px + delta[1] * py + delta[2] * pz + delta[3] - px;
                            my = delta[4] * px + delta[5] * py + delta[6] * pz + delta[7] - py;
                            mz = delta[8] * px + delta[9] * py + delta[10] * pz + delta[11] - pz;
                        }
                        anyMotion |= mx != 0.0F || my != 0.0F || mz != 0.0F;
                        motion.putFloat(mx).putFloat(my).putFloat(mz);
                    }
                    if (positionStore != null) {
                        final int motionIndex = vertex * 3;
                        positionStore[motionIndex] = px;
                        positionStore[motionIndex + 1] = py;
                        positionStore[motionIndex + 2] = pz;
                    }
                }
                if (positionStore != null) {
                    batch.storeVertexHistory(positionStore);
                }
                if (needMotion && anyMotion) {
                    motion.flip();
                    motionPointer = MemoryUtil.memAddress(motion);
                }
            }
        }

        // 2026-11-04 诊断：旗帜批次的几何事实。
        // 要回答三个问题：
        //   1) 底布与图案层各自的顶点数（能否对上"同一面旗帜的几层"）；
        //   2) applyCoplanarLayerOffset 到底有没有命中（coplanarHits=0 ⇒ 共面判定从未触发）；
        //   3) 一次都没触发的话，说明各层质心并不重合 ⇒ 沿法线外移这条路从一开始就不成立。
        if (isDiagnosticFrame() && batch.textureSlot >= 1
                && batch.textureSlot <= MAX_TEXTURE_SLOTS) {
            final String bannerLabel = frameTextureLabels[batch.textureSlot - 1];
            if (bannerLabel != null && bannerLabel.contains("banner") && batch.vertexCount > 0) {
                // 2026-11-04：打印**整批的 UV 范围**（不是首顶点）。
                // 原版某个图案 PNG 是 64×64，图集由 entity/banner/ 下约 40 张拼成，
                // 所以一个格子的 UV 跨度应该是 64/图集宽（512 → 0.125）。
                // 如果实测跨度远小于这个值，就是 UV 被压缩（映射叠了两次）；
                // 如果约等于，说明 UV 是对的，问题在 alpha/材质通道。
                float uMin = Float.MAX_VALUE;
                float uMax = -Float.MAX_VALUE;
                float vMin = Float.MAX_VALUE;
                float vMax = -Float.MAX_VALUE;
                for (int vertex = 0; vertex < batch.vertexCount; ++vertex) {
                    final int base = vertex * BYTES_PER_VERTEX;
                    final float u = batch.buffer.getFloat(base + 3 * Float.BYTES);
                    final float v = batch.buffer.getFloat(base + 4 * Float.BYTES);
                    if (u < uMin) uMin = u;
                    if (u > uMax) uMax = u;
                    if (v < vMin) vMin = v;
                    if (v > vMax) vMax = v;
                }
                final float c0 = batch.buffer.getFloat(5 * Float.BYTES);
                final float c1 = batch.buffer.getFloat(6 * Float.BYTES);
                final float c2 = batch.buffer.getFloat(7 * Float.BYTES);
                final float c3 = batch.buffer.getFloat(8 * Float.BYTES);
                customDiagLog("banner_geom verts=" + batch.vertexCount
                        + " hits=" + batch.coplanarHits
                        + String.format(" u=[%.4f,%.4f](d=%.4f) v=[%.4f,%.4f](d=%.4f) rgba=%.3f,%.3f,%.3f,%.3f",
                                uMin, uMax, uMax - uMin, vMin, vMax, vMax - vMin, c0, c1, c2, c3),
                        bannerLabel, null);
            }
        }

        // 2026-11-03 诊断：第一人称手部批次的 UV 范围。用来判定这批几何到底是
        // "皮肤 UV 空间"（手臂，0..1 且贴合皮肤布局）还是"item/方块图集空间"
        // （手持物，UV 会带图集偏移），从而确认手持物是否被并进了皮肤纹理批次。
        if (batch.diagHand && isDiagnosticFrame() && batch.vertexCount > 0) {
            final ByteBuffer source = batch.buffer;
            float uMin = Float.MAX_VALUE;
            float uMax = -Float.MAX_VALUE;
            float vMin = Float.MAX_VALUE;
            float vMax = -Float.MAX_VALUE;
            for (int vertex = 0; vertex < batch.vertexCount; ++vertex) {
                final int base = vertex * BYTES_PER_VERTEX + 3 * Float.BYTES;
                final float u = source.getFloat(base);
                final float v = source.getFloat(base + Float.BYTES);
                if (u < uMin) uMin = u;
                if (u > uMax) uMax = u;
                if (v < vMin) vMin = v;
                if (v > vMax) vMax = v;
            }
            System.err.println("[RTX-HANDUV] slot=" + batch.textureSlot
                    + " vertices=" + batch.vertexCount
                    + " u=[" + uMin + "," + uMax + "]"
                    + " v=[" + vMin + "," + vMax + "]"
                    + " texture=" + frameTextureLabels[
                        Math.min(Math.max(batch.textureSlot - 1, 0), MAX_TEXTURE_SLOTS - 1)]);
        }

        // 2026-11-03 回退：曾把第一人称手部批次打到独立的 RT 层（pad1 = 2.0），
        // 由 native 建 SECTION_AS_CLASS_HAND + mask 0x04，目的是让手不投世界阴影。
        // 实测第一视角贴图错乱 —— 该层的顶点上传路径没有接通，instance 指向无效的
        // 顶点地址，读到的贴图槽是垃圾。整项已回退（rgen 的 cullMask 也已还原为
        // 0xff / 0x01）；native 侧新增的 HAND 层定义保留为死代码，不影响运行。
        NativeVulkan.uploadEntityBatch(batch.textureSlot, MemoryUtil.memAddress(batch.buffer), batch.vertexCount, BYTES_PER_VERTEX, motionPointer);
    }

    /**
     * 短格式 {@code addVertex(FFF)} 的顶点入口（由 {@code BufferBuilderMixin} 的第二个
     * 钩子调用）。**只在"投影 UV 批次"里捕获** —— 那一档（末地传送门）的 UV 本来就在
     * {@link #captureVertex} 里按顶点位置合成，颜色用不透明白即可；其余几何走短格式时
     * 不带 UV/颜色，通用捕获会让它们带着错值进来，所以刻意放行条件收得很紧。
     */
    public static void capturePositionOnlyVertex(float x, float y, float z) {
        final ActiveBatch batch = activeBatch.get();
        if (batch == null || !batch.projectiveUv) {
            return;
        }
        captureVertex(x, y, z, 0xFFFFFFFF, 0.0f, 0.0f);
    }

    public static void captureVertex(float x, float y, float z, int color, float u, float v) {
        ActiveBatch batch = activeBatch.get();
        if (batch == null) return;
        if (handPhase && handTransformMode != 0) {
            final float rx;
            final float ry;
            final float rz;
            if (handTransformMode == 1) {
                rx = handRotation[0] * x + handRotation[4] * y + handRotation[8] * z;
                ry = handRotation[1] * x + handRotation[5] * y + handRotation[9] * z;
                rz = handRotation[2] * x + handRotation[6] * y + handRotation[10] * z;
            } else {
                rx = handRotation[0] * x + handRotation[1] * y + handRotation[2] * z;
                ry = handRotation[4] * x + handRotation[5] * y + handRotation[6] * z;
                rz = handRotation[8] * x + handRotation[9] * y + handRotation[10] * z;
            }
            x = rx;
            y = ry;
            z = rz;
        }
        if (batch.projectiveUv) {
            final float wx = x + cameraWorldX;
            final float wy = y + cameraWorldY;
            final float wz = z + cameraWorldZ;
            u = fract01(wx * 0.25f);
            v = fract01(wz * 0.25f + wy * 0.125f);
        }
        final int incomingColor = color;
        if (batch.projectiveUv || handPhase) {
            color = 0xFFFFFFFF;
        }
        if ((batch.projectiveUv || handPhase) && !vertexColorLogged) {
            vertexColorLogged = true;
            System.out.println("[RTX-VCOLOR] incoming color=0x"
                    + Integer.toHexString(incomingColor)
                    + " -> forced white; hand=" + handPhase
                    + " portal=" + batch.projectiveUv
                    + " slot=" + batch.textureSlot);
        }
        // =====================================================
        // 2026-11-04：UV 重映射**默认关闭**（曾经的"双重映射"事故）。
        //
        // ModelFeatureRenderer.renderModel 内部已经用 Submit.uvMapping 去 wrap 过
        // VertexConsumer 了（那个接口专门提供 wrap() 就是给这一步用的）。而我们抓的
        // 是**渲染阶段**的顶点 —— getVertexBuilder 返回的那个 consumer —— 拿到的 u/v
        // 已经是映射后的最终值。这里再乘一次 getU/getV 就是压两遍：
        //   · 箱子/告示牌只采到 sprite 内部一小块，颜色相近，看起来"好像正常"；
        //   · 旗帜图案有精细花纹，被压两遍后只剩图案一角 ⇒ 错乱。
        // 直接证据：旗帜实测 u 跨度 0.0027（≈1/370），而一个 sprite 正常应是 0.015 量级。
        //
        // A/B：-DORION_ENTITY_UV_REMAP=1 打开（回到双重映射那版，仅用于对比）。
        // =====================================================
        if (UV_REMAP_ENABLED && batch.uvMapping != null && !batch.projectiveUv) {
            u = batch.uvMapping.getU(u);
            v = batch.uvMapping.getV(v);
        }
        batch.capture(x, y, z, color, u, v);
    }

    /**
     * Mirrors ExperienceOrbRenderer's submitCustomGeometry quad into the
     * existing dynamic-entity RT batch. The original raster submission is
     * untouched.
     */
    public static void captureExperienceOrb(
            PoseStack.Pose pose,
            float u0, float u1, float v0, float v1,
            int red, int green, int blue) {
        if (activeBatch.get() != null) {
            throw new IllegalStateException("Nested experience-orb RT capture");
        }
        int textureSlot = slotFor(EXPERIENCE_ORB_TEXTURE);
        if (textureSlot == 0) return;

        ActiveBatch batch = acquireBatch(textureSlot, false, true);
        activeBatch.set(batch);
        CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            consumer.addVertex(pose, -0.5F, -0.25F, 0.0F).setColor(red, green, blue, 128).setUv(u0, v1);
            consumer.addVertex(pose, 0.5F, -0.25F, 0.0F).setColor(red, green, blue, 128).setUv(u1, v1);
            consumer.addVertex(pose, 0.5F, 0.75F, 0.0F).setColor(red, green, blue, 128).setUv(u1, v0);
            consumer.addVertex(pose, -0.5F, 0.75F, 0.0F).setColor(red, green, blue, 128).setUv(u0, v0);
        } finally {
            consumer.finish();
            endModel();
        }
    }

    /**
     * 2026-10-06 末地传送门：**自合成几何**（不再拦截 BufferBuilder）。
     *
     * <p>为什么换路线：末地门走的是短格式 {@code addVertex(FFF)}（字节码已核对），原来的
     * 钩子只覆盖全格式 ⇒ 顶点一个都进不来。直接补钩短格式会**崩驱动**
     *（{@code hs_err_pid45160.log}：nvoglv64.dll 里 EXCEPTION_ACCESS_VIOLATION，
     * Render thread / in_native），因为它按"方向 × 面 × 方块"提交、**顶点数没有上界**，
     * 超出实体批次缓冲的容量假设 ⇒ 上传越界。所以这里照 {@link #captureExperienceOrb}
     * 的先例，由 Java 侧自己画 —— 顶点数与内容全部可控、有硬上界。</p>
     *
     * <p>几何（2026-10-18 修）：原版末地门是**压扁的立方体**（{@code TheEndPortalRenderer}
     * 的 TRANSFORMATION：平移 y=0.375、y 缩放 0.375 ⇒ 盒体 y∈[0.375,0.75]，x,z∈[0,1]），
     * 可见面是**水平面**。此前手搓的是两片竖直交叉四边形（本意"任意侧可见"），观感就是
     * 用户报的"十字交叉竖着"。现改为还原水平面：上（y=0.75）与下（y=0.375）两块水平
     * 四边形，各正反两遍（8 顶点）。pose 原点是**方块角点**（LevelRenderer
     * {@code submitBlockEntities} 只平移到 {@code blockPos − camPos}，无 0.5 偏移）
     * ⇒ 覆盖 [0,1]²。UV 按顶点世界坐标合成（projectiveUv 路径）。</p>
     */
    public static void captureEndPortal(PoseStack.Pose pose) {
        if (!endPortalSyntheticLogged) {
            endPortalSyntheticLogged = true;
            System.out.println("[RTX-ENTITY] end portal synthetic reached: activeBatch="
                    + (activeBatch.get() != null));
        }
        if (activeBatch.get() != null) {
            return;
        }
        final int textureSlot = slotForLabel("end_portal");
        if (textureSlot == 0) {
            if (!endPortalSyntheticSlotLogged) {
                endPortalSyntheticSlotLogged = true;
                System.out.println("[RTX-ENTITY] end portal synthetic: slot lookup FAILED"
                        + " (label not registered this frame)");
            }
            return;
        }
        ActiveBatch batch = acquireBatch(textureSlot, false, true);
        batch.projectiveUv = true;
        activeBatch.set(batch);
        CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            emitPortalHorizontalQuad(consumer, pose, 0.75F, true);
            emitPortalHorizontalQuad(consumer, pose, 0.375F, true);
            emitPortalHorizontalQuad(consumer, pose, 0.75F, false);
            emitPortalHorizontalQuad(consumer, pose, 0.375F, false);
        } finally {
            consumer.finish();
            endModel();
        }
    }

    /**
     * 末地门的**水平**四边形（XZ 平面，y 恒定），覆盖整个方块 [0,1]²。顶点顺序按原版
     * CCW（从上往下看）；forward=false 时反向，供从下方看。UV 由批次的 {@code projectiveUv}
     * 路径按顶点世界坐标合成（这里的 setUv 占位，会被覆盖）。
     */
    private static void emitPortalHorizontalQuad(
            CapturingVertexConsumer consumer,
            PoseStack.Pose pose,
            float y,
            boolean forward) {
        final float ax = forward ? 0.0F : 1.0F;
        final float bx = forward ? 1.0F : 0.0F;
        consumer.addVertex(pose, ax, y, 0.0F).setColor(255, 255, 255, 255).setUv(0.0F, 1.0F);
        consumer.addVertex(pose, bx, y, 0.0F).setColor(255, 255, 255, 255).setUv(1.0F, 1.0F);
        consumer.addVertex(pose, bx, y, 1.0F).setColor(255, 255, 255, 255).setUv(1.0F, 0.0F);
        consumer.addVertex(pose, ax, y, 1.0F).setColor(255, 255, 255, 255).setUv(0.0F, 0.0F);
    }

    /**
     * 2026-10-21 闪电：原版 {@code LightningBoltRenderer.submit} 用
     * {@code addVertex(pose,…)+setColor} 链写顶点，不经过
     * {@code BufferBuilder.addVertex(FFFIFFIIFFF)} 的全格式钩子；同时
     * {@code RenderTypes.lightning()} 没有 Sampler0，通用捕获会在贴图解析处
     * 直接 return。两条路都收不到 ⇒ 闪电完全不可见。
     *
     * <p>这里照原版算法合成 3 分支 quad（种子/偏移/半径逐项一致；只发最内层壳，
     * 见下方 radiusScale 的说明），贴图取 8 列闪电精灵图（CC0，见
     * {@code rt_lightning.png} 的下载来源注释）里的一列 + 顶点色，并把整批标成
     * 自发光（pad1 = -1）。透明像素由 any-hit 的 alpha 裁切（{@code rt_test.rahit}
     * 的 combinedAlpha &lt; 0.1）剔掉，所以闪电是贴图里的细亮线而不是整块卡片。
     * 顶点数有硬上界，不会重演末地门短格式钩子的驱动越界。</p>
     */
    public static void captureLightning(long seed, PoseStack poseStack) {
        if (activeBatch.get() != null) {
            return;
        }
        final int textureSlot = slotFor(LIGHTNING_TEXTURE);
        if (textureSlot == 0) {
            if (!lightningSyntheticSlotLogged) {
                lightningSyntheticSlotLogged = true;
                System.out.println("[RTX-ENTITY] lightning synthetic: slot lookup FAILED");
            }
            return;
        }
        if (!lightningSyntheticLogged) {
            lightningSyntheticLogged = true;
            System.out.println("[RTX-ENTITY] lightning synthetic captured, slot=" + textureSlot);
        }
        final ActiveBatch batch = acquireBatch(textureSlot, false, true);
        activeBatch.set(batch);
        final CapturingVertexConsumer consumer = batch.captureConsumer();
        final PoseStack.Pose pose = poseStack.last();
        try {
            final float[] xOffsets = new float[8];
            final float[] zOffsets = new float[8];
            float xOffset = 0.0F;
            float zOffset = 0.0F;
            final RandomSource random = RandomSource.createThreadLocalInstance(seed);
            for (int i = 7; i >= 0; --i) {
                xOffsets[i] = xOffset;
                zOffsets[i] = zOffset;
                xOffset += random.nextInt(11) - 5;
                zOffset += random.nextInt(11) - 5;
            }
            final float finalXOffset = xOffset;
            final float finalZOffset = zOffset;
            final float radiusScale = 1.8F;
            for (int layer = 0; layer < 1; ++layer) {
                final RandomSource layerRandom = RandomSource.createThreadLocalInstance(seed);
                for (int branch = 0; branch < 3; ++branch) {
                    final boolean trunk = branch == 0;
                    final int branchStart = 7 - branch;
                    final int branchEnd = trunk ? 0 : branchStart - 3 + 1;
                    float startX = xOffsets[branchStart] - finalXOffset;
                    float startZ = zOffsets[branchStart] - finalZOffset;
                    for (int segment = branchStart; segment >= branchEnd; --segment) {
                        final float endX = startX;
                        final float endZ = startZ;
                        if (trunk) {
                            startX += layerRandom.nextInt(11) - 5;
                            startZ += layerRandom.nextInt(11) - 5;
                        } else {
                            startX += layerRandom.nextInt(31) - 15;
                            startZ += layerRandom.nextInt(31) - 15;
                        }
                        float topRadius = 0.1F + layer * 0.2F;
                        float bottomRadius = topRadius;
                        if (trunk) {
                            topRadius *= segment * 0.1F + 1.0F;
                            bottomRadius *= (segment - 1) * 0.1F + 1.0F;
                        }
                        topRadius *= radiusScale;
                        bottomRadius *= radiusScale;
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, false, false, true, false,
                                (segment + branch) & 7);
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, true, false, true, true,
                                (segment + branch + 2) & 7);
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, true, true, false, true,
                                (segment + branch + 4) & 7);
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, false, true, false, false,
                                (segment + branch + 6) & 7);
                    }
                }
            }
        } finally {
            consumer.finish();
            endModel();
        }
    }

    private static void emitLightningQuad(
            CapturingVertexConsumer consumer,
            PoseStack.Pose pose,
            float segmentStartX,
            float segmentStartZ,
            float segmentEndX,
            float segmentEndZ,
            int segment,
            float topRadius,
            float bottomRadius,
            boolean rightXPositive,
            boolean rightZPositive,
            boolean leftXPositive,
            boolean leftZPositive,
            int column) {
        final int color = 0xFFB9C8FF;
        final float u0 = (column + 0.03F) * 0.125F;
        final float u1 = (column + 0.97F) * 0.125F;
        consumer.addVertex(pose,
                        segmentStartX + (rightXPositive ? bottomRadius : -bottomRadius),
                        segment * 16,
                        segmentStartZ + (rightZPositive ? bottomRadius : -bottomRadius))
                .setColor(color).setUv(u0, 0.0F);
        consumer.addVertex(pose,
                        segmentEndX + (rightXPositive ? topRadius : -topRadius),
                        (segment + 1) * 16,
                        segmentEndZ + (rightZPositive ? topRadius : -topRadius))
                .setColor(color).setUv(u1, 0.0F);
        consumer.addVertex(pose,
                        segmentEndX + (leftXPositive ? topRadius : -topRadius),
                        (segment + 1) * 16,
                        segmentEndZ + (leftZPositive ? topRadius : -topRadius))
                .setColor(color).setUv(u1, 1.0F);
        consumer.addVertex(pose,
                        segmentStartX + (leftXPositive ? bottomRadius : -bottomRadius),
                        segment * 16,
                        segmentStartZ + (leftZPositive ? bottomRadius : -bottomRadius))
                .setColor(color).setUv(u0, 1.0F);
    }

    /**
     * 2026-10-21 信标光柱：{@code BeaconRenderer.submit} 同样走
     * {@code addVertex(pose,…)+setColor/setUv} 链，全格式钩子收不到顶点，
     * 通用捕获只开了一个空批次 ⇒ 光柱不可见。
     *
     * <p>按原版 {@code submitBeaconBeam} 的几何、UV 滚动与转速重建实心柱
     *（外辉那层是加色混合，RT 里没有对应通路，省略；实心柱本身已带贴图渐变）。
     * 每个 section 4 个 quad、16 个顶点，上界固定。</p>
     */
    public static void captureBeaconBeam(BeaconRenderState state, PoseStack poseStack) {
        if (state.sections.isEmpty() || activeBatch.get() != null) {
            return;
        }
        final int textureSlot = slotFor(BeaconRenderer.BEAM_LOCATION);
        if (textureSlot == 0) {
            if (!beaconSyntheticSlotLogged) {
                beaconSyntheticSlotLogged = true;
                System.out.println("[RTX-ENTITY] beacon synthetic: slot lookup FAILED");
            }
            return;
        }
        if (!beaconSyntheticLogged) {
            beaconSyntheticLogged = true;
            System.out.println("[RTX-ENTITY] beacon synthetic captured, slot=" + textureSlot);
        }
        final ActiveBatch batch = acquireBatch(textureSlot, false, true);
        activeBatch.set(batch);
        final CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            int beamStart = 0;
            final int sectionCount = state.sections.size();
            for (int i = 0; i < sectionCount; ++i) {
                final BeaconRenderState.Section section = state.sections.get(i);
                final int height = i == sectionCount - 1 ? 2048 : section.height();
                emitBeaconBeam(consumer, poseStack, state.animationTime,
                        beamStart, height, section.color(), 0.2F * state.beamRadiusScale);
                beamStart += section.height();
            }
        } finally {
            consumer.finish();
            endModel();
        }
    }

    private static void emitBeaconBeam(
            CapturingVertexConsumer consumer,
            PoseStack poseStack,
            float animationTime,
            int beamStart,
            int height,
            int color,
            float radius) {
        if (radius <= 1.0E-4F) {
            return;
        }
        final int beamEnd = beamStart + height;
        final float scroll = height < 0 ? animationTime : -animationTime;
        final float rawV = scroll * 0.2F - (float) Math.floor(scroll * 0.1F);
        final float texVOff = rawV - (float) Math.floor(rawV);
        final float vv2 = -1.0F + texVOff;
        final float vv1 = height * (0.5F / radius) + vv2;

        poseStack.pushPose();
        poseStack.translate(0.5F, 0.0F, 0.5F);
        poseStack.rotateDegrees(Axis.YP, animationTime * 2.25F - 45.0F);
        final PoseStack.Pose pose = poseStack.last();
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                0.0F, radius, radius, 0.0F, 0.0F, 1.0F, vv1, vv2);
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                -radius, 0.0F, 0.0F, -radius, 0.0F, 1.0F, vv1, vv2);
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                radius, 0.0F, 0.0F, -radius, 0.0F, 1.0F, vv1, vv2);
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                0.0F, -radius, 0.0F, radius, 0.0F, 1.0F, vv1, vv2);
        poseStack.popPose();
    }

    private static void emitBeaconQuad(
            CapturingVertexConsumer consumer,
            PoseStack.Pose pose,
            int color,
            int beamStart,
            int beamEnd,
            float westX,
            float westZ,
            float eastX,
            float eastZ,
            float uu1,
            float uu2,
            float vv1,
            float vv2) {
        consumer.addVertex(pose, westX, beamEnd, westZ).setColor(color).setUv(uu2, vv1);
        consumer.addVertex(pose, westX, beamStart, westZ).setColor(color).setUv(uu2, vv2);
        consumer.addVertex(pose, eastX, beamStart, eastZ).setColor(color).setUv(uu1, vv2);
        consumer.addVertex(pose, eastX, beamEnd, eastZ).setColor(color).setUv(uu1, vv1);
    }

    /**
     * 按**贴图标签子串**取槽位（末地门用）。刻意不自己构造 {@code Identifier}：
     * 标签来自 {frameTextureLabels}，也就是渲染管线自己解析出的那个，
     * 避免路径/命名空间约定与版本不一致时静默失配。
     */
    private static int slotForLabel(String needle) {
        for (int index = 0; index < frameTextureSlotCount; ++index) {
            final String label = frameTextureLabels[index];
            if (label != null && label.contains(needle)) {
                slotLastSeenFrame[index] = captureFrameSerial;
                return index + 1;
            }
        }
        return 0;
    }

    public static void onTextureBound(String textureLabel, long imageView, long sampler) {
        for (int index = 0; index < frameTextureSlotCount; ++index) {
            if (frameTextureLabels[index].equals(textureLabel)) {
                publishTextureHandle(index + 1, imageView, sampler);
                return;
            }
        }
    }

    private static int slotFor(Identifier texture) {
        if (texture == null) return 0;
        for (int index = 0; index < frameTextureSlotCount; ++index) {
            if (texture.equals(frameTextureSlots[index])) {
                slotLastSeenFrame[index] = captureFrameSerial;
                assignTextureClass(index, texture);
                if (!slotTexturePublished[index]) {
                    publishTexture(index + 1, texture);
                }
                if (!entityPbrSettled[index]) {
                    publishEntityPbrTexture(index + 1, texture);
                }
                return index + 1;
            }
        }
        int victim;
        if (frameTextureSlotCount < MAX_REGULAR_TEXTURE_SLOTS) {
            victim = frameTextureSlotCount;
        } else {
            victim = -1;
            int bestAge = -1;
            for (int index = 0; index < MAX_REGULAR_TEXTURE_SLOTS; ++index) {
                final int age = captureFrameSerial - slotLastSeenFrame[index];
                if (age >= SLOT_EVICTION_GRACE_FRAMES) {
                    victim = index;
                    break;
                }
                if (age > bestAge) {
                    bestAge = age;
                    victim = index;
                }
            }
            if (victim < 0) {
                if (isDiagnosticFrame()) {
                    System.err.println("[RTX-ENTITY] slot table full, dropping texture " + texture);
                }
                return 0;
            }
            if (slotLastSeenFrame[victim] == captureFrameSerial) {
                ++evictionsInUse;
                ++evictionsInUseTotal;
            }
        }
        frameTextureSlots[victim] = texture;
        frameTextureLabels[victim] = texture.toString();
        assignTextureClass(victim, texture);
        slotLastSeenFrame[victim] = captureFrameSerial;
        if (victim == frameTextureSlotCount) ++frameTextureSlotCount;
        entityPbrSettled[victim] = false;
        publishTexture(victim + 1, texture);
        return victim + 1;
    }

    private static void assignTextureClass(int index, Identifier texture) {
        final int materialClass = isUncutoutTexture(texture)
                ? MATERIAL_CLASS_UNCUTOUT
                : classifyEntityMaterial(texture);


        if (frameTextureClasses[index] == materialClass) {
            return;
        }


        frameTextureClasses[index] = materialClass;


        if (materialClass != MATERIAL_CLASS_DEFAULT) {
            System.out.println(
                    "[RTX-ENTITY] material class " + materialClass
                            + " slot " + (index + 1)
                            + " for " + texture
            );
        }
    }

    private static final java.util.HashMap<Identifier, Integer> MATERIAL_CLASS_CACHE =
            new java.util.HashMap<>();
    private static boolean materialClassCacheMetalEnabled;

    private static int classifyEntityMaterial(Identifier texture) {
        if (texture == null) {
            return MATERIAL_CLASS_DEFAULT;
        }


        boolean metalEnabled = RTXSettingsConfig.isEntityArmorMetalEnabled();
        if (!metalEnabled) {
            if (materialClassCacheMetalEnabled) {
                materialClassCacheMetalEnabled = false;
                MATERIAL_CLASS_CACHE.clear();
            }
            return MATERIAL_CLASS_DEFAULT;
        }
        if (!materialClassCacheMetalEnabled) {
            materialClassCacheMetalEnabled = true;
            MATERIAL_CLASS_CACHE.clear();
        }

        Integer cached = MATERIAL_CLASS_CACHE.get(texture);
        if (cached != null) {
            return cached;
        }

        int materialClass = classifyEntityMaterialUncached(texture);
        MATERIAL_CLASS_CACHE.put(texture, materialClass);
        return materialClass;
    }

    private static int classifyEntityMaterialUncached(Identifier texture) {
        final String path =
                texture.toString().toLowerCase(Locale.ROOT);


        if (!path.contains("/equipment/")
                && !path.contains("armor")
                && !path.contains("elytra")) {
            return MATERIAL_CLASS_DEFAULT;
        }


        if (path.contains("netherite")) {
            return MATERIAL_CLASS_NETHERITE;
        }

        if (path.contains("diamond")) {
            return MATERIAL_CLASS_DIAMOND;
        }

        if (path.contains("gold")) {
            return MATERIAL_CLASS_GOLD;
        }

        if (path.contains("copper")) {
            return MATERIAL_CLASS_COPPER;
        }

        if (path.contains("iron") || path.contains("chainmail")) {
            return MATERIAL_CLASS_IRON;
        }


        return MATERIAL_CLASS_SOFT;
        }

        /**
        * 大片透明但必须**整体可见**的贴图（目前只有末地传送门的星幕
        * {@code end_portal.png}）。走"不裁切"档，且刻意**不经过**
        * {@link #classifyEntityMaterial}（那条路在 {@code entityArmorMetal} 关闭时返回
        * DEFAULT，会让末地门的可见性跟着盔甲设置变化）。消费端见 {@code rt_test.rahit}。
        */
        private static boolean isUncutoutTexture(Identifier texture) {
        return texture != null && texture.toString().contains("end_portal");
        }

    private static void publishTexture(int slot, Identifier textureId) {
        AbstractTexture texture = Minecraft.getInstance()
                .getTextureManager()
                .getTexture(textureId);
        GpuTextureView view = texture.getTextureView();
        GpuSampler sampler = texture.getSampler();
        if (view instanceof VulkanGpuTextureView vulkanView
                && sampler instanceof VulkanGpuSampler vulkanSampler) {
            publishTextureHandle(slot, vulkanView.vkImageView(), vulkanSampler.vkSampler());
        } else if (isDiagnosticFrame()) {
            // 2026-11-04 诊断：纹理槽绑定失败 ⇒ shader 采样返回 0 ⇒
            // cutout 把每个纹素都当透明丢掉 ⇒ 几何"消失"（旗帜图案正是这种表现：
            // 底色在、图案完全不见，也不遮挡）。只打印失败，不刷屏。
            System.err.println("[RTX-TEX] publish failed slot=" + slot
                    + " texture=" + textureId
                    + " view=" + view + " sampler=" + sampler);
        }
        publishEntityPbrTexture(slot, textureId);
    }


    /** 一次解析并推送一个槽的 `_n`/`_s`；没有贴图/视图未就绪时会保持 unsettled 以便重试。 */
    private static void publishEntityPbrTexture(int slot, Identifier albedoTexture) {
        if (slot < 1 || slot > MAX_TEXTURE_SLOTS || albedoTexture == null) return;
        int index = slot - 1;

        Identifier[] siblings = resolveEntityPbrSiblings(albedoTexture);
        Identifier normalId = siblings[0];
        Identifier specularId = siblings[1];

        long normalView = 0L, normalSampler = 0L, specularView = 0L, specularSampler = 0L;
        boolean pending = false;
        if (normalId != null) {
            long[] handle = entityTextureHandle(normalId);
            if (handle == null) pending = true;
            else { normalView = handle[0]; normalSampler = handle[1]; }
        }
        if (specularId != null) {
            long[] handle = entityTextureHandle(specularId);
            if (handle == null) pending = true;
            else { specularView = handle[0]; specularSampler = handle[1]; }
        }
        if (pending) return;

        entityPbrSettled[index] = true;

        if (normalView == publishedEntityNormalViews[index]
                && normalSampler == publishedEntityNormalSamplers[index]
                && specularView == publishedEntitySpecularViews[index]
                && specularSampler == publishedEntitySpecularSamplers[index]) {
            return;
        }

        publishedEntityNormalViews[index] = normalView;
        publishedEntityNormalSamplers[index] = normalSampler;
        publishedEntitySpecularViews[index] = specularView;
        publishedEntitySpecularSamplers[index] = specularSampler;

        if (!entityPbrNativeAvailable) return;
        try {
            NativeVulkan.updateEntityPbrTexture(
                    slot, normalView, normalSampler, specularView, specularSampler);
        } catch (UnsatisfiedLinkError staleNative) {
            entityPbrNativeAvailable = false;
        }
    }

    /**
     * 解析 albedo Identifier 对应的 `_n`/`_s`（存在的返回 Identifier，不存在返回
     * null）。结果按 albedo Identifier 缓存 —— ResourceManager 查表要扫资源包，
     * 绝不能逐帧做；资源包重载时随 {@link #invalidateTextureCache()} 清空。
     */
    private static Identifier[] resolveEntityPbrSiblings(Identifier albedoTexture) {
        Identifier[] cached = ENTITY_PBR_SIBLINGS.get(albedoTexture);
        if (cached != null) return cached;

        Identifier normalId = siblingTextureId(albedoTexture, "_n");
        Identifier specularId = siblingTextureId(albedoTexture, "_s");
        ResourceManager manager = Minecraft.getInstance().getResourceManager();
        if (normalId != null && manager.getResource(normalId).isEmpty()) normalId = null;
        if (specularId != null && manager.getResource(specularId).isEmpty()) specularId = null;

        Identifier[] result = new Identifier[] { normalId, specularId };
        ENTITY_PBR_SIBLINGS.put(albedoTexture, result);
        return result;
    }

    /** {@code .../foo.png} → {@code .../foo_n.png}；非 .png 的贴图路径直接放弃。 */
    private static Identifier siblingTextureId(Identifier albedo, String suffix) {
        String path = albedo.getPath();
        if (!path.endsWith(".png")) return null;
        return Identifier.fromNamespaceAndPath(
                albedo.getNamespace(),
                path.substring(0, path.length() - 4) + suffix + ".png");
    }

    /** 取一张已注册贴图的 Vulkan 句柄；视图尚未就绪时返回 null（调用方据此重试）。 */
    private static long[] entityTextureHandle(Identifier textureId) {
        try {
            AbstractTexture texture = Minecraft.getInstance()
                    .getTextureManager()
                    .getTexture(textureId);
            if (texture == null) return null;
            GpuTextureView view = texture.getTextureView();
            GpuSampler sampler = texture.getSampler();
            if (view instanceof VulkanGpuTextureView vulkanView
                    && sampler instanceof VulkanGpuSampler vulkanSampler) {
                long imageView = vulkanView.vkImageView();
                long vkSampler = vulkanSampler.vkSampler();
                if (imageView != 0L && vkSampler != 0L) {
                    return new long[] { imageView, vkSampler };
                }
            }
        } catch (RuntimeException ignored) {
        }
        return null;
    }

    private static void publishTextureHandle(int slot, long imageView, long sampler) {
        if (slot < 1 || slot > MAX_TEXTURE_SLOTS || imageView == 0L || sampler == 0L) return;
        int index = slot - 1;
        if (publishedEntityViews[index] == imageView && publishedEntitySamplers[index] == sampler) {
            slotTexturePublished[index] = true;
            return;
        }
        publishedEntityViews[index] = imageView;
        publishedEntitySamplers[index] = sampler;
        slotTexturePublished[index] = true;
        NativeVulkan.updateEntityTexture(slot, imageView, sampler);
    }

    private static RenderType customRenderType;
    private static boolean customBatchOpen;

    /** 由 {@code RenderTypeFeatureRendererMixin} 在 getVertexBuilder 返回时调用。 */
    public static void beginCustomSubmit(RenderType renderType) {
        if (activeBatch.get() != null) {
            return;
        }
        if (customBatchOpen && customRenderType == renderType) {
            return;
        }
        flushCustomBatch();
        if (renderType == null) {
            return;
        }
        final String typeName = renderTypeName(renderType);
        if (typeName != null && typeName.contains("shadow")) {
            customDiagLog("drop:shadow", typeName, null);
            return;
        }
        if (isScreenOverlayRenderType(typeName)) {
            customDiagLog("drop:screen_overlay", typeName, null);
            return;
        }
        final boolean endPortal = typeName != null && typeName.contains("end_portal");
        Identifier texture = endPortal ? resolveSampler(renderType, "Sampler1") : null;
        if (texture == null) {
            texture = resolveSampler(renderType, "Sampler0");
        }
        if (texture == null) {
            customDiagLog("drop:no_texture", typeName, null);
            return;
        }
        int textureSlot = slotFor(texture);
        if (textureSlot == 0) {
            customDiagLog("drop:no_slot", typeName, texture);
            return;
        }
        customDiagLog("captured", typeName, texture);
        ActiveBatch customBatch = acquireBatch(textureSlot, false,
                endPortal || isEmissiveGeometry(texture));
        customBatch.projectiveUv = endPortal;
        // 2026-11-04：旗帜图案层沿法线外移固定两步（理由见 beginModel 处）。
        customBatch.coplanarLayerBoost = isBannerPattern(renderType) ? 2 : 0;
        // 2026-11-04：旗帜图案层整体外移一个 rank（见 bannerLayerBoost）。
        // 2026-11-03 诊断：第一人称下这条 custom 路径就是"手持物品"（RenderTypeFeatureRendererMixin
        // 的注释第 28 行：Item → 物品展示框里的物品、掉落物、第一人称手持物品）。
        // 打上标记后 uploadBatch 会顺带打印它的 vertexCount 与 UV 范围。
        customBatch.diagHand = Minecraft.getInstance().options.getCameraType().isFirstPerson();
        if (endPortal && !endPortalLogged) {
            endPortalLogged = true;
            System.out.println("[RTX-ENTITY] end portal geometry captured, slot="
                    + textureSlot + " texture=" + texture);
        }
        activeBatch.set(customBatch);
        customBatchOpen = true;
        customRenderType = renderType;
    }

    /** 由 {@code RenderTypeFeatureRendererMixin} 在 prepareGroup 收尾时调用。 */
    public static void endCustomGroup() {
        flushCustomBatch();
    }

    // =====================================================
    // 2026-11-04 诊断：custom 路径（物品展示框里的物品、掉落物、第一人称手持物、
    // 地图、末地传送门……）到底有没有被捕获。
    // 物品展示框里的**地图**不显形，需要先分清是"根本没进这条路径"（被某个 return
    // 挡了）还是"进来了但没画对"。每 64 帧打一组，每组最多 14 条。
    // =====================================================
    private static long customDiagFrame = -1L;
    private static int customDiagCount;

    private static void customDiagLog(String reason, String typeName, Identifier texture) {
        if (!isDiagnosticFrame()) {
            return;
        }
        if (customDiagFrame != captureFrameSerial) {
            customDiagFrame = captureFrameSerial;
            customDiagCount = 0;
        }
        if (customDiagCount >= 14) {
            return;
        }
        ++customDiagCount;
        System.err.println("[RTX-CUSTOM] " + reason
                + " type=" + typeName
                + " texture=" + texture);
    }

    private static void flushCustomBatch() {
        if (!customBatchOpen) {
            return;
        }
        customBatchOpen = false;
        customRenderType = null;
        endModel();
    }

    /**
     * 屏幕特效（水下 / 火焰 / 方块遮罩）也是 submitCustomGeometry，但它是**贴着
     * 相机**的全屏四边形 —— 一旦当世界几何收进 TLAS，就会变成糊在镜头上的一层。
     * 只能按 RenderType 名字拒：火焰那一档的 Sampler0 是方块图集，按贴图路径
     * 会连正常方块一起拒掉。
     */
    private static boolean isScreenOverlayRenderType(String typeName) {
        return typeName != null && typeName.contains("screen_effect");
    }

    /** RenderType 的构造名（26.3 字段 {@code name}，经 {@code RenderTypeAccessor} 读）。 */
    private static String renderTypeName(RenderType renderType) {
        return ((RenderTypeAccessor) (Object) renderType).rtx$getName();
    }

    /**
     * 自带发光的几何。判据是 Sampler0 的贴图路径（这些渲染类型都把发光贴图
     * 挂在 Sampler0 上），命名与 26.3 资源路径一致。
     */
    private static boolean isEmissiveGeometry(Identifier texture) {
        final String path = texture.toString();
        return path.contains("end_sky")
                || path.contains("end_portal")
                || path.contains("end_gateway")
                || path.contains("beacon_beam")
                || path.contains("dragon_beam")
                || path.contains("dragon_fireball")
                || path.contains("guardian_beam");
    }

    private static Identifier resolveSampler0(RenderType renderType) {
        return resolveSampler(renderType, "Sampler0");
    }

    /**
     * 2026-11-04：这是不是旗帜的**图案层**。
     *
     * <p>旗帜 = 底布 + 逐层图案，几何完全共面。原版给 base-colour pass 的 order 是 0，
     * 但它其实画在"无色的、同样共面的底布"之后 —— 两者在 BVH 里完全打平，命中哪个
     * 是随机的，于是旗帜会随机显示成白布或缺失图案。</p>
     *
     * <p>判据与 Caustica 的 {@code RtEntityCollector#isBannerPattern} 一致：
     * 看 RenderPipeline 的 location。命中后本批次整体外移一个 rank
     * （见 {@link ActiveBatch#coplanarLayerBoost}）。</p>
     */
    /**
     * 2026-11-04：这批几何是不是旗帜（底布或图案层）。
     *
     * <p>旗帜的 6 个 quad 是"3 个方向 × 正反两面"，**正反两面顶点位置完全相同**，
     * 于是 {@code applyCoplanarLayerOffset} 的质心键必然重复、会把正反两面互相推开，
     * 每帧都在破坏几何（实测 coplanarHits 累计到十万级）。所以对旗帜整批禁用该机制，
     * 层序交给 alpha cutout 的 any-hit 穿透（图案的透明纹素被丢弃后自然落到下层）。</p>
     */
    private static boolean isBannerTexture(Identifier texture) {
        return texture != null && texture.getPath().contains("banner");
    }

    private static boolean isBannerPattern(RenderType renderType) {
        if (renderType == null) {
            return false;
        }
        final RenderSetup setup = ((RenderTypeAccessor) (Object) renderType).rtx$getState();
        if (setup == null) {
            return false;
        }
        final var pipeline = ((RenderSetupAccessor) (Object) setup).rtx$getPipeline();
        if (pipeline == null) {
            return false;
        }
        return "minecraft:pipeline/banner_pattern".equals(pipeline.getLocation().toString());
    }

    /**
     * 2026-11-04：旗帜图案层的层序提升量。
     *
     * <p>两条判据取或：</p>
     * <ol>
     *   <li>{@link #isBannerPattern}：看 RenderPipeline 的 location（与 Caustica 同口径）；</li>
     *   <li><b>纹理名兜底</b>：{@code banner_patterns.png}。实测日志里图案层就是这个纹理，
     *       而 pipeline 的 location 字符串一旦命名不同，第一条会静默返回 false ⇒
     *       boost 恒 0 ⇒ 图案层与底布完全重合 ⇒ BVH 随机挑，图案时有时无。</li>
     * </ol>
     * <p>底布走的是方块图集里的白布（不同纹理槽），这里不动它，所以相对偏移正好一层。</p>
     */
    // 2026-11-04：旗帜层序的多次尝试（固定 rank、按质心分组、全局递增）**全部无效**，
    // 相关代码已删除，回到共面层序的原始实现（applyCoplanarLayerOffset 只用提交顺序）。
    // 结论记录：问题不在"层序 rank"这一层，需要先拿到以下事实再动手 ——
    //   · 同一面旗帜的底布与图案层，各自批次的 vertexCount / quad 数
    //   · applyCoplanarLayerOffset 在那两个批次里是否真的被调用（duplicates 是否 > 0）
    //   · 图案层相对底布的偏移方向（几何法线朝向是否与"朝观察者"一致）
    // 这三个只要有一个不成立，"沿法线外移"就永远推不出去。

    private static Identifier resolveSampler(RenderType renderType, String samplerName) {
        RenderSetup setup = ((RenderTypeAccessor) (Object) renderType).rtx$getState();
        Object binding = ((RenderSetupAccessor) (Object) setup).rtx$getTextures().get(samplerName);
        return binding == null ? null : ((TextureBindingAccessor) binding).rtx$getLocation();
    }

    private static ActiveBatch acquireBatch(
            int textureSlot, boolean localPlayerProxy, boolean emissive) {
        ActiveBatch batch = reusableBatch.get().reset(textureSlot, localPlayerProxy, emissive);
        batch.coplanarLayering = true;
        return batch;
    }

    private static final class ActiveBatch {
        private int textureSlot;
        private boolean localPlayerProxy;
        private boolean emissive;
        // 2026-11-03 诊断：这批是不是第一人称手部（state=Unit）。每次 beginModel 重设，
        // 不需要在 reset() 里清（避免漏一条路径就串味）。
        private boolean diagHand;
        // 2026-11-04：旗帜图案层的额外层序偏移。
        // ⚠ 多次尝试（固定 rank / 按质心分组 / 全局递增）全部无效，现恒为 0。
        // 保留字段只为将来有证据后再启用。
        private int coplanarLayerBoost;
        // 2026-11-04 诊断：本批次里 applyCoplanarLayerOffset 真正命中重复质心的次数。
        // 0 ⇒ 共面判定从未触发（那才是旗帜修不好的根因）；>0 ⇒ 偏移发生了但没起作用（方向问题）。
        private int coplanarHits;
        // 2026-11-04：本批次的 UV 映射（来自 ModelFeatureRenderer.Submit 的 record 字段）。
        //   null  ⇒ 整张贴图，UV 原样（mob 皮肤、items atlas 都是这种）
        //   非 null ⇒ ModelPart 的 0..1 UV 需要按 sprite 在图集里的矩形重映射
        //            （箱子 / 告示牌 / 床 / 旗帜 等方块实体走这条）
        // 不重映射的后果：采样落到图集左上角，贴图整片错乱。
        private net.minecraft.client.renderer.texture.UvMapping uvMapping;
        

        private boolean projectiveUv;
        private float minX = Float.MAX_VALUE;
        private float minY = Float.MAX_VALUE;
        private float minZ = Float.MAX_VALUE;
        private float maxX = -Float.MAX_VALUE;
        private float maxY = -Float.MAX_VALUE;
        private float maxZ = -Float.MAX_VALUE;
        private ByteBuffer buffer = ByteBuffer.allocateDirect(768 * BYTES_PER_VERTEX).order(ByteOrder.nativeOrder());

        private final float[] poseDelta = new float[12];
        private boolean hasPoseDelta;
        private float motionX, motionY, motionZ;
        private ByteBuffer motionScratch;
        

        private java.util.HashMap<ModelBatchKey, VertexHistory> vertexHistory;
        private ModelBatchKey vertexHistoryKey;

        private static final int PENDING_VERTEX_FLOATS = 5;
        private final float[] pendingQuadVertices = new float[4 * PENDING_VERTEX_FLOATS];
        private final int[] pendingQuadColors = new int[4];
        private final CapturingVertexConsumer consumer = new CapturingVertexConsumer(this);
        private int pendingQuadVertexCount;
        private int vertexCount;
        private final java.util.HashMap<Long, Integer> coplanarQuadCounts = new java.util.HashMap<>();
        private boolean coplanarLayering;

        private ActiveBatch reset(int textureSlot, boolean localPlayerProxy, boolean emissive) {
            this.textureSlot = textureSlot;
            this.localPlayerProxy = localPlayerProxy;
            this.emissive = emissive;
            this.projectiveUv = false;
            this.coplanarLayering = false;
            this.coplanarHits = 0;
            this.minX = this.minY = this.minZ = Float.MAX_VALUE;
            this.maxX = this.maxY = this.maxZ = -Float.MAX_VALUE;
            buffer.clear();
            pendingQuadVertexCount = 0;
            vertexCount = 0;
            coplanarQuadCounts.clear();
            hasPoseDelta = false;
            uvMapping = null;
            coplanarLayerBoost = 0;
            java.util.Arrays.fill(poseDelta, 0.0F);
            motionX = motionY = motionZ = 0.0F;
            vertexHistory = null;
            vertexHistoryKey = null;
            return this;
        }

        private CapturingVertexConsumer captureConsumer() {
            consumer.reset();
            return consumer;
        }

        /**
         * 本批次顶点位置的落点数组；null = 这一批不缓存（文字/通用几何/开关关闭）。
         * 命中上一帧时返回历史数组本身（读一个下标、写同一个下标，原地滚动）。
         */
        private float[] vertexHistoryPositions(int floats) {
            if (vertexHistory == null || vertexHistoryKey == null) {
                return null;
            }
            final VertexHistory entry = vertexHistory.get(vertexHistoryKey);
            if (entry != null && entry.positions != null && entry.positions.length == floats) {
                return entry.positions;
            }
            return new float[floats];
        }

        /** 历史里这一批是否恰好是上一帧采样的（帧号连续才信任下标对应关系）。 */
        private boolean vertexHistoryIsPreviousFrame(int floats) {
            if (vertexHistory == null || vertexHistoryKey == null) {
                return false;
            }
            final VertexHistory entry = vertexHistory.get(vertexHistoryKey);
            return entry != null
                    && entry.frame == entityMotionFrame - 1
                    && entry.positions != null
                    && entry.positions.length == floats;
        }

        /** 把本帧位置登记为下一帧的 p_prev。 */
        private void storeVertexHistory(float[] positions) {
            VertexHistory entry = vertexHistory.get(vertexHistoryKey);
            if (entry == null) {
                if (vertexHistory.size() >= ENTITY_VERTEX_HISTORY_MAX_KEYS) {
                    vertexHistory.clear();
                }
                entry = new VertexHistory();
                vertexHistory.put(vertexHistoryKey, entry);
            }
            entry.positions = positions;
            entry.frame = entityMotionFrame;
        }

        private ByteBuffer motionScratchFor(int floats) {
            final int need = floats * Float.BYTES;
            if (motionScratch == null || motionScratch.capacity() < need) {
                motionScratch = ByteBuffer.allocateDirect(need).order(ByteOrder.nativeOrder());
            } else {
                motionScratch.clear();
            }
            return motionScratch;
        }

        private void capture(float x, float y, float z, int color, float u, float v) {
            int vertex = pendingQuadVertexCount++;
            int offset = vertex * PENDING_VERTEX_FLOATS;
            pendingQuadVertices[offset] = x;
            pendingQuadVertices[offset + 1] = y;
            pendingQuadVertices[offset + 2] = z;
            pendingQuadVertices[offset + 3] = u;
            pendingQuadVertices[offset + 4] = v;
            pendingQuadColors[vertex] = color;
            if (pendingQuadVertexCount == 4) {
                if (coplanarLayering) {
                    applyCoplanarLayerOffset();
                }
                emit(0); emit(1); emit(2);
                emit(0); emit(2); emit(3);
                pendingQuadVertexCount = 0;
            }
        }

        /**
         * 共面重合 quad 的稳定层序（见 {@code ENTITY_COPLANAR_LAYER_STEP} 的说明）。
         * 在 4 个顶点凑齐、准备 emit 前调用：质心与先前 quad 重合的，沿自身几何
         * 法线外移 dup × STEP —— 先画的不动，后画的依次前移，平局变确定性。
         * 顶点顺序按原版 CCW（法线朝外）约定，所以外移方向就是"朝观察者那一侧"。
         */
        private void applyCoplanarLayerOffset() {
            final float ax = pendingQuadVertices[0];
            final float ay = pendingQuadVertices[1];
            final float az = pendingQuadVertices[2];
            final float bx = pendingQuadVertices[PENDING_VERTEX_FLOATS];
            final float by = pendingQuadVertices[PENDING_VERTEX_FLOATS + 1];
            final float bz = pendingQuadVertices[PENDING_VERTEX_FLOATS + 2];
            final float cx = pendingQuadVertices[2 * PENDING_VERTEX_FLOATS];
            final float cy = pendingQuadVertices[2 * PENDING_VERTEX_FLOATS + 1];
            final float cz = pendingQuadVertices[2 * PENDING_VERTEX_FLOATS + 2];
            final float dx = pendingQuadVertices[3 * PENDING_VERTEX_FLOATS];
            final float dy = pendingQuadVertices[3 * PENDING_VERTEX_FLOATS + 1];
            final float dz = pendingQuadVertices[3 * PENDING_VERTEX_FLOATS + 2];

            final float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
            final float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
            float nx = e1y * e2z - e1z * e2y;
            float ny = e1z * e2x - e1x * e2z;
            float nz = e1x * e2y - e1y * e2x;
            final float length = (float) Math.sqrt(nx * nx + ny * ny + nz * nz);

            final long key = coplanarKey(
                    (ax + bx + cx + dx) * 0.25f,
                    (ay + by + cy + dy) * 0.25f,
                    (az + bz + cz + dz) * 0.25f);
            final Integer previous = coplanarQuadCounts.get(key);
            final int seenBefore = previous == null ? 0 : previous;
            coplanarQuadCounts.put(key, seenBefore + 1);
            if (seenBefore > 0) {
                ++coplanarHits;
            }

            // 2026-11-04：rank = 提交顺序 + 旗帜图案层的提升量。
            // 提升量由 bannerLayerBoost 给出（本帧第几个 banner 图案层）。
            // 注意：多面旗帜会共享这个计数器而累加，所以 ENTITY_COPLANAR_LAYER_MAX
            // 必须够大 —— 0.02 在 rank ≥ 10 时会把各层压到同一点（实测 14 层），
            // 表现为"前几面旗帜正确、后面全错"。
            final int rank = seenBefore + coplanarLayerBoost;
            if (rank == 0 || length < 1.0e-6f) {
                return;
            }

            final float offset = Math.min(
                    rank * ENTITY_COPLANAR_LAYER_STEP, ENTITY_COPLANAR_LAYER_MAX);
            if (coplanarLayerBoost > 0) {
                // 2026-11-04：旗帜图案层**直接朝相机推**，不用几何法线。
                // 旗帜是双面渲染，顶点绕序不保证"法线朝外"；实测几何法线可能朝内，
                // 那样偏移会把图案推到底布**后面** ⇒ 表现正是"只有底色、没有图案"，
                // 而且因为被遮挡得干净，连错乱都看不到。
                // 顶点是相机相对坐标（native 侧才加 cameraWorld），所以相机在原点。
                final float centerX = (ax + bx + cx + dx) * 0.25f;
                final float centerY = (ay + by + cy + dy) * 0.25f;
                final float centerZ = (az + bz + cz + dz) * 0.25f;
                final float toCamera = (float) Math.sqrt(
                        centerX * centerX + centerY * centerY + centerZ * centerZ);
                if (toCamera <= 1.0e-4f) {
                    return;
                }
                nx = -centerX / toCamera * offset;
                ny = -centerY / toCamera * offset;
                nz = -centerZ / toCamera * offset;
            } else {
                nx = nx / length * offset;
                ny = ny / length * offset;
                nz = nz / length * offset;
            }
            for (int vertex = 0; vertex < 4; ++vertex) {
                final int slot = vertex * PENDING_VERTEX_FLOATS;
                pendingQuadVertices[slot] += nx;
                pendingQuadVertices[slot + 1] += ny;
                pendingQuadVertices[slot + 2] += nz;
            }
        }

        /** 质心量化到 1/256 格的 64 位散列键（重合 quad 量化后必得同键）。 */
        private static long coplanarKey(float x, float y, float z) {
            final long qx = Math.round(x * 256.0f);
            final long qy = Math.round(y * 256.0f);
            final long qz = Math.round(z * 256.0f);
            long hash = qx * 2654435761L;
            hash = hash * 31L + qy * 2246822519L;
            hash = hash * 31L + qz * 3266489917L;
            return hash;
        }

        private void emit(int vertex) {
            ensureCapacity(BYTES_PER_VERTEX);
            int offset = vertex * PENDING_VERTEX_FLOATS;
            int color = pendingQuadColors[vertex];
            buffer.putFloat(pendingQuadVertices[offset]).putFloat(pendingQuadVertices[offset + 1])
                    .putFloat(pendingQuadVertices[offset + 2]).putFloat(pendingQuadVertices[offset + 3])
                    .putFloat(pendingQuadVertices[offset + 4]);
            buffer.putFloat(((color >>> 16) & 0xFF) / 255.0f).putFloat(((color >>> 8) & 0xFF) / 255.0f);
            buffer.putFloat((color & 0xFF) / 255.0f).putFloat(((color >>> 24) & 0xFF) / 255.0f);
            buffer.putFloat(0.0f)
                    .putFloat(localPlayerProxy ? 1.0f : emissive ? -1.0f : 0.0f)
                    .putFloat(encodedTextureSlot());
            ++vertexCount;
            final float px = pendingQuadVertices[offset];
            final float py = pendingQuadVertices[offset + 1];
            final float pz = pendingQuadVertices[offset + 2];
            if (px < minX) minX = px;
            if (py < minY) minY = py;
            if (pz < minZ) minZ = pz;
            if (px > maxX) maxX = px;
            if (py > maxY) maxY = py;
            if (pz > maxZ) maxZ = pz;
        }

        private float encodedTextureSlot() {
            if (textureSlot < 1 || textureSlot > MAX_TEXTURE_SLOTS) {
                return 0.0f;
            }


            final int materialClass =
                    frameTextureClasses[textureSlot - 1];


            return materialClass <= 0
                    ? textureSlot
                    : textureSlot + materialClass / MATERIAL_CLASS_SCALE;
        }

        private void ensureCapacity(int extra) {
            if (buffer.remaining() >= extra) return;
            int target = buffer.position() + extra;
            int capacity = buffer.capacity();
            while (capacity < target) capacity = Math.multiplyExact(capacity, 2);
            ByteBuffer replacement = ByteBuffer.allocateDirect(capacity).order(ByteOrder.nativeOrder());
            buffer.flip(); replacement.put(buffer); buffer = replacement;
        }
    }

    private static final class CapturingVertexConsumer implements VertexConsumer {
        private final ActiveBatch batch;
        private boolean hasVertex;
        private float x, y, z, u, v;
        private int color = 0xFFFFFFFF;

        private CapturingVertexConsumer(ActiveBatch batch) { this.batch = batch; }

        @Override public VertexConsumer addVertex(float x, float y, float z) {
            flush();
            this.x = x; this.y = y; this.z = z;
            this.color = 0xFFFFFFFF; this.u = 0.0f; this.v = 0.0f;
            hasVertex = true;
            return this;
        }
        @Override public VertexConsumer setColor(int r, int g, int b, int a) {
            color = (a & 0xFF) << 24 | (r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF);
            return this;
        }
        @Override public VertexConsumer setColor(int color) { this.color = color; return this; }
        @Override public VertexConsumer setUv(float u, float v) { this.u = u; this.v = v; return this; }
        @Override public VertexConsumer setUv1(int u, int v) { return this; }
        @Override public VertexConsumer setUv2(int u, int v) { return this; }
        @Override public VertexConsumer setUv3(float u, float v) { return this; }
        @Override public VertexConsumer setNormal(float x, float y, float z) { return this; }
        @Override public VertexConsumer setLineWidth(float lineWidth) { return this; }
        private void finish() { flush(); }
        private void reset() { hasVertex = false; }
        private void flush() {
            if (hasVertex) {
                final float cu;
                final float cv;
                if (batch.projectiveUv) {
                    final float wx = x + cameraWorldX;
                    final float wy = y + cameraWorldY;
                    final float wz = z + cameraWorldZ;
                    cu = fract01(wx * 0.25f);
                    cv = fract01(wz * 0.25f + wy * 0.125f);
                } else {
                    cu = u;
                    cv = v;
                }
                batch.capture(x, y, z, color, cu, cv);
            }
            hasVertex = false;
        }
    }

}
