package com.example.client.lod;

import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.tags.BlockTags;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.material.MapColor;

import java.nio.ByteBuffer;

/**
 * Orion 内置远景 LOD 颜色与材质调色盘。
 * 负责将真实方块/群系颜色以及程序生成的群系地形编码为 16-byte OrionLodColumn：
 *   int16 topY, int16 subY,
 *   uint8 r, g, b, flags (bit0..2=matId, bit3=hasWater, bit4=valid),
 *   uint8 subR, subG, subB, skyLight,
 *   uint8 waterDepth, pad0, pad1, pad2
 */
public final class OrionLodColorPalette {

    public static final int MAT_OPAQUE = 0;
    public static final int MAT_WATER = 1;
    public static final int MAT_FOLIAGE = 2;
    public static final int MAT_ICE_SNOW = 3;
    public static final int MAT_EMISSIVE = 4;

    public static final int FLAG_HAS_WATER = 1 << 3;
    public static final int FLAG_VALID = 1 << 4;

    private OrionLodColorPalette() {
    }

    public static void writeColumn(
            ByteBuffer leBuf, int index,
            int topY, int subY,
            int r, int g, int b,
            int matId, boolean hasWater, boolean valid,
            int subR, int subG, int subB,
            int skyLight, int waterDepth
    ) {
        final int off = index << 4;
        leBuf.putShort(off, (short) Math.max(-32768, Math.min(32767, topY)));
        leBuf.putShort(off + 2, (short) Math.max(-32768, Math.min(32767, subY)));
        leBuf.put(off + 4, (byte) clampByte(r));
        leBuf.put(off + 5, (byte) clampByte(g));
        leBuf.put(off + 6, (byte) clampByte(b));
        int flags = (matId & 0x07)
                | (hasWater ? FLAG_HAS_WATER : 0)
                | (valid ? FLAG_VALID : 0);
        leBuf.put(off + 7, (byte) flags);
        leBuf.put(off + 8, (byte) clampByte(subR));
        leBuf.put(off + 9, (byte) clampByte(subG));
        leBuf.put(off + 10, (byte) clampByte(subB));
        leBuf.put(off + 11, (byte) clampByte(skyLight));
        leBuf.put(off + 12, (byte) clampByte(waterDepth));
        leBuf.put(off + 13, (byte) 0);
        leBuf.put(off + 14, (byte) 0);
        leBuf.put(off + 15, (byte) 0);
    }

    public static int resolveRealBlockColor(
            BlockState state,
            Holder<Biome> biomeHolder,
            int wx, int wz,
            int[] outMatId
    ) {
        if (state == null || state.isAir()) {
            outMatId[0] = MAT_OPAQUE;
            return 0x7F7F7F;
        }

        final Biome biome = biomeHolder != null ? biomeHolder.value() : null;
        final FluidState fluid = state.getFluidState();

        if (!fluid.isEmpty()) {
            if (fluid.is(FluidTags.LAVA)) {
                outMatId[0] = MAT_EMISSIVE;
                return 0xEA6416;
            }
            if (fluid.is(FluidTags.WATER)) {
                outMatId[0] = MAT_WATER;
                int wc = biome != null ? biome.getWaterColor() : 0x3F76E4;
                return blendRgb(wc, 0x2F6FD8, 0.25f);
            }
        }

        if (state.getLightEmission() >= 8) {
            outMatId[0] = MAT_EMISSIVE;
            MapColor mc = state.getBlock().defaultMapColor();
            return mc != null && mc.col != 0 ? mc.col : 0xFFCC77;
        }

        if (state.is(Blocks.GRASS_BLOCK) || state.is(Blocks.SHORT_GRASS)
                || state.is(Blocks.TALL_GRASS) || state.is(Blocks.FERN)) {
            outMatId[0] = state.is(Blocks.GRASS_BLOCK) ? MAT_OPAQUE : MAT_FOLIAGE;
            int gc = biome != null ? biome.getGrassColor(wx, wz) : 0x79C05A;
            return scaleRgb(gc, 0.92f);
        }

        if (state.is(BlockTags.LEAVES) || state.is(Blocks.VINE)) {
            outMatId[0] = MAT_FOLIAGE;
            if (state.is(Blocks.SPRUCE_LEAVES)) {
                return 0x3D623D;
            }
            if (state.is(Blocks.BIRCH_LEAVES)) {
                return 0x6F9B4E;
            }
            if (state.is(Blocks.CHERRY_LEAVES)) {
                return 0xF0A8C8;
            }
            if (state.is(Blocks.AZALEA_LEAVES) || state.is(Blocks.FLOWERING_AZALEA_LEAVES)) {
                return 0x5E883E;
            }
            int fc = biome != null ? biome.getFoliageColor() : 0x59AE30;
            return scaleRgb(fc, 0.88f);
        }

        if (state.is(BlockTags.SNOW) || state.is(BlockTags.ICE) || state.is(Blocks.PACKED_ICE) || state.is(Blocks.BLUE_ICE)) {
            outMatId[0] = MAT_ICE_SNOW;
            if (state.is(BlockTags.ICE) || state.is(Blocks.PACKED_ICE) || state.is(Blocks.BLUE_ICE)) {
                return 0x97C3F5;
            }
            return 0xEDF4FA;
        }

        outMatId[0] = MAT_OPAQUE;
        if (state.is(Blocks.SAND)) return 0xDBD3A0;
        if (state.is(Blocks.RED_SAND)) return 0xBF6721;
        if (state.is(Blocks.TERRACOTTA)) return 0x985D43;
        if (state.is(Blocks.STONE) || state.is(Blocks.COBBLESTONE) || state.is(Blocks.ANDESITE)) return 0x7C7C7C;
        if (state.is(Blocks.DEEPSLATE)) return 0x4E4E54;
        if (state.is(Blocks.PODZOL)) return 0x6A4E2E;
        if (state.is(Blocks.MYCELIUM)) return 0x6F626E;
        if (state.is(Blocks.QUARTZ_BLOCK) || state.is(Blocks.SMOOTH_QUARTZ)) return 0xEBE6DF;
        if (state.is(Blocks.IRON_BLOCK)) return 0xDCDCDC;

        MapColor mc = state.getBlock().defaultMapColor();
        if (mc == MapColor.SNOW) return 0xCFD5D6;
        if (mc != null && mc.col != 0) {
            return mc.col;
        }
        return 0x7E827A;
    }

    public static void resolveProceduralCell(
            int wx, int wz,
            int surfaceY, int floorY, int seaLevel,
            int dimKind,
            Holder<Biome> biomeHolder,
            String biomeId,
            int step,
            ByteBuffer outBuf, int index
    ) {
        final Biome biome = biomeHolder != null ? biomeHolder.value() : null;
        final String id = biomeId == null ? "" : biomeId;
        final int hash = hash2D(wx / Math.max(1, step), wz / Math.max(1, step));
        final float jitter = ((hash & 0xFF) / 255.0f - 0.5f) * 0.08f;

        // 1. 下界 (dimKind == 1)
        if (dimKind == 1) {
            if (floorY <= 32) {
                // 岩浆海平面
                writeColumn(outBuf, index, 31, 31, 234, 98, 22, MAT_EMISSIVE, false, true,
                        180, 60, 14, 240, 0);
                return;
            }
            int r = 104, g = 42, b = 42;
            int mat = MAT_OPAQUE;
            if (id.contains("crimson")) { r = 135; g = 34; b = 42; }
            else if (id.contains("warped")) { r = 32; g = 118; b = 108; }
            else if (id.contains("soul")) { r = 82; g = 65; b = 53; }
            else if (id.contains("basalt")) { r = 68; g = 69; b = 77; }
            r = clampByte(r * (1.0f + jitter));
            g = clampByte(g * (1.0f + jitter));
            b = clampByte(b * (1.0f + jitter));
            writeColumn(outBuf, index, floorY, floorY - 8, r, g, b, mat, false, true,
                    78, 30, 30, 175, 0);
            return;
        }

        // 2. 末地 (dimKind == 2)
        if (dimKind == 2) {
            if (floorY <= 0) {
                writeColumn(outBuf, index, -32768, -32768, 0, 0, 0, MAT_OPAQUE, false, false,
                        0, 0, 0, 0, 0);
                return;
            }
            int r = clampByte(219 * (1.0f + jitter));
            int g = clampByte(223 * (1.0f + jitter));
            int b = clampByte(158 * (1.0f + jitter));
            writeColumn(outBuf, index, floorY, floorY - 12, r, g, b, MAT_OPAQUE, false, true,
                    188, 191, 134, 210, 0);
            return;
        }

        // 3. 主世界
        // 水体判断：海平面以下且是水域/平坦地面低于海平面
        final boolean isAquaticBiome = id.contains("ocean") || id.contains("river") || id.contains("swamp");
        if (floorY < seaLevel && (surfaceY <= seaLevel + 1 || isAquaticBiome)) {
            int waterCol = biome != null ? biome.getWaterColor() : 0x3F76E4;
            int r = (waterCol >> 16) & 0xFF;
            int g = (waterCol >> 8) & 0xFF;
            int b = waterCol & 0xFF;

            // 冰冻海洋冰面处理
            final float temp = biome != null ? biome.getBaseTemperature() : 0.8f;
            if ((id.contains("frozen") || temp < 0.05f) && ((hash >> 8) & 3) != 0) {
                writeColumn(outBuf, index, seaLevel, floorY, 152, 194, 240, MAT_ICE_SNOW, false, true,
                        120, 160, 210, 255, 0);
                return;
            }

            int depth = Math.max(1, seaLevel - floorY);
            int depthByte = Math.min(255, depth * 8);
            writeColumn(outBuf, index, seaLevel, floorY, r, g, b, MAT_WATER, true, true,
                    118, 114, 98, 255, depthByte);
            return;
        }

        // 陆地地形与群系着色
        int topY = Math.max(surfaceY, floorY);
        final float temp = biome != null ? biome.getBaseTemperature() : 0.8f;
        int mat = MAT_OPAQUE;
        int col = biome != null ? biome.getGrassColor(wx, wz) : 0x72B44E;
        int subR = 122, subG = 122, subB = 122;
        int sky = 250;

        if (id.contains("snow") || id.contains("frozen") || id.contains("ice") || id.contains("grove")
                || topY >= 158 || (temp < 0.15f && topY >= seaLevel + 2)) {
            col = 0xECF3FA;
            mat = MAT_ICE_SNOW;
            subR = 135; subG = 142; subB = 152;
        } else if (id.contains("peaks") || id.contains("stony") || id.contains("winds") || topY >= 132) {
            col = 0x7F8286;
            subR = 108; subG = 110; subB = 114;
        } else if (id.contains("badlands") || id.contains("eroded")) {
            col = (topY & 3) == 0 ? 0xC66E36 : ((topY & 3) == 1 ? 0x9C553A : 0xD29A63);
            subR = 165; subG = 92; subB = 58;
        } else if (id.contains("desert") || (id.contains("beach") && topY <= seaLevel + 5)) {
            col = 0xDDD49F;
            subR = 200; subG = 190; subB = 142;
        } else if (id.contains("mushroom")) {
            col = 0x736370;
            subR = 116; subG = 116; subB = 120;
        } else {
            // 森林 / 针叶林 / 丛林 轻度树冠凹凸与色彩融合
            boolean hasForest = id.contains("forest") || id.contains("taiga") || id.contains("jungle")
                    || id.contains("wood") || id.contains("grove") || id.contains("savanna");
            if (hasForest && ((hash >> 10) & 7) < 5) {
                int fc = biome != null ? biome.getFoliageColor() : 0x56A832;
                if (id.contains("taiga")) fc = 0x456E45;
                else if (id.contains("birch")) fc = 0x76A456;
                else if (id.contains("cherry")) fc = 0xF0A8C8;
                col = blendRgb(fc, col, 0.28f);
                mat = MAT_FOLIAGE;
                topY += 2 + ((hash >> 14) & 3);
                sky = 232;
                subR = 114; subG = 116; subB = 112;
            } else {
                col = scaleRgb(col, 0.96f);
                subR = 124; subG = 122; subB = 118;
            }
        }

        int r = clampByte(((col >> 16) & 0xFF) * (1.0f + jitter));
        int g = clampByte(((col >> 8) & 0xFF) * (1.0f + jitter));
        int b = clampByte((col & 0xFF) * (1.0f + jitter));

        writeColumn(outBuf, index, topY, Math.max(-64, floorY - 6), r, g, b, mat, false, true,
                subR, subG, subB, sky, 0);
    }

    private static int hash2D(int x, int z) {
        int h = x * 0x27d4eb2d ^ z * 0x165667b1;
        h = (h ^ (h >>> 15)) * 0x85ebca6b;
        return h ^ (h >>> 13);
    }

    private static int blendRgb(int rgbA, int rgbB, float t) {
        float it = 1.0f - t;
        int r = clampByte(((rgbA >> 16) & 0xFF) * it + ((rgbB >> 16) & 0xFF) * t);
        int g = clampByte(((rgbA >> 8) & 0xFF) * it + ((rgbB >> 8) & 0xFF) * t);
        int b = clampByte((rgbA & 0xFF) * it + (rgbB & 0xFF) * t);
        return (r << 16) | (g << 8) | b;
    }

    private static int scaleRgb(int rgb, float scale) {
        int r = clampByte(((rgb >> 16) & 0xFF) * scale);
        int g = clampByte(((rgb >> 8) & 0xFF) * scale);
        int b = clampByte((rgb & 0xFF) * scale);
        return (r << 16) | (g << 8) | b;
    }

    public static int clampByte(float v) {
        int iv = Math.round(v);
        return Math.max(0, Math.min(255, iv));
    }

    public static int clampByte(int v) {
        return Math.max(0, Math.min(255, v));
    }
}
