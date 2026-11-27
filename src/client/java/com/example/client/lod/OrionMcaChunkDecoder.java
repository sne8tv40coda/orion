package com.example.client.lod;

import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.server.level.ChunkMap;
import net.minecraft.world.level.ChunkPos;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Optional;

/**
 * 后台线程专用的只读 Anvil (.mca) 区块 NBT 解码器。
 * <p>仅通过 {@link ChunkMap#read(ChunkPos)} 读取磁盘已经保存的区块，
 * <b>绝不</b>触发新区块生成、世界加载或服务端 Tick，用于让远景 LOD 100% 忠实于
 * 存档磁盘中的真实地形（海面、建筑、人工改造地形、旧版本地形），防止种子预测造出假山。</p>
 */
public final class OrionMcaChunkDecoder {

    private static final int KIND_AIR = 0;
    private static final int KIND_SOLID = 1;
    private static final int KIND_WATER = 2;
    private static final int KIND_LAVA = 3;
    private static final int KIND_LEAVES = 4;
    private static final int KIND_SNOW_ICE = 5;
    private static final int KIND_EMISSIVE = 6;

    private OrionMcaChunkDecoder() {
    }

    public static final class DecodedChunk {
        final List<DecodedSection> sectionsDesc; // 按 secY 从高到低排列
        final int dimKind;

        DecodedChunk(List<DecodedSection> sectionsDesc, int dimKind) {
            this.sectionsDesc = sectionsDesc;
            this.dimKind = dimKind;
        }
    }

    private static final class PaletteEntry {
        final int kind;
        final int rgb;

        PaletteEntry(int kind, int rgb) {
            this.kind = kind;
            this.rgb = rgb;
        }
    }

    private static final class DecodedSection {
        final int baseY;
        final PaletteEntry[] palette;
        final long[] data;
        final int bitsPerBlock;
        final int blocksPerLong;
        final long mask;
        final boolean allAir;
        final int singleIdx; // 若 data == null，直接索引 palette[0]

        DecodedSection(
                int baseY,
                PaletteEntry[] palette,
                long[] data,
                int bitsPerBlock,
                int blocksPerLong,
                boolean allAir
        ) {
            this.baseY = baseY;
            this.palette = palette;
            this.data = data;
            this.bitsPerBlock = bitsPerBlock;
            this.blocksPerLong = blocksPerLong;
            this.mask = (bitsPerBlock >= 64) ? -1L : ((1L << bitsPerBlock) - 1L);
            this.allAir = allAir;
            this.singleIdx = 0;
        }

        PaletteEntry sample(int lx, int ly, int lz) {
            if (palette.length == 0) {
                return null;
            }
            if (palette.length == 1 || data == null || blocksPerLong <= 0) {
                return palette[singleIdx];
            }
            final int index = (ly << 8) | (lz << 4) | lx;
            final int longIdx = index / blocksPerLong;
            if (longIdx < 0 || longIdx >= data.length) {
                return palette[0];
            }
            final int bitOff = (index % blocksPerLong) * bitsPerBlock;
            final int palId = (int) ((data[longIdx] >>> bitOff) & mask);
            if (palId < 0 || palId >= palette.length) {
                return palette[0];
            }
            return palette[palId];
        }
    }

    /**
     * 只读读取磁盘已有区块 NBT（无磁盘区块返回 null）。只可在后台线程调用。
     */
    public static DecodedChunk readChunk(ChunkMap chunkMap, int cx, int cz, int dimKind) {
        if (chunkMap == null) {
            return null;
        }
        try {
            Optional<CompoundTag> opt = chunkMap.read(new ChunkPos(cx, cz)).join();
            if (opt == null || opt.isEmpty()) {
                return null;
            }
            return decodeTag(opt.get(), dimKind);
        } catch (Throwable ignored) {
            return null;
        }
    }

    public static DecodedChunk decodeTag(CompoundTag root, int dimKind) {
        if (root == null) {
            return null;
        }
        ListTag secList = root.getListOrEmpty("sections");
        if (secList.isEmpty()) {
            secList = root.getListOrEmpty("Sections");
        }
        if (secList.isEmpty()) {
            return null;
        }

        List<DecodedSection> sections = new ArrayList<>(secList.size());
        for (int i = 0; i < secList.size(); i++) {
            CompoundTag sec = secList.getCompoundOrEmpty(i);
            int secY = sec.getByteOr("Y", (byte) 0);
            CompoundTag blockStates = sec.getCompoundOrEmpty("block_states");
            if (blockStates.isEmpty()) {
                continue;
            }
            ListTag palList = blockStates.getListOrEmpty("palette");
            if (palList.isEmpty()) {
                continue;
            }

            // 读取该段主群系（如有），用于修正草/水/树叶颜色
            String biomeName = "";
            CompoundTag biomesTag = sec.getCompoundOrEmpty("biomes");
            if (!biomesTag.isEmpty()) {
                ListTag biomePal = biomesTag.getListOrEmpty("palette");
                if (!biomePal.isEmpty()) {
                    biomeName = biomePal.getStringOr(0, "");
                }
            }

            PaletteEntry[] pal = new PaletteEntry[palList.size()];
            boolean anyNonAir = false;
            for (int p = 0; p < palList.size(); p++) {
                CompoundTag pTag = palList.getCompoundOrEmpty(p);
                String name = pTag.getStringOr("Name", "minecraft:air");
                PaletteEntry entry = classifyBlock(name, biomeName);
                pal[p] = entry;
                if (entry.kind != KIND_AIR) {
                    anyNonAir = true;
                }
            }
            if (!anyNonAir) {
                continue;
            }

            long[] data = null;
            int bits = 0;
            int perLong = 0;
            if (pal.length > 1) {
                Optional<long[]> dataOpt = blockStates.getLongArray("data");
                if (dataOpt.isPresent()) {
                    data = dataOpt.get();
                    bits = Math.max(4, 32 - Integer.numberOfLeadingZeros(pal.length - 1));
                    perLong = 64 / bits;
                }
            }

            sections.add(new DecodedSection(secY << 4, pal, data, bits, perLong, false));
        }

        if (sections.isEmpty()) {
            return null;
        }
        sections.sort(Comparator.comparingInt((DecodedSection s) -> s.baseY).reversed());
        return new DecodedChunk(sections, dimKind);
    }

    /**
     * 从已解码区块抽取指定列 (lx, lz ∈ [0..15]) 写入 16-byte OrionLodColumn 槽位。
     */
    public static void sampleColumn(
            DecodedChunk chunk,
            int lx, int lz,
            ByteBuffer outBuf, int cellIndex
    ) {
        if (chunk == null || chunk.sectionsDesc.isEmpty()) {
            OrionLodColorPalette.writeColumn(
                    outBuf, cellIndex,
                    -32768, -32768, 0, 0, 0,
                    0, false, false, 0, 0, 0, 0, 0
            );
            return;
        }

        PaletteEntry topEntry = null;
        int topBlockY = -32768;
        boolean foundWater = false;
        int waterTopY = -32768;
        PaletteEntry waterEntry = null;
        PaletteEntry solidBelow = null;
        int solidBelowY = -32768;

        int scannedNonAir = 0;
        final List<DecodedSection> secs = chunk.sectionsDesc;
        for (int sIdx = 0; sIdx < secs.size(); sIdx++) {
            final DecodedSection sec = secs.get(sIdx);
            if (sec.allAir) {
                continue;
            }
            // 下界跳过顶部基岩层 (Y >= 120)，免得看到基岩盖子
            if (chunk.dimKind == 1 && sec.baseY >= 120) {
                continue;
            }

            for (int ly = 15; ly >= 0; ly--) {
                final int worldY = sec.baseY + ly;
                if (chunk.dimKind == 1 && worldY >= 120) {
                    continue;
                }
                final PaletteEntry e = sec.sample(lx, ly, lz);
                if (e == null || e.kind == KIND_AIR) {
                    continue;
                }
                scannedNonAir++;

                if (e.kind == KIND_WATER) {
                    if (!foundWater) {
                        foundWater = true;
                        waterTopY = worldY;
                        waterEntry = e;
                    }
                    if (topEntry == null) {
                        topEntry = e;
                        topBlockY = worldY;
                    }
                    if (scannedNonAir > 28) {
                        break;
                    }
                    continue;
                }

                if (topEntry == null) {
                    topEntry = e;
                    topBlockY = worldY;
                }
                if (solidBelow == null && e.kind != KIND_LEAVES) {
                    solidBelow = e;
                    solidBelowY = worldY;
                    break;
                }
                if (scannedNonAir > 28) {
                    break;
                }
            }
            if (solidBelow != null || scannedNonAir > 28) {
                break;
            }
        }

        if (topEntry == null) {
            OrionLodColorPalette.writeColumn(
                    outBuf, cellIndex,
                    -32768, -32768, 0, 0, 0,
                    0, false, false, 0, 0, 0, 0, 0
            );
            return;
        }

        final PaletteEntry surface = foundWater ? waterEntry : topEntry;
        final int effTopBlockY = foundWater ? waterTopY : topBlockY;
        final int topFaceY = effTopBlockY + 1;
        final int subFaceY = (solidBelowY > -32000 ? solidBelowY : (effTopBlockY - 4)) + 1;
        final int subRgb = (solidBelow != null) ? solidBelow.rgb : surface.rgb;

        int matId = OrionLodColorPalette.MAT_OPAQUE;
        if (surface.kind == KIND_WATER) matId = OrionLodColorPalette.MAT_WATER;
        else if (surface.kind == KIND_LEAVES) matId = OrionLodColorPalette.MAT_FOLIAGE;
        else if (surface.kind == KIND_SNOW_ICE) matId = OrionLodColorPalette.MAT_ICE_SNOW;
        else if (surface.kind == KIND_EMISSIVE) matId = OrionLodColorPalette.MAT_EMISSIVE;

        int waterDepthByte = 0;
        if (foundWater) {
            int wd = Math.max(1, waterTopY - (solidBelowY > -32000 ? solidBelowY : (waterTopY - 8)));
            waterDepthByte = Math.min(255, wd * 8);
        }

        OrionLodColorPalette.writeColumn(
                outBuf, cellIndex,
                topFaceY, subFaceY,
                (surface.rgb >> 16) & 0xFF, (surface.rgb >> 8) & 0xFF, surface.rgb & 0xFF,
                matId, foundWater, true,
                (subRgb >> 16) & 0xFF, (subRgb >> 8) & 0xFF, subRgb & 0xFF,
                chunk.dimKind == 1 ? 185 : 245, waterDepthByte
        );
    }

    private static PaletteEntry classifyBlock(String fullName, String biomeName) {
        if (fullName == null) {
            return new PaletteEntry(KIND_AIR, 0);
        }
        String name = fullName;
        int colon = fullName.indexOf(':');
        if (colon >= 0) {
            name = fullName.substring(colon + 1);
        }

        if ("air".equals(name) || "cave_air".equals(name) || "void_air".equals(name)
                || "barrier".equals(name) || "structure_void".equals(name)
                || "glass".equals(name) || "glass_pane".equals(name)) {
            return new PaletteEntry(KIND_AIR, 0);
        }

        if ("water".equals(name) || "bubble_column".equals(name) || "kelp".equals(name)
                || "kelp_plant".equals(name) || "seagrass".equals(name) || "tall_seagrass".equals(name)) {
            int wc = 0x3D74DC;
            if (biomeName.contains("cold") || biomeName.contains("frozen")) wc = 0x395AC2;
            else if (biomeName.contains("warm") || biomeName.contains("lukewarm")) wc = 0x43A8D5;
            else if (biomeName.contains("swamp")) wc = 0x617B64;
            return new PaletteEntry(KIND_WATER, wc);
        }

        if ("lava".equals(name) || "magma_block".equals(name) || "glowstone".equals(name)
                || "sea_lantern".equals(name) || "shroomlight".equals(name) || "ochre_froglight".equals(name)) {
            return new PaletteEntry(KIND_EMISSIVE, name.contains("sea_lantern") ? 0xB8D8D8 : 0xEA6416);
        }

        if (name.endsWith("_leaves") || "vine".equals(name) || "azalea".equals(name) || "moss_carpet".equals(name)) {
            int fc = 0x579D31;
            if (name.contains("spruce")) fc = 0x436943;
            else if (name.contains("birch")) fc = 0x739E52;
            else if (name.contains("cherry")) fc = 0xF0A8C8;
            else if (biomeName.contains("swamp") || biomeName.contains("mangrove")) fc = 0x6A7039;
            else if (biomeName.contains("savanna") || biomeName.contains("badlands")) fc = 0xAEA42A;
            return new PaletteEntry(KIND_LEAVES, fc);
        }

        if ("snow".equals(name) || "snow_block".equals(name) || "powder_snow".equals(name)
                || "ice".equals(name) || "packed_ice".equals(name) || "blue_ice".equals(name) || "frosted_ice".equals(name)) {
            return new PaletteEntry(KIND_SNOW_ICE, name.contains("ice") ? 0x97C3F5 : 0xEDF4FA);
        }

        if ("grass_block".equals(name) || "short_grass".equals(name) || "tall_grass".equals(name)
                || "fern".equals(name) || "large_fern".equals(name) || "moss_block".equals(name)) {
            int gc = 0x72B44E;
            if (biomeName.contains("snow") || biomeName.contains("frozen") || biomeName.contains("grove")) gc = 0x80B497;
            else if (biomeName.contains("taiga") || biomeName.contains("windswept")) gc = 0x86B783;
            else if (biomeName.contains("savanna") || biomeName.contains("desert")) gc = 0xBFB755;
            else if (biomeName.contains("jungle")) gc = 0x59C93C;
            else if (biomeName.contains("swamp")) gc = 0x6A7039;
            return new PaletteEntry(KIND_SOLID, gc);
        }

        if ("sand".equals(name) || "sandstone".equals(name) || "smooth_sandstone".equals(name)) return new PaletteEntry(KIND_SOLID, 0xDBD3A0);
        if ("red_sand".equals(name) || "red_sandstone".equals(name)) return new PaletteEntry(KIND_SOLID, 0xBF6721);
        if ("dirt".equals(name) || "coarse_dirt".equals(name) || "farmland".equals(name) || "dirt_path".equals(name)) return new PaletteEntry(KIND_SOLID, 0x866043);
        if ("podzol".equals(name)) return new PaletteEntry(KIND_SOLID, 0x6E5130);
        if ("mycelium".equals(name)) return new PaletteEntry(KIND_SOLID, 0x6F626E);
        if ("gravel".equals(name) || "clay".equals(name)) return new PaletteEntry(KIND_SOLID, 0x888B90);
        if ("stone".equals(name) || "cobblestone".equals(name) || "andesite".equals(name) || "stone_bricks".equals(name)) return new PaletteEntry(KIND_SOLID, 0x7E8082);
        if ("granite".equals(name)) return new PaletteEntry(KIND_SOLID, 0x956755);
        if ("diorite".equals(name) || "calcite".equals(name)) return new PaletteEntry(KIND_SOLID, 0xD2D2D4);
        if ("deepslate".equals(name) || "cobbled_deepslate".equals(name) || "tuff".equals(name)) return new PaletteEntry(KIND_SOLID, 0x525259);
        if (name.contains("terracotta")) return new PaletteEntry(KIND_SOLID, 0x9E6045);
        if ("white_concrete".equals(name) || "white_wool".equals(name) || "quartz_block".equals(name) || "smooth_quartz".equals(name)) return new PaletteEntry(KIND_SOLID, 0xE2E4E6);
        if ("iron_block".equals(name)) return new PaletteEntry(KIND_SOLID, 0xDCDCDC);
        if ("gold_block".equals(name)) return new PaletteEntry(KIND_SOLID, 0xF5D248);
        if ("diamond_block".equals(name)) return new PaletteEntry(KIND_SOLID, 0x62EDE4);
        if ("netherrack".equals(name) || "crimson_nylium".equals(name)) return new PaletteEntry(KIND_SOLID, 0x783232);
        if ("warped_nylium".equals(name)) return new PaletteEntry(KIND_SOLID, 0x267A6F);
        if ("soul_sand".equals(name) || "soul_soil".equals(name)) return new PaletteEntry(KIND_SOLID, 0x524135);
        if ("basalt".equals(name) || "blackstone".equals(name) || "obsidian".equals(name)) return new PaletteEntry(KIND_SOLID, 0x3C3B45);
        if ("end_stone".equals(name) || "end_stone_bricks".equals(name)) return new PaletteEntry(KIND_SOLID, 0xDBDF9E);
        if (name.endsWith("_log") || name.endsWith("_wood") || name.endsWith("_planks")) return new PaletteEntry(KIND_SOLID, 0x7A5C3C);

        return new PaletteEntry(KIND_SOLID, 0x828480);
    }
}
