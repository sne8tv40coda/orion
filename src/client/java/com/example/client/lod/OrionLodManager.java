package com.example.client.lod;

import com.example.client.compat.dh.DhBridge;
import com.example.client.config.RTXSettingsConfig;
import com.example.vulkan.NativeVulkan;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientChunkCache;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.multiplayer.ServerData;
import net.minecraft.client.server.IntegratedServer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.QuartPos;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ChunkMap;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeResolver;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.levelgen.FlatLevelSource;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.lighting.LayerLightEventListener;
import net.minecraft.world.phys.Vec3;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.PriorityBlockingQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Orion 内置自研远景 LOD 管理器。
 * <p>严格遵守零卡顿纪律与「真实性优先」原则：
 * <ul>
 *   <li>客户端渲染线程每帧最多提取 {@code <= 2} 个实时区块且总时长控制在 {@code <= 450 us}；</li>
 *   <li>后台守护线程优先通过 {@link ChunkMap#read} 只读读取存档 {@code .mca} 真实区块，
 *       仅在磁盘未生成荒野且种子一致性校验通过时才调用 {@link ChunkGenerator#getBaseHeight}，
 *       杜绝与存档地形不符的种子造出“远看是山、飞过去是海”的假地形；</li>
 *   <li>实机与离线重建的真实区块都会持久化至 {@code .minecraft/orion_lod/}，二次加载秒恢复。</li>
 * </ul>
 */
public final class OrionLodManager {

    private static final int MAX_REAL_CHUNKS_PER_FRAME = 2;
    private static final long MAX_CAPTURE_NS_PER_FRAME = 450_000L;
    private static final int MAX_PENDING_REAL_CHUNKS = 2048;
    private static final int MAX_PENDING_GEN_TILES = 1024;
    private static final int GEN_THREADS = 2;

    private static final ConcurrentLinkedQueue<Long> PENDING_REAL_CHUNKS = new ConcurrentLinkedQueue<>();
    private static final Set<Long> PENDING_REAL_CHUNK_SET = ConcurrentHashMap.newKeySet();
    private static final Set<Long> CAPTURED_CHUNKS_THIS_SESSION = Collections.synchronizedSet(new HashSet<>());

    private static final PriorityBlockingQueue<GenTask> GEN_QUEUE = new PriorityBlockingQueue<>();
    private static final Set<Long> GEN_SCHEDULED_KEYS = ConcurrentHashMap.newKeySet();

    private static final ConcurrentLinkedQueue<SeedProbe> SEED_VERIFY_QUEUE = new ConcurrentLinkedQueue<>();
    private static final AtomicInteger SEED_CHECK_SAMPLES = new AtomicInteger(0);
    private static final AtomicLong SEED_CHECK_ERROR_SUM = new AtomicLong(0L);
    private static final AtomicBoolean SEED_MATCHES_WORLD = new AtomicBoolean(true);

    private static final AtomicLong WORLD_EPOCH = new AtomicLong(1L);
    private static final AtomicInteger CONFIGURED_MAX_DIST = new AtomicInteger(96);

    private static final byte[] REAL_SCRATCH_BYTES = new byte[4096];
    private static final ByteBuffer REAL_SCRATCH_BUF = ByteBuffer.wrap(REAL_SCRATCH_BYTES).order(ByteOrder.LITTLE_ENDIAN);
    private static final int[] MAT_ID_SCRATCH = new int[1];

    private static volatile boolean workersStarted = false;
    private static volatile GenContext currentGenContext = null;

    private static String lastWorldKey = "";
    private static boolean lastEnabled = false;
    private static int lastMaxDistChunks = -1;
    private static long frameCounter = 0L;
    private static int lastPlanChunkX = Integer.MIN_VALUE;
    private static int lastPlanChunkZ = Integer.MIN_VALUE;

    private OrionLodManager() {
    }

    private record SeedProbe(long epoch, int wx, int wz, int realTopY) {
    }

    private record GenContext(
            long epoch,
            ChunkGenerator generator,
            ChunkMap chunkMap,
            RandomState randomState,
            BiomeResolver biomeResolver,
            LevelHeightAccessor heightAccessor,
            int seaLevel,
            int dimKind,
            boolean isFlat,
            int flatTopY,
            int flatSubY,
            int flatTopRgb,
            int flatSubRgb,
            int flatMatId,
            boolean flatHasWater,
            int flatWaterDepthByte
    ) {
    }

    private static final class GenTask implements Comparable<GenTask> {
        final long epoch;
        final int lod;
        final int tx;
        final int tz;
        final double distSq;
        final GenContext ctx;

        GenTask(long epoch, int lod, int tx, int tz, double distSq, GenContext ctx) {
            this.epoch = epoch;
            this.lod = lod;
            this.tx = tx;
            this.tz = tz;
            this.distSq = distSq;
            this.ctx = ctx;
        }

        @Override
        public int compareTo(GenTask o) {
            int c = Integer.compare(this.lod, o.lod);
            if (c != 0) return c;
            return Double.compare(this.distSq, o.distSq);
        }
    }

    public static void onChunkLoaded(int chunkX, int chunkZ) {
        if (!RTXSettingsConfig.isBuiltinLodEnabled() || DhBridge.active()) {
            return;
        }
        long key = ChunkPos.pack(chunkX, chunkZ);
        if (PENDING_REAL_CHUNK_SET.size() >= MAX_PENDING_REAL_CHUNKS) {
            return;
        }
        if (PENDING_REAL_CHUNK_SET.add(key)) {
            PENDING_REAL_CHUNKS.offer(key);
        }
    }

    public static void onSettingsChanged() {
        lastPlanChunkX = Integer.MIN_VALUE;
        lastPlanChunkZ = Integer.MIN_VALUE;
    }

    public static void reset() {
        WORLD_EPOCH.incrementAndGet();
        currentGenContext = null;
        lastWorldKey = "";
        lastEnabled = false;
        resetPlannerAndSeedCheckState();
        if (NativeVulkan.isLoaded()) {
            NativeVulkan.clearOrionLod();
        }
    }

    private static void resetPlannerAndSeedCheckState() {
        lastPlanChunkX = Integer.MIN_VALUE;
        lastPlanChunkZ = Integer.MIN_VALUE;
        PENDING_REAL_CHUNKS.clear();
        PENDING_REAL_CHUNK_SET.clear();
        CAPTURED_CHUNKS_THIS_SESSION.clear();
        GEN_QUEUE.clear();
        GEN_SCHEDULED_KEYS.clear();
        SEED_VERIFY_QUEUE.clear();
        SEED_CHECK_SAMPLES.set(0);
        SEED_CHECK_ERROR_SUM.set(0L);
        SEED_MATCHES_WORLD.set(true);
    }

    public static void tick(Vec3 cameraPos) {
        if (!NativeVulkan.isLoaded()) {
            return;
        }
        final Minecraft mc = Minecraft.getInstance();
        final ClientLevel level = mc.level;
        if (level == null || cameraPos == null) {
            if (lastEnabled) {
                lastEnabled = false;
                NativeVulkan.configureOrionLod(false, 96, 63, -64, 320, 0, "");
            }
            return;
        }

        final boolean enabled = RTXSettingsConfig.isBuiltinLodEnabled() && !DhBridge.active();
        final int maxDistChunks = RTXSettingsConfig.getBuiltinLodDistanceChunks();
        CONFIGURED_MAX_DIST.set(maxDistChunks);

        final int dimKind = resolveDimKind(level);
        final int seaLevel = level.getSeaLevel();
        final int minY = level.getMinY();
        final int maxY = level.getMaxY();
        final String worldKey = buildWorldCacheKey(mc, level);

        if (enabled != lastEnabled || maxDistChunks != lastMaxDistChunks || !worldKey.equals(lastWorldKey)) {
            final boolean worldChanged = !worldKey.equals(lastWorldKey);
            lastEnabled = enabled;
            lastMaxDistChunks = maxDistChunks;
            lastWorldKey = worldKey;

            final String cacheDir = resolveCacheDirectory(mc, worldKey);
            NativeVulkan.configureOrionLod(enabled, maxDistChunks, seaLevel, minY, maxY, dimKind, cacheDir);

            if (worldChanged) {
                WORLD_EPOCH.incrementAndGet();
                currentGenContext = null;
                resetPlannerAndSeedCheckState();
            }
        }

        if (!enabled) {
            return;
        }

        final int vanillaRd = Math.max(4, mc.options.getEffectiveRenderDistance());
        NativeVulkan.updateOrionLodCamera(cameraPos.x, cameraPos.y, cameraPos.z, vanillaRd);

        // 1. 定期温和扫描周边已加载区块补缺
        frameCounter++;
        if ((frameCounter % 15L) == 1L) {
            enqueueNearbyLoadedChunks(level, cameraPos, vanillaRd);
        }

        // 2. 严格限额捕捉真实区块（绝不制造主线程帧尖峰）
        capturePendingRealChunks(level);

        // 3. 单人世界：确保后台生成器就绪并排程远景瓦片
        ensureGenWorkersStarted();
        ensureSingleplayerGenContext(mc, level, dimKind, seaLevel, minY, level.getHeight());
        scheduleFarTilesIfNeeded(cameraPos, maxDistChunks);
    }

    private static void enqueueNearbyLoadedChunks(ClientLevel level, Vec3 cam, int vanillaRd) {
        if (PENDING_REAL_CHUNK_SET.size() >= MAX_PENDING_REAL_CHUNKS) {
            return;
        }
        final int camCx = ((int) Math.floor(cam.x)) >> 4;
        final int camCz = ((int) Math.floor(cam.z)) >> 4;
        final int r = Math.min(vanillaRd, 16);
        final ClientChunkCache cache = level.getChunkSource();
        int added = 0;

        for (int dz = -r; dz <= r && added < 16; dz++) {
            for (int dx = -r; dx <= r && added < 16; dx++) {
                final int cx = camCx + dx;
                final int cz = camCz + dz;
                final long key = ChunkPos.pack(cx, cz);
                if (CAPTURED_CHUNKS_THIS_SESSION.contains(key) || PENDING_REAL_CHUNK_SET.contains(key)) {
                    continue;
                }
                if (cache.getChunk(cx, cz, ChunkStatus.FULL, false) != null) {
                    if (PENDING_REAL_CHUNK_SET.add(key)) {
                        PENDING_REAL_CHUNKS.offer(key);
                        added++;
                    }
                }
            }
        }
    }

    private static void capturePendingRealChunks(ClientLevel level) {
        final ClientChunkCache cache = level.getChunkSource();
        final LayerLightEventListener skyListener = level.getLightEngine().getLayerListener(LightLayer.SKY);
        final int minY = level.getMinY();
        final int maxY = level.getMaxY() - 1;
        final BlockPos.MutableBlockPos mpos = new BlockPos.MutableBlockPos();
        final long startNs = System.nanoTime();

        int processed = 0;
        while (processed < MAX_REAL_CHUNKS_PER_FRAME) {
            if (processed > 0 && (System.nanoTime() - startNs) >= MAX_CAPTURE_NS_PER_FRAME) {
                break;
            }
            Long boxedKey = PENDING_REAL_CHUNKS.poll();
            if (boxedKey == null) {
                break;
            }
            long packed = boxedKey;
            PENDING_REAL_CHUNK_SET.remove(packed);

            int cx = ChunkPos.getX(packed);
            int cz = ChunkPos.getZ(packed);
            LevelChunk chunk = cache.getChunk(cx, cz, ChunkStatus.FULL, false);
            if (chunk == null) {
                continue;
            }

            extractRealChunkColumns(chunk, skyListener, minY, maxY, mpos);
            NativeVulkan.submitOrionLodRealChunk(cx, cz, REAL_SCRATCH_BYTES);
            CAPTURED_CHUNKS_THIS_SESSION.add(packed);
            processed++;
        }
    }

    private static void extractRealChunkColumns(
            LevelChunk chunk,
            LayerLightEventListener skyListener,
            int minY, int maxY,
            BlockPos.MutableBlockPos mpos
    ) {
        final int baseX = chunk.getPos().getMinBlockX();
        final int baseZ = chunk.getPos().getMinBlockZ();
        int centerTopY = -32768;

        for (int lz = 0; lz < 16; lz++) {
            final int wz = baseZ + lz;
            for (int lx = 0; lx < 16; lx++) {
                final int wx = baseX + lx;
                final int idx = (lz << 4) | lx;

                int hmTop = chunk.getHeight(Heightmap.Types.WORLD_SURFACE, lx, lz);
                int scanStart = Math.min(maxY, Math.max(minY, hmTop));
                int scanFloor = Math.max(minY, scanStart - 28);

                BlockState topState = null;
                int topY = -32768;
                boolean foundWater = false;
                int waterTopY = -32768;
                BlockState waterState = null;
                int solidBelowY = -32768;
                BlockState solidBelowState = null;

                for (int y = scanStart; y >= scanFloor; y--) {
                    mpos.set(wx, y, wz);
                    BlockState bs = chunk.getBlockState(mpos);
                    if (bs.isAir()) {
                        continue;
                    }
                    if (!bs.getFluidState().isEmpty()) {
                        if (!foundWater && bs.getFluidState().is(FluidTags.WATER)) {
                            foundWater = true;
                            waterTopY = y;
                            waterState = bs;
                        }
                        if (topState == null) {
                            topState = bs;
                            topY = y;
                        }
                        continue;
                    }
                    if (topState == null) {
                        topState = bs;
                        topY = y;
                    }
                    if (solidBelowState == null && !bs.is(net.minecraft.tags.BlockTags.LEAVES)) {
                        solidBelowState = bs;
                        solidBelowY = y;
                        break;
                    }
                }

                if (topState == null) {
                    OrionLodColorPalette.writeColumn(
                            REAL_SCRATCH_BUF, idx,
                            -32768, -32768, 0, 0, 0,
                            0, false, false, 0, 0, 0, 0, 0
                    );
                    continue;
                }

                final int qx = QuartPos.fromBlock(wx);
                final int qy = QuartPos.fromBlock(topY);
                final int qz = QuartPos.fromBlock(wz);
                final Holder<Biome> biome = chunk.getNoiseBiome(qx, qy, qz);

                final int topRgb = OrionLodColorPalette.resolveRealBlockColor(
                        foundWater ? waterState : topState,
                        biome, wx, wz, MAT_ID_SCRATCH
                );
                int matId = MAT_ID_SCRATCH[0];
                int effectiveTopY = foundWater ? waterTopY : topY;
                if (lx == 8 && lz == 8) {
                    centerTopY = effectiveTopY + 1;
                }

                int subY = solidBelowY > -32000 ? solidBelowY : (effectiveTopY - 4);
                int subRgb = topRgb;
                if (solidBelowState != null) {
                    final int[] tmpMat = new int[1];
                    subRgb = OrionLodColorPalette.resolveRealBlockColor(solidBelowState, biome, wx, wz, tmpMat);
                }

                int waterDepthByte = 0;
                if (foundWater) {
                    int wd = Math.max(1, waterTopY - (solidBelowY > -32000 ? solidBelowY : (waterTopY - 8)));
                    waterDepthByte = Math.min(255, wd * 8);
                }

                int skyVal = 15;
                if (skyListener != null) {
                    mpos.set(wx, Math.min(maxY, effectiveTopY + 1), wz);
                    skyVal = skyListener.getLightValue(mpos);
                }
                int skyByte = Math.max(24, Math.min(255, skyVal * 17));

                OrionLodColorPalette.writeColumn(
                        REAL_SCRATCH_BUF, idx,
                        effectiveTopY + 1, subY + 1,
                        (topRgb >> 16) & 0xFF, (topRgb >> 8) & 0xFF, topRgb & 0xFF,
                        matId, foundWater, true,
                        (subRgb >> 16) & 0xFF, (subRgb >> 8) & 0xFF, subRgb & 0xFF,
                        skyByte, waterDepthByte
                );
            }
        }

        if (centerTopY > -32000 && SEED_CHECK_SAMPLES.get() < 16
                && ((chunk.getPos().x() + chunk.getPos().z()) & 3) == 0
                && SEED_VERIFY_QUEUE.size() < 32) {
            SEED_VERIFY_QUEUE.offer(new SeedProbe(WORLD_EPOCH.get(), baseX + 8, baseZ + 8, centerTopY));
        }
    }

    private static void ensureSingleplayerGenContext(
            Minecraft mc, ClientLevel clientLevel,
            int dimKind, int seaLevel, int minY, int height
    ) {
        final long epoch = WORLD_EPOCH.get();
        final GenContext existing = currentGenContext;
        if (existing != null && existing.epoch == epoch) {
            return;
        }

        final IntegratedServer server = mc.getSingleplayerServer();
        if (server == null) {
            return;
        }
        try {
            final ResourceKey<Level> dimKey = clientLevel.dimension();
            final ServerLevel serverLevel = server.getLevel(dimKey);
            if (serverLevel == null) {
                return;
            }
            final ChunkGenerator generator = serverLevel.getChunkSource().getGenerator();
            final ChunkMap chunkMap = serverLevel.getChunkSource().chunkMap;
            final RandomState randomState = serverLevel.getChunkSource().randomState();
            final BiomeSource biomeSource = generator.getBiomeSource();
            final BiomeResolver resolver = biomeSource.createCachingResolver(randomState);
            final LevelHeightAccessor heightAccessor = LevelHeightAccessor.create(minY, height);
            final int effectiveSeaLevel = generator.getSeaLevel();

            boolean isFlat = false;
            int flatTopY = -32768;
            int flatSubY = -32768;
            int flatTopRgb = 0x72B44E;
            int flatSubRgb = 0x7C7C7C;
            int flatMatId = OrionLodColorPalette.MAT_OPAQUE;
            boolean flatHasWater = false;
            int flatWaterDepthByte = 0;

            if (generator instanceof FlatLevelSource flatSource) {
                isFlat = true;
                var layers = flatSource.settings().getLayers();
                Holder<Biome> flatBiome = flatSource.settings().getBiome();
                int topIdx = -1;
                BlockState topState = null;
                int subIdx = -1;
                BlockState subState = null;
                for (int yIdx = layers.size() - 1; yIdx >= 0; yIdx--) {
                    BlockState bs = layers.get(yIdx);
                    if (bs != null && !bs.isAir()) {
                        if (topState == null) {
                            topState = bs;
                            topIdx = yIdx;
                        } else if (subState == null && bs.getFluidState().isEmpty()) {
                            subState = bs;
                            subIdx = yIdx;
                            break;
                        }
                    }
                }
                if (topState != null) {
                    int[] tmpMat = new int[1];
                    flatTopY = minY + topIdx + 1;
                    flatTopRgb = OrionLodColorPalette.resolveRealBlockColor(topState, flatBiome, 0, 0, tmpMat);
                    flatMatId = tmpMat[0];
                    flatHasWater = (flatMatId == OrionLodColorPalette.MAT_WATER);
                    if (subState != null) {
                        flatSubY = minY + subIdx + 1;
                        int[] subMat = new int[1];
                        flatSubRgb = OrionLodColorPalette.resolveRealBlockColor(subState, flatBiome, 0, 0, subMat);
                    } else {
                        flatSubY = flatTopY - 4;
                        flatSubRgb = flatTopRgb;
                    }
                    if (flatHasWater) {
                        int wd = Math.max(1, flatTopY - flatSubY);
                        flatWaterDepthByte = Math.min(255, wd * 8);
                    }
                }
            }

            currentGenContext = new GenContext(
                    epoch, generator, chunkMap, randomState, resolver, heightAccessor,
                    effectiveSeaLevel, dimKind,
                    isFlat, flatTopY, flatSubY, flatTopRgb, flatSubRgb,
                    flatMatId, flatHasWater, flatWaterDepthByte
            );
        } catch (Throwable t) {
            currentGenContext = null;
        }
    }

    private static void scheduleFarTilesIfNeeded(Vec3 cam, int maxDistChunks) {
        final GenContext ctx = currentGenContext;
        if (ctx == null || ctx.epoch != WORLD_EPOCH.get()) {
            return;
        }
        final int curCx = ((int) Math.floor(cam.x)) >> 4;
        final int curCz = ((int) Math.floor(cam.z)) >> 4;
        if (Math.abs(curCx - lastPlanChunkX) < 2 && Math.abs(curCz - lastPlanChunkZ) < 2
                && GEN_QUEUE.size() > 32) {
            return;
        }
        lastPlanChunkX = curCx;
        lastPlanChunkZ = curCz;

        final double maxBlocks = maxDistChunks * 16.0;
        for (int lod = 0; lod < 4; lod++) {
            if (GEN_QUEUE.size() >= MAX_PENDING_GEN_TILES) {
                break;
            }
            final int step = 2 << lod;
            final int tileBlocks = step * 16;
            final double ringMax = (lod == 0) ? Math.min(maxBlocks, 360.0)
                    : ((lod == 1) ? Math.min(maxBlocks, 720.0)
                    : ((lod == 2) ? Math.min(maxBlocks, Math.max(1100.0, maxBlocks * 0.65))
                    : maxBlocks));
            final int camTx = Math.floorDiv((int) Math.floor(cam.x), tileBlocks);
            final int camTz = Math.floorDiv((int) Math.floor(cam.z), tileBlocks);
            final int tileR = Math.max(1, (int) Math.ceil(ringMax / tileBlocks));

            for (int dz = -tileR; dz <= tileR; dz++) {
                for (int dx = -tileR; dx <= tileR; dx++) {
                    if (GEN_QUEUE.size() >= MAX_PENDING_GEN_TILES) {
                        return;
                    }
                    final int tx = camTx + dx;
                    final int tz = camTz + dz;
                    final double cx = (tx + 0.5) * tileBlocks;
                    final double cz = (tz + 0.5) * tileBlocks;
                    final double dSq = (cx - cam.x) * (cx - cam.x) + (cz - cam.z) * (cz - cam.z);
                    if (dSq > (ringMax + tileBlocks) * (ringMax + tileBlocks)) {
                        continue;
                    }
                    final long packedKey = packTileKey(lod, tx, tz);
                    if (GEN_SCHEDULED_KEYS.add(packedKey)) {
                        GEN_QUEUE.offer(new GenTask(ctx.epoch, lod, tx, tz, dSq, ctx));
                    }
                }
            }
        }
    }

    private static void ensureGenWorkersStarted() {
        if (workersStarted) {
            return;
        }
        synchronized (OrionLodManager.class) {
            if (workersStarted) {
                return;
            }
            workersStarted = true;
            for (int i = 0; i < GEN_THREADS; i++) {
                Thread t = new Thread(OrionLodManager::genWorkerLoop, "Orion-LOD-Gen-" + i);
                t.setDaemon(true);
                t.setPriority(Thread.MIN_PRIORITY + 1);
                t.start();
            }
        }
    }

    private static void drainSeedVerifyQueue(GenContext ctx) {
        if (ctx.isFlat || ctx.dimKind != 0) {
            SEED_VERIFY_QUEUE.clear();
            return;
        }
        SeedProbe probe;
        while ((probe = SEED_VERIFY_QUEUE.poll()) != null) {
            if (probe.epoch != WORLD_EPOCH.get()) {
                continue;
            }
            int surfY = ctx.generator.getBaseHeight(
                    probe.wx, probe.wz, Heightmap.Types.WORLD_SURFACE_WG,
                    ctx.heightAccessor, ctx.randomState);
            long diff = Math.abs((long) probe.realTopY - (long) surfY);
            int count = SEED_CHECK_SAMPLES.incrementAndGet();
            long totalErr = SEED_CHECK_ERROR_SUM.addAndGet(diff);
            if (count >= 8 && (totalErr / count) > 22L) {
                if (SEED_MATCHES_WORLD.compareAndSet(true, false)) {
                    // 确认当前存档地形与种子预测不匹配（如改造图/旧版本升级/自定义地形），
                    // 立即清理此前由种子生成的未探索预测瓦片，只保留 .mca 与实机真实区块！
                    GEN_QUEUE.clear();
                    GEN_SCHEDULED_KEYS.clear();
                    CAPTURED_CHUNKS_THIS_SESSION.clear();
                    lastPlanChunkX = Integer.MIN_VALUE;
                    lastPlanChunkZ = Integer.MIN_VALUE;
                    if (NativeVulkan.isLoaded()) {
                        NativeVulkan.clearOrionLod();
                    }
                }
            }
        }
    }

    private static void genWorkerLoop() {
        final byte[] tileBytes = new byte[4096];
        final ByteBuffer tileBuf = ByteBuffer.wrap(tileBytes).order(ByteOrder.LITTLE_ENDIAN);

        while (true) {
            try {
                GenTask task = GEN_QUEUE.take();
                if (task.epoch != WORLD_EPOCH.get() || !RTXSettingsConfig.isBuiltinLodEnabled() || DhBridge.active()) {
                    continue;
                }
                final GenContext ctx = task.ctx;
                drainSeedVerifyQueue(ctx);

                final int step = 2 << task.lod;
                final int tileBlocks = step * 16;
                final int baseX = task.tx * tileBlocks;
                final int baseZ = task.tz * tileBlocks;
                final int quarterSampleY = ctx.dimKind == 1 ? 16 : 20;

                if (ctx.isFlat) {
                    final boolean flatValid = ctx.flatTopY > -32000;
                    for (int idx = 0; idx < 256; idx++) {
                        OrionLodColorPalette.writeColumn(
                                tileBuf, idx,
                                ctx.flatTopY, ctx.flatSubY,
                                (ctx.flatTopRgb >> 16) & 0xFF,
                                (ctx.flatTopRgb >> 8) & 0xFF,
                                ctx.flatTopRgb & 0xFF,
                                ctx.flatMatId, ctx.flatHasWater, flatValid,
                                (ctx.flatSubRgb >> 16) & 0xFF,
                                (ctx.flatSubRgb >> 8) & 0xFF,
                                ctx.flatSubRgb & 0xFF,
                                255, ctx.flatWaterDepthByte
                        );
                    }
                } else {
                    // 仅对本 Tile 内涉及到的区块各从磁盘读取一次 .mca（若无则标记为 MISS），绝不阻塞服务端主线程
                    final Map<Long, OrionMcaChunkDecoder.DecodedChunk> tileChunkCache = new HashMap<>(32);
                    final boolean allowProceduralFallback = SEED_MATCHES_WORLD.get();
                    int mcaReadsInTile = 0;

                    for (int gz = 0; gz < 16; gz++) {
                        final int wz = baseZ + gz * step + (step >> 1);
                        final int cz = wz >> 4;
                        final int lz = wz & 15;
                        for (int gx = 0; gx < 16; gx++) {
                            if (task.epoch != WORLD_EPOCH.get()) {
                                break;
                            }
                            final int wx = baseX + gx * step + (step >> 1);
                            final int cx = wx >> 4;
                            final int lx = wx & 15;
                            final int idx = (gz << 4) | gx;

                            final long cKey = ChunkPos.pack(cx, cz);
                            OrionMcaChunkDecoder.DecodedChunk decoded;
                            if (tileChunkCache.containsKey(cKey)) {
                                decoded = tileChunkCache.get(cKey);
                            } else {
                                decoded = OrionMcaChunkDecoder.readChunk(ctx.chunkMap, cx, cz, ctx.dimKind);
                                tileChunkCache.put(cKey, decoded);
                                if ((++mcaReadsInTile & 15) == 0) {
                                    Thread.yield();
                                }
                            }

                            if (decoded != null) {
                                OrionMcaChunkDecoder.sampleColumn(decoded, lx, lz, tileBuf, idx);
                            } else if (allowProceduralFallback) {
                                int surfY = ctx.generator.getBaseHeight(
                                        wx, wz, Heightmap.Types.WORLD_SURFACE_WG,
                                        ctx.heightAccessor, ctx.randomState);
                                int floorY = ctx.generator.getBaseHeight(
                                        wx, wz, Heightmap.Types.OCEAN_FLOOR_WG,
                                        ctx.heightAccessor, ctx.randomState);

                                final int qx = QuartPos.fromBlock(wx);
                                final int qz = QuartPos.fromBlock(wz);
                                final int qy = Math.max(QuartPos.fromBlock(ctx.heightAccessor.getMinY()),
                                        Math.min(QuartPos.fromBlock(Math.max(surfY, floorY)), quarterSampleY + 60));
                                final Holder<Biome> biome = ctx.biomeResolver.getNoiseBiome(qx, qy, qz);
                                String biomeId = "";
                                if (biome != null) {
                                    var keyOpt = biome.unwrapKey();
                                    if (keyOpt.isPresent()) {
                                        biomeId = keyOpt.get().identifier().getPath();
                                    }
                                }

                                OrionLodColorPalette.resolveProceduralCell(
                                        wx, wz, surfY, floorY, ctx.seaLevel,
                                        ctx.dimKind, biome, biomeId, step, tileBuf, idx
                                );
                            } else {
                                OrionLodColorPalette.writeColumn(
                                        tileBuf, idx,
                                        -32768, -32768, 0, 0, 0,
                                        0, false, false, 0, 0, 0, 0, 0
                                );
                            }
                        }
                    }
                }

                if (task.epoch == WORLD_EPOCH.get() && NativeVulkan.isLoaded()) {
                    NativeVulkan.submitOrionLodTile(task.lod, task.tx, task.tz, tileBytes, false);
                }
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
                return;
            } catch (Throwable ignored) {
                // 单瓦片异常直接跳过，永不影响主循环
            }
        }
    }

    private static long packTileKey(int lod, int tx, int tz) {
        long xBits = ((long) tx & 0x3FFFFFFL) << 28;
        long zBits = ((long) tz & 0x3FFFFFFL) << 2;
        return ((long) (lod & 3)) | xBits | zBits;
    }

    private static int resolveDimKind(ClientLevel level) {
        final String path = level.dimension().identifier().getPath();
        if ("the_nether".equals(path)) return 1;
        if ("the_end".equals(path)) return 2;
        return 0;
    }

    private static String buildWorldCacheKey(Minecraft mc, ClientLevel level) {
        String worldName = "unknown";
        IntegratedServer server = mc.getSingleplayerServer();
        if (server != null) {
            worldName = "sp_" + server.getWorldData().getLevelName();
        } else {
            ServerData sd = mc.getCurrentServer();
            if (sd != null && sd.ip != null && !sd.ip.isEmpty()) {
                worldName = "mp_" + sd.ip;
            }
        }
        String dim = level.dimension().identifier().getNamespace() + "_" + level.dimension().identifier().getPath();
        return sanitize(worldName) + "/" + sanitize(dim);
    }

    private static String resolveCacheDirectory(Minecraft mc, String worldKey) {
        if (worldKey == null || worldKey.isEmpty()) {
            return "";
        }
        Path root = mc.gameDirectory.toPath().resolve("orion_lod").resolve(worldKey);
        return root.toAbsolutePath().toString();
    }

    private static String sanitize(String raw) {
        if (raw == null || raw.isEmpty()) {
            return "default";
        }
        return raw.replaceAll("[^a-zA-Z0-9._-]", "_");
    }
}
