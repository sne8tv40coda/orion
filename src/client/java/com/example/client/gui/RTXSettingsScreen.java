package com.example.client.gui;

import com.example.vulkan.NativeVulkan;

import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.CycleButton;
import net.minecraft.client.gui.components.AbstractSliderButton;
import net.minecraft.client.gui.components.AbstractWidget;
import net.minecraft.client.Options;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.options.OptionsSubScreen;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.layouts.LinearLayout;
import net.minecraft.network.chat.Component;
import com.example.client.config.RTXSettingsConfig;
import java.util.ArrayList;
import java.util.List;
import java.util.function.IntConsumer;
import java.util.function.DoubleConsumer;

public final class RTXSettingsScreen extends OptionsSubScreen {


    private static final Component TITLE =
            Component.literal("RTX 设置");
    private int pendingReflexMode;

    private int pendingRRMode;
    private int pendingCustomUpscalePercent;

    private int pendingGiBounces;
    private int pendingAoNearSamples;
    private int pendingVolumetricRestir;
    private int pendingAoMidSamples;
    private int pendingSunShadowSamples;
    private int pendingLocalLightRisCandidates;
    private int pendingLocalLightAreaSamples;
    private int pendingWaterReflectionInterval;
    private int pendingGlossyFullDistance;
    private int pendingDynamicRtasInterval;
    private int pendingMaxRtParticles;
    private float pendingRtSpp;

    private float pendingNoDirectLightAmbient;

    private float pendingPOMDepth;
    private boolean pendingPOMSharp;

    // 2026-11-02 屏幕空间降级链（单位：区块；0 = 关）
    private float pendingSsptRange;
    private float pendingSsrRange;

    // 2026-11-03 RT 光泽射线总开关（出厂 true）
    private boolean pendingRtGlossy;

    private int pendingGiNearRate;
    private boolean pendingGlossyRoughLod;

    private final float[] pendingUeCloud =
            RTXSettingsConfig.getUeCloudAll();

    private final String[] UE_CLOUD_LABELS = {
            "多散射 octaves", "  每阶贡献", "  每阶遮蔽", "散射亮度补偿",
            "地面反照贡献", "云环境光遮蔽", "逐采样大气透射", "天气图频率倍率"
    };
    private final float[] UE_CLOUD_MIN = {1.0f, 0.0f, 0.05f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    private final float[] UE_CLOUD_MAX = {3.0f, 1.0f, 1.00f, 3.0f, 1.0f, 4.0f, 2.0f, 4.0f};
    private final float[] UE_CLOUD_STEP = {1.0f, 0.05f, 0.05f, 0.05f,
            0.05f, 0.10f, 0.05f, 0.10f};

    private AbstractSliderButton makeUeCloudSlider(
            int index,
            String label,
            float min,
            float max,
            float step
    ) {
        return new AbstractSliderButton(
                0,
                0,
                310,
                20,
                Component.empty(),
                (this.pendingUeCloud[index] - min) / (max - min)
        ) {

            {
                this.updateMessage();
            }


            private float snapped() {
                final float raw =
                        min + (float) this.value * (max - min);
                final float snapped =
                        Math.round(raw / step) * step;
                return Math.max(min, Math.min(max, snapped));
            }


            @Override
            protected void updateMessage() {
                this.setMessage(
                        Component.literal(
                                String.format(
                                        java.util.Locale.ROOT,
                                        "%s: %.2f",
                                        label,
                                        this.snapped()
                                )
                        )
                );
            }


            @Override
            protected void applyValue() {
                final float value =
                        this.snapped();

                this.value =
                        (value - min) / (max - min);

                RTXSettingsScreen.this.pendingUeCloud[index] =
                        value;

                this.updateMessage();
            }
        };
    }
    private boolean pendingTrueSubsurfaceScattering;
    private float pendingLocalLightMaxDistance;
    private float pendingLocalLightIntensity;
    private float pendingDynamicLightIntensity;
    private boolean pendingBuiltinLodEnabled;
    private int pendingBuiltinLodDistanceChunks;
    private float pendingSunDiskLuminance;
    private float pendingSunHaloStrength;
    private float pendingSunAngularDiameterDegrees;
    private float pendingStarIntensity;
    private float pendingGalaxyScale;
    private float pendingGalaxyTiltDegrees;
    private float pendingGalaxyYawDegrees;
    private float pendingGalaxyMix;
    private float pendingSunTintStrength;
    private float pendingAuroraBrightness;
    private float pendingAuroraFlowSpeed;
    private boolean pendingPhysicalSkyEnabled;
    private float pendingScatterIntensity;
    private float pendingOzoneStrength;
    private float pendingPowderStrength;
    private int pendingSkyModel;
    private float pendingSkyModelScale;
    private float pendingNightSkyAmbientScale;
    private float pendingRainSkyDimScale;
    private float pendingMoonLightFloor;
    private float pendingMoonDaylightDim;
    private float pendingFogShapeStrength;
    private float pendingFogDawnBoost;
    private float pendingCloudSunForwardScattering;
    private float pendingCloudSunReddening;
    private float pendingCloudTopStretch;
    private float pendingCloudDetileStrength;
    private float pendingCloudType;
    private float pendingCloudErosionScale;
    private float pendingCloudWeatherScale;
    private float pendingCloudSilver;
    private float pendingCloudViewDistance;

    private boolean pendingCloudEnabled;
    private float pendingCloudBottom;
    private float pendingCloudThickness;
    private float pendingCloudCoverage;
    private float pendingCloudDensity;
    private float pendingCloudDetailStrength;
    private float pendingCloudWindSpeed;
    private float pendingCloudEvolution;
    private int pendingCloudQuality;
    private float pendingCloudSize;
    private float pendingCloudDistribution;
    private float pendingCloudBaseFeather;
    private float pendingCloudSunlightStrength;
    private float msPhiIntensity;
    private float msPhiDepthPow;
    private float msPhiDepthBias;
    private float msOctaveStrength;
    private float msAdaptiveLightRange;
    private float msFarFadeDistance;

    private float phiBoundaryConfidence;
    private float phiMsBuildScale;
    private float phiCompress;
    private float phiConeRatio;


    private float shapeBottomSmoothHeight;
    private float shapeBottomSmoothPow;
    private float shapeTwoTierThreshold;
    private float shapeLightErosion;
    private float moonStrength;
    private float moonPhase;

    private float shapeDetailScale;
    private float shapeDetailContrast;
    private float shapeProfileCurve;

    private float ambTopMul;
    private float ambBottomMul;
    private float ambSunSide;
    private float ambHueFollow;
    private float cloudGroundAlbedo;
    private float cloudGroundBounce;
    private float cloudSunlightResidual;
    private float cloudPhaseFloor;
    private float ms2ScatterTint;
    private float ms2Octaves;
    private float ms2Attenuation;
    private float ms2Eccentricity;
    private float profileType;
    private float profileTypeSpread;
    private float wispyStrength;
    private float wispyEdgeWidth;
    private float wispyReach;
    private float wispyTopHeight;
    private float cloudSkyVisibilityStrength;
    private float cloudSkyVisibilitySteps;
    private float cloudTemporalDampStrength;
    private float cloudBottomOcclusion;
    private float fogExtrasDynamicLightStrength;
    private float fogExtrasGroundAnchorStrength;
    private float fogExtrasSkyAmbientStrength;
    private float fogExtrasTemporalJitterStrength;
    private boolean pendingFogGroundMistEnabled;
    private float pendingFogGroundMistHeight;
    private float pendingFogGroundMistExtinction;
    private float pendingFogGroundMistDistance;
    private float pendingFogGroundMistSpeed;
    private boolean pendingCloudGapEnabled;
    private float pendingCloudGapStrength;
    private float pendingCloudGapWidth;
    private float pendingRainRippleSize;
    private float pendingRainRippleStrength;
    private float pendingFogCurlStrength;
    private float pendingFogTintR;
    private float pendingFogTintG;
    private float pendingFogTintB;
    private float pendingCloudAerialStrength;
    private float pendingCloudStormDarkness;
    private float pendingCloudLightningStrength;
    private float pendingCloudSunsetWarmth;
    private float pendingCloudWeatherCoupling;
    private float pendingCloudPowderEffect;
    private float pendingCloudSilverSpread;
    private float pendingCloudSharpness;
    private float pendingCloudBaseErosion;
    private float groundAerialStrength;
    private float groundAerialOcclusion;
    private boolean msTwoTierStep;
    private boolean msMeanReproject;
    private float wpCrestBacklit;
    private float wpCausticDispersion;
    private float wpCausticDepthDiffusion;
    private boolean wpPhysicalPhase;

    private float waterWaveCalibration;
    private float waterWaveStrength;
    private float waterWaveBlend;
    private float waterReflectCausticStrength;
    private float waterSimSpongeWidth;
    private float waterSimGaussianSource;
    private float waterSimDampingScale;
    private float waterSimPropagationSpeed;
    private float waterSimRainRippleScale;
    private float waterSssNonlinear;
    private float waterSssBoost;
    private float waterSssPhaseG;
    private float waterSssStrength;
    private float waterWaveLengthScale;
    private float waterWaveDirection;
    private float waterWaveCrestSharpness;
    private float waterWaveWarpStrength;
    private float waterWaveAflScale;
    private float waterWaveAflLayers;
    private boolean waterFftEnabled;
    private float waterFftAmplitude;
    private float waterFftChoppiness;
    private float waterFftTilePeriod;
    private float waterFftDetail;
    private float waterFftSwellWeight;
    private float waterFftDetailWeight;
    private float waterFftWindSpeed;
    // 2026-11-22 统一风场（「风场」折叠组，CameraUBO.windParams）
    private float windAngleDeg;
    private float windStrength;
    private float windGust;
    private int postTonemapMode;
    private float postExposureStrength;
    private float postContrast;
    private float postSaturation;
    private float postGamma;
    private int postMirrorBounceCount;
    private int postTlasCullDistance;
    private int postTlasFrustumCullMargin;
    private int postRrFarCloudDistance;
    private float postBloomStrength;
    

    private float postGainR = 1.0f;
    private float postGainG = 1.0f;
    private float postGainB = 1.0f;

    private boolean pendingFogEnabled;
    private float pendingFogDensity;
    private float pendingFogHeight;
    private float pendingFogScaleHeight;
    private float pendingFogAnisotropy;
    private float pendingFogSunStrength;
    private float pendingFogAmbientStrength;
    private float pendingFogMaxDistance;
    private float pendingFogStartDistance;
    private int pendingFogSunSteps;
    private float pendingFogRainBoost;
    private boolean pendingGodRayEnabled;
    private int pendingGodRaySteps;
    private int pendingGodRayOcclusionSamples;
    private float pendingGodRayIntensity;
    private float pendingGodRayReprojection;
    private float pendingRainOpacity;
    private float pendingRainStreakLength;
    private float pendingRainBrightness;
    private float pendingRainWidth;
    private float pendingRainDensityScale;
    private float pendingSnowSize;
    private float pendingSnowDensity;
    private float pendingSnowOpacity;
    private float pendingSnowBrightness;
    private float pendingSnowFallSpeed;
    private float pendingSnowSway;
    private boolean pendingRainWallEnabled;
    private float pendingRainWallWetness;
    private float pendingRainWallDripDensity;
    private float pendingRainWallDripSpeed;
    private float pendingRainWallDripSize;
    private float pendingRainWallDripBump;
    private float pendingRainWallDistance;

    private float pendingWaterStyleWaveScale;
    private float pendingWaterStyleWaveSpeed;
    private float pendingWaterStyleSwellScale;
    private float pendingWaterStyleMidScale;
    private float pendingWaterStyleFineScale;
    private float pendingWaterStyleTintMix;
    private float pendingWaterStyleDensityScale;
    private float pendingWaterStyleCausticStrength;

    private boolean pendingHDR;

    private boolean pendingAutoExposureEnabled;
    private float pendingAutoExposureCompensation;

    private boolean pendingFGFrameSync;

    private boolean pendingFGRefreshCap;

    private boolean pendingSLDevPlugins;

    private boolean pendingSerEnabled;

    private int pendingMotionVectors;

    private int pendingFGMultiplier;

    private int pendingFGBaseFrameLimit;

    private boolean pendingArmorMetal;

    private boolean pendingCloudFadeFollowsRrMode;

    private int pendingCloudResFloorPercent;
    private int pendingCloudTemporalMode;
    private float pendingCloudFarStepDensity;

    private boolean pendingPlaneCloudEnabled;
    private float pendingPlaneCloudHeightOffset;
    private float pendingPlaneCloudScale;
    private float pendingPlaneCloudSpeed;
    private float pendingPlaneCloudDensity;
    private float pendingPlaneCloudCoverage;
    private float pendingPlaneCloudOctaves;
    private float pendingPlaneCloudFadeDistance;
    private float pendingPlaneCloudBrightness;

    private int pendingGiRoulette;
    private int pendingSunMarchFarDistance;

    private int pendingMotionDebugView;

    private int pendingDirectLightMode;
    private int pendingIndirectMode;
    private int pendingSparseRatePercent;


    private final List<AbstractWidget> optionRow =
            new ArrayList<>();

    // 2026-11-13 紧凑预览：把设置压到底部条带，腾出画面实时对比效果。
    // 用静态字段保存，重开界面/换页都保持；不写进存档（纯查看偏好）。
    private static boolean compactPreview;

    // 2026-11-16 UI 审计：三处「默认收起」的静态状态（重开界面保持，不写存档）。
    //   showAdvancedGates —— 光追性能页末尾的实验闸（SHaRC/RTXDI/稀疏/调试图…）
    //   cloudHpExpanded   —— 体积云页的 28 项 HP 全参数
    //   colorGradeExpanded—— 后处理页的 24 项精细分级
    // 默认关：玩家第一眼看到的是能调出效果的项，不是 A/B 闸。
    private static boolean showAdvancedGates;
    private static boolean cloudHpExpanded;
    private static boolean colorGradeExpanded;
    // 2026-11-21 精简批（顾问 §4.1/§4.3）：六个合并页各自的「高级选项」折叠。
    // 界面级状态，不写存档；默认收起。
    private static boolean showUpscalerAdvanced;
    private static boolean showSkyAdvanced;
    private static boolean showWindAdvanced;
    private static boolean showCloudAdvanced;
    private static boolean showFogAdvanced;
    private static boolean showWaterAdvanced;
    private static boolean showPostAdvanced;

    // 底部条带高度（像素）：约 4~5 行滑条，够调当前这一项。
    private static final int COMPACT_STRIP_HEIGHT = 132;

    private static final int PAGE_HUB = -1;
    private static final int PAGE_UPSCALER = 0;
    private static final int PAGE_PERF = 1;
    private static final int PAGE_SKY = 2;
    private static final int PAGE_CLOUD = 3;
    private static final int PAGE_FOG = 4;
    private static final int PAGE_RAIN = 5;
    private static final int PAGE_WATER = 6;
    private static final int PAGE_MATERIAL = 7;
    private static final int PAGE_POST = 8;
    // 9 曾用于「色彩分级」独立页；2026-11-16 并回后处理页，编号保留不复用。
    // 2026-11-13 UI 审计：新增两页
    private static final int PAGE_CELESTIAL = 10;
    private static final int PAGE_CULL = 11;

    private int page = PAGE_HUB;

    private int section = PAGE_HUB;


    public RTXSettingsScreen(
            Screen lastScreen,
            Options options
    ) {
        super(
                lastScreen,
                options,
                TITLE
        );


        this.pendingReflexMode =
                RTXSettingsConfig.getReflexMode();

        this.pendingRRMode =
                RTXSettingsConfig.getRRMode();
        this.pendingCustomUpscalePercent =
                RTXSettingsConfig.getCustomUpscalePercent();

        this.pendingGiBounces =
                RTXSettingsConfig.getPerformanceSettings().giBounces();
        RTXSettingsConfig.PerformanceSettings performance =
                RTXSettingsConfig.getPerformanceSettings();
        this.pendingAoNearSamples = performance.aoNearSamples();
        this.pendingVolumetricRestir = performance.volumetricRestir();
        this.pendingAoMidSamples = performance.aoMidSamples();
        this.pendingSunShadowSamples = performance.sunShadowSamples();
        this.pendingLocalLightRisCandidates = performance.localLightRisCandidates();
        this.pendingLocalLightAreaSamples = performance.localLightAreaSamples();
        this.pendingWaterReflectionInterval = performance.waterReflectionInterval();
        this.pendingGlossyFullDistance = performance.glossyFullDistance();
        this.pendingDynamicRtasInterval = performance.dynamicRtasInterval();
        this.pendingMaxRtParticles = performance.maxRtParticles();
        this.pendingRtSpp = performance.rtSpp();

        this.pendingNoDirectLightAmbient =
                RTXSettingsConfig.getNoDirectLightAmbient();

        this.pendingPOMDepth =
                RTXSettingsConfig.getPOMDepth();

        this.pendingPOMSharp =
                RTXSettingsConfig.isPOMSharp();

        this.pendingSsptRange =
                RTXSettingsConfig.getSsptRange();

        this.pendingSsrRange =
                RTXSettingsConfig.getSsrRange();

        this.pendingRtGlossy =
                RTXSettingsConfig.isRTGlossyEnabled();

        // 2026-11-07：改读存档（原来读 native，native 没有持久化 ⇒ 重启即回默认）
        this.pendingGiNearRate =
                RTXSettingsConfig.getGiNearRate();

        this.pendingGlossyRoughLod =
                RTXSettingsConfig.isGlossyRoughLod();

        this.pendingTrueSubsurfaceScattering =
                RTXSettingsConfig.isTrueSubsurfaceScattering();

        this.pendingLocalLightMaxDistance =
                RTXSettingsConfig.getLocalLightMaxDistance();

        this.pendingLocalLightIntensity =
                RTXSettingsConfig.getLocalLightIntensity();

        this.pendingDynamicLightIntensity =
                RTXSettingsConfig.getDynamicLightIntensity();
        this.pendingBuiltinLodEnabled =
                RTXSettingsConfig.isBuiltinLodEnabled();
        this.pendingBuiltinLodDistanceChunks =
                RTXSettingsConfig.getBuiltinLodDistanceChunks();

        RTXSettingsConfig.SunVisualSettings sunVisuals =
                RTXSettingsConfig.getSunVisualSettings();
        this.pendingSunDiskLuminance = sunVisuals.diskLuminance();
        this.pendingSunHaloStrength = sunVisuals.haloStrength();
        this.pendingSunAngularDiameterDegrees = sunVisuals.angularDiameterDegrees();

        RTXSettingsConfig.AtmosphereSettings atmosphere =
                RTXSettingsConfig.getAtmosphereSettings();
        this.pendingStarIntensity = atmosphere.starIntensity();
        this.pendingSunTintStrength = atmosphere.sunTintStrength();
        this.pendingPhysicalSkyEnabled = atmosphere.physicalSkyEnabled();
        this.pendingScatterIntensity = atmosphere.scatterIntensity();
        this.pendingOzoneStrength = atmosphere.ozoneStrength();
        this.pendingSkyModel = RTXSettingsConfig.getSkyModel();
        this.pendingSkyModelScale = RTXSettingsConfig.getSkyModelScale();
        this.pendingNightSkyAmbientScale = RTXSettingsConfig.getNightSkyAmbientScale();
        this.pendingRainSkyDimScale = RTXSettingsConfig.getRainSkyDimScale();
        this.pendingMoonLightFloor = RTXSettingsConfig.getMoonLightFloor();
        this.pendingMoonDaylightDim = RTXSettingsConfig.getMoonDaylightDim();
        this.pendingFogShapeStrength = RTXSettingsConfig.getFogShapeStrength();
        this.pendingFogDawnBoost = RTXSettingsConfig.getFogDawnBoost();
        this.pendingPowderStrength =
                RTXSettingsConfig.getPowderStrength();
        this.pendingAuroraBrightness =
                RTXSettingsConfig.getAuroraBrightness();
        this.pendingAuroraFlowSpeed =
                RTXSettingsConfig.getAuroraFlowSpeed();
        this.pendingCloudSunForwardScattering =
                RTXSettingsConfig.getCloudSunForwardScattering();
        this.pendingCloudSunReddening =
                RTXSettingsConfig.getCloudSunReddening();
        this.pendingCloudTopStretch =
                RTXSettingsConfig.getCloudTopStretch();
        this.pendingCloudDetileStrength =
                RTXSettingsConfig.getCloudDetileStrength();
        this.pendingCloudType = RTXSettingsConfig.getCloudType();
        this.pendingCloudErosionScale = RTXSettingsConfig.getCloudErosionScale();
        this.pendingCloudWeatherScale = RTXSettingsConfig.getCloudWeatherScale();
        this.pendingCloudSilver = RTXSettingsConfig.getCloudSilver();
        this.pendingCloudViewDistance = RTXSettingsConfig.getCloudViewDistance();
        this.loadCloudMarchSettings(RTXSettingsConfig.getCloudMarchSettings());
        this.loadWaterParamsSettings(RTXSettingsConfig.getWaterParamsSettings());
        this.loadWaterWaveSettings(RTXSettingsConfig.getWaterWaveSettings());
        this.loadWaterSimWaveSettings(RTXSettingsConfig.getWaterSimWaveSettings());
        this.loadWaterWaveShapeSettings(RTXSettingsConfig.getWaterWaveShapeSettings());
        this.loadWaterWaveAflSettings(RTXSettingsConfig.getWaterWaveAflSettings());
        this.loadWaterSssSettings(RTXSettingsConfig.getWaterSssSettings());
        this.loadWaterFftSettings(RTXSettingsConfig.getWaterFftSettings());
        this.loadWindSettings(RTXSettingsConfig.getWindSettings());
        this.loadPostProcessingSettings(RTXSettingsConfig.getPostProcessingSettings());

        RTXSettingsConfig.CloudSettings clouds =
                RTXSettingsConfig.getCloudSettings();
        this.loadCloudSettings(clouds);
        this.loadCloudMsSettings(RTXSettingsConfig.getCloudMsSettings());
        this.loadCloudPhiSettings(RTXSettingsConfig.getCloudPhiSettings());
        this.loadCloudShapeSettings(RTXSettingsConfig.getCloudShapeSettings());
        this.loadCloudMoonSettings(RTXSettingsConfig.getCloudMoonSettings());
        this.loadCloudShape2Settings(RTXSettingsConfig.getCloudShape2Settings());
        this.loadCloudAmbientSettings(RTXSettingsConfig.getCloudAmbientSettings());
        this.loadCloudPhysicsSettings(RTXSettingsConfig.getCloudPhysicsSettings());
        this.loadCloudMs2Settings(RTXSettingsConfig.getCloudMs2Settings());
        this.loadCloudProfileSettings(RTXSettingsConfig.getCloudProfileSettings());
        this.loadCloudDetailSettings(RTXSettingsConfig.getCloudDetailSettings());
        this.loadCloudTemporalSettings(RTXSettingsConfig.getCloudTemporalSettings());

        this.loadFogExtrasSettings(RTXSettingsConfig.getFogExtrasSettings());

        RTXSettingsConfig.FogGroundMistSettings mist =
                RTXSettingsConfig.getFogGroundMistSettings();
        this.pendingFogGroundMistEnabled = mist.enabled();
        this.pendingFogGroundMistHeight = mist.height();
        this.pendingFogGroundMistExtinction = mist.extinction();
        this.pendingFogGroundMistDistance = mist.distance();
        this.pendingFogGroundMistSpeed = mist.speed();
        final RTXSettingsConfig.CloudGapSettings cloudGap =
                RTXSettingsConfig.getCloudGapSettings();
        this.pendingCloudGapEnabled = cloudGap.enabled();
        this.pendingCloudGapStrength = cloudGap.strength();
        this.pendingCloudGapWidth = cloudGap.width();
        final RTXSettingsConfig.RainRippleSettings ripple =
                RTXSettingsConfig.getRainRippleSettings();
        this.pendingRainRippleSize = ripple.size();
        this.pendingRainRippleStrength = ripple.strength();
        final RTXSettingsConfig.FogOpticsSettings optics =
                RTXSettingsConfig.getFogOpticsSettings();
        this.pendingFogCurlStrength = optics.curlStrength();
        this.pendingFogTintR = optics.tintR();
        this.pendingFogTintG = optics.tintG();
        this.pendingFogTintB = optics.tintB();

        this.loadCloudAerialSettings(RTXSettingsConfig.getCloudAerialSettings());
        this.loadCloudWeatherStyleSettings(RTXSettingsConfig.getCloudWeatherStyleSettings());
        this.loadCloudLight2Settings(RTXSettingsConfig.getCloudLight2Settings());

        RTXSettingsConfig.VolumetricFogSettings fog =
                RTXSettingsConfig.getVolumetricFogSettings();
        this.pendingFogEnabled = fog.enabled();
        this.pendingFogDensity = fog.density();
        this.pendingFogHeight = fog.height();
        this.pendingFogScaleHeight = fog.scaleHeight();
        this.pendingFogAnisotropy = fog.anisotropy();
        this.pendingFogSunStrength = fog.sunStrength();
        this.pendingFogAmbientStrength = fog.ambientStrength();
        this.pendingFogMaxDistance = fog.maxDistance();
        this.pendingFogStartDistance = fog.startDistance();
        this.pendingFogSunSteps = fog.sunSteps();
        this.pendingFogRainBoost = fog.rainBoost();

        RTXSettingsConfig.VolumetricLightSettings godRays =
                RTXSettingsConfig.getVolumetricLightSettings();
        this.pendingGodRayEnabled = godRays.enabled();
        this.pendingGodRaySteps = godRays.steps();
        this.pendingGodRayOcclusionSamples = godRays.occlusionSamples();
        this.pendingGodRayIntensity = godRays.intensity();
        this.pendingGodRayReprojection = godRays.reprojection();

        RTXSettingsConfig.RainSettings rainLook =
                RTXSettingsConfig.getRainSettings();
        this.pendingRainOpacity = rainLook.opacity();
        this.pendingRainStreakLength = rainLook.streakLength();
        this.pendingRainBrightness = rainLook.brightness();
        this.pendingRainWidth = rainLook.width();
        this.pendingRainDensityScale = rainLook.densityScale();

        RTXSettingsConfig.SnowSettings snowLook =
                RTXSettingsConfig.getSnowSettings();
        this.pendingSnowSize = snowLook.sizeScale();
        this.pendingSnowDensity = snowLook.densityScale();
        this.pendingSnowOpacity = snowLook.opacity();
        this.pendingSnowBrightness = snowLook.brightness();
        this.pendingSnowFallSpeed = snowLook.fallSpeedScale();
        this.pendingSnowSway = snowLook.swayBlocks();

        RTXSettingsConfig.RainWallSettings rainWall =
                RTXSettingsConfig.getRainWallSettings();
        this.pendingRainWallEnabled = rainWall.enabled();
        this.pendingRainWallWetness = rainWall.wetness();
        this.pendingRainWallDripDensity = rainWall.dripDensity();
        this.pendingRainWallDripSpeed = rainWall.dripSpeed();
        this.pendingRainWallDripSize = rainWall.dripSize();
        this.pendingRainWallDripBump = rainWall.dripBump();
        this.pendingRainWallDistance = rainWall.distance();

        RTXSettingsConfig.GalaxySettings galaxy =
                RTXSettingsConfig.getGalaxySettings();
        this.pendingGalaxyScale = galaxy.scale();
        this.pendingGalaxyTiltDegrees = galaxy.tiltDegrees();
        this.pendingGalaxyYawDegrees = galaxy.yawDegrees();
        this.pendingGalaxyMix = galaxy.mix();

        RTXSettingsConfig.WaterStyleSettings waterStyle =
                RTXSettingsConfig.getWaterStyleSettings();
        this.pendingWaterStyleWaveScale = waterStyle.waveScale();
        this.pendingWaterStyleWaveSpeed = waterStyle.waveSpeed();
        this.pendingWaterStyleSwellScale = waterStyle.swellScale();
        this.pendingWaterStyleMidScale = waterStyle.midScale();
        this.pendingWaterStyleFineScale = waterStyle.fineScale();
        this.pendingWaterStyleTintMix = waterStyle.tintMix();
        this.pendingWaterStyleDensityScale = waterStyle.densityScale();
        this.pendingWaterStyleCausticStrength = waterStyle.causticStrength();

        this.pendingHDR =
                RTXSettingsConfig.isHDREnabled();

        this.pendingAutoExposureEnabled =
                RTXSettingsConfig.isAutoExposureEnabled();

        this.pendingAutoExposureCompensation =
                RTXSettingsConfig.getAutoExposureCompensation();

        this.pendingFGFrameSync =
                RTXSettingsConfig.isFGFrameSyncEnabled();

        this.pendingFGRefreshCap =
                RTXSettingsConfig.isFGRefreshCapEnabled();

        this.pendingSLDevPlugins =
                RTXSettingsConfig.isSLDevPluginsEnabled();

        this.pendingSerEnabled =
                RTXSettingsConfig.isSerEnabled();

        this.pendingMotionVectors =
                RTXSettingsConfig.getMotionVectorsEnabled();

        this.pendingFGMultiplier =
                RTXSettingsConfig.getFGMultiplier();

        this.pendingFGBaseFrameLimit =
                RTXSettingsConfig.getFGBaseFrameLimit();

        this.pendingArmorMetal =
                RTXSettingsConfig.isEntityArmorMetalEnabled();

        this.pendingCloudFadeFollowsRrMode =
                RTXSettingsConfig.isCloudFadeFollowsRrMode();
        RTXSettingsConfig.PlaneCloudSettings planeCloud =
                RTXSettingsConfig.getPlaneCloudSettings();
        this.pendingPlaneCloudEnabled = planeCloud.enabled();
        this.pendingPlaneCloudHeightOffset = planeCloud.heightOffset();
        this.pendingPlaneCloudScale = planeCloud.scale();
        this.pendingPlaneCloudSpeed = planeCloud.speed();
        this.pendingPlaneCloudDensity = planeCloud.density();
        this.pendingPlaneCloudCoverage = planeCloud.coverage();
        this.pendingPlaneCloudOctaves = planeCloud.octaves();
        this.pendingPlaneCloudFadeDistance = planeCloud.fadeDistance();
        this.pendingPlaneCloudBrightness = planeCloud.brightness();
        this.pendingCloudResFloorPercent =
                RTXSettingsConfig.getCloudResFloorPercent();
        this.pendingCloudTemporalMode =
                RTXSettingsConfig.getCloudTemporalMode();
        this.pendingCloudFarStepDensity =
                RTXSettingsConfig.getCloudFarStepDensity();
        this.pendingGiRoulette =
                RTXSettingsConfig.getGiRoulette();
        this.pendingSunMarchFarDistance =
                RTXSettingsConfig.getSunMarchFarDistance();
        this.pendingMotionDebugView =
                RTXSettingsConfig.getMotionDebugView();
        this.pendingDirectLightMode =
                RTXSettingsConfig.getDirectLightMode();
        this.pendingIndirectMode =
                RTXSettingsConfig.getIndirectMode();
        this.pendingSparseRatePercent =
                RTXSettingsConfig.getSparseRatePercent();
    }


    @Override
    protected void addOptions() {

        this.optionRow.clear();

        // 2026-11-13：每个页面（含主页）都先放一行「紧凑预览」开关。
        // 先把 section 对齐当前页，否则 addOptionRow 会因为 section != page 直接丢弃。
        this.section = this.page;
        this.addOptionRow(compactToggleButton());
        this.flushOptionRow();

        if(this.page == PAGE_HUB) {
            this.section = PAGE_HUB;
            this.addHubButtons();
            return;
        }

        this.section = PAGE_UPSCALER;

        final boolean reflexAvailable =
                NativeVulkan.isLoaded()
                        && NativeVulkan.isReflexAvailable();


        final int currentMode =
                this.pendingReflexMode;


        CycleButton<Integer> reflexButton =
                CycleButton.builder(
                                mode -> {
                                    if(!reflexAvailable) {
                                        return Component.literal(
                                                "不可用"
                                        );
                                    }


                                    return switch(mode) {
                                        case 0 ->
                                                Component.literal(
                                                        "Off"
                                                );

                                        case 1 ->
                                                Component.literal(
                                                        "On"
                                                );

                                        case 2 ->
                                                Component.literal(
                                                        "On + Boost"
                                                );

                                        default ->
                                                Component.literal(
                                                        "On"
                                                );
                                    };
                                },
                                currentMode
                        )
                        .withValues(
                                0,
                                1,
                                2
                        )
                        .create(
                                Component.literal(
                                        "Reflex 低延迟"
                                ),
                                (button, mode) -> {

                                    if(!reflexAvailable) {
                                        return;
                                    }


                                    this.pendingReflexMode =
                                            mode;
                                }
                        );


        reflexButton.active =
                reflexAvailable;


        this.addOptionRow(
                reflexButton
        );

        final int fgMultiplierMax =
                NativeVulkan.isLoaded()
                        ? NativeVulkan.getFGMultiplierMax()
                        : 2;

        CycleButton<Integer> fgMultiplierButton =
                CycleButton.builder(
                                multiplier -> Component.literal(
                                        multiplier == RTXSettingsConfig.FG_MULTIPLIER_OFF
                                                ? "关"
                                                : multiplier > fgMultiplierMax
                                                        ? multiplier + "x（上限 "
                                                                + fgMultiplierMax + "x）"
                                                        : multiplier + "x"
                                ),
                                this.pendingFGMultiplier
                        )
                        .withValues(RTXSettingsConfig.FG_MULTIPLIER_OFF, 2, 3, 4, 6)
                        .create(
                                Component.literal("帧生成倍增值"),
                                (button, multiplier) ->
                                        this.pendingFGMultiplier = multiplier
                        );

        this.addOptionRow(
                fgMultiplierButton
        );

        final int displayRefreshHz =
                NativeVulkan.isLoaded()
                        ? NativeVulkan.getDisplayRefreshRateHz()
                        : 0;

        final int autoBaseHz =
                displayRefreshHz > 0
                        && this.pendingFGMultiplier > 0
                        ? Math.max(1, displayRefreshHz / this.pendingFGMultiplier)
                        : 0;

        final String reflexWarning =
                this.pendingReflexMode == 0
                        ? "（需 Reflex 开）"
                        : "";

        CycleButton<Integer> fgBaseLimitButton =
                CycleButton.builder(
                                fps -> {
                                    if(fps == RTXSettingsConfig.FG_BASE_LIMIT_AUTO) {
                                        return Component.literal(
                                                autoBaseHz > 0
                                                        ? "自动（" + autoBaseHz + "fps）"
                                                        : "自动"
                                        );
                                    }

                                    if(fps == RTXSettingsConfig.FG_BASE_LIMIT_OFF) {
                                        return Component.literal("关");
                                    }


                                    return Component.literal(
                                            fps + "fps" + reflexWarning
                                    );
                                },
                                this.pendingFGBaseFrameLimit
                        )
                        .withValues(
                                RTXSettingsConfig.FG_BASE_LIMIT_AUTO,
                                RTXSettingsConfig.FG_BASE_LIMIT_OFF,
                                30,
                                41,
                                55,
                                82,
                                120,
                                165,
                                240,
                                300
                        )
                        .create(
                                Component.literal("基础帧率上限"),
                                (button, fps) ->
                                        this.pendingFGBaseFrameLimit = fps
                        );

        this.addOptionRow(
                fgBaseLimitButton
        );

        // 2026-11-21 精简批（顾问 §4.1）：FG 帧同步 / 按刷新率限制 / 运动矢量
        // 收进「高级选项」折叠；主界面保留帧生成倍增值 + 基础帧率上限。
        this.addOptionRow(this.upscalerAdvancedToggle());
        this.flushOptionRow();

        if(showUpscalerAdvanced) {
            CycleButton<Boolean> fgFrameSyncButton =
                    CycleButton.builder(
                                    enabled -> Component.literal(
                                            enabled ? "开" : "关"
                                    ),
                                    this.pendingFGFrameSync
                            )
                            .withValues(false, true)
                            .create(
                                    Component.literal("FG 帧同步"),
                                    (button, enabled) ->
                                            this.pendingFGFrameSync = enabled
                            );

            this.addOptionRow(
                    fgFrameSyncButton
            );

            CycleButton<Boolean> fgRefreshCapButton =
                    CycleButton.builder(
                                    enabled -> Component.literal(
                                            enabled ? "开" : "关"
                                    ),
                                    this.pendingFGRefreshCap
                            )
                            .withValues(true, false)
                            .create(
                                    Component.literal("按刷新率限制倍率"),
                                    (button, enabled) ->
                                            this.pendingFGRefreshCap = enabled
                            );

            this.addOptionRow(
                    fgRefreshCapButton
            );

            // 2026-11-16 UI 审计：运动矢量是 DLSS 的输入（关闭后由 DLSS 自补相机
            // 运动），从「光追性能」搬到这里，跟超分/帧生成放一起。
            CycleButton<Boolean> motionVectorsButton =
                    CycleButton.builder(
                                    enabled -> Component.literal(
                                            enabled ? "开" : "关"
                                    ),
                                    this.pendingMotionVectors != 0
                            )
                            .withValues(false, true)
                            .create(
                                    Component.literal("运动矢量（关闭后由 DLSS 自补相机运动）"),
                                    (button, enabled) ->
                                            this.pendingMotionVectors = enabled ? 1 : 0
                            );

            this.addOptionRow(
                    motionVectorsButton
            );
            this.flushOptionRow();
        }

        // 「SL 调试 UI（dev 构建）」已移到「光追性能 → 高级 / 实验选项」折叠组。

        this.flushOptionRow();

        this.section = PAGE_UPSCALER;

        // 2026-11-21 精简批（顾问 §4.1）：原「光追性能」页整页并入「性能与超分」。
        // SER 二级 GI / SL 调试 UI / GI 轮盘等仍在下方「高级 / 实验选项」折叠组；
        // 「盔甲金属反射」移到「水面与材质」页的高级折叠。

        CycleButton<Integer> rrQualityButton =
                CycleButton.builder(
                                RTXSettingsScreen::qualityLabel,
                                this.pendingRRMode
                        )
                        .withValues(
                                RTXSettingsConfig.RR_MODE_PERFORMANCE,
                                RTXSettingsConfig.RR_MODE_BALANCED,
                                RTXSettingsConfig.RR_MODE_QUALITY,
                                RTXSettingsConfig.RR_MODE_DLAA,
                                RTXSettingsConfig.RR_MODE_ULTRA_PERFORMANCE
                        )
                        .create(
                                Component.literal("DLSS RR 光线重构档位"),
                                (button, mode) -> this.pendingRRMode = mode
                        );

        AbstractSliderButton customUpscaleSlider =
                new AbstractSliderButton(
                        0,
                        0,
                        310,
                        20,
                        Component.empty(),
                        customUpscaleToSlider(this.pendingCustomUpscalePercent)
                ) {
                    {
                        this.updateMessage();
                    }

                    @Override
                    protected void updateMessage() {
                        final int pct =
                                customUpscaleFromSlider(this.value);
                        this.setMessage(Component.literal(
                                pct <= 0
                                        ? "自定义超分: 跟随档位"
                                        : "自定义超分: " + pct + "%"
                        ));
                    }

                    @Override
                    protected void applyValue() {
                        final int pct =
                                customUpscaleFromSlider(this.value);
                        this.value = customUpscaleToSlider(pct);
                        RTXSettingsScreen.this.pendingCustomUpscalePercent = pct;
                    }
                };

        this.addOptionRow(rrQualityButton);
        this.addOptionRow(customUpscaleSlider);
        this.flushOptionRow();

        CycleButton<Integer> giBouncesButton =
                CycleButton.builder(
                                value -> Component.literal(
                                        value == 0 ? "关" : Integer.toString(value)
                                ),
                                this.pendingGiBounces
                        )
                        .withValues(0, 1, 2, 3, 4, 6, 8)
                        .create(
                                Component.literal("GI 跳数"),
                                (button, value) -> this.pendingGiBounces = value
                        );

        this.addOptionRow(giBouncesButton);

        // 2026-11-21 精简批：采样/间隔类性能项收进「高级选项」折叠。
        if(showUpscalerAdvanced) {
            this.addOptionRow(performanceButton("体积光 ReSTIR", pendingVolumetricRestir,
                    List.of(0, 1), value -> pendingVolumetricRestir = value));
            this.addOptionRow(performanceButton("RTAO 近距采样", pendingAoNearSamples,
                    List.of(5, 0, 1, 2, 3, 4), value -> pendingAoNearSamples = value));
            this.addOptionRow(performanceButton("RTAO 中距采样", pendingAoMidSamples,
                    List.of(0, 1, 2), value -> pendingAoMidSamples = value));
            this.addOptionRow(performanceButton("太阳阴影采样", pendingSunShadowSamples,
                    List.of(1, 2), value -> pendingSunShadowSamples = value));
            this.addOptionRow(performanceButton("局部光 RIS 候选", pendingLocalLightRisCandidates,
                    List.of(1, 2, 4, 8, 16, 32), value -> pendingLocalLightRisCandidates = value));
            this.addOptionRow(performanceButton("局部光区域采样", pendingLocalLightAreaSamples,
                    List.of(1, 2), value -> pendingLocalLightAreaSamples = value));
            this.addOptionRow(performanceButton("水面反射间隔", pendingWaterReflectionInterval,
                    List.of(0, 1, 2, 4), value -> pendingWaterReflectionInterval = value));
            this.addOptionRow(performanceButton("光泽全采样距离", pendingGlossyFullDistance,
                    List.of(16, 32, 64, 96), value -> pendingGlossyFullDistance = value));
            this.addOptionRow(performanceButton("动态 RTAS 间隔", pendingDynamicRtasInterval,
                    List.of(1, 2, 4, 8), value -> pendingDynamicRtasInterval = value));
            this.addOptionRow(performanceButton("RT 粒子上限", pendingMaxRtParticles,
                    List.of(0, 64, 128, 256), value -> pendingMaxRtParticles = value));
            this.flushOptionRow();
        }

        CycleButton<Float> rtSppButton =
                CycleButton.builder(
                                value -> Component.literal(
                                        value == Math.floor(value)
                                                ? Integer.toString((int) (float) value) + " spp"
                                                : value + " spp"
                                ),
                                this.pendingRtSpp
                        )
                        .withValues(0.5f, 1.0f, 2.0f, 3.0f, 4.0f)
                        .create(
                                Component.literal("每像素采样数 (spp)"),
                                (button, value) -> this.pendingRtSpp = value
                        );
        this.addOptionRow(rtSppButton);
        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingBuiltinLodEnabled)
                .withValues(false, true)
                .create(Component.literal("内置远景 LOD"),
                        (button, value) -> this.pendingBuiltinLodEnabled = value));
        this.addOptionRow(CycleButton.builder(
                        chunks -> Component.literal(chunks + " 区块"),
                        this.pendingBuiltinLodDistanceChunks)
                .withValues(32, 48, 64, 96, 128, 160, 192, 256)
                .create(Component.literal("远景 LOD 视距"),
                        (button, value) -> this.pendingBuiltinLodDistanceChunks = value));
        this.flushOptionRow();

        // =====================================================
        // 2026-11-16 UI 审计：A/B 闸与调试开关默认收起。
        //
        // 这些项此前以「名称(1=开)」的形式直接排在玩家预算项后面，
        // 既看不懂又容易被误触（关掉 GI 轮盘 = 退回全弹射）。
        // 现在统一进这个折叠组，默认关；状态是界面级静态字段，不写存档。
        // =====================================================
        this.addOptionRow(advancedGatesToggle());
        this.flushOptionRow();

        if(showAdvancedGates)
        {
            this.addSectionHeader("实验 / 调试（改动会直接影响画面与性能）");

            CycleButton<Boolean> serButton =
                    CycleButton.builder(
                                    enabled -> Component.literal(
                                            enabled ? "开（重启后生效）" : "关（重启后生效）"
                                    ),
                                    this.pendingSerEnabled
                            )
                            .withValues(false, true)
                            .create(
                                    Component.literal("SER 二级 GI"),
                                    (button, enabled) -> this.pendingSerEnabled = enabled
                            );

            this.addOptionRow(serButton);

            CycleButton<Boolean> slDevPluginsButton =
                    CycleButton.builder(
                                    enabled -> Component.literal(
                                            enabled ? "开（重启生效）" : "关"
                                    ),
                                    this.pendingSLDevPlugins
                            )
                            .withValues(false, true)
                            .create(
                                    Component.literal("SL 调试 UI（dev 构建）"),
                                    (button, enabled) ->
                                            this.pendingSLDevPlugins = enabled
                            );

            this.addOptionRow(slDevPluginsButton);

            this.addOptionRow(performanceButton("GI 俄罗斯轮盘", pendingGiRoulette,
                    List.of(0, 1), value -> pendingGiRoulette = value));
            this.addOptionRow(performanceButton("太阳透射距离", pendingSunMarchFarDistance,
                    List.of(0, 8, 16, 32, 64, 128, 256), value -> pendingSunMarchFarDistance = value));
            this.addOptionRow(performanceButton("直接光采样（RTXDI 复用）", pendingDirectLightMode,
                    List.of(0, 1), value -> pendingDirectLightMode = value));
            this.addOptionRow(performanceButton("间接光模式（SHaRC，需重启）", pendingIndirectMode,
                    List.of(0, 1), value -> pendingIndirectMode = value));
            this.addOptionRow(performanceButton("稀疏渲染采样率 %", pendingSparseRatePercent,
                    List.of(0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100),
                    value -> pendingSparseRatePercent = value));
            this.addOptionRow(performanceButton("MV 调试图", pendingMotionDebugView,
                    List.of(0, 1, 2), value -> pendingMotionDebugView = value));
            this.flushOptionRow();
        }

        this.section = PAGE_SKY;

        // 2026-11-21 精简批（顾问 §4.1 页 2「天空与天体」）：原「天空与大气」+
        // 「天体与夜光」两页合并；主界面只留高频项，其余进「高级选项」折叠。
        this.addOptionRow(this.presetCycle("光照风格",
                new String[] {"现役 · 冷峻高反差", "真实 · 写实", "暖调 · 午后", "暗夜 · 星海"},
                this::applyLightStylePreset, this.lightStyleIndex));
        this.flushOptionRow();
        this.addOptionRow(CycleButton.builder(
                        model -> Component.literal(model == 0 ? "Bruneton LUT" : "Hillaire"),
                        this.pendingSkyModel)
                .withValues(0, 1)
                .create(Component.literal("大气散射模型"),
                        (button, value) -> this.pendingSkyModel = value));
        // 2026-11-07 复核：范围保持 0~8 不变。该值落在 skyParams1.z，被
        // sky_atmosphere.comp:289 当乘数用（之后才 clamp 0..8），8 是有效区间；
        // 上一版我把滑条收到 2.0 是错的，已改为放开 JNI 的上限。
        this.addOptionRow(cloudSlider("大气散射强度", 0.0, 8.0, 0.05,
                pendingScatterIntensity, "%.2f", value -> pendingScatterIntensity = (float) value));
        this.addOptionRow(cloudSlider("臭氧吸收强度", 0.0, 2.0, 0.05,
                pendingOzoneStrength, "%.2f", value -> pendingOzoneStrength = (float) value));
        this.addOptionRow(this.skyAdvancedToggle());
        this.flushOptionRow();

        if(showSkyAdvanced) {
            this.addOptionRow(CycleButton.builder(
                            enabled -> Component.literal(enabled ? "开" : "关"),
                            this.pendingPhysicalSkyEnabled)
                    .withValues(false, true)
                    .create(Component.literal("物理大气散射"),
                            (button, enabled) -> this.pendingPhysicalSkyEnabled = enabled));
            this.addOptionRow(cloudSlider("Hillaire 亮度标定", 0.10, 4.0, 0.01,
                    pendingSkyModelScale, "%.2f", value -> pendingSkyModelScale = (float) value));
            this.addOptionRow(cloudSlider("夜间天空底光", 0.0, 2.0, 0.05,
                    pendingNightSkyAmbientScale, "%.2f", value -> pendingNightSkyAmbientScale = (float) value));
            this.addOptionRow(cloudSlider("雨天天空压暗", 0.0, 2.0, 0.05,
                    pendingRainSkyDimScale, "%.2f", value -> pendingRainSkyDimScale = (float) value));
            // 2026-11-16 月光（月亮按月相移动后新增的两个旋钮，车道 = CameraUBO.moonParams）
            this.addOptionRow(cloudSlider("新月月光", 0.0, 1.0, 0.05,
                    pendingMoonLightFloor, "%.2f", value -> pendingMoonLightFloor = (float) value));
            this.addOptionRow(cloudSlider("白天月亮亮度", 0.0, 1.0, 0.05,
                    pendingMoonDaylightDim, "%.2f", value -> pendingMoonDaylightDim = (float) value));
            this.flushOptionRow();
        }

        // 2026-11-22 统一风场（折叠组，默认收起）：一条风向喂云/雾/水波
        //（批1），植物摇摆（批3）复用同一车道。出厂 30.5° = HP 云原方向，
        // 出厂时云/雾逐位不变、水波从 45° 转到与云同向。
        this.addOptionRow(this.windAdvancedToggle());
        if(showWindAdvanced) {
            this.addOptionRow(cloudSlider("风向（0°=+X 顺时针）", 0.0, 360.0, 0.5,
                    windAngleDeg, "%.1f°", value -> windAngleDeg = (float) value));
            this.addOptionRow(cloudSlider("风强（植物摇摆倍率）", 0.0, 2.0, 0.05,
                    windStrength, "%.2f", value -> windStrength = (float) value));
            this.addOptionRow(cloudSlider("阵风强度（0 = 恒定风）", 0.0, 1.0, 0.05,
                    windGust, "%.2f", value -> windGust = (float) value));
            this.flushOptionRow();
        }
        this.section = PAGE_CLOUD;

        this.addSectionHeader("体积云");

        // 2026-11-21 精简批（顾问 §4.2 A）：云风格一键预设，第 1 档 = 现役基线。
        this.addOptionRow(this.presetCycle("云风格",
                new String[] {"现役 · 史诗对流", "晴朗 · 轻积云", "阴沉 · 低云层", "日落 · 火烧云"},
                this::applyCloudStylePreset, this.cloudStyleIndex));
        this.flushOptionRow();

        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingCloudEnabled)
                .withValues(false, true)
                .create(Component.literal("体积云"),
                        (button, enabled) -> this.pendingCloudEnabled = enabled));
        this.addOptionRow(CycleButton.builder(
                        value -> Component.literal(switch(value) {
                            case 0 -> "性能";
                            case 2 -> "高质量";
                            default -> "平衡";
                        }),
                        this.pendingCloudQuality)
                .withValues(0, 1, 2)
                .create(Component.literal("体积云质量"),
                        (button, value) -> this.pendingCloudQuality = value));
        final float sizeRef = 0.55f;
        final float sizeSlider = sizeRef / Math.max(pendingCloudSize, 0.05f);
        // 反向滑条（值越大云越小）。范围对齐 native 钳制 size∈[0.5,3.0]：
        // 0.55/0.20=2.75、0.55/1.10=0.5。旧上限 2.0 → size 0.275 会被 native
        // 静默拒绝（setCloudSettings 返回 false，设置不生效）。
        this.addOptionRow(cloudSlider("云的大小", 0.20, 1.10, 0.05, sizeSlider, "%.2f 倍",
                value -> pendingCloudSize = sizeRef / (float) value));
        this.addOptionRow(cloudSlider("云量（分布密度）", 0.10, 0.90, 0.01,
                pendingCloudCoverage, "%.2f", value -> pendingCloudCoverage = (float) value));
        this.addOptionRow(cloudSlider("云底高度", 96.0, 4000.0, 16.0,
                pendingCloudBottom, "%.0f", value -> pendingCloudBottom = (float) value));
        // 厚度下限对齐 native 钳制（256）：128~255 会被 setCloudSettings 拒绝。
        this.addOptionRow(cloudSlider("云层厚度", 256.0, 5000.0, 16.0,
                pendingCloudThickness, "%.0f", value -> pendingCloudThickness = (float) value));
        this.addOptionRow(cloudSlider("云体密度", 0.25, 5.0, 0.05,
                pendingCloudDensity, "%.2f", value -> pendingCloudDensity = (float) value));
        this.addOptionRow(cloudSlider("云渲染清晰度 (防糊)", 0, 100, 5,
                pendingCloudResFloorPercent, "%.0f%% (0=自动50%%)",
                value -> pendingCloudResFloorPercent = (int) Math.round(value)));
        this.addOptionRow(CycleButton.builder(
                        mode -> Component.literal(switch(mode) {
                            case 0 -> "全像素 (原画)";
                            case 1 -> "1/2 棋盘 (均衡)";
                            default -> "1/4 四帧 (最快)";
                        }),
                        this.pendingCloudTemporalMode)
                .withValues(2, 1, 0)
                .create(Component.literal("云分帧渲染 (提速)"),
                        (button, mode) -> this.pendingCloudTemporalMode = mode));
        this.addOptionRow(cloudSlider("远带边缘锐度", 0.0, 2.0, 0.05,
                pendingCloudSharpness, "%.2f",
                value -> pendingCloudSharpness = (float) value));
        this.flushOptionRow();

        this.addSectionHeader("体积云 · 天气与大气氛围");
        this.addOptionRow(cloudSlider("大气透视融入天空", 0.0, 2.0, 0.05,
                pendingCloudAerialStrength, "%.2f",
                value -> pendingCloudAerialStrength = (float) value));
        this.addOptionRow(cloudSlider("天气系统联动强度", 0.0, 2.0, 0.05,
                pendingCloudWeatherCoupling, "%.2f",
                value -> pendingCloudWeatherCoupling = (float) value));
        this.addOptionRow(cloudSlider("云底阴影 / 雷暴暗度", 0.0, 2.0, 0.05,
                pendingCloudStormDarkness, "%.2f",
                value -> pendingCloudStormDarkness = (float) value));
        this.addOptionRow(cloudSlider("雷暴云内闪电强度", 0.0, 3.0, 0.05,
                pendingCloudLightningStrength, "%.2f",
                value -> pendingCloudLightningStrength = (float) value));
        this.addOptionRow(cloudSlider("晚霞 / 曙暮光染色", 0.0, 2.0, 0.05,
                pendingCloudSunsetWarmth, "%.2f",
                value -> pendingCloudSunsetWarmth = (float) value));
        this.addOptionRow(cloudSlider("云内粉末 / 糖霜效应", 0.0, 2.0, 0.05,
                pendingCloudPowderEffect, "%.2f",
                value -> pendingCloudPowderEffect = (float) value));
        // 2026-11-21 精简批（顾问 §1.1-②）：银边双旋钮合一 —— 宏值 s 同时驱动
        // cloudSilverSpread = s 与 cloudSilver = clamp(0.5·s, 0, 1)；原「逆光银边」
        // 单滑条已删（JSON 键保留，由宏联动驱动）。
        this.addOptionRow(new AbstractSliderButton(0, 0, 310, 20, Component.empty(),
                Math.min(Math.max(pendingCloudSilverSpread, 0.0f), 2.0f) / 2.0) {
            {
                this.updateMessage();
            }

            @Override
            protected void updateMessage() {
                double s = Math.round(this.value * 200.0) / 100.0;
                this.setMessage(Component.literal(String.format(
                        java.util.Locale.ROOT, "逆光银边（幅+宽联动）: %.2f", s)));
            }

            @Override
            protected void applyValue() {
                double s = Math.round(this.value * 200.0) / 100.0;
                this.value = s / 2.0;
                RTXSettingsScreen.this.pendingCloudSilverSpread = (float) s;
                RTXSettingsScreen.this.pendingCloudSilver =
                        (float) Math.min(Math.max(0.5 * s, 0.0), 1.0);
                this.updateMessage();
            }
        });
        this.addOptionRow(cloudSlider("云底自然起伏", 0.0, 1.5, 0.05,
                pendingCloudBaseErosion, "%.2f",
                value -> pendingCloudBaseErosion = (float) value));
        this.flushOptionRow();

        this.addSectionHeader("高空卷云（Cirrus）");
        // 2026-11-21 精简批（顾问 §2.2）：卷云风格一键预设（第 1 档 = 现役带状）。
        this.addOptionRow(this.presetCycle("卷云风格",
                new String[] {"带状 · 现役", "关", "薄纱 · 晴空", "密集 · 变天"},
                this::applyCirrusStylePreset, this.cirrusStyleIndex));
        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingPlaneCloudEnabled)
                .withValues(false, true)
                .create(Component.literal("高空卷云"),
                        (button, enabled) -> this.pendingPlaneCloudEnabled = enabled));
        this.addOptionRow(cloudSlider("卷云覆盖率", 0.0, 1.0, 0.02,
                pendingPlaneCloudCoverage, "%.2f",
                value -> pendingPlaneCloudCoverage = (float) value));
        this.addOptionRow(this.cloudAdvancedToggle());
        this.flushOptionRow();

        if(showCloudAdvanced) {
        this.addOptionRow(cloudSlider("卷云亮度", 0.0, 3.0, 0.05,
                pendingPlaneCloudBrightness, "%.2f",
                value -> pendingPlaneCloudBrightness = (float) value));
        this.addOptionRow(cloudSlider("卷云高度偏移", 1000.0, 20000.0, 500.0,
                pendingPlaneCloudHeightOffset, "%.0f 格",
                value -> pendingPlaneCloudHeightOffset = (float) value));
        this.addOptionRow(cloudSlider("卷云纹理尺度", 0.2, 5.0, 0.1,
                pendingPlaneCloudScale, "%.1f",
                value -> pendingPlaneCloudScale = (float) value));
        this.addOptionRow(cloudSlider("卷云流动速度", 0.0, 5.0, 0.1,
                pendingPlaneCloudSpeed, "%.1f",
                value -> pendingPlaneCloudSpeed = (float) value));
        this.flushOptionRow();
        }

        this.addSectionHeader("体积云 · 外形");
        // 2026-11-07 修复：滑条原为 0.0~1.0，但 record 与 JNI 都只允许 <=0.50
        // ⇒ 拖过 0.5 抛 IllegalArgumentException，整页（含后面的雾/雨/水/后处理）
        // 全部不落地。上限收到 0.50 与下游对齐。
        this.addOptionRow(cloudSlider("竖向拉伸（0扁片↔1高塔）", 0.0, 0.50, 0.01,
                pendingCloudDetailStrength, "%.2f",
                value -> pendingCloudDetailStrength = (float) value));
        this.addOptionRow(cloudSlider("边缘卷曲（0圆润↔1絮状）", 0.0, 0.33, 0.01,
                pendingCloudBaseFeather, "%.2f",
                value -> pendingCloudBaseFeather = (float) value));
        this.addOptionRow(cloudSlider("多次散射强度", 0.0, 1.0, 0.01,
                pendingCloudFarStepDensity, "%.2f",
                value -> pendingCloudFarStepDensity = (float) value));
        // v5：detailScale = 值/10 ⇒ 侵蚀平铺 14 km~1.2 km；<1.0 平铺 >14 km 无意义。
        this.addOptionRow(cloudSlider("云缘细节尺度", 1.0, 12.0, 0.1,
                pendingCloudDetileStrength, "%.1f",
                value -> pendingCloudDetileStrength = (float) value));
        this.addOptionRow(cloudSlider("云层竖向铺展（0薄↔1厚）", 0.0, 1.0, 0.01,
                pendingCloudType, "%.2f", value -> pendingCloudType = (float) value));
        this.addOptionRow(cloudSlider("云缘侵蚀幅度", 0.0, 2.0, 0.05,
                pendingCloudErosionScale, "%.2f",
                value -> pendingCloudErosionScale = (float) value));
        // 范围对齐着色器钳制 hp.viewDistance ∈ [4000, 60000]；旧上限 200000 会被静默钳掉。
        this.addOptionRow(cloudSlider("云距（硬上限）", 4000.0, 60000.0, 1000.0,
                pendingCloudViewDistance, "%.0f 格",
                value -> pendingCloudViewDistance = (float) value));
        // 2026-11-21 精简批：原「逆光银边」滑条已并入上方"幅+宽联动"宏（顾问 §1.1-②）。
        this.flushOptionRow();

        // 2026-11-21 精简批：云页「高级选项」折叠——软化/絮状、步进、受光高级项、
        // UE 八项、时域默认收起（卷云高级参数在上方已随同一开关折叠）。
        this.addOptionRow(this.cloudAdvancedToggle());
        this.flushOptionRow();

        // 2026-11-10：补上"有消费者但一直没有 UI 入口"的外形项。
        // 依据（都 grep 过读取点）：cloudShapeParams.x/.y 在
        // tyndall_cloud_field.glsl:162-165（云底软化高度/幂），
        // cloudDetailParams.x/.y 在 :237-241（絮状强度/边缘宽度）。
        // 同组的 cloudShapeParams.z（两级步进阈值）与 cloudMsParams2 四 lane
        // 目前**没有任何着色器读取**，故不加滑条（避免再造死旋钮）。
        if(showCloudAdvanced) {
        this.addOptionRow(cloudSlider("云底软化高度", 0.0, 1.0, 0.02,
                shapeBottomSmoothHeight, "%.2f",
                value -> shapeBottomSmoothHeight = (float) value));
        this.addOptionRow(cloudSlider("云底软化幂", 0.05, 8.0, 0.05,
                shapeBottomSmoothPow, "%.2f",
                value -> shapeBottomSmoothPow = (float) value));
        this.addOptionRow(cloudSlider("絮状细节强度", 0.0, 2.0, 0.05,
                wispyStrength, "%.2f",
                value -> wispyStrength = (float) value));
        this.addOptionRow(cloudSlider("絮状边缘宽度", 0.0, 1.0, 0.01,
                wispyEdgeWidth, "%.2f",
                value -> wispyEdgeWidth = (float) value));
        this.flushOptionRow();
        }

        // 2026-11-21 精简批（顾问 §2.1）：云形宏调——四个宏滑条批量覆写 HP 活槽位
        // 并即时下发；v5 死槽 18-23 不碰。专家 28 项仍在下方 HP 折叠区，拖动会
        // 覆盖对应宏的锚点槽（下次进页宏值按锚点槽重新反解）。
        this.addSectionHeader("体积云 · 云形宏调");
        this.addOptionRow(this.hpMacroSlider("积云雕刻对比（柔和↔强对流）",
                2, 0.85f, 1.50f,
                new int[] {0, 1, 2, 12},
                new float[][] {{0.55f, 0.00f}, {0.70f, 0.24f}, {0.85f, 1.50f}, {1.00f, 3.00f}}));
        this.addOptionRow(this.hpMacroSlider("云缘絮团质感（紧致↔蓬松）",
                8, 0.45f, 2.00f,
                new int[] {4, 5, 6, 7, 8, 9},
                new float[][] {{0.40f, 0.01f}, {0.15f, 0.00f}, {0.75f, 0.83f},
                        {2.00f, 0.90f}, {0.45f, 2.00f}, {0.45f, 1.78f}}));
        this.addOptionRow(this.hpMacroSlider("层积云基底（稀薄↔厚实连片）",
                13, 0.25f, 1.00f,
                new int[] {3, 13, 14, 15, 10, 11},
                new float[][] {{0.08f, 0.015f}, {0.25f, 1.00f}, {0.35f, 1.00f},
                        {0.60f, 1.00f}, {2.20f, 3.00f}, {1.00f, 0.00f}}));
        this.addOptionRow(this.hpMacroSlider("相位偏心（柔和↔前向锐利）",
                16, 0.85f, 1.00f,
                new int[] {16, 17, 24},
                new float[][] {{0.85f, 1.00f}, {0.30f, 0.00f}, {0.50f, 0.88f}}));
        this.flushOptionRow();

        // 2026-11-16 UI 审计：28 项 HP 全参数默认折叠（本页原来 63 行）。
        // 这些是调形用的实验参数，玩家第一眼该看到的是质量档/大小/云量。
        this.addOptionRow(cloudHpToggleButton());
        this.flushOptionRow();

        if(cloudHpExpanded)
        {
            this.addSectionHeader("体积云 · HP 全参数（v5）");
            float[] hpValues = RTXSettingsConfig.getCloudHpSettings().toParamArray();
            for(int hpIndex = 0; hpIndex < RTXSettingsConfig.CLOUD_HP_LABELS.length; ++hpIndex)
            {
                // 死槽：phi 前向强度/深度幂/深度偏移/压缩(18-21)、散射 OD 尺度/曲线幂(22-23)
                // 在 v5 云重构后着色器不再读取（cloud_v5_core.glsl 只剩默认值赋值）。
                if(hpIndex >= 18 && hpIndex <= 23)
                {
                    continue;
                }
                final int hpSlot = hpIndex;
                final float[] hpRange = RTXSettingsConfig.CLOUD_HP_RANGES[hpSlot];
                this.addOptionRow(cloudSlider(RTXSettingsConfig.CLOUD_HP_LABELS[hpSlot],
                        hpRange[0], hpRange[1], hpRange[2], hpValues[hpSlot], "%.3f",
                        value -> {
                            float[] next = RTXSettingsConfig.getCloudHpSettings().toParamArray();
                            next[hpSlot] = (float) value;
                            RTXSettingsConfig.CloudHpSettings updated =
                                    new RTXSettingsConfig.CloudHpSettings(next);
                            if(!NativeVulkan.isLoaded()
                                    || NativeVulkan.setCloudHpParams(updated.toParamArray())) {
                                RTXSettingsConfig.setCloudHpSettings(updated);
                            }
                        }));
            }
            this.flushOptionRow();
        }

        if(showCloudAdvanced) {
        this.addSectionHeader("体积云 · 步进（性能 / 画质）");
        // 2026-11-07 复核更正：标签「光线步进」是**对的**，误名在字段一侧。
        // pendingCloudSunReddening → g_cloudSunReddening → cloudParams[25]
        // = cloudParams6.y，而 volumetric_clouds_v5.comp:159-162 正是把它当作
        // `stepsOverride`（光线步数覆盖，>0.9 时 clamp 到 8..256）。
        // 也就是说 "SunReddening" 这个名字是历史遗留，功能一直是步数。
        this.addOptionRow(cloudSliderWithZero("光线步进（0=跟随质量档）", 0.0, 160.0, 8.0,
                pendingCloudSunReddening, "%.0f 步", "跟随质量档",
                value -> pendingCloudSunReddening = (float) value));
        // 光步数着色器钳制在 [4,12]（锥形采样）；>12 的滑条段是死的。
        // 2026-11-07 复核更正：同上，「阴影步进」也是**对的**。
        // pendingCloudTopStretch → g_cloudTopStretch → cloudParams[26]
        // = cloudParams6.z，volumetric_clouds_v5.comp:172-175 把它当作
        // `shadowOverride`（阴影步数覆盖，>0.9 时 clamp 到 1..32）。
        // "TopStretch" 是历史误名。注意 tyndall_cloud_field.glsl:138 仍把同一个
        // lane 当"顶部拉伸"用 —— 那是旧路径，别按它理解。
        this.addOptionRow(cloudSliderWithZero("阴影步进（0=跟随质量档）", 0.0, 12.0, 1.0,
                pendingCloudTopStretch, "%.0f 步", "跟随质量档",
                value -> pendingCloudTopStretch = (float) value));
        // 光步侵蚀细节（cloudShapeParams.w）：参考行为是光步也做 erosion（自阴影
        // 带细节），代价云 pass +~40%。性能优先默认关。
        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开（云 pass +~40%）" : "关（性能优先）"),
                        this.shapeLightErosion > 0.5)
                .withValues(false, true)
                .create(Component.literal("光步侵蚀细节"),
                        (button, value) -> this.shapeLightErosion = value ? 1.0f : 0.0f));
        this.flushOptionRow();

        this.addSectionHeader("体积云 · 受光");
        this.addOptionRow(cloudSlider("太阳照云强度", 0.0, 3.0, 0.01,
                pendingCloudSunlightStrength, "%.2f", value -> pendingCloudSunlightStrength = (float) value));
        // 云顶环境光倍率（cloudAmbientParams.x，≤0 回落 1.5）。此前通道下发但
        // 着色器从未读取；云底暂无独立通道（固定 0.15×天光近似）。
        // 2026-11-07 复核：范围保持 0.1~4.0 不变 —— 着色器对
        // cloudAmbientParams.x 的钳制就是 clamp(x, 0.1, 4.0)。上一版我把滑条
        // 收到 3.0 是错的（那会砍掉 3~4 这段有效区间），改的是 record 与 JNI。
        this.addOptionRow(cloudSlider("云顶环境光倍率", 0.1, 4.0, 0.05,
                ambTopMul, "%.2f", value -> ambTopMul = (float) value));
        this.addOptionRow(cloudSlider("天光强度", 0.0, 3.0, 0.05,
                pendingPowderStrength, "%.2f", value -> pendingPowderStrength = (float) value));
        this.flushOptionRow();

        // 2026-11-10：地面反弹受光（cloudPhysicsParams.x/.y）。
        // 这两条此前存档/JNI/原子量三层齐全、**没有任何着色器读取**，滑条也从没做过；
        // volumetric_clouds_v5.comp 现已把 .x/.y 接进 core 的 ambientTerm
        // （见 cloud_v5_core.glsl 的 CLOUD_GROUND_BOUNCE 段）。
        // 另一个 lane（.z 自阴影残光 / .w 相位地板）目前仍无消费者，故不加滑条。
        this.addOptionRow(cloudSlider("地面反照率", 0.0, 0.8, 0.01,
                cloudGroundAlbedo, "%.2f", value -> cloudGroundAlbedo = (float) value));
        this.addOptionRow(cloudSlider("地面反射强度", 0.0, 1.0, 0.01,
                cloudGroundBounce, "%.2f", value -> cloudGroundBounce = (float) value));
        this.flushOptionRow();

        this.addSectionHeader("体积云 · 多次散射 / 能量");
        // phi_fwd 漫射场（cloudPhiParams）：通道 10-02 起就下发、回存管线齐全，
        // 但一直没有 GUI 行，且着色器 2026-11-01 才接上 —— 此前边界 wrap 光照
        // 实际永远关闭。压缩/锥形比率两项仍走 HP 面板（散射曲线幂/phi 压缩）。
        // 2026-11-21 死设置清理：「漫射场边界受光 / 建立速率」(cloudPhiParams.x/.y)
        // 已移除 —— v5 云重构后 phi_fwd 被精确能量守恒取代，着色器只读 .w。存档字段保留。

        for(int i = 0; i < 4; i++) {

            this.addOptionRow(
                    this.makeUeCloudSlider(
                            i,
                            this.UE_CLOUD_LABELS[i],
                            this.UE_CLOUD_MIN[i],
                            this.UE_CLOUD_MAX[i],
                            this.UE_CLOUD_STEP[i]
                    )
            );

            if(i % 2 == 1) {
                this.flushOptionRow();
            }
        }

        this.addSectionHeader("体积云 · 时域");
        this.addOptionRow(CycleButton.builder(
                enabled -> Component.literal(enabled ? "开" : "关"), msMeanReproject)
                .withValues(false, true).create(Component.literal("软边时域重投影"),
                        (button, value) -> msMeanReproject = value));
        this.flushOptionRow();

        // 2026-11-10：cloudTemporalParams.z（时域阻尼）一直是**活**的
        // （volumetric_cloud_resolve.comp:182 用它衰减 history），
        // 但一直没有 UI 入口 —— 这里补上。同组另外三个 lane
        // （天光可见性强度/步数、云底遮蔽）无消费者，已按死设置清理，故不加滑条。
        this.addOptionRow(cloudSlider("时域阻尼强度", 0.0, 1.0, 0.01,
                cloudTemporalDampStrength, "%.2f",
                value -> cloudTemporalDampStrength = (float) value));
        this.flushOptionRow();
        }


        // 2026-11-07 死设置清理：「云层大气透视」滑条已移除 —— 它写的是
        // CameraUBO.cloudAerialParams.x，唯一读取点是退役模型
        // volumetric_clouds_v3.comp:294（需 cloud_v3.flag 才启用），默认 v5 档
        // 下拖动没有任何效果。C++ 侧该 lane 已常量化写 0
        // （Settings.cpp populateCloudAerialParams），atomic 与 JNI 写入已删。

        this.section = PAGE_FOG;

        // 2026-11-21 精简批（顾问 §4.1 页 4「雾与天气」）：原「体积雾与体积光」
        // +「降雨」两页合并；主界面留高频项，其余进「高级选项」折叠。
        this.addOptionRow(this.presetCycle("雾氛围",
                new String[] {"现役 · 电影晨雾", "通透 · 高能见度", "浓密 · 林间薄雾", "剧场 · 强丁达尔"},
                this::applyFogStylePreset, this.fogStyleIndex));
        this.addOptionRow(this.presetCycle("天气氛围",
                new String[] {"暴雨 · 现役", "阴天 · 柔和", "雪天 · 冷白"},
                this::applyWeatherMoodPreset, this.weatherMoodIndex));
        this.addOptionRow(new AbstractSliderButton(0, 0, 310, 20, Component.empty(),
                Math.min(Math.max(this.weatherMoodW, 0.0f), 2.0f) / 2.0) {
            {
                this.updateMessage();
            }

            // 2026-11-21 精简批（顾问 §3.3）：天气氛围总强度 w —— 先选"天气氛围"
            // 预设定基线 B，再拖本滑条按 lane = B·w 联动；未选预设时以当前存档为基线。
            @Override
            protected void updateMessage() {
                double w = Math.round(this.value * 200.0) / 100.0;
                this.setMessage(Component.literal(String.format(
                        java.util.Locale.ROOT, "天气氛围强度: %.2f×", w)));
            }

            @Override
            protected void applyValue() {
                double w = Math.round(this.value * 200.0) / 100.0;
                this.value = w / 2.0;
                RTXSettingsScreen.this.weatherMoodW = (float) w;
                if(RTXSettingsScreen.this.weatherMoodBase == null) {
                    RTXSettingsConfig.CloudWeatherStyleSettings weather =
                            RTXSettingsConfig.getCloudWeatherStyleSettings();
                    RTXSettingsConfig.FogGroundMistSettings mist =
                            RTXSettingsConfig.getFogGroundMistSettings();
                    RTXSettingsScreen.this.weatherMoodBase = new float[] {
                            RTXSettingsConfig.getRainSkyDimScale(),
                            RTXSettingsConfig.getVolumetricFogSettings().rainBoost(),
                            weather.stormDarkness(), weather.lightningStrength(),
                            mist.extinction()};
                }
                float[] b = RTXSettingsScreen.this.weatherMoodBase;
                RTXSettingsScreen.this.pushWeatherMoodLanes(
                        b[0] * (float) w, b[1] * (float) w, b[2] * (float) w,
                        b[3] * (float) w, b[4] * (float) w,
                        RTXSettingsConfig.getCloudWeatherStyleSettings().weatherCoupling());
                RTXSettingsConfig.flush();
                this.updateMessage();
            }
        });
        this.flushOptionRow();

        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingFogEnabled)
                .withValues(false, true)
                .create(Component.literal("体积雾"),
                        (button, enabled) -> this.pendingFogEnabled = enabled));
        this.addOptionRow(cloudSlider("雾密度基准 (VFOG_DENSITY_BASE)", 0.00, 100.00, 0.50,
                pendingFogDensity, "%.2f", value -> pendingFogDensity = (float) value));
        this.addOptionRow(cloudSlider("雾太阳前向散射", 0.00, 4.00, 0.05,
                pendingFogSunStrength, "%.2f", value -> pendingFogSunStrength = (float) value));
        this.addOptionRow(cloudSlider("雾最远作用距离", 64, 1024, 32,
                pendingFogMaxDistance, "%.0f 格", value -> pendingFogMaxDistance = (float) value));
        this.addOptionRow(cloudSlider("晨雾增强（峰值倍率）", 1.0, 3.0, 0.05,
                pendingFogDawnBoost, "%.2f 倍", value -> pendingFogDawnBoost = (float) value));
        this.addOptionRow(cloudSlider("体积光柱强度", 0.0, 3.0, 0.05,
                pendingGodRayIntensity, "%.2f", value -> pendingGodRayIntensity = (float) value));
        this.addOptionRow(this.fogAdvancedToggle());
        this.flushOptionRow();

        if(showFogAdvanced) {
        this.addOptionRow(cloudSlider("雾高度上界 (VFOG_HEIGHT)", -64, 320, 1,
                pendingFogHeight, "%.0f 格", value -> pendingFogHeight = (float) value));
        this.addOptionRow(cloudSlider("雾高度下界 (VFOG_HEIGHT_2)", -64, 320, 1,
                pendingFogScaleHeight, "%.0f 格", value -> pendingFogScaleHeight = (float) value));
        this.addOptionRow(cloudSlider("环境散射强度", 0.00, 4.00, 0.05,
                pendingFogAmbientStrength, "%.2f", value -> pendingFogAmbientStrength = (float) value));
        this.addOptionRow(cloudSlider("散射各向异性 g", 0.00, 0.90, 0.05,
                pendingFogAnisotropy, "%.2f", value -> pendingFogAnisotropy = (float) value));
        this.addOptionRow(cloudSlider("雨天雾增强", 0.0, 8.0, 0.25,
                pendingFogRainBoost, "%.2f 倍", value -> pendingFogRainBoost = (float) value));
        this.addOptionRow(cloudSlider("近处留白距离", 0, 64, 2,
                pendingFogStartDistance, "%.0f 格", value -> pendingFogStartDistance = (float) value));
        this.addOptionRow(cloudSlider("太阳取样点（性能）", 1, 8, 1,
                pendingFogSunSteps, "%.0f", value -> pendingFogSunSteps = (int) Math.round(value)));
        // 2026-11-16：主体积雾原本是纯解析指数 ⇒ 一层均匀灰膜（形态噪声只服务
        // 1 格厚的贴地雾，且被降雨门控）。这两个旋钮分别控制大团块起伏和晨雾峰值。
        this.addOptionRow(cloudSlider("雾形态（大团块起伏）", 0.0, 2.0, 0.05,
                pendingFogShapeStrength, "%.2f",
                value -> pendingFogShapeStrength = (float) value));

        // 2026-11-10：贴地薄雾 → 平流雾（rolling advection ground fog）。
        // 参数仍是 godRayParams1 四车道 + godRayParams2.z 的速度；
        // 层厚/浓度在 shader 里有 ×6 / ×0.05 的标定映射，GUI 值维持旧档范围。
        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingFogGroundMistEnabled)
                .withValues(false, true)
                .create(Component.literal("平流雾（雨天贴地滚动）"),
                        (button, enabled) -> this.pendingFogGroundMistEnabled = enabled));
        this.addOptionRow(cloudSlider("平流雾层厚", 0.25, 8.0, 0.25,
                pendingFogGroundMistHeight, "%.2f",
                value -> pendingFogGroundMistHeight = (float) value));
        // 2026-11-12：上限 1.00 → 4.00。半格厚的薄层需要更大消光才看得出来，
        // 4.0 对应 shader 里 6/格，足以压到不透明。
        // 2026-11-13：上限 4.00 → 2.00。σ=3/格 已经完全不透明，再高只会"变黑"
        // （看到的是雾自己的颜色，而雨天环境光很暗）。
        this.addOptionRow(cloudSlider("平流雾浓度（雨天）", 0.00, 2.00, 0.05,
                pendingFogGroundMistExtinction, "%.2f",
                value -> pendingFogGroundMistExtinction = (float) value));
        this.addOptionRow(cloudSlider("平流雾滚动速度", 0.0, 6.0, 0.25,
                pendingFogGroundMistSpeed, "%.2f 格/秒",
                value -> pendingFogGroundMistSpeed = (float) value));
        this.addOptionRow(cloudSlider("平流雾作用距离（性能）", 8, 96, 2,
                pendingFogGroundMistDistance, "%.0f 格",
                value -> pendingFogGroundMistDistance = (float) value));

        // 2026-11-13：雾光学（fogOpticsParams.x/yzw）
        this.addOptionRow(cloudSlider("平流雾涡流强度", 0.0, 2.0, 0.05,
                pendingFogCurlStrength, "%.2f",
                value -> pendingFogCurlStrength = (float) value));
        this.addOptionRow(cloudSlider("雾色偏 红", 0.5, 1.5, 0.05,
                pendingFogTintR, "%.2f",
                value -> pendingFogTintR = (float) value));
        this.addOptionRow(cloudSlider("雾色偏 绿", 0.5, 1.5, 0.05,
                pendingFogTintG, "%.2f",
                value -> pendingFogTintG = (float) value));
        this.addOptionRow(cloudSlider("雾色偏 蓝（远雾偏蓝）", 0.5, 1.5, 0.05,
                pendingFogTintB, "%.2f",
                value -> pendingFogTintB = (float) value));

        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingGodRayEnabled)
                .withValues(false, true)
                .create(Component.literal("水下体积光"),
                        (button, enabled) -> this.pendingGodRayEnabled = enabled));
        this.addOptionRow(cloudSlider("体积光采样步数（性能）", 4, 48, 2,
                pendingGodRaySteps, "%.0f", value -> pendingGodRaySteps = (int) Math.round(value)));
        this.addOptionRow(cloudSlider("体积光云影采样（性能）", 1, 6, 1,
                pendingGodRayOcclusionSamples, "%.0f", value -> pendingGodRayOcclusionSamples = (int) Math.round(value)));
        this.addOptionRow(cloudSlider("体积光时间重投影", 0.00, 0.95, 0.05,
                pendingGodRayReprojection, "%.2f",
                value -> pendingGodRayReprojection = (float) value));
        this.flushOptionRow();
        }

        // 2026-11-11：云隙光（云缝里漏下来的太阳光柱）。三根滑条对应
        // cloudParams4.y/.z/.w —— 强度=云层光学厚度倍率，缝宽=洞的大小。
        // 2026-11-21 精简批：归入「体积云」页主界面（顾问 §4.1）。
        this.section = PAGE_CLOUD;
        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingCloudGapEnabled)
                .withValues(false, true)
                .create(Component.literal("云隙光（云缝漏光）"),
                        (button, enabled) -> this.pendingCloudGapEnabled = enabled));
        this.addOptionRow(cloudSlider("云隙光强度", 0.0, 4.0, 0.05,
                pendingCloudGapStrength, "%.2f",
                value -> pendingCloudGapStrength = (float) value));
        this.addOptionRow(cloudSlider("云隙缝宽", 0.0, 1.0, 0.05,
                pendingCloudGapWidth, "%.2f",
                value -> pendingCloudGapWidth = (float) value));
        this.flushOptionRow();
        this.section = PAGE_FOG;

        // 2026-11-21 精简批（顾问 §4.1）：「降雨」整页并入「雾与天气」，
        // 同用本页「高级选项」折叠（开关在本区块上方）。
        this.section = PAGE_FOG;

        // 2026-11-13 UI 审计：从"体积雾"页搬来（雨波纹属于降雨）
        // 2026-11-11：雨波纹（水面 + 地面水洼共用同一个解析场）。
        this.addOptionRow(cloudSlider("雨波纹明显度", 0.0, 3.0, 0.05,
                pendingRainRippleStrength, "%.2f 倍",
                value -> pendingRainRippleStrength = (float) value));

        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.pendingRainWallEnabled)
                .withValues(false, true)
                .create(Component.literal("墙面雨痕"),
                        (button, enabled) -> this.pendingRainWallEnabled = enabled));
        this.flushOptionRow();

        if(showFogAdvanced) {
        this.addOptionRow(cloudSlider("雨波纹大小", 0.25, 4.0, 0.05,
                pendingRainRippleSize, "%.2f 倍",
                value -> pendingRainRippleSize = (float) value));
        this.addOptionRow(cloudSlider("雨线粗细", 1.0, 40.0, 0.1,
                pendingRainWidth, "%.1f 毫米", value -> pendingRainWidth = (float) value));
        this.addOptionRow(cloudSlider("雨线长度", 0.4, 8.0, 0.1,
                pendingRainStreakLength, "%.1f 格", value -> pendingRainStreakLength = (float) value));
        this.addOptionRow(cloudSlider("雨线浓度", 0.05, 1.0, 0.01,
                pendingRainOpacity, "%.2f", value -> pendingRainOpacity = (float) value));
        this.addOptionRow(cloudSlider("雨线数量（性能）", 0.1, 2.0, 0.05,
                pendingRainDensityScale, "%.2f 倍",
                value -> pendingRainDensityScale = (float) value));
        this.addOptionRow(cloudSlider("雨线亮度", 0.05, 3.0, 0.01,
                pendingRainBrightness, "%.2f 倍", value -> pendingRainBrightness = (float) value));

        this.addOptionRow(cloudSlider("雪密度", 0.25, 4.0, 0.05,
                pendingSnowDensity, "%.2f 倍", value -> pendingSnowDensity = (float) value));
        this.addOptionRow(cloudSlider("雪片大小", 0.25, 4.0, 0.05,
                pendingSnowSize, "%.2f 倍", value -> pendingSnowSize = (float) value));
        this.addOptionRow(cloudSlider("雪浓度", 0.05, 1.0, 0.01,
                pendingSnowOpacity, "%.2f", value -> pendingSnowOpacity = (float) value));
        this.addOptionRow(cloudSlider("雪亮度", 0.05, 3.0, 0.01,
                pendingSnowBrightness, "%.2f 倍", value -> pendingSnowBrightness = (float) value));
        this.addOptionRow(cloudSlider("雪下落速度", 0.25, 4.0, 0.05,
                pendingSnowFallSpeed, "%.2f 倍", value -> pendingSnowFallSpeed = (float) value));
        this.addOptionRow(cloudSlider("雪摇摆幅度", 0.0, 1.0, 0.05,
                pendingSnowSway, "%.2f 格", value -> pendingSnowSway = (float) value));

        this.addOptionRow(cloudSlider("墙面湿度", 0.0, 2.0, 0.05,
                pendingRainWallWetness, "%.2f 倍", value -> pendingRainWallWetness = (float) value));
        this.addOptionRow(cloudSlider("墙面水滴密度", 0.0, 1.0, 0.01,
                pendingRainWallDripDensity, "%.2f", value -> pendingRainWallDripDensity = (float) value));
        this.addOptionRow(cloudSlider("墙面水流速度", 0.1, 4.0, 0.05,
                pendingRainWallDripSpeed, "%.2f 倍", value -> pendingRainWallDripSpeed = (float) value));
        this.addOptionRow(cloudSlider("雨滴大小", 0.25, 4.0, 0.05,
                pendingRainWallDripSize, "%.2f 倍", value -> pendingRainWallDripSize = (float) value));
        this.addOptionRow(cloudSlider("墙面水滴凸起", 0.0, 2.0, 0.05,
                pendingRainWallDripBump, "%.2f 倍", value -> pendingRainWallDripBump = (float) value));
        this.addOptionRow(cloudSlider("墙面雨范围（性能）", 16, 160, 8,
                pendingRainWallDistance, "%.0f 格", value -> pendingRainWallDistance = (float) value));
        this.flushOptionRow();
        }

        this.section = PAGE_WATER;

        // 2026-11-21 精简批（顾问 §4.1 页 5「水面与材质」）：原「水面」+
        // 「材质与画面」两页合并；主界面留高频项，FFT/交互波/SSS 微调进「高级选项」。
        this.addOptionRow(this.presetCycle("水面风格",
                new String[] {"现役 · 清澈长涌", "热带 · 浅滩", "深海 · 风暴", "静谧 · 湖面"},
                this::applyWaterStylePreset, this.waterStyleIndex));
        this.flushOptionRow();

        this.addOptionRow(cloudSlider("波浪强度", 0.0, 2.0, 0.05,
                pendingWaterStyleWaveScale, "%.2f 倍", value -> pendingWaterStyleWaveScale = (float) value));
        this.addOptionRow(cloudSlider("青绿水色", 0.0, 1.0, 0.01,
                pendingWaterStyleTintMix, "%.2f", value -> pendingWaterStyleTintMix = (float) value));
        this.addOptionRow(cloudSlider("水体浓度", 0.25, 4.0, 0.05,
                pendingWaterStyleDensityScale, "%.2f 倍", value -> pendingWaterStyleDensityScale = (float) value));
        this.addOptionRow(cloudSlider("焦散亮度", 0.0, 4.0, 0.05,
                pendingWaterStyleCausticStrength, "%.2f", value -> pendingWaterStyleCausticStrength = (float) value));
        this.addOptionRow(cloudSlider("交互波强度", 0.0, 2.0, 0.05,
                waterWaveStrength, "%.2f 倍", value -> waterWaveStrength = (float)value));
        this.addOptionRow(this.waterAdvancedToggle());
        this.flushOptionRow();

        if(showWaterAdvanced) {
        // 2026-11-20 审计 ⑤：删除四个**死控件**（波速 / 大涌起伏 / 细波纹理 /
        // 碎浪颗粒）。它们写 waterStyleParams1 的 x/y/z/w，全仓读取点只剩
        //   rt_test_translucent.rchit:280（waterStyleParams1.x）与
        //   water_noise_wave.glsl:19/29/39（.y/.z/.w 的噪声波幅度）
        // 而 rt_test_translucent 这个着色器**宿主从未加载**（cpp\backend 里
        // 找不到任何 "translucent" 字样，部署清单只校验 rt_test / rt_test_ser /
        // rt_test_sharc_update 三个 rgen）⇒ 主 FFT 水面走 water_wave_field.glsl，
        // 一个都不读。这三档起伏现在由「涌浪权重 / 短波权重」两级 cascade 接管。
        // 波速同理：真实时间推进读的是 cam.waterFftParams.w（speedMul），不是这里。
        // 存档字段保留（不改保存格式），只是不再暴露入口。
        this.addOptionRow(cloudSlider("波峰透光强度", 0, 2, 0.05,
                wpCrestBacklit, "%.2f", value -> wpCrestBacklit = (float)value));
        this.addOptionRow(cloudSlider("焦散色散", 0, 1, 0.01,
                wpCausticDispersion, "%.2f", value -> wpCausticDispersion = (float)value));
        this.addOptionRow(cloudSlider("焦散水深弥散", 0, 1, 0.01,
                wpCausticDepthDiffusion, "%.2f", value -> wpCausticDepthDiffusion = (float)value));
        this.addOptionRow(CycleButton.builder(
                enabled -> Component.literal(enabled ? "开" : "关"), wpPhysicalPhase)
                .withValues(false, true).create(Component.literal("水下物理相位"),
                        (button, value) -> wpPhysicalPhase = value));
        // 2026-11-20 审计 ⑥：「反射焦散强度」已无生效消费者。它写的是
        // waterWaveParams.w，唯一读取点是 rgen_glossy.glsl:1364-1372 的反射焦散增益，
        // 而那条增益已按 RTX Remix 的水面模型整体关掉
        // （WATER_REFLECT_CAUSTIC_ENABLED=false：Remix 的水面反射只吃场景辐亮度，
        // 不采焦散图）。留个拨了没反应的滑条只会误导，入口移除，存档字段保留。

        this.addOptionRow(cloudSlider("交互波标定", 0.0, 1.0, 0.05,
                waterWaveCalibration, "%.2f", value -> waterWaveCalibration = (float)value));
        this.addOptionRow(cloudSlider("波面混合", 0.0, 1.0, 0.05,
                waterWaveBlend, "%.2f", value -> waterWaveBlend = (float)value));
        this.addOptionRow(cloudSlider("波场海绵层", 0.0, 0.45, 0.01,
                waterSimSpongeWidth, "%.2f", value -> waterSimSpongeWidth = (float)value));
        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "高斯凸起" : "同心环"),
                        waterSimGaussianSource > 0.5f)
                .withValues(false, true)
                .create(Component.literal("波源形状"),
                        (button, enabled) -> waterSimGaussianSource = enabled ? 1.0f : 0.0f));
        this.addOptionRow(cloudSlider("波场阻尼", 0.0, 3.0, 0.05,
                waterSimDampingScale, "%.2f 倍", value -> waterSimDampingScale = (float)value));
        this.addOptionRow(cloudSlider("波场传播速度", 0.0, 2.0, 0.05,
                waterSimPropagationSpeed, "%.2f 倍", value -> waterSimPropagationSpeed = (float)value));
        // 2026-11-21 顾问轮1：雨滴涟漪独立滑条已删——细雨环层与 sim 大滴
        // 统一走降雨页「雨波纹明显度」/「雨波纹大小」（RainRippleSettings）。
        // waterSimRainRippleScale 字段与 JSON 键保留（存档兼容，只读往返）。
        this.addOptionRow(cloudSlider("薄层SSS强度", 0.0, 2.0, 0.05,
                waterSssStrength, "%.2f 倍", value -> waterSssStrength = (float)value));
        this.addOptionRow(cloudSlider("SSS非线性光程", 0.0, 1.0, 0.05,
                waterSssNonlinear, "%.2f", value -> waterSssNonlinear = (float)value));
        this.addOptionRow(cloudSlider("SSS能量补偿", 0.0, 4.0, 0.1,
                waterSssBoost, "%.2f", value -> waterSssBoost = (float)value));
        this.addOptionRow(cloudSlider("SSS相位g", 0.0, 0.95, 0.05,
                waterSssPhaseG, "%.2f", value -> waterSssPhaseG = (float)value));

        // 2026-11-13 审计 ④：删除四个**死控件**（风源波纹尺度 / 波纹方向 /
        // 波纹幅度 / 波峰不规则度）。它们写的是
        //   g_waterWaveLengthScale / Direction / CrestSharpness / WarpStrength，
        // 全仓**没有任何读取点** —— 旧波浪系统已被 FBM 波场取代，接线无从可接。
        // 存档字段保留（不改保存格式），只是不再暴露入口。
        this.flushOptionRow();

        this.addOptionRow(cloudSlider("最长波长", 2.0, 8.0, 0.1,
        waterWaveAflScale, "%.1f 格", value -> waterWaveAflScale = (float)value));
        // 2026-11-13 审计 ③：这个滑条原标"波数 1..12 根"，但着色器
        // （water_wave_field.glsl:73）把它当 **drag（阻尼）** 用，JNI 又钳到
        // 0..1.5 ⇒ 原来 7 以上全一样，默认 12 实际一直是 1.5。
        // 改成与真实语义一致：0..1.5 的阻尼，默认 1.5（等价于旧的 12）。
        this.addOptionRow(cloudSlider("波形阻尼", 0.0, 1.5, 0.05,
        waterWaveAflLayers, "%.2f", value -> waterWaveAflLayers = (float)value));
        this.flushOptionRow();

        this.addOptionRow(CycleButton.builder(
                        enabled -> Component.literal(enabled ? "开" : "关"),
                        this.waterFftEnabled)
                .withValues(false, true)
                .create(Component.literal("海浪"),
                        (button, enabled) -> this.waterFftEnabled = enabled));
        // 2026-11-20「风速」：浪的推进速度（0.5~14，出厂 3.5）。
        // 这是唯一一个真正能调"浪走多快"的旋钮 —— 它同时缩放 FFT 频谱的演化时钟、
        // 波面的采样漂移、以及旧解析波路径的相位，三处同源，不会出现纹理动、波峰不动的错位。
        this.addOptionRow(cloudSlider("风速", 0.5, 14.0, 0.5,
        waterFftWindSpeed, "%.1f", value -> waterFftWindSpeed = (float)value));
        this.addOptionRow(cloudSlider("振幅", 0.0, 3.0, 0.05,
        waterFftAmplitude, "%.2f 倍", value -> waterFftAmplitude = (float)value));
        this.addOptionRow(cloudSlider("平铺尺度", 2.0, 128.0, 1.0,
        waterFftTilePeriod, "%.0f 格", value -> waterFftTilePeriod = (float)value));
        this.addOptionRow(cloudSlider("波峰锐化", 0.0, 3.0, 0.05,
        waterFftChoppiness, "%.2f", value -> waterFftChoppiness = (float)value));
        this.addOptionRow(cloudSlider("波浪细节", 0.0, 3.0, 0.05,
        waterFftDetail, "%.2f 倍", value -> waterFftDetail = (float)value));
        // 2026-11-20 波面 cascade 配平：两级 cascade 各管一段波长，权重决定谁占主视觉。
        // 长波级是瓦片重复感的唯一来源，压低它 = 看得见浪、看不住格。
        this.addOptionRow(cloudSlider("涌浪权重", 0.02, 2.0, 0.01,
        waterFftSwellWeight, "%.2f 倍", value -> waterFftSwellWeight = (float)value));
        this.addOptionRow(cloudSlider("短波权重", 0.02, 3.0, 0.01,
        waterFftDetailWeight, "%.2f 倍", value -> waterFftDetailWeight = (float)value));
        this.flushOptionRow();
        }

        // =====================================================
        // 2026-11-13 UI 审计：新增两页，把散落的/错页的控件归位。
        // 只搬"明确错页"的项，不动其它布局。
        // =====================================================
        this.section = PAGE_SKY;

        // 太阳与月亮的外观（2026-11-21 精简批：原「天体与夜光」页并入
        // 「天空与天体」，同用上方 skyAdvancedToggle 折叠）。
        this.addOptionRow(cloudSlider("太阳圆盘亮度", 0.0, 8.0, 0.01,
                pendingSunDiskLuminance, "%.2f", value -> pendingSunDiskLuminance = (float) value));
        this.addOptionRow(cloudSlider("太阳角直径（度）", 0.10, 2.00, 0.01,
                pendingSunAngularDiameterDegrees, "%.2f", value -> pendingSunAngularDiameterDegrees = (float) value));
        this.addOptionRow(cloudSlider("星空亮度", 0.0, 2.0, 0.05,
                pendingStarIntensity, "%.2f", value -> pendingStarIntensity = (float) value));
        this.addOptionRow(cloudSlider("极光亮度", 0.0, 3.0, 0.05,
                pendingAuroraBrightness, "%.2f", value -> pendingAuroraBrightness = (float) value));

        if(showSkyAdvanced) {
        this.addOptionRow(cloudSlider("太阳日冕强度", 0.0, 1.0, 0.01,
                pendingSunHaloStrength, "%.2f", value -> pendingSunHaloStrength = (float) value));
        this.addOptionRow(cloudSlider("太阳透射染色", 0.0, 1.0, 0.01,
                pendingSunTintStrength, "%.2f", value -> pendingSunTintStrength = (float) value));
        this.addOptionRow(cloudSlider("极光流动速度", 0.0, 4.0, 0.05,
                pendingAuroraFlowSpeed, "%.2f", value -> pendingAuroraFlowSpeed = (float) value));
        // 2026-11-13 审计 ②：范围收窄到 0..3（着色器是 >=0 ? 值 : 1.0，
        // 负值段全部等价于"回出厂"，滑条没有意义）。
        this.addOptionRow(cloudSlider("月光强度", 0.0, 3.0, 0.05,
                moonStrength, "%.2f", value -> moonStrength = (float)value));
        this.addOptionRow(cloudSlider("月光相位", 0.0, 1.0, 0.05,
                moonPhase, "%.2f", value -> moonPhase = (float)value));
        this.flushOptionRow();
        }
        this.flushOptionRow();

        this.section = PAGE_UPSCALER;

        // 剔除与分辨率（2026-11-21 精简批：原「性能与剔除」页并入「性能与超分」高级折叠）
        if(showUpscalerAdvanced) {
        this.addOptionRow(cloudSliderWithZero("TLAS剔除距离", 0, 320, 32,
        postTlasCullDistance, "%.0f 格", "自动", value -> postTlasCullDistance = (int)(float)value));
        this.addOptionRow(cloudSliderWithZero("TLAS视锥剔除边距", 0, 96, 8,
        postTlasFrustumCullMargin, "%.0f 格", "关", value -> postTlasFrustumCullMargin = (int)(float)value));
        this.addOptionRow(cloudSliderWithZero("远云RR引导距离", 0, 20000, 250,
        postRrFarCloudDistance, "%.0f 格", "关", value -> postRrFarCloudDistance = (int)(float)value));
        // 「云分辨率下限」与体积云页的「云渲染清晰度」是同一个值，重复入口已移除。
        this.flushOptionRow();
        }

        this.section = PAGE_POST;

        // =====================================================
        // 2026-11-16 UI 审计：曝光链归位。
        //
        // HDR / 自动曝光 / 曝光补偿 / 自动曝光强度 原来散在两处：前三项在
        // 「材质与画面」页末尾，强度在后处理页中段。它们和色调映射是同一条
        // 显示链（rr_sdr_post.comp:1066 用 exposureStrength 施加 EV，1070 乘
        // 通道增益，1108 起才做 gamma 与分级），所以按**渲染顺序**排在一起：
        //
        //   曝光 → 色调映射 → 快速调整（分级之前）→ 泛光 → 精细分级（折叠）
        //
        // 只搬控件与标题；参数车道、存档字段、JNI 调用全部不改名。
        // =====================================================
        this.addSectionHeader("曝光（色调映射之前）");

        CycleButton<Boolean> hdrButton =
                CycleButton.builder(
                                enabled -> Component.literal(enabled ? "开" : "关"),
                                this.pendingHDR
                        )
                        .withValues(false, true)
                        .create(Component.literal("HDR"),
                                (button, enabled) -> this.pendingHDR = enabled);

        this.addOptionRow(hdrButton);

        CycleButton<Boolean> autoExposureButton =
                CycleButton.builder(
                                enabled -> Component.literal(enabled ? "开" : "关"),
                                this.pendingAutoExposureEnabled
                        )
                        .withValues(false, true)
                        .create(Component.literal("自动曝光"),
                                (button, enabled) ->
                                        this.pendingAutoExposureEnabled = enabled);

        this.addOptionRow(autoExposureButton);
        this.flushOptionRow();

        AbstractSliderButton exposureCompSlider =
                new AbstractSliderButton(
                        0,
                        0,
                        310,
                        20,
                        Component.empty(),
                        (this.pendingAutoExposureCompensation + 2.0) / 4.0
                ) {

                    {
                        this.updateMessage();
                    }


                    private double snappedEV() {
                        return Math.round(
                                (this.value * 4.0 - 2.0) *
                                        10.0
                        ) / 10.0;
                    }


                    @Override
                    protected void updateMessage() {
                        double ev =
                                this.snappedEV();

                        this.setMessage(
                                Component.literal(
                                        String.format(
                                                java.util.Locale.ROOT,
                                                "曝光补偿: %+.1f EV",
                                                ev
                                        )
                                )
                        );
                    }


                    @Override
                    protected void applyValue() {
                        double ev =
                                this.snappedEV();

                        this.value =
                                (ev + 2.0) / 4.0;

                        RTXSettingsScreen.this.pendingAutoExposureCompensation =
                                (float) ev;

                        this.updateMessage();
                    }
                };

        this.addOptionRow(exposureCompSlider);
        this.flushOptionRow();

        this.addSectionHeader("色调映射与快速调整（在精细分级之前）");

        this.addOptionRow(CycleButton.builder(
                        mode -> postTonemapLabel(mode), this.postTonemapMode)
                .withValues(0, 1, 2, 3, 4, 5, 6)
                .create(Component.literal("色彩映射"),
                        (button, value) -> this.postTonemapMode = value));
        this.addOptionRow(cloudSlider("对比度（映射曲线）", 0.5, 2.0, 0.05,
        postContrast, "%.2f 倍", value -> postContrast = (float)value));
        this.addOptionRow(cloudSlider("饱和度（分级前）", 0.0, 2.0, 0.05,
        postSaturation, "%.2f 倍", value -> postSaturation = (float)value));
        this.addOptionRow(cloudSlider("Gamma", 1.8, 2.6, 0.02,
        postGamma, "%.2f", value -> postGamma = (float)value));

        // 2026-11-21 精简批：曝光强度 / 通道增益 / 泛光收进「高级选项」折叠。
        this.addOptionRow(this.postAdvancedToggle());
        this.flushOptionRow();

        if(showPostAdvanced) {
        this.addOptionRow(cloudSlider("自动曝光强度", 0.0, 1.0, 0.05,
        postExposureStrength, "%.2f", value -> postExposureStrength = (float)value));
        this.addOptionRow(cloudSlider("通道增益 R", 0.25, 2.5, 0.01,
        postGainR, "%.2f", value -> postGainR = (float)value));
        this.addOptionRow(cloudSlider("通道增益 G", 0.25, 2.5, 0.01,
        postGainG, "%.2f", value -> postGainG = (float)value));
        this.addOptionRow(cloudSlider("通道增益 B", 0.25, 2.5, 0.01,
        postGainB, "%.2f", value -> postGainB = (float)value));
        this.addOptionRow(cloudSlider("泛光强度", 0.0, 2.0, 0.05,
        postBloomStrength, "%.2f (0=关)", value -> postBloomStrength = (float)value));
        this.flushOptionRow();
        }

        this.flushOptionRow();

        // =====================================================
        // 2026-11-16 UI 审计：色彩分级从独立页并回后处理页。
        //
        // 两条调色链本来就在同一个着色器里串联（rr_sdr_post.comp:1070 通道
        // 增益 → 1108 gamma → 1110 饱和度 → 1132 起 CDL/分离色调/暗角），
        // 拆成两页只会让人以为它们是替代关系。现在同页、按渲染顺序、24 项
        // 默认折叠。
        //
        // 注意：这里**没有删**「通道增益 / 对比度 / 饱和度」。它们与 CDL 的
        // Gain / 对比 / 自然饱和度分别位于「色调映射前」和「gamma 后」，
        // 不是同一个算子的两份入口；删掉会给已有存档留下改不到的隐藏值。
        // =====================================================
        this.section = PAGE_POST;
        this.addSectionHeader("精细分级（在快速调整之后）");
        this.addOptionRow(colorGradeToggleButton());
        this.flushOptionRow();

        if(colorGradeExpanded) {
            this.addSectionHeader("预设（只改分级；色彩映射请在上面选，推荐 AgX）");
            this.addOptionRow(Button.builder(
                            Component.literal("预设：冰岛 · 冷寂"),
                            button -> this.applyColorGradePreset(PRESET_ICELAND))
                    .width(150).build());
            this.addOptionRow(Button.builder(
                            Component.literal("预设：格陵兰 · 极夜"),
                            button -> this.applyColorGradePreset(PRESET_GREENLAND))
                    .width(150).build());
            this.addOptionRow(Button.builder(
                            Component.literal("预设：晨雾 · 苍灰"),
                            button -> this.applyColorGradePreset(PRESET_MIST))
                    .width(150).build());
            this.addOptionRow(Button.builder(
                            Component.literal("预设：复位（中性）"),
                            button -> this.applyColorGradePreset(PRESET_NEUTRAL))
                    .width(150).build());
            this.flushOptionRow();

            float[] cgValues = RTXSettingsConfig.getColorGradeSettings().toParamArray();
            for(int cgIndex = 0; cgIndex < RTXSettingsConfig.COLOR_GRADE_LABELS.length; ++cgIndex)
            {
                if(cgIndex == 0)
                {
                    this.addSectionHeader("① 曝光与黑白场");
                }
                if(cgIndex == 5)
                {
                    this.addSectionHeader("② 逐通道 Lift / Gamma / Gain（ASC-CDL）");
                }
                if(cgIndex == 14)
                {
                    this.addSectionHeader("③ 分离色调（暗部 / 亮部各推一个色相）");
                }
                if(cgIndex == 18)
                {
                    this.addSectionHeader("④ 氛围（饱和度 / 褪色 / 暗角 / 色相）");
                }

                final int cgSlot = cgIndex;
                final float[] cgRange = RTXSettingsConfig.COLOR_GRADE_RANGES[cgSlot];
                final String cgFormat =
                        cgRange[2] >= 1.0f ? "%.0f"
                                : (cgRange[2] >= 0.01f ? "%.2f" : "%.3f");
                this.addOptionRow(cloudSlider(RTXSettingsConfig.COLOR_GRADE_LABELS[cgSlot],
                        cgRange[0], cgRange[1], cgRange[2], cgValues[cgSlot], cgFormat,
                        value -> {
                            float[] next = RTXSettingsConfig.getColorGradeSettings().toParamArray();
                            next[cgSlot] = (float) value;
                            RTXSettingsConfig.ColorGradeSettings updated =
                                    new RTXSettingsConfig.ColorGradeSettings(next);
                            if(!NativeVulkan.isLoaded()
                                    || NativeVulkan.setColorGradeParams(updated.toParamArray())) {
                                RTXSettingsConfig.setColorGradeSettings(updated);
                            }
                        }));
            }
            this.flushOptionRow();
        }

        this.section = PAGE_WATER;

        // 2026-11-21 精简批（顾问 §4.1）：原「材质与画面」页并入「水面与材质」。
        // 「大气透视」两行归入天空页；SSPT/SSR/RT 光泽/GI 近场/光泽采样归入
        // 性能与超分页；盔甲/镜面弹射/POM 采样/树叶 SSS/动态光收进本页高级折叠。

        // 2026-11-13 UI 审计：从"体积云"页搬来（地面材质的大气透视）
        this.section = PAGE_SKY;
        this.addSectionHeader("大气透视");
        this.addOptionRow(cloudSlider("地面大气透视强度", 0, 2, 0.05,
                groundAerialStrength, "%.2f", value -> groundAerialStrength = (float) value));
        this.addOptionRow(cloudSlider("地面遮挡衰减", 0, 1, 0.05,
                groundAerialOcclusion, "%.2f", value -> groundAerialOcclusion = (float) value));
        this.flushOptionRow();
        this.section = PAGE_WATER;


        AbstractSliderButton ambientBrightnessSlider =
                new AbstractSliderButton(
                        0,
                        0,
                        310,
                        20,
                        Component.empty(),
                        this.pendingNoDirectLightAmbient
                ) {

                    {
                        this.updateMessage();
                    }


                    @Override
                    protected void updateMessage() {

                        double snapped =
                                Math.round(
                                        this.value * 10000.0
                                ) / 10000.0;


                        this.setMessage(
                                Component.literal(
                                        String.format(
                                                java.util.Locale.ROOT,
                                                "无直射光环境亮度: %.4f",
                                                snapped
                                        )
                                )
                        );
                    }


                    @Override
                    protected void applyValue() {

                        double snapped =
                                Math.round(
                                        this.value * 10000.0
                                ) / 10000.0;


                        this.value =
                                snapped;


                        RTXSettingsScreen.this.pendingNoDirectLightAmbient =
                                (float) snapped;


                        this.updateMessage();
                    }
                };


        this.addOptionRow(
                ambientBrightnessSlider
        );


        AbstractSliderButton pomDepthSlider =
                new AbstractSliderButton(
                        0,
                        0,
                        310,
                        20,
                        Component.empty(),
                        this.pendingPOMDepth / 0.20
                ) {

                    {
                        this.updateMessage();
                    }


                    private double snappedDepth() {
                        return Math.round(
                                this.value *
                                        0.20 *
                                        100.0
                        ) / 100.0;
                    }


                    @Override
                    protected void updateMessage() {
                        double depth =
                                this.snappedDepth();


                        this.setMessage(
                                Component.literal(
                                        String.format(
                                                java.util.Locale.ROOT,
                                                "视差深度: %.2f",
                                                depth
                                        )
                                )
                        );
                    }


                    @Override
                    protected void applyValue() {
                        double depth =
                                this.snappedDepth();


                        this.value =
                                depth / 0.20;


                        RTXSettingsScreen.this.pendingPOMDepth =
                                (float) depth;


                        this.updateMessage();
                    }
                };


        this.addOptionRow(
                pomDepthSlider
        );


        // 2026-11-21 精简批：SSPT / SSR / RT 光泽归入「性能与超分」页主界面；
        // GI 近场 / 光泽采样随该页高级折叠。
        this.section = PAGE_UPSCALER;
        // 2026-11-02 屏幕空间降级链（走 G-buffer；两项默认都是 0 = 关）
        //   SSPT：射程 0..12 区块，屏幕空间路径追踪替换该半径内的间接光项
        //   SSR ：射程上限 0..64 区块，硬性下限 12 —— 只有"RT 镜面命中距离
        //         > 12 区块"的像素才允许被 SSR 接手（判据在 ss_hybrid.comp）
        this.addOptionRow(cloudSlider("SSPT 射程（0=关）", 0.0, 12.0, 1.0,
                this.pendingSsptRange, "%.0f 区块",
                value -> this.pendingSsptRange = (float) value));
        this.addOptionRow(cloudSlider("SSR 射程（0=关，最低 12）", 0.0, 64.0, 1.0,
                this.pendingSsrRange, "%.0f 区块",
                value -> this.pendingSsrRange = (float) value));

        // 2026-11-03：RT 光泽射线总开关。关 = 镜面只由 SSR（远场）/ SSPT（射程内）
        // 提供，对应的 RT 反射射线整段不再发射（这是"仅 SSPT + RTAO"的入口）。
        CycleButton<Boolean> rtGlossyButton =
                CycleButton.builder(
                                enabled -> Component.literal(
                                        enabled ? "开（RT 射线）" : "关（仅屏幕空间）"
                                ),
                                this.pendingRtGlossy
                        )
                        .withValues(true, false)
                        .create(
                                Component.literal("RT 光泽反射"),
                                (button, enabled) -> this.pendingRtGlossy = enabled
                        );

        this.addOptionRow(rtGlossyButton);


        if(showUpscalerAdvanced) {
        CycleButton<Integer> giNearRateButton =
                CycleButton.builder(
                                level -> Component.literal(
                                        level == 0 ? "全速"
                                                : (level == 1 ? "半速" : "四分之一")
                                ),
                                this.pendingGiNearRate
                        )
                        .withValues(0, 1, 2)
                        .create(
                                Component.literal("GI 近场采样"),
                                (button, level) -> this.pendingGiNearRate = level
                        );

        this.addOptionRow(
                giNearRateButton
        );


        CycleButton<Boolean> glossyRoughLodButton =
                CycleButton.builder(
                                enabled -> Component.literal(
                                        enabled ? "按粗糙度降速" : "全速"
                                ),
                                this.pendingGlossyRoughLod
                        )
                        .withValues(false, true)
                        .create(
                                Component.literal("光泽采样"),
                                (button, enabled) -> this.pendingGlossyRoughLod = enabled
                        );

        this.addOptionRow(
                glossyRoughLodButton
        );
        this.flushOptionRow();
        }

        this.section = PAGE_WATER;


        // 2026-11-21 精简批：「树叶次表面散射」收进本页高级折叠（在下方）。


        AbstractSliderButton localLightDistanceSlider =
                new AbstractSliderButton(
                        0,
                        0,
                        310,
                        20,
                        Component.empty(),
                        (this.pendingLocalLightMaxDistance - 0.25) / 1.75
                ) {

                    {
                        this.updateMessage();
                    }


                    // 2026-10-31：滑杆语义从"最远距离（格）"改成**整体系数**。
                    // 每个光源方块在 local_light_profile.glsl 里有各自的基础传播距离，
                    // 这里只统一缩放（0.25x ~ 2.00x），乘积仍由着色器封顶 30 格。
                    private double snappedScale() {
                        return 0.25 +
                                Math.round(
                                        this.value * 175.0
                                ) / 100.0;
                    }


                    @Override
                    protected void updateMessage() {
                        double scale =
                                this.snappedScale();


                        this.setMessage(
                                Component.literal(
                                        String.format(
                                                java.util.Locale.ROOT,
                                                "本地光距离系数: %.2fx (封顶30格)",
                                                scale
                                        )
                                )
                        );
                    }


                    @Override
                    protected void applyValue() {
                        double scale =
                                this.snappedScale();


                        this.value =
                                (scale - 0.25) / 1.75;


                        RTXSettingsScreen.this.pendingLocalLightMaxDistance =
                                (float) scale;


                        this.updateMessage();
                    }
                };


        this.addOptionRow(
                localLightDistanceSlider
        );


        AbstractSliderButton localLightIntensitySlider =
                new AbstractSliderButton(
                        0,
                        0,
                        310,
                        20,
                        Component.empty(),
                        this.pendingLocalLightIntensity / 4.0
                ) {

                    {
                        this.updateMessage();
                    }


                    private double snappedIntensity() {
                        return Math.round(
                                this.value * 80.0
                        ) / 20.0;
                    }


                    @Override
                    protected void updateMessage() {
                        double intensity =
                                this.snappedIntensity();


                        this.setMessage(
                                Component.literal(
                                        String.format(
                                                java.util.Locale.ROOT,
                                                "整体光源亮度系数: %.2fx",
                                                intensity
                                        )
                                )
                        );
                    }


                    @Override
                    protected void applyValue() {
                        double intensity =
                                this.snappedIntensity();


                        this.value =
                                intensity / 4.0;


                        RTXSettingsScreen.this.pendingLocalLightIntensity =
                                (float) intensity;


                        this.updateMessage();
                    }
                };


        this.addOptionRow(
                localLightIntensitySlider
        );

        // 2026-11-21 精简批：材质细项收进「高级选项」折叠。
        this.addOptionRow(this.waterAdvancedToggle());
        this.flushOptionRow();

        if(showWaterAdvanced) {
        CycleButton<Boolean> armorMetalButton =
                CycleButton.builder(
                                enabled -> Component.literal(
                                        enabled ? "开" : "关"
                                ),
                                this.pendingArmorMetal
                        )
                        .withValues(false, true)
                        .create(
                                Component.literal("盔甲金属反射"),
                                (button, enabled) -> this.pendingArmorMetal = enabled
                        );

        this.addOptionRow(
                armorMetalButton
        );

        this.addOptionRow(cloudSlider("镜面弹射层数", 1, 8, 1,
        postMirrorBounceCount, "%.0f 层", value -> postMirrorBounceCount = (int)(float)value));

        CycleButton<Boolean> pomSamplingButton =
                CycleButton.builder(
                                sharp -> Component.literal(
                                        sharp ? "锐利（单纹素）" : "平滑（双线性）"
                                ),
                                this.pendingPOMSharp
                        )
                        .withValues(false, true)
                        .create(
                                Component.literal("视差采样"),
                                (button, sharp) -> this.pendingPOMSharp = sharp
                        );

        this.addOptionRow(
                pomSamplingButton
        );

        CycleButton<Boolean> subsurfaceButton =
                CycleButton.builder(
                                trueSss -> Component.literal(
                                        trueSss
                                                ? "真次表面散射（天光透射）"
                                                : "太阳背光（仅直射）"
                                ),
                                this.pendingTrueSubsurfaceScattering
                        )
                        .withValues(false, true)
                        .create(
                                Component.literal("树叶次表面散射"),
                                (button, trueSss) ->
                                        this.pendingTrueSubsurfaceScattering = trueSss
                        );

        this.addOptionRow(
                subsurfaceButton
        );
        this.flushOptionRow();
        }

        if(showWaterAdvanced) {
        final AbstractSliderButton dynamicLightIntensitySlider =
                new AbstractSliderButton(
                        0,
                        0,
                        310,
                        20,
                        Component.empty(),
                        this.pendingDynamicLightIntensity / 4.0
                ) {

                    {
                        this.updateMessage();
                    }


                    private double snappedIntensity() {
                        return Math.round(
                                this.value * 80.0
                        ) / 20.0;
                    }


                    @Override
                    protected void updateMessage() {
                        double intensity =
                                this.snappedIntensity();


                        this.setMessage(
                                Component.literal(
                                        intensity <= 0.0
                                                ? "移动光源: 关"
                                                : String.format(
                                                        java.util.Locale.ROOT,
                                                        "移动光源强度: %.2fx",
                                                        intensity
                                                )
                                )
                        );
                    }


                    @Override
                    protected void applyValue() {
                        double intensity =
                                this.snappedIntensity();


                        this.value =
                                intensity / 4.0;


                        RTXSettingsScreen.this.pendingDynamicLightIntensity =
                                (float) intensity;


                        this.updateMessage();
                    }
                };


        this.addOptionRow(
                dynamicLightIntensitySlider
        );
        this.flushOptionRow();
        }

        // 2026-11-16 UI 审计：HDR / 自动曝光 / 曝光补偿 已搬到「后处理」页的
        // 「曝光（色调映射之前）」组（PAGE_POST 段），按渲染顺序排在色调映射
        // 之前。这里不再重复出现，参数车道与存档字段不变。
        this.flushOptionRow();
    }


    // 2026-11-07：applyPendingSettings 里每个 record 的紧凑构造器都会做范围校验，
    // 一旦某个值越界就抛 IllegalArgumentException —— 而按钮回调没有 try/catch，
    // 结果是"点了应用、界面没报错、设置却没保存"，下次打开又回到旧值
    // （RTAO 关档 =5 就是这么暴露的）。这里兜一层：至少把异常打到 stderr 里，
    // 而不是静默中断。
    private void applyPendingSettingsSafe() {
        try {
            this.applyPendingSettings();
        } catch (RuntimeException ex) {
            System.err.println("[RTX CONFIG] apply aborted: " + ex);
        }
    }

    private void applyPendingSettings() {

        boolean allApplied =
                true;



        final boolean reflexAvailable =
                NativeVulkan.isLoaded()
                        && NativeVulkan.isReflexAvailable();


        if(reflexAvailable)
        {
            if(
                    NativeVulkan.setReflexMode(
                            this.pendingReflexMode
                    )
            )
            {
                RTXSettingsConfig.setReflexMode(
                        this.pendingReflexMode
                );
            }
            else
            {
                this.pendingReflexMode =
                        NativeVulkan.getReflexMode();

                allApplied =
                        false;
            }
        }

        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setFGFrameSync(
                        this.pendingFGFrameSync
                ))
        {
            RTXSettingsConfig.setFGFrameSyncEnabled(
                    this.pendingFGFrameSync
            );
        }
        else
        {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setFGRefreshCapEnabled(
                        this.pendingFGRefreshCap
                ))
        {
            RTXSettingsConfig.setFGRefreshCapEnabled(
                    this.pendingFGRefreshCap
            );
        }
        else
        {
            allApplied = false;
        }

        RTXSettingsConfig.setSLDevPluginsEnabled(
                this.pendingSLDevPlugins
        );

        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setFGMultiplier(
                        this.pendingFGMultiplier
                ))
        {
            RTXSettingsConfig.setFGMultiplier(
                    this.pendingFGMultiplier
            );
        }
        else
        {
            this.pendingFGMultiplier =
                    RTXSettingsConfig.getFGMultiplier();

            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setFGBaseFrameLimit(
                        this.pendingFGBaseFrameLimit
                ))
        {
            RTXSettingsConfig.setFGBaseFrameLimit(
                    this.pendingFGBaseFrameLimit
            );
        }
        else
        {
            this.pendingFGBaseFrameLimit =
                    RTXSettingsConfig.getFGBaseFrameLimit();

            allApplied = false;
        }

        RTXSettingsConfig.setSerEnabled(
                this.pendingSerEnabled
        );

        if(!NativeVulkan.isLoaded())
        {
            RTXSettingsConfig.setMotionVectorsEnabled(
                    this.pendingMotionVectors
            );
        }
        else
        {
            this.pendingMotionVectors =
                    NativeVulkan.setMotionVectorScaleMode(
                            this.pendingMotionVectors
                    );

            RTXSettingsConfig.setMotionVectorsEnabled(
                    this.pendingMotionVectors
            );
        }

        RTXSettingsConfig.setEntityArmorMetalEnabled(
                this.pendingArmorMetal
        );



        if(
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setRRMode(
                                this.pendingRRMode
                        )
        )
        {
            RTXSettingsConfig.setRRMode(
                    this.pendingRRMode
            );
        }
        else
        {
            this.pendingRRMode =
                    NativeVulkan.getRRMode();

            allApplied =
                false;
        }

        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setCustomUpscalePercent(
                        this.pendingCustomUpscalePercent)) {
            RTXSettingsConfig.setCustomUpscalePercent(
                    this.pendingCustomUpscalePercent);
        } else {
            this.pendingCustomUpscalePercent =
                    NativeVulkan.getCustomUpscalePercent();
            allApplied = false;
        }

        final boolean upscalerApplied =
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setUpscalerMode(RTXSettingsConfig.UPSCALER_MODE_RR);

        if(upscalerApplied)
        {
            RTXSettingsConfig.setUpscalerMode(RTXSettingsConfig.UPSCALER_MODE_RR);
        }
        else
        {
            allApplied = false;
        }

        RTXSettingsConfig.PerformanceSettings updatedPerformance =
                new RTXSettingsConfig.PerformanceSettings(
                        this.pendingAoNearSamples,
                        this.pendingAoMidSamples,
                        this.pendingGiBounces,
                        this.pendingSunShadowSamples,
                        this.pendingLocalLightRisCandidates,
                        this.pendingLocalLightAreaSamples,
                        this.pendingWaterReflectionInterval,
                        this.pendingGlossyFullDistance,
                        this.pendingDynamicRtasInterval,
                        this.pendingMaxRtParticles,
                        this.pendingRtSpp,
                        this.pendingVolumetricRestir
                );
        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setPerformanceSettings(updatedPerformance))
        {
            RTXSettingsConfig.setPerformanceSettings(updatedPerformance);
        }
        else
        {
            allApplied = false;
        }

        RTXSettingsConfig.setRainSettings(
                new RTXSettingsConfig.RainSettings(
                        this.pendingRainOpacity,
                        this.pendingRainStreakLength,
                        this.pendingRainBrightness,
                        this.pendingRainWidth,
                        this.pendingRainDensityScale
                )
        );

        RTXSettingsConfig.setSnowSettings(
                new RTXSettingsConfig.SnowSettings(
                        this.pendingSnowSize,
                        this.pendingSnowDensity,
                        this.pendingSnowOpacity,
                        this.pendingSnowBrightness,
                        this.pendingSnowFallSpeed,
                        this.pendingSnowSway
                )
        );

        RTXSettingsConfig.RainWallSettings updatedRainWall =
                new RTXSettingsConfig.RainWallSettings(
                        this.pendingRainWallEnabled,
                        this.pendingRainWallWetness,
                        this.pendingRainWallDripDensity,
                        this.pendingRainWallDripSpeed,
                        this.pendingRainWallDripSize,
                        this.pendingRainWallDripBump,
                        this.pendingRainWallDistance
                );
        if(!NativeVulkan.isLoaded() || NativeVulkan.setRainWallSettings(
                updatedRainWall.enabled(),
                updatedRainWall.wetness(),
                updatedRainWall.dripDensity(),
                updatedRainWall.dripSpeed(),
                updatedRainWall.dripSize(),
                updatedRainWall.dripBump(),
                updatedRainWall.distance()
        )) {
            RTXSettingsConfig.setRainWallSettings(updatedRainWall);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.GalaxySettings updatedGalaxy =
                new RTXSettingsConfig.GalaxySettings(
                        this.pendingGalaxyScale,
                        this.pendingGalaxyTiltDegrees,
                        this.pendingGalaxyYawDegrees,
                        this.pendingGalaxyMix
                );
        if(!NativeVulkan.isLoaded() || NativeVulkan.setGalaxySettings(
                updatedGalaxy.scale(),
                updatedGalaxy.tiltDegrees(),
                updatedGalaxy.yawDegrees(),
                updatedGalaxy.mix()
        )) {
            RTXSettingsConfig.setGalaxySettings(updatedGalaxy);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.WaterStyleSettings updatedWaterStyle =
                new RTXSettingsConfig.WaterStyleSettings(
                        this.pendingWaterStyleWaveScale,
                        this.pendingWaterStyleTintMix,
                        this.pendingWaterStyleDensityScale,
                        this.pendingWaterStyleCausticStrength,
                        this.pendingWaterStyleWaveSpeed,
                        this.pendingWaterStyleSwellScale,
                        this.pendingWaterStyleMidScale,
                        this.pendingWaterStyleFineScale
                );
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterStyleSettings(
                updatedWaterStyle.waveScale(),
                updatedWaterStyle.tintMix(),
                updatedWaterStyle.densityScale(),
                updatedWaterStyle.causticStrength(),
                updatedWaterStyle.waveSpeed(),
                updatedWaterStyle.swellScale(),
                updatedWaterStyle.midScale(),
                updatedWaterStyle.fineScale()
        )) {
            RTXSettingsConfig.setWaterStyleSettings(updatedWaterStyle);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.WaterWaveSettings updatedWaterWave =
                new RTXSettingsConfig.WaterWaveSettings(
                        waterWaveCalibration, waterWaveStrength, waterWaveBlend,
                        waterReflectCausticStrength);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterWaveParams(updatedWaterWave.toParamArray())) {
            RTXSettingsConfig.setWaterWaveSettings(updatedWaterWave);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.WaterSssSettings updatedWaterSss =
                new RTXSettingsConfig.WaterSssSettings(
                        waterSssNonlinear, waterSssBoost, waterSssPhaseG, waterSssStrength);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterSssParams(updatedWaterSss.toParamArray())) {
            RTXSettingsConfig.setWaterSssSettings(updatedWaterSss);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.WaterSimWaveSettings updatedWaterSimWave =
                new RTXSettingsConfig.WaterSimWaveSettings(
                        waterSimSpongeWidth, waterSimGaussianSource,
                        waterSimDampingScale, waterSimPropagationSpeed,
                        waterSimRainRippleScale);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterSimWaveParams(updatedWaterSimWave.toParamArray())) {
            RTXSettingsConfig.setWaterSimWaveSettings(updatedWaterSimWave);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.WaterWaveShapeSettings updatedWaterWaveShape =
                new RTXSettingsConfig.WaterWaveShapeSettings(
                        waterWaveLengthScale, waterWaveDirection, waterWaveCrestSharpness,
                        waterWaveWarpStrength);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterWaveShapeParams(updatedWaterWaveShape.toParamArray())) {
            RTXSettingsConfig.setWaterWaveShapeSettings(updatedWaterWaveShape);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.WaterWaveAflSettings updatedWaterAfl =
                new RTXSettingsConfig.WaterWaveAflSettings(
                        waterWaveAflScale, waterWaveAflLayers);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterWaveAflParams(updatedWaterAfl.toParamArray())) {
            RTXSettingsConfig.setWaterWaveAflSettings(updatedWaterAfl);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.WaterFftSettings updatedWaterFft =
                new RTXSettingsConfig.WaterFftSettings(
                        waterFftEnabled, waterFftAmplitude, waterFftChoppiness,
                        waterFftTilePeriod, waterFftDetail,
                        waterFftSwellWeight, waterFftDetailWeight, waterFftWindSpeed);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterFftParams(updatedWaterFft.toParamArray())) {
            RTXSettingsConfig.setWaterFftSettings(updatedWaterFft);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.WindSettings updatedWind =
                new RTXSettingsConfig.WindSettings(
                        windAngleDeg, windStrength, windGust);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWindParams(updatedWind.toParamArray())) {
            RTXSettingsConfig.setWindSettings(updatedWind);
        } else {
            allApplied = false;
        }
        int effectiveTlasCull = postTlasCullDistance;
        if(effectiveTlasCull == 0 && this.minecraft != null) {
            effectiveTlasCull = Math.min(512, Math.max(64,
                    this.minecraft.options.renderDistance().get() * 16 * 5 / 4));
        }
        RTXSettingsConfig.PostProcessingSettings updatedPost =
                new RTXSettingsConfig.PostProcessingSettings(
                        postTonemapMode, postExposureStrength, postContrast,
                        postSaturation, postGamma, postMirrorBounceCount,
                        postTlasCullDistance, postTlasFrustumCullMargin,
                        postRrFarCloudDistance);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setPostProcessingParams(
                new float[] {
                    updatedPost.tonemapMode(), updatedPost.exposureStrength(),
                    updatedPost.contrast(), updatedPost.saturation(),
                    updatedPost.gamma(), updatedPost.mirrorBounceCount(),
                    effectiveTlasCull, postTlasFrustumCullMargin,
                    this.postBloomStrength,
                    this.postGainR, this.postGainG, this.postGainB })) {
            RTXSettingsConfig.setPostProcessingSettings(updatedPost);
            RTXSettingsConfig.setPostBloomStrength(this.postBloomStrength);
            RTXSettingsConfig.setPostGains(this.postGainR, this.postGainG, this.postGainB);
        } else {
            allApplied = false;
        }
        if(NativeVulkan.isLoaded() &&
                !NativeVulkan.setRrGuideSettings(true, postRrFarCloudDistance)) {
            this.postRrFarCloudDistance = (int)NativeVulkan.getRrFarCloudDistance();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudResFloorPercent(
                this.pendingCloudResFloorPercent)) {
            RTXSettingsConfig.setCloudResFloorPercent(
                    this.pendingCloudResFloorPercent);
        } else {
            this.pendingCloudResFloorPercent =
                    NativeVulkan.getCloudResFloorPercent();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudTemporalMode(
                this.pendingCloudTemporalMode)) {
            RTXSettingsConfig.setCloudTemporalMode(
                    this.pendingCloudTemporalMode);
        } else {
            this.pendingCloudTemporalMode =
                    NativeVulkan.getCloudTemporalMode();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudFarStepDensity(
                this.pendingCloudFarStepDensity)) {
            RTXSettingsConfig.setCloudFarStepDensity(
                    this.pendingCloudFarStepDensity);
        } else {
            this.pendingCloudFarStepDensity =
                    NativeVulkan.getCloudFarStepDensity();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setGiRoulette(
                this.pendingGiRoulette)) {
            RTXSettingsConfig.setGiRoulette(
                    this.pendingGiRoulette);
        } else {
            this.pendingGiRoulette =
                    NativeVulkan.getGiRoulette();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setDirectLightMode(
                this.pendingDirectLightMode)) {
            RTXSettingsConfig.setDirectLightMode(
                    this.pendingDirectLightMode);
        } else {
            this.pendingDirectLightMode =
                    NativeVulkan.getDirectLightMode();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setIndirectMode(
                this.pendingIndirectMode)) {
            RTXSettingsConfig.setIndirectMode(
                    this.pendingIndirectMode);
        } else {
            this.pendingIndirectMode =
                    NativeVulkan.getIndirectMode();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setSparseRatePercent(
                this.pendingSparseRatePercent)) {
            RTXSettingsConfig.setSparseRatePercent(
                    this.pendingSparseRatePercent);
        } else {
            this.pendingSparseRatePercent =
                    NativeVulkan.getSparseRatePercent();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setMotionDebugView(
                this.pendingMotionDebugView)) {
            RTXSettingsConfig.setMotionDebugView(
                    this.pendingMotionDebugView);
        } else {
            this.pendingMotionDebugView =
                    NativeVulkan.getMotionDebugView();
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setSunMarchFarDistance(
                this.pendingSunMarchFarDistance)) {
            RTXSettingsConfig.setSunMarchFarDistance(
                    this.pendingSunMarchFarDistance);
        } else {
            this.pendingSunMarchFarDistance =
                    NativeVulkan.getSunMarchFarDistance();
            allApplied = false;
        }

        RTXSettingsConfig.CloudHpSettings updatedCloudHp =
                new RTXSettingsConfig.CloudHpSettings(
                        RTXSettingsConfig.getCloudHpSettings().toParamArray());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudHpParams(updatedCloudHp.toParamArray())) {
            RTXSettingsConfig.setCloudHpSettings(updatedCloudHp);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.CloudSettings updatedClouds =
                new RTXSettingsConfig.CloudSettings(
                        this.pendingCloudEnabled,
                        this.pendingCloudBottom,
                        this.pendingCloudThickness,
                        this.pendingCloudCoverage,
                        this.pendingCloudDensity,
                        this.pendingCloudDetailStrength,
                        this.pendingCloudWindSpeed,
                        this.pendingCloudEvolution,
                        this.pendingCloudQuality,
                        this.pendingCloudSize,
                        this.pendingCloudDistribution,
                        this.pendingCloudBaseFeather,
                        this.pendingCloudSunlightStrength
                );

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudFadeFollowsRrMode(
                this.pendingCloudFadeFollowsRrMode)) {
            RTXSettingsConfig.setCloudFadeFollowsRrMode(
                    this.pendingCloudFadeFollowsRrMode);
        } else {
            this.pendingCloudFadeFollowsRrMode =
                    RTXSettingsConfig.isCloudFadeFollowsRrMode();
            allApplied = false;
        }
        RTXSettingsConfig.CloudMsSettings updatedCloudMs =
                new RTXSettingsConfig.CloudMsSettings(
                        msPhiIntensity, msPhiDepthPow, msPhiDepthBias, msOctaveStrength);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudMsParams(updatedCloudMs.toParamArray())) {
            RTXSettingsConfig.setCloudMsSettings(updatedCloudMs);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.PlaneCloudSettings updatedPlaneCloud =
                new RTXSettingsConfig.PlaneCloudSettings(
                        this.pendingPlaneCloudEnabled,
                        this.pendingPlaneCloudHeightOffset,
                        this.pendingPlaneCloudScale,
                        this.pendingPlaneCloudSpeed,
                        this.pendingPlaneCloudDensity,
                        this.pendingPlaneCloudCoverage,
                        Math.round(this.pendingPlaneCloudOctaves),
                        this.pendingPlaneCloudFadeDistance,
                        this.pendingPlaneCloudBrightness);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setPlaneCloudParams(updatedPlaneCloud.toParamArray())) {
            RTXSettingsConfig.setPlaneCloudSettings(updatedPlaneCloud);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudPhiSettings updatedCloudPhi =
                new RTXSettingsConfig.CloudPhiSettings(
                        phiBoundaryConfidence, phiMsBuildScale, phiCompress, phiConeRatio);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudPhiParams(updatedCloudPhi.toParamArray())) {
            RTXSettingsConfig.setCloudPhiSettings(updatedCloudPhi);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudShapeSettings updatedCloudShape =
                new RTXSettingsConfig.CloudShapeSettings(
                        shapeBottomSmoothHeight, shapeBottomSmoothPow, shapeTwoTierThreshold,
                        shapeLightErosion);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudShapeParams(updatedCloudShape.toParamArray())) {
            RTXSettingsConfig.setCloudShapeSettings(updatedCloudShape);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudMoonSettings updatedCloudMoon =
                new RTXSettingsConfig.CloudMoonSettings(moonStrength, moonPhase);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudMoonParams(updatedCloudMoon.toParamArray())) {
            RTXSettingsConfig.setCloudMoonSettings(updatedCloudMoon);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudShape2Settings updatedCloudShape2 =
                new RTXSettingsConfig.CloudShape2Settings(
                        shapeDetailScale, shapeDetailContrast, shapeProfileCurve);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudShape2Params(updatedCloudShape2.toParamArray())) {
            RTXSettingsConfig.setCloudShape2Settings(updatedCloudShape2);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudAmbientSettings updatedCloudAmbient =
                new RTXSettingsConfig.CloudAmbientSettings(
                        ambTopMul, ambBottomMul, ambSunSide, ambHueFollow);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudAmbientParams(updatedCloudAmbient.toParamArray())) {
            RTXSettingsConfig.setCloudAmbientSettings(updatedCloudAmbient);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudPhysicsSettings updatedCloudPhysics =
                new RTXSettingsConfig.CloudPhysicsSettings(cloudGroundAlbedo,
                        cloudGroundBounce, cloudSunlightResidual, cloudPhaseFloor);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudPhysicsParams(updatedCloudPhysics.toParamArray())) {
            RTXSettingsConfig.setCloudPhysicsSettings(updatedCloudPhysics);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudMs2Settings updatedCloudMs2 =
                new RTXSettingsConfig.CloudMs2Settings(
                        ms2ScatterTint, ms2Octaves, ms2Attenuation, ms2Eccentricity);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudMs2Params(updatedCloudMs2.toParamArray())) {
            RTXSettingsConfig.setCloudMs2Settings(updatedCloudMs2);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudProfileSettings updatedCloudProfile =
                new RTXSettingsConfig.CloudProfileSettings(profileType, profileTypeSpread);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudProfileParams(updatedCloudProfile.toParamArray())) {
            RTXSettingsConfig.setCloudProfileSettings(updatedCloudProfile);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudDetailSettings updatedCloudDetail =
                new RTXSettingsConfig.CloudDetailSettings(
                        wispyStrength, wispyEdgeWidth, wispyReach, wispyTopHeight);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudDetailParams(updatedCloudDetail.toParamArray())) {
            RTXSettingsConfig.setCloudDetailSettings(updatedCloudDetail);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudTemporalSettings updatedCloudTemporal =
                new RTXSettingsConfig.CloudTemporalSettings(
                        cloudSkyVisibilityStrength, cloudSkyVisibilitySteps, cloudTemporalDampStrength,
                        cloudBottomOcclusion);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudTemporalParams(updatedCloudTemporal.toParamArray())) {
            RTXSettingsConfig.setCloudTemporalSettings(updatedCloudTemporal);
        } else {
            allApplied = false;
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSettings(
                updatedClouds.enabled(),
                updatedClouds.bottom(),
                updatedClouds.thickness(),
                updatedClouds.coverage(),
                updatedClouds.density(),
                updatedClouds.detailStrength(),
                updatedClouds.windSpeed(),
                updatedClouds.evolution(),
                updatedClouds.quality(),
                updatedClouds.size(),
                updatedClouds.distribution(),
                updatedClouds.baseFeather(),
                updatedClouds.sunlightStrength()
        )) {
            RTXSettingsConfig.setCloudSettings(updatedClouds);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.VolumetricFogSettings updatedFog =
                new RTXSettingsConfig.VolumetricFogSettings(
                        this.pendingFogEnabled,
                        this.pendingFogDensity,
                        this.pendingFogHeight,
                        this.pendingFogScaleHeight,
                        this.pendingFogAnisotropy,
                        this.pendingFogSunStrength,
                        this.pendingFogAmbientStrength,
                        this.pendingFogMaxDistance,
                        this.pendingFogStartDistance,
                        this.pendingFogSunSteps,
                        this.pendingFogRainBoost
                );
        if(!NativeVulkan.isLoaded() || NativeVulkan.setVolumetricFogSettings(
                updatedFog.enabled(),
                updatedFog.density(),
                updatedFog.height(),
                updatedFog.scaleHeight(),
                updatedFog.anisotropy(),
                updatedFog.sunStrength(),
                updatedFog.ambientStrength(),
                updatedFog.maxDistance(),
                updatedFog.startDistance(),
                updatedFog.sunSteps(),
                updatedFog.rainBoost()
        )) {
            RTXSettingsConfig.setVolumetricFogSettings(updatedFog);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.FogGroundMistSettings updatedFogGroundMist =
                new RTXSettingsConfig.FogGroundMistSettings(
                        this.pendingFogGroundMistEnabled,
                        this.pendingFogGroundMistHeight,
                        this.pendingFogGroundMistExtinction,
                        this.pendingFogGroundMistDistance,
                        this.pendingFogGroundMistSpeed
                );
        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setFogGroundMistParams(updatedFogGroundMist.toParamArray())) {
            RTXSettingsConfig.setFogGroundMistSettings(updatedFogGroundMist);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.CloudGapSettings updatedCloudGap =
                new RTXSettingsConfig.CloudGapSettings(
                        this.pendingCloudGapEnabled,
                        this.pendingCloudGapStrength,
                        this.pendingCloudGapWidth
                );
        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setCloudGapParams(updatedCloudGap.toParamArray())) {
            RTXSettingsConfig.setCloudGapSettings(updatedCloudGap);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.RainRippleSettings updatedRipple =
                new RTXSettingsConfig.RainRippleSettings(
                        this.pendingRainRippleSize,
                        this.pendingRainRippleStrength
                );
        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setRainRippleParams(updatedRipple.toParamArray())) {
            RTXSettingsConfig.setRainRippleSettings(updatedRipple);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.FogOpticsSettings updatedOptics =
                new RTXSettingsConfig.FogOpticsSettings(
                        this.pendingFogCurlStrength,
                        this.pendingFogTintR,
                        this.pendingFogTintG,
                        this.pendingFogTintB
                );
        if(!NativeVulkan.isLoaded() ||
                NativeVulkan.setFogOpticsParams(updatedOptics.toParamArray())) {
            RTXSettingsConfig.setFogOpticsSettings(updatedOptics);
        } else {
            allApplied = false;
        }

        // 2026-11-13：玻璃光学焦散设置已删除（用户要求整体回退焦散）

        RTXSettingsConfig.CloudAerialSettings updatedCloudAerial =
                new RTXSettingsConfig.CloudAerialSettings(
                        pendingCloudAerialStrength,
                        groundAerialStrength,
                        groundAerialOcclusion);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudAerialParams(updatedCloudAerial.toParamArray())) {
            RTXSettingsConfig.setCloudAerialSettings(updatedCloudAerial);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudWeatherStyleSettings updatedWeatherStyle =
                new RTXSettingsConfig.CloudWeatherStyleSettings(
                        pendingCloudStormDarkness,
                        pendingCloudLightningStrength,
                        pendingCloudSunsetWarmth,
                        pendingCloudWeatherCoupling);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSkyHueParams(updatedWeatherStyle.toParamArray())) {
            RTXSettingsConfig.setCloudWeatherStyleSettings(updatedWeatherStyle);
        } else {
            allApplied = false;
        }
        RTXSettingsConfig.CloudLight2Settings updatedLight2 =
                new RTXSettingsConfig.CloudLight2Settings(
                        pendingCloudPowderEffect,
                        pendingCloudSilverSpread,
                        pendingCloudSharpness,
                        pendingCloudBaseErosion);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudLight2Params(updatedLight2.toParamArray())) {
            RTXSettingsConfig.setCloudLight2Settings(updatedLight2);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.VolumetricLightSettings updatedGodRays =
                new RTXSettingsConfig.VolumetricLightSettings(
                        this.pendingGodRayEnabled,
                        this.pendingGodRaySteps,
                        this.pendingGodRayOcclusionSamples,
                        this.pendingGodRayIntensity,
                        this.pendingGodRayReprojection
                );
        if(!NativeVulkan.isLoaded() || NativeVulkan.setVolumetricLightSettings(
                updatedGodRays.enabled(),
                updatedGodRays.steps(),
                updatedGodRays.occlusionSamples(),
                updatedGodRays.intensity()
        )) {
            RTXSettingsConfig.setVolumetricLightSettings(updatedGodRays);
            if(NativeVulkan.isLoaded() &&
                    !NativeVulkan.setGodRayReprojection(updatedGodRays.reprojection())) {
                allApplied = false;
            }
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.SunVisualSettings updatedSunVisuals =
                new RTXSettingsConfig.SunVisualSettings(
                        this.pendingSunDiskLuminance,
                        this.pendingSunHaloStrength,
                        this.pendingSunAngularDiameterDegrees
                );
        if(!NativeVulkan.isLoaded() || NativeVulkan.setSunVisualSettings(
                updatedSunVisuals.diskLuminance(),
                updatedSunVisuals.haloStrength(),
                updatedSunVisuals.angularDiameterDegrees()
        )) {
            RTXSettingsConfig.setSunVisualSettings(updatedSunVisuals);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.AtmosphereSettings updatedAtmosphere =
                new RTXSettingsConfig.AtmosphereSettings(
                        this.pendingStarIntensity,
                        this.pendingSunTintStrength,
                        this.pendingPhysicalSkyEnabled,
                        this.pendingScatterIntensity,
                        this.pendingOzoneStrength
                );
        if(!NativeVulkan.isLoaded() || NativeVulkan.setAtmosphereSettings(
                updatedAtmosphere.starIntensity(),
                updatedAtmosphere.sunTintStrength(),
                updatedAtmosphere.physicalSkyEnabled(),
                updatedAtmosphere.scatterIntensity(),
                updatedAtmosphere.ozoneStrength()
        )) {
            RTXSettingsConfig.setAtmosphereSettings(updatedAtmosphere);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setSkyModelSettings(
                this.pendingSkyModel,
                this.pendingSkyModelScale,
                this.pendingNightSkyAmbientScale,
                this.pendingRainSkyDimScale
        )) {
            RTXSettingsConfig.setSkyModel(this.pendingSkyModel);
            RTXSettingsConfig.setSkyModelScale(this.pendingSkyModelScale);
            RTXSettingsConfig.setNightSkyAmbientScale(this.pendingNightSkyAmbientScale);
            RTXSettingsConfig.setRainSkyDimScale(this.pendingRainSkyDimScale);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setMoonSettings(
                this.pendingMoonLightFloor,
                this.pendingMoonDaylightDim
        )) {
            RTXSettingsConfig.setMoonLightFloor(this.pendingMoonLightFloor);
            RTXSettingsConfig.setMoonDaylightDim(this.pendingMoonDaylightDim);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setFogMoodSettings(
                this.pendingFogShapeStrength,
                this.pendingFogDawnBoost
        )) {
            RTXSettingsConfig.setFogShapeStrength(this.pendingFogShapeStrength);
            RTXSettingsConfig.setFogDawnBoost(this.pendingFogDawnBoost);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setPowderStrength(
                this.pendingPowderStrength
        )) {
            RTXSettingsConfig.setPowderStrength(
                    this.pendingPowderStrength
            );
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setAuroraBrightness(
                this.pendingAuroraBrightness
        )) {
            RTXSettingsConfig.setAuroraBrightness(
                    this.pendingAuroraBrightness
            );
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setAuroraFlowSpeed(
                this.pendingAuroraFlowSpeed
        )) {
            RTXSettingsConfig.setAuroraFlowSpeed(
                    this.pendingAuroraFlowSpeed
            );
        } else {
            allApplied = false;
        }

        // 2026-11-02 屏幕空间降级链。native 失败时回滚 pending（与其它项同口径）。
        if(!NativeVulkan.isLoaded() || NativeVulkan.setSSPTRange(
                this.pendingSsptRange
        )) {
            RTXSettingsConfig.setSsptRange(
                    this.pendingSsptRange
            );
        } else {
            this.pendingSsptRange = RTXSettingsConfig.getSsptRange();
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setSSRRange(
                this.pendingSsrRange
        )) {
            RTXSettingsConfig.setSsrRange(
                    this.pendingSsrRange
            );

            // 1..11 会被硬性约束抬到 12，滑条读数必须跟着回写，
            // 否则界面上仍显示"5 区块"而实际是 12
            this.pendingSsrRange = RTXSettingsConfig.getSsrRange();
        } else {
            this.pendingSsrRange = RTXSettingsConfig.getSsrRange();
            allApplied = false;
        }

        // 2026-11-03 RT 光泽射线总开关
        if(!NativeVulkan.isLoaded() || NativeVulkan.setRTGlossyEnabled(
                this.pendingRtGlossy
        )) {
            RTXSettingsConfig.setRTGlossyEnabled(
                    this.pendingRtGlossy
            );
        } else {
            this.pendingRtGlossy = RTXSettingsConfig.isRTGlossyEnabled();
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSunForwardScattering(
                this.pendingCloudSunForwardScattering
        )) {
            RTXSettingsConfig.setCloudSunForwardScattering(
                    this.pendingCloudSunForwardScattering
            );
        } else {
            allApplied = false;
        }


        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSunReddening(
                this.pendingCloudSunReddening
        )) {
            RTXSettingsConfig.setCloudSunReddening(
                    this.pendingCloudSunReddening
            );
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudTopStretch(
                this.pendingCloudTopStretch
        )) {
            RTXSettingsConfig.setCloudTopStretch(
                    this.pendingCloudTopStretch
            );
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudDetileStrength(
                this.pendingCloudDetileStrength
        )) {
            RTXSettingsConfig.setCloudDetileStrength(
                    this.pendingCloudDetileStrength
            );
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudType(this.pendingCloudType)) {
            // 2026-11-07：native 对旧出厂值 0.35 做了 0.35→0.8 的迁移
            // （JniSettings.cpp 的 setCloudType），存档若继续存 0.35，界面读数
            // 就和实际跑的 0.8 不一致。这里按 native 回读值落存档。
            if(NativeVulkan.isLoaded()) {
                this.pendingCloudType = NativeVulkan.getCloudType();
            }
            RTXSettingsConfig.setCloudType(this.pendingCloudType);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudErosionScale(
                this.pendingCloudErosionScale
        )) {
            RTXSettingsConfig.setCloudErosionScale(this.pendingCloudErosionScale);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudWeatherScale(
                this.pendingCloudWeatherScale
        )) {
            RTXSettingsConfig.setCloudWeatherScale(this.pendingCloudWeatherScale);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSilver(this.pendingCloudSilver)) {
            RTXSettingsConfig.setCloudSilver(this.pendingCloudSilver);
        } else {
            allApplied = false;
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudViewDistance(
                this.pendingCloudViewDistance
        )) {
            RTXSettingsConfig.setCloudViewDistance(this.pendingCloudViewDistance);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.CloudMarchSettings updatedCloudMarch =
                new RTXSettingsConfig.CloudMarchSettings(
                        msAdaptiveLightRange, msTwoTierStep, msMeanReproject,
                        msFarFadeDistance);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudMarchParams(updatedCloudMarch.toParamArray())) {
            RTXSettingsConfig.setCloudMarchSettings(updatedCloudMarch);
        } else {
            allApplied = false;
        }

        RTXSettingsConfig.WaterParamsSettings updatedWaterParams =
                new RTXSettingsConfig.WaterParamsSettings(
                        wpCrestBacklit, wpCausticDispersion, wpCausticDepthDiffusion,
                        wpPhysicalPhase);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterParams(updatedWaterParams.toParamArray())) {
            RTXSettingsConfig.setWaterParamsSettings(updatedWaterParams);
        } else {
            allApplied = false;
        }



        // 2026-11-07：补 !isLoaded() 逃逸 —— 原来 native 未就绪时 set 必然失败，
        // 走失败分支把 pending 回滚成旧值，这一项等于永远不生效。
        if(
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setNoDirectLightAmbient(
                        this.pendingNoDirectLightAmbient
                )
        )
        {
            RTXSettingsConfig.setNoDirectLightAmbient(
                    this.pendingNoDirectLightAmbient
            );
        }
        else
        {
            this.pendingNoDirectLightAmbient =
                    NativeVulkan.getNoDirectLightAmbient();

            allApplied =
                    false;
        }




        // 2026-11-07：补 !isLoaded() 逃逸 —— 原来 native 未就绪时 set 必然失败，
        // 走失败分支把 pending 回滚成旧值，这一项等于永远不生效。
        if(
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setPOMDepth(
                        this.pendingPOMDepth
                )
        )
        {
            RTXSettingsConfig.setPOMDepth(
                    this.pendingPOMDepth
            );
        }
        else
        {
            this.pendingPOMDepth =
                    NativeVulkan.getPOMDepth();

            allApplied =
                    false;
        }


        // 2026-11-07：补 !isLoaded() 逃逸 —— 原来 native 未就绪时 set 必然失败，
        // 走失败分支把 pending 回滚成旧值，这一项等于永远不生效。
        if(
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setPOMSharp(
                        this.pendingPOMSharp
                )
        )
        {
            RTXSettingsConfig.setPOMSharp(
                    this.pendingPOMSharp
            );
        }
        else
        {
            this.pendingPOMSharp =
                    NativeVulkan.getPOMSharp();

            allApplied =
                    false;
        }


        // 2026-11-07：原写法是 `if(!set(...)) { 把 pending 回滚成 native 旧值 }`，
        // native 未就绪时 set 必然失败 ⇒ 这一项永远不生效。改成"只在 native
        // 就绪且真的失败时才回滚"，成功则写进存档（此前它根本没有存档字段）。
        if(
                NativeVulkan.isLoaded() &&
                        !NativeVulkan.setGiNearRate(
                                this.pendingGiNearRate
                        )
        )
        {
            this.pendingGiNearRate =
                    NativeVulkan.getGiNearRate();

            allApplied =
                    false;
        }
        else
        {
            RTXSettingsConfig.setGiNearRate(this.pendingGiNearRate);
        }

        // 2026-11-07：同上，改成"只在 native 就绪且真失败时回滚"，成功写存档。
        if(
                NativeVulkan.isLoaded() &&
                        !NativeVulkan.setGlossyRoughLod(
                                this.pendingGlossyRoughLod
                        )
        )
        {
            this.pendingGlossyRoughLod =
                    NativeVulkan.getGlossyRoughLod();

            allApplied =
                    false;
        }
        else
        {
            RTXSettingsConfig.setGlossyRoughLod(this.pendingGlossyRoughLod);
        }


        for(int i = 0; i < RTXSettingsConfig.UE_CLOUD_PARAM_COUNT; i++) {

            if(
                    !NativeVulkan.isLoaded()
                            || NativeVulkan.setUeCloudParam(
                                    i,
                                    this.pendingUeCloud[i]
                            )
            )
            {
                RTXSettingsConfig.setUeCloud(
                        i,
                        this.pendingUeCloud[i]
                );
            }
            else
            {
                this.pendingUeCloud[i] =
                        NativeVulkan.getUeCloudParam(i);

                allApplied =
                        false;
            }
        }


        // 2026-11-07：补 !isLoaded() 逃逸 —— 原来 native 未就绪时 set 必然失败，
        // 走失败分支把 pending 回滚成旧值，这一项等于永远不生效。
        if(
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setSSSMode(
                        this.pendingTrueSubsurfaceScattering
                )
        )
        {
            RTXSettingsConfig.setTrueSubsurfaceScattering(
                    this.pendingTrueSubsurfaceScattering
            );
        }
        else
        {
            this.pendingTrueSubsurfaceScattering =
                    NativeVulkan.getSSSMode();

            allApplied =
                    false;
        }


        // 2026-11-07：补 !isLoaded() 逃逸 —— 原来 native 未就绪时 set 必然失败，
        // 走失败分支把 pending 回滚成旧值，这一项等于永远不生效。
        if(
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setLocalLightMaxDistance(
                        this.pendingLocalLightMaxDistance
                )
        )
        {
            RTXSettingsConfig.setLocalLightMaxDistance(
                    this.pendingLocalLightMaxDistance
            );
        }
        else
        {
            this.pendingLocalLightMaxDistance =
                    NativeVulkan.getLocalLightMaxDistance();

            allApplied =
                    false;
        }


        // 2026-11-07：补 !isLoaded() 逃逸 —— 原来 native 未就绪时 set 必然失败，
        // 走失败分支把 pending 回滚成旧值，这一项等于永远不生效。
        if(
                !NativeVulkan.isLoaded() ||
                        NativeVulkan.setLocalLightIntensity(
                        this.pendingLocalLightIntensity
                )
        )
        {
            RTXSettingsConfig.setLocalLightIntensity(
                    this.pendingLocalLightIntensity
            );
        }
        else
        {
            this.pendingLocalLightIntensity =
                    NativeVulkan.getLocalLightIntensity();

            allApplied =
                    false;
        }


        try
        {
            RTXSettingsConfig.setDynamicLightIntensity(
                    this.pendingDynamicLightIntensity
            );
        }
        catch(IllegalArgumentException invalidDynamicLightIntensity)
        {
            this.pendingDynamicLightIntensity =
                    RTXSettingsConfig.getDynamicLightIntensity();

            allApplied =
                    false;
        }


        final boolean hdrChanged =
                RTXSettingsConfig.isHDREnabled() !=
                        this.pendingHDR;

        if(NativeVulkan.isLoaded())
        {
            NativeVulkan.setHDREnabledNative(
                    this.pendingHDR
            );

            if(hdrChanged)
            {
                NativeVulkan.hdrSwapchainDirty =
                        true;
            }
        }

        RTXSettingsConfig.setHDREnabled(
                this.pendingHDR
        );


        if(NativeVulkan.isLoaded())
        {
            NativeVulkan.setAutoExposureEnabledNative(
                    this.pendingAutoExposureEnabled
            );

            if(!NativeVulkan.setAutoExposureCompensationNative(
                    this.pendingAutoExposureCompensation
            ))
            {
                this.pendingAutoExposureCompensation =
                        NativeVulkan.getAutoExposureCompensation();

                allApplied =
                        false;
            }
        }

        RTXSettingsConfig.setAutoExposureEnabled(
                this.pendingAutoExposureEnabled
        );

        RTXSettingsConfig.setAutoExposureCompensation(
                this.pendingAutoExposureCompensation
        );

        RTXSettingsConfig.setBuiltinLodEnabled(this.pendingBuiltinLodEnabled);
        RTXSettingsConfig.setBuiltinLodDistanceChunks(this.pendingBuiltinLodDistanceChunks);
        com.example.client.lod.OrionLodManager.onSettingsChanged();

        System.out.println(
                allApplied
                        ? "[RTX SETTINGS] Confirmed"
                        : "[RTX SETTINGS] Confirmed with native failure"
        );

        RTXSettingsConfig.flush();
    }

    private static double customUpscaleToSlider(int percent) {
        if (percent <= 0) {
            return 0.0;
        }
        final int clamped = Math.max(
                RTXSettingsConfig.CUSTOM_UPSCALE_MIN,
                Math.min(RTXSettingsConfig.CUSTOM_UPSCALE_MAX, percent));
        return (double) (clamped - 32) / 68.0;
    }

    private static int customUpscaleFromSlider(double sliderValue) {
        final int idx = (int) Math.round(sliderValue * 68.0);
        if (idx <= 0) {
            return RTXSettingsConfig.CUSTOM_UPSCALE_OFF;
        }
        return Math.min(RTXSettingsConfig.CUSTOM_UPSCALE_MAX, 32 + idx);
    }

    private static Component qualityLabel(int mode) {
        return switch(mode) {
            case RTXSettingsConfig.RR_MODE_PERFORMANCE -> Component.literal("Performance");
            case RTXSettingsConfig.RR_MODE_BALANCED -> Component.literal("Balanced");
            case RTXSettingsConfig.RR_MODE_QUALITY -> Component.literal("Quality");
            case RTXSettingsConfig.RR_MODE_DLAA -> Component.literal("DLAA");
            case RTXSettingsConfig.RR_MODE_ULTRA_PERFORMANCE -> Component.literal("Ultra Performance");
            default -> Component.literal("Quality");
        };
    }

    private CycleButton<Integer> performanceButton(
            String label,
            int current,
            List<Integer> values,
            IntConsumer setter
    ) {
        return CycleButton.builder(
                        value -> Component.literal(
                                // 2026-11-21 精简批：删 value==5→"关闭" 的误映射。
                                // 原生侧 aoNearSamples 0..5 都是合法采样数（5=最高档），
                                // 没有"5=关"的语义；0 统一显示"关"。
                                value == 0 ? "关" : Integer.toString(value)
                        ),
                        current
                )
                .withValues(values)
                .create(
                        Component.literal(label),
                        (button, value) -> setter.accept(value)
                );
    }

    private void loadCloudSettings(RTXSettingsConfig.CloudSettings clouds) {
        this.pendingCloudEnabled = clouds.enabled();
        this.pendingCloudBottom = clouds.bottom();
        this.pendingCloudThickness = clouds.thickness();
        this.pendingCloudCoverage = clouds.coverage();
        this.pendingCloudDensity = clouds.density();
        this.pendingCloudDetailStrength = clouds.detailStrength();
        this.pendingCloudWindSpeed = clouds.windSpeed();
        this.pendingCloudEvolution = clouds.evolution();
        this.pendingCloudQuality = clouds.quality();
        this.pendingCloudSize = clouds.size();
        this.pendingCloudDistribution = clouds.distribution();
        this.pendingCloudBaseFeather = clouds.baseFeather();
        this.pendingCloudSunlightStrength = clouds.sunlightStrength();
    }

    private void loadCloudMsSettings(RTXSettingsConfig.CloudMsSettings ms) {
        this.msPhiIntensity = ms.phiIntensity();
        this.msPhiDepthPow = ms.phiDepthPow();
        this.msPhiDepthBias = ms.phiDepthBias();
        this.msOctaveStrength = ms.octaveStrength();
    }

    private void loadCloudPhiSettings(RTXSettingsConfig.CloudPhiSettings phi) {
        this.phiBoundaryConfidence = phi.boundaryConfidence();
        this.phiMsBuildScale = phi.msBuildScale();
        this.phiCompress = phi.compress();
        this.phiConeRatio = phi.coneRatio();
    }

    private void loadCloudShapeSettings(RTXSettingsConfig.CloudShapeSettings shape) {
        this.shapeBottomSmoothHeight = shape.bottomSmoothHeight();
        this.shapeBottomSmoothPow = shape.bottomSmoothPow();
        this.shapeTwoTierThreshold = shape.twoTierThreshold();
        this.shapeLightErosion = shape.lightErosion();
    }

    private void loadCloudMoonSettings(RTXSettingsConfig.CloudMoonSettings moon) {
        this.moonStrength = moon.moonStrength();
        this.moonPhase = moon.moonPhase();
    }

    private void loadCloudShape2Settings(RTXSettingsConfig.CloudShape2Settings shape2) {
        this.shapeDetailScale = shape2.detailScale();
        this.shapeDetailContrast = shape2.detailContrast();
        this.shapeProfileCurve = shape2.profileCurve();
    }

    private void loadCloudAmbientSettings(RTXSettingsConfig.CloudAmbientSettings ambient) {
        this.ambTopMul = ambient.topMultiplier();
        this.ambBottomMul = ambient.bottomMultiplier();
        this.ambSunSide = ambient.sunSideStrength();
        this.ambHueFollow = ambient.hueFollow();
    }

    private void loadCloudPhysicsSettings(RTXSettingsConfig.CloudPhysicsSettings physics) {
        this.cloudGroundAlbedo = physics.groundAlbedo();
        this.cloudGroundBounce = physics.groundBounce();
        this.cloudSunlightResidual = physics.sunlightResidual();
        this.cloudPhaseFloor = physics.phaseFloor();
    }

    private void loadCloudMs2Settings(RTXSettingsConfig.CloudMs2Settings ms2) {
        this.ms2ScatterTint = ms2.scatterTint();
        this.ms2Octaves = ms2.octaves();
        this.ms2Attenuation = ms2.attenuation();
        this.ms2Eccentricity = ms2.eccentricity();
    }

    private void loadCloudProfileSettings(RTXSettingsConfig.CloudProfileSettings profile) {
        this.profileType = profile.cloudType();
        this.profileTypeSpread = profile.typeSpread();
    }

    private void loadCloudDetailSettings(RTXSettingsConfig.CloudDetailSettings detail) {
        this.wispyStrength = detail.wispyStrength();
        this.wispyEdgeWidth = detail.wispyEdgeWidth();
        this.wispyReach = detail.wispyReach();
        this.wispyTopHeight = detail.wispyTopHeight();
    }

    private void loadCloudTemporalSettings(RTXSettingsConfig.CloudTemporalSettings temporal) {
        this.cloudSkyVisibilityStrength = temporal.skyVisibilityStrength();
        this.cloudSkyVisibilitySteps = temporal.skyVisibilitySteps();
        this.cloudTemporalDampStrength = temporal.temporalDampStrength();
        this.cloudBottomOcclusion = temporal.bottomOcclusion();
    }

    private void loadFogExtrasSettings(RTXSettingsConfig.FogExtrasSettings extras) {
        this.fogExtrasDynamicLightStrength = extras.dynamicLightStrength();
        this.fogExtrasGroundAnchorStrength = extras.groundAnchorStrength();
        this.fogExtrasSkyAmbientStrength = extras.skyAmbientStrength();
        this.fogExtrasTemporalJitterStrength = extras.temporalJitterStrength();
    }

    private void loadCloudAerialSettings(RTXSettingsConfig.CloudAerialSettings aerial) {
        this.pendingCloudAerialStrength = aerial.cloudAerialStrength();
        this.groundAerialStrength = aerial.groundAerialStrength();
        this.groundAerialOcclusion = aerial.groundAerialOcclusion();
    }

    private void loadCloudWeatherStyleSettings(RTXSettingsConfig.CloudWeatherStyleSettings weatherStyle) {
        this.pendingCloudStormDarkness = weatherStyle.stormDarkness();
        this.pendingCloudLightningStrength = weatherStyle.lightningStrength();
        this.pendingCloudSunsetWarmth = weatherStyle.sunsetWarmth();
        this.pendingCloudWeatherCoupling = weatherStyle.weatherCoupling();
    }

    private void loadCloudLight2Settings(RTXSettingsConfig.CloudLight2Settings light2) {
        this.pendingCloudPowderEffect = light2.powderEffect();
        this.pendingCloudSilverSpread = light2.silverSpread();
        this.pendingCloudSharpness = light2.sharpness();
        this.pendingCloudBaseErosion = light2.baseErosion();
    }

    private void loadCloudMarchSettings(RTXSettingsConfig.CloudMarchSettings m) {
        this.msAdaptiveLightRange = m.adaptiveLightRange();
        this.msFarFadeDistance = m.farFadeDistance();
        this.msTwoTierStep = m.twoTierStep();
        this.msMeanReproject = m.meanDepthReproject();
    }

    private void loadWaterParamsSettings(RTXSettingsConfig.WaterParamsSettings wp) {
        this.wpCrestBacklit = wp.crestBacklit();
        this.wpCausticDispersion = wp.causticDispersion();
        this.wpCausticDepthDiffusion = wp.causticDepthDiffusion();
        this.wpPhysicalPhase = wp.physicalPhase();
    }

    private void loadWaterWaveSettings(RTXSettingsConfig.WaterWaveSettings wave) {
        this.waterWaveCalibration = wave.calibration();
        this.waterWaveStrength = wave.strength();
        this.waterWaveBlend = wave.blend();
        this.waterReflectCausticStrength = wave.reflectCausticStrength();
    }

    private void loadWaterSimWaveSettings(RTXSettingsConfig.WaterSimWaveSettings sim) {
        this.waterSimSpongeWidth = sim.spongeWidth();
        this.waterSimGaussianSource = sim.gaussianSource();
        this.waterSimDampingScale = sim.dampingScale();
        this.waterSimPropagationSpeed = sim.propagationSpeed();
        this.waterSimRainRippleScale = sim.rainRippleScale();
    }

    private void loadWaterFftSettings(RTXSettingsConfig.WaterFftSettings fft) {
        this.waterFftEnabled = fft.enabled();
        this.waterFftAmplitude = fft.amplitude();
        this.waterFftChoppiness = fft.choppiness();
        this.waterFftTilePeriod = fft.tilePeriod();
        this.waterFftDetail = fft.detail();
        this.waterFftSwellWeight = fft.swellWeight();
        this.waterFftDetailWeight = fft.detailWeight();
        this.waterFftWindSpeed = fft.windSpeed();
    }

    // 2026-11-22 统一风场：读配置进界面字段（「风场」折叠组）。
    private void loadWindSettings(RTXSettingsConfig.WindSettings wind) {
        this.windAngleDeg = wind.angleDeg();
        this.windStrength = wind.strength();
        this.windGust = wind.gust();
    }

    private static final int PRESET_NEUTRAL = 0;
    private static final int PRESET_ICELAND = 1;
    private static final int PRESET_GREENLAND = 2;
    private static final int PRESET_MIST = 3;

    private static final float[] GRADE_ICELAND = {
            0.10f, 0.015f, 0.94f, 1.05f, 0.17f,
            0.008f, 0.014f, 0.024f,
            0.98f, 1.00f, 1.05f,
            0.94f, 1.00f, 1.07f,
            195.0f, 0.32f, 215.0f, 0.15f,
            -0.08f, 0.16f, 0.20f, 0.62f, -6.0f, 0.05f
    };

    private static final float[] GRADE_GREENLAND = {
            -0.25f, 0.02f, 0.88f, 1.12f, 0.14f,
            0.005f, 0.010f, 0.026f,
            0.96f, 1.00f, 1.08f,
            0.90f, 1.00f, 1.12f,
            205.0f, 0.45f, 220.0f, 0.10f,
            -0.15f, 0.10f, 0.34f, 0.50f, -10.0f, 0.02f
    };

    private static final float[] GRADE_MIST = {
            0.20f, 0.05f, 0.85f, 0.90f, 0.20f,
            0.030f, 0.032f, 0.036f,
            1.00f, 1.00f, 1.00f,
            0.98f, 1.00f, 1.02f,
            180.0f, 0.15f, 210.0f, 0.10f,
            -0.25f, 0.30f, 0.12f, 0.70f, 0.0f, 0.0f
    };

    // =====================================================================
    // 2026-11-21 精简批（顾问 §2.1 / §3.3 / §4.2）：云形宏滑条 + 六域一键预设。
    // 全部 Java 侧：改存档字段 + 推 native + flush + 重建页面。每组第 1 档 =
    // 2026-11-21 现役基线（"一键回家"）。HP 宏只覆盖 v5 活槽位（18-23 是死槽）。
    // =====================================================================

    private int cloudStyleIndex;
    private int fogStyleIndex;
    private int weatherMoodIndex;
    private int waterStyleIndex;
    private int lightStyleIndex;
    private int cirrusStyleIndex;
    // 天气氛围总强度 w（会话内）：lane = B * w。首次拖动时快照当前 lane 为基线。
    private float weatherMoodW = 1.0f;
    private float[] weatherMoodBase;
    private float[] weatherMoodLastB;

    /** HP 单槽按锚点 lerp 并钳制到该槽合法范围。 */
    private static void lerpHp(float[] hp, int slot, float lo, float hi, float u) {
        float[] range = RTXSettingsConfig.CLOUD_HP_RANGES[slot];
        float v = lo + (hi - lo) * u;
        hp[slot] = Math.min(Math.max(v, range[0]), range[1]);
    }

    /** 四个云形宏（u∈[0,1]）展开到 HP 活槽位（顾问 §2.1 公式，死槽 18-23 不碰）。 */
    private RTXSettingsConfig.CloudHpSettings expandHpMacros(
            float uCarve, float uWispy, float uStrato, float uPhase) {
        float[] hp = RTXSettingsConfig.getCloudHpSettings().toParamArray();
        lerpHp(hp, 0, 0.55f, 0.00f, uCarve);
        lerpHp(hp, 1, 0.70f, 0.24f, uCarve);
        lerpHp(hp, 2, 0.85f, 1.50f, uCarve);
        lerpHp(hp, 12, 1.00f, 3.00f, uCarve);
        lerpHp(hp, 4, 0.40f, 0.01f, uWispy);
        lerpHp(hp, 5, 0.15f, 0.00f, uWispy);
        lerpHp(hp, 6, 0.75f, 0.83f, uWispy);
        lerpHp(hp, 7, 2.00f, 0.90f, uWispy);
        lerpHp(hp, 8, 0.45f, 2.00f, uWispy);
        lerpHp(hp, 9, 0.45f, 1.78f, uWispy);
        lerpHp(hp, 3, 0.08f, 0.015f, uStrato);
        lerpHp(hp, 13, 0.25f, 1.00f, uStrato);
        lerpHp(hp, 14, 0.35f, 1.00f, uStrato);
        lerpHp(hp, 15, 0.60f, 1.00f, uStrato);
        lerpHp(hp, 10, 2.20f, 3.00f, uStrato);
        lerpHp(hp, 11, 1.00f, 0.00f, uStrato);
        lerpHp(hp, 16, 0.85f, 1.00f, uPhase);
        lerpHp(hp, 17, 0.30f, 0.00f, uPhase);
        lerpHp(hp, 24, 0.50f, 0.88f, uPhase);
        return new RTXSettingsConfig.CloudHpSettings(hp);
    }

    private void pushHp(RTXSettingsConfig.CloudHpSettings updated) {
        if(!NativeVulkan.isLoaded()
                || NativeVulkan.setCloudHpParams(updated.toParamArray())) {
            RTXSettingsConfig.setCloudHpSettings(updated);
        }
    }

    /** 云形宏滑条：u 由锚点槽从现役 HP 状态反解；拖动批量覆写 HP 子集并即时下发。 */
    private AbstractWidget hpMacroSlider(
            String label,
            int anchorSlot, float anchorLo, float anchorHi,
            int[] slots, float[][] anchors
    ) {
        float[] hp = RTXSettingsConfig.getCloudHpSettings().toParamArray();
        float u = Math.clamp(
                (hp[anchorSlot] - anchorLo) / (anchorHi - anchorLo), 0.0f, 1.0f);
        return new AbstractSliderButton(0, 0, 310, 20, Component.empty(), u) {
            {
                this.updateMessage();
            }

            @Override
            protected void updateMessage() {
                this.setMessage(Component.literal(label + ": " + String.format(
                        java.util.Locale.ROOT, "%.2f", this.value)));
            }

            @Override
            protected void applyValue() {
                float v = (float) this.value;
                float[] next = RTXSettingsConfig.getCloudHpSettings().toParamArray();
                for(int i = 0; i < slots.length && i < anchors.length; ++i) {
                    float lo = anchors[i][0];
                    float hi = anchors[i][1];
                    float val = lo + (hi - lo) * v;
                    float[] range = RTXSettingsConfig.CLOUD_HP_RANGES[slots[i]];
                    next[slots[i]] = Math.min(Math.max(val, range[0]), range[1]);
                }
                pushHp(new RTXSettingsConfig.CloudHpSettings(next));
                this.updateMessage();
            }
        };
    }

    /** 一键预设下拉：选中即应用（存档 + native + flush + 重建）。 */
    private CycleButton<Integer> presetCycle(
            String label, String[] names, java.util.function.IntConsumer applier,
            int currentIndex
    ) {
        Integer[] values = new Integer[names.length];
        for(int i = 0; i < names.length; ++i) {
            values[i] = i;
        }
        return CycleButton.builder(
                        i -> Component.literal(names[i]),
                        currentIndex)
                .withValues(values)
                .create(Component.literal(label),
                        (button, i) -> applier.accept(i));
    }

    // —— 云风格（顾问 §4.2 A 表） ——

    private void applyCloudStylePreset(int i) {
        final float coverage;
        final float density;
        final float bottom;
        final float thickness;
        final float size;
        final float silver;
        final float sunsetWarmth;
        final float aerial;
        final float stormDarkness;
        final float gapStrength;
        final float uCarve;
        final float uWispy;
        final float uStrato;
        final float uPhase;
        switch(i) {
            case 1 -> { coverage = 0.14f; density = 2.40f; bottom = 2400f; thickness = 1400f;
                size = 0.65f; silver = 1.20f; sunsetWarmth = 1.10f; aerial = 1.20f;
                stormDarkness = 0.85f; gapStrength = 1.50f;
                uCarve = 0.35f; uWispy = 0.45f; uStrato = 0.25f; uPhase = 0.50f; }
            case 2 -> { coverage = 0.62f; density = 4.20f; bottom = 1200f; thickness = 1600f;
                size = 0.85f; silver = 0.60f; sunsetWarmth = 0.80f; aerial = 1.60f;
                stormDarkness = 1.70f; gapStrength = 0.80f;
                uCarve = 0.20f; uWispy = 0.30f; uStrato = 0.95f; uPhase = 0.25f; }
            case 3 -> { coverage = 0.38f; density = 3.20f; bottom = 1850f; thickness = 2000f;
                size = 0.55f; silver = 1.85f; sunsetWarmth = 2.00f; aerial = 1.80f;
                stormDarkness = 1.40f; gapStrength = 3.50f;
                uCarve = 0.85f; uWispy = 0.90f; uStrato = 0.70f; uPhase = 0.95f; }
            default -> { coverage = 0.20f; density = 3.65f; bottom = 2112f; thickness = 2208f;
                size = 0.50f; silver = 2.00f; sunsetWarmth = 2.00f; aerial = 2.00f;
                stormDarkness = 2.00f; gapStrength = 4.00f;
                uCarve = 1.00f; uWispy = 1.00f; uStrato = 1.00f; uPhase = 1.00f; }
        }

        RTXSettingsConfig.CloudSettings old = RTXSettingsConfig.getCloudSettings();
        RTXSettingsConfig.CloudSettings clouds = new RTXSettingsConfig.CloudSettings(
                old.enabled(), bottom, thickness, coverage, density,
                old.detailStrength(), old.windSpeed(), old.evolution(), old.quality(),
                size, old.distribution(), old.baseFeather(), old.sunlightStrength());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSettings(
                clouds.enabled(), clouds.bottom(), clouds.thickness(), clouds.coverage(),
                clouds.density(), clouds.detailStrength(), clouds.windSpeed(),
                clouds.evolution(), clouds.quality(), clouds.size(), clouds.distribution(),
                clouds.baseFeather(), clouds.sunlightStrength())) {
            RTXSettingsConfig.setCloudSettings(clouds);
        }

        pushHp(expandHpMacros(uCarve, uWispy, uStrato, uPhase));

        RTXSettingsConfig.CloudWeatherStyleSettings oldWeather =
                RTXSettingsConfig.getCloudWeatherStyleSettings();
        RTXSettingsConfig.CloudWeatherStyleSettings weather =
                new RTXSettingsConfig.CloudWeatherStyleSettings(
                        stormDarkness, oldWeather.lightningStrength(),
                        sunsetWarmth, oldWeather.weatherCoupling());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSkyHueParams(weather.toParamArray())) {
            RTXSettingsConfig.setCloudWeatherStyleSettings(weather);
        }

        RTXSettingsConfig.CloudAerialSettings oldAerial =
                RTXSettingsConfig.getCloudAerialSettings();
        RTXSettingsConfig.CloudAerialSettings aerialSettings =
                new RTXSettingsConfig.CloudAerialSettings(
                        aerial, oldAerial.groundAerialStrength(),
                        oldAerial.groundAerialOcclusion());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudAerialParams(aerialSettings.toParamArray())) {
            RTXSettingsConfig.setCloudAerialSettings(aerialSettings);
        }

        RTXSettingsConfig.CloudGapSettings oldGap = RTXSettingsConfig.getCloudGapSettings();
        RTXSettingsConfig.CloudGapSettings gap = new RTXSettingsConfig.CloudGapSettings(
                oldGap.enabled(), gapStrength, oldGap.width());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudGapParams(gap.toParamArray())) {
            RTXSettingsConfig.setCloudGapSettings(gap);
        }

        RTXSettingsConfig.setCloudSilver(Math.min(Math.max(0.5f * silver, 0.0f), 1.0f));
        if(NativeVulkan.isLoaded()) {
            NativeVulkan.setCloudSilver(Math.min(Math.max(0.5f * silver, 0.0f), 1.0f));
        }
        RTXSettingsConfig.CloudLight2Settings oldLight2 =
                RTXSettingsConfig.getCloudLight2Settings();
        RTXSettingsConfig.CloudLight2Settings light2 =
                new RTXSettingsConfig.CloudLight2Settings(
                        oldLight2.powderEffect(), silver,
                        oldLight2.sharpness(), oldLight2.baseErosion());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudLight2Params(light2.toParamArray())) {
            RTXSettingsConfig.setCloudLight2Settings(light2);
        }

        this.loadCloudSettings(RTXSettingsConfig.getCloudSettings());
        this.loadCloudWeatherStyleSettings(RTXSettingsConfig.getCloudWeatherStyleSettings());
        this.loadCloudAerialSettings(RTXSettingsConfig.getCloudAerialSettings());
        this.loadCloudLight2Settings(RTXSettingsConfig.getCloudLight2Settings());
        this.pendingCloudGapStrength = gap.strength();
        this.pendingCloudSilver = Math.min(Math.max(0.5f * silver, 0.0f), 1.0f);
        this.pendingCloudSilverSpread = silver;
        this.cloudStyleIndex = i;
        RTXSettingsConfig.flush();
        this.scheduleRebuild();
    }

    // —— 雾氛围（顾问 §4.2 B 表） ——

    private void applyFogStylePreset(int i) {
        final float density;
        final float height;
        final float scaleHeight;
        final float maxDistance;
        final float sunStrength;
        final float ambientStrength;
        final float shapeStrength;
        final float dawnBoost;
        final float godRayIntensity;
        final boolean groundMistEnabled;
        final float curlStrength;
        switch(i) {
            case 1 -> { density = 0.55f; height = 64f; scaleHeight = 40f; maxDistance = 768f;
                sunStrength = 0.80f; ambientStrength = 0.90f; shapeStrength = 0.60f;
                dawnBoost = 1.30f; godRayIntensity = 0.60f; groundMistEnabled = false;
                curlStrength = 0.30f; }
            case 2 -> { density = 3.40f; height = 68f; scaleHeight = 24f; maxDistance = 256f;
                sunStrength = 1.40f; ambientStrength = 2.40f; shapeStrength = 1.80f;
                dawnBoost = 2.20f; godRayIntensity = 1.20f; groundMistEnabled = true;
                curlStrength = 1.10f; }
            case 3 -> { density = 1.80f; height = 56f; scaleHeight = -32f; maxDistance = 448f;
                sunStrength = 3.20f; ambientStrength = 1.40f; shapeStrength = 1.60f;
                dawnBoost = 2.60f; godRayIntensity = 2.85f; groundMistEnabled = true;
                curlStrength = 0.60f; }
            default -> { density = 2.20f; height = 51f; scaleHeight = -64f; maxDistance = 384f;
                sunStrength = 2.00f; ambientStrength = 2.00f; shapeStrength = 2.00f;
                dawnBoost = 3.00f; godRayIntensity = 2.00f; groundMistEnabled = true;
                curlStrength = 0.70f; }
        }

        RTXSettingsConfig.VolumetricFogSettings oldFog =
                RTXSettingsConfig.getVolumetricFogSettings();
        RTXSettingsConfig.VolumetricFogSettings fog =
                new RTXSettingsConfig.VolumetricFogSettings(
                        oldFog.enabled(), density, height, scaleHeight, oldFog.anisotropy(),
                        sunStrength, ambientStrength, maxDistance,
                        oldFog.startDistance(), oldFog.sunSteps(), oldFog.rainBoost());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setVolumetricFogSettings(
                fog.enabled(), fog.density(), fog.height(), fog.scaleHeight(),
                fog.anisotropy(), fog.sunStrength(), fog.ambientStrength(),
                fog.maxDistance(), fog.startDistance(), fog.sunSteps(), fog.rainBoost())) {
            RTXSettingsConfig.setVolumetricFogSettings(fog);
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setFogMoodSettings(shapeStrength, dawnBoost)) {
            RTXSettingsConfig.setFogShapeStrength(shapeStrength);
            RTXSettingsConfig.setFogDawnBoost(dawnBoost);
        }

        RTXSettingsConfig.VolumetricLightSettings oldGodRay =
                RTXSettingsConfig.getVolumetricLightSettings();
        RTXSettingsConfig.VolumetricLightSettings godRay =
                new RTXSettingsConfig.VolumetricLightSettings(
                        oldGodRay.enabled(), oldGodRay.steps(),
                        oldGodRay.occlusionSamples(), godRayIntensity,
                        oldGodRay.reprojection());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setVolumetricLightSettings(
                godRay.enabled(), godRay.steps(), godRay.occlusionSamples(),
                godRay.intensity())) {
            RTXSettingsConfig.setVolumetricLightSettings(godRay);
            if(NativeVulkan.isLoaded()) {
                NativeVulkan.setGodRayReprojection(godRay.reprojection());
            }
        }

        RTXSettingsConfig.FogGroundMistSettings oldMist =
                RTXSettingsConfig.getFogGroundMistSettings();
        RTXSettingsConfig.FogGroundMistSettings mist =
                new RTXSettingsConfig.FogGroundMistSettings(
                        groundMistEnabled, oldMist.height(), oldMist.extinction(),
                        oldMist.distance(), oldMist.speed());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setFogGroundMistParams(mist.toParamArray())) {
            RTXSettingsConfig.setFogGroundMistSettings(mist);
        }

        RTXSettingsConfig.FogOpticsSettings oldOptics =
                RTXSettingsConfig.getFogOpticsSettings();
        RTXSettingsConfig.FogOpticsSettings optics = new RTXSettingsConfig.FogOpticsSettings(
                curlStrength, oldOptics.tintR(), oldOptics.tintG(), oldOptics.tintB());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setFogOpticsParams(optics.toParamArray())) {
            RTXSettingsConfig.setFogOpticsSettings(optics);
        }

        this.pendingFogDensity = fog.density();
        this.pendingFogHeight = fog.height();
        this.pendingFogScaleHeight = fog.scaleHeight();
        this.pendingFogSunStrength = fog.sunStrength();
        this.pendingFogAmbientStrength = fog.ambientStrength();
        this.pendingFogMaxDistance = fog.maxDistance();
        this.pendingFogShapeStrength = shapeStrength;
        this.pendingFogDawnBoost = dawnBoost;
        this.pendingGodRayIntensity = godRay.intensity();
        this.pendingFogGroundMistEnabled = mist.enabled();
        this.pendingFogCurlStrength = optics.curlStrength();
        this.fogStyleIndex = i;
        RTXSettingsConfig.flush();
        this.scheduleRebuild();
    }

    // —— 天气氛围（顾问 §3.3：5 lane = B × w，coupling 独立） ——

    private void applyWeatherMoodPreset(int i) {
        final float sky;
        final float fogBoost;
        final float storm;
        final float bolt;
        final float mist;
        final float coupling;
        switch(i) {
            case 1 -> { sky = 1.15f; fogBoost = 1.30f; storm = 1.00f; bolt = 0.40f;
                mist = 0.50f; coupling = 0.80f; }
            case 2 -> { sky = 0.65f; fogBoost = 2.60f; storm = 0.75f; bolt = 0.00f;
                mist = 0.85f; coupling = 0.70f; }
            default -> { sky = 2.00f; fogBoost = 2.00f; storm = 2.00f; bolt = 2.30f;
                mist = 1.20f; coupling = 0.95f; }
        }
        this.weatherMoodLastB = new float[] {sky, fogBoost, storm, bolt, mist};
        this.weatherMoodW = Math.max(this.weatherMoodW, 0.0f);
        pushWeatherMoodLanes(
                sky * this.weatherMoodW,
                fogBoost * this.weatherMoodW,
                storm * this.weatherMoodW,
                bolt * this.weatherMoodW,
                mist * this.weatherMoodW,
                coupling);
        this.weatherMoodIndex = i;
        RTXSettingsConfig.flush();
        this.scheduleRebuild();
    }

    private void pushWeatherMoodLanes(
            float skyDim, float fogBoost, float storm, float bolt, float mist, float coupling
    ) {
        if(!NativeVulkan.isLoaded() || NativeVulkan.setSkyModelSettings(
                RTXSettingsConfig.getSkyModel(), RTXSettingsConfig.getSkyModelScale(),
                RTXSettingsConfig.getNightSkyAmbientScale(), Math.min(Math.max(skyDim, 0f), 2f))) {
            RTXSettingsConfig.setRainSkyDimScale(Math.min(Math.max(skyDim, 0f), 2f));
        }

        RTXSettingsConfig.VolumetricFogSettings oldFog =
                RTXSettingsConfig.getVolumetricFogSettings();
        RTXSettingsConfig.VolumetricFogSettings fog =
                new RTXSettingsConfig.VolumetricFogSettings(
                        oldFog.enabled(), oldFog.density(), oldFog.height(),
                        oldFog.scaleHeight(), oldFog.anisotropy(), oldFog.sunStrength(),
                        oldFog.ambientStrength(), oldFog.maxDistance(),
                        oldFog.startDistance(), oldFog.sunSteps(),
                        Math.min(Math.max(fogBoost, 0f), 8f));
        if(!NativeVulkan.isLoaded() || NativeVulkan.setVolumetricFogSettings(
                fog.enabled(), fog.density(), fog.height(), fog.scaleHeight(),
                fog.anisotropy(), fog.sunStrength(), fog.ambientStrength(),
                fog.maxDistance(), fog.startDistance(), fog.sunSteps(), fog.rainBoost())) {
            RTXSettingsConfig.setVolumetricFogSettings(fog);
        }

        RTXSettingsConfig.CloudWeatherStyleSettings oldWeather =
                RTXSettingsConfig.getCloudWeatherStyleSettings();
        RTXSettingsConfig.CloudWeatherStyleSettings weather =
                new RTXSettingsConfig.CloudWeatherStyleSettings(
                        Math.min(Math.max(storm, 0f), 2f), Math.min(Math.max(bolt, 0f), 3f),
                        oldWeather.sunsetWarmth(), Math.min(Math.max(coupling, 0f), 2f));
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudSkyHueParams(weather.toParamArray())) {
            RTXSettingsConfig.setCloudWeatherStyleSettings(weather);
        }

        RTXSettingsConfig.FogGroundMistSettings oldMist =
                RTXSettingsConfig.getFogGroundMistSettings();
        RTXSettingsConfig.FogGroundMistSettings mistSettings =
                new RTXSettingsConfig.FogGroundMistSettings(
                        oldMist.enabled(), oldMist.height(),
                        Math.min(Math.max(mist, 0f), 2f),
                        oldMist.distance(), oldMist.speed());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setFogGroundMistParams(mistSettings.toParamArray())) {
            RTXSettingsConfig.setFogGroundMistSettings(mistSettings);
        }

        this.pendingRainSkyDimScale = Math.min(Math.max(skyDim, 0f), 2f);
        this.pendingFogRainBoost = fog.rainBoost();
        this.loadCloudWeatherStyleSettings(weather);
        this.pendingFogGroundMistExtinction = mistSettings.extinction();
    }

    // —— 水面风格（顾问 §4.2 C 表；只动光学与 FFT 权重类旋钮，外观模型不动） ——

    private void applyWaterStylePreset(int i) {
        final float tintMix;
        final float densityScale;
        final float waveStrength;
        final float crestBacklit;
        final float dispersion;
        final float depthDiffusion;
        final float causticStrength;
        final float sssStrength;
        final float sssBoost;
        final float fftAmplitude;
        final float fftChoppiness;
        final float fftDetail;
        final float fftSwellWeight;
        final float fftDetailWeight;
        final float fftWindSpeed;
        switch(i) {
            case 1 -> { tintMix = 1.00f; densityScale = 0.38f; waveStrength = 0.65f;
                crestBacklit = 1.80f; dispersion = 0.65f; depthDiffusion = 0.45f;
                causticStrength = 1.85f; sssStrength = 1.40f; sssBoost = 2.40f;
                fftAmplitude = 0.45f; fftChoppiness = 1.10f; fftDetail = 0.20f;
                fftSwellWeight = 0.70f; fftDetailWeight = 1.80f; fftWindSpeed = 2.20f; }
            case 2 -> { tintMix = 0.85f; densityScale = 1.45f; waveStrength = 1.55f;
                crestBacklit = 1.40f; dispersion = 0.30f; depthDiffusion = 1.00f;
                causticStrength = 0.65f; sssStrength = 0.70f; sssBoost = 1.40f;
                fftAmplitude = 1.45f; fftChoppiness = 2.40f; fftDetail = 0.35f;
                fftSwellWeight = 1.60f; fftDetailWeight = 3.50f; fftWindSpeed = 6.50f; }
            case 3 -> { tintMix = 0.90f; densityScale = 0.80f; waveStrength = 0.35f;
                crestBacklit = 0.60f; dispersion = 0.45f; depthDiffusion = 0.75f;
                causticStrength = 0.90f; sssStrength = 0.60f; sssBoost = 1.20f;
                fftAmplitude = 0.25f; fftChoppiness = 0.60f; fftDetail = 0.00f;
                fftSwellWeight = 0.40f; fftDetailWeight = 0.90f; fftWindSpeed = 1.40f; }
            default -> { tintMix = 1.00f; densityScale = 0.65f; waveStrength = 1.00f;
                crestBacklit = 2.00f; dispersion = 0.54f; depthDiffusion = 1.00f;
                causticStrength = 1.10f; sssStrength = 1.00f; sssBoost = 2.00f;
                fftAmplitude = 0.85f; fftChoppiness = 1.60f; fftDetail = 0.00f;
                fftSwellWeight = 1.00f; fftDetailWeight = 3.00f; fftWindSpeed = 3.50f; }
        }

        RTXSettingsConfig.WaterStyleSettings oldStyle =
                RTXSettingsConfig.getWaterStyleSettings();
        RTXSettingsConfig.WaterStyleSettings style = new RTXSettingsConfig.WaterStyleSettings(
                oldStyle.waveScale(), tintMix, densityScale, causticStrength,
                oldStyle.waveSpeed(), oldStyle.swellScale(), oldStyle.midScale(),
                oldStyle.fineScale());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterStyleSettings(
                style.waveScale(), style.tintMix(), style.densityScale(),
                style.causticStrength(), style.waveSpeed(), style.swellScale(),
                style.midScale(), style.fineScale())) {
            RTXSettingsConfig.setWaterStyleSettings(style);
        }

        RTXSettingsConfig.WaterWaveSettings oldWave = RTXSettingsConfig.getWaterWaveSettings();
        RTXSettingsConfig.WaterWaveSettings wave = new RTXSettingsConfig.WaterWaveSettings(
                oldWave.calibration(), waveStrength, oldWave.blend(),
                oldWave.reflectCausticStrength());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterWaveParams(wave.toParamArray())) {
            RTXSettingsConfig.setWaterWaveSettings(wave);
        }

        RTXSettingsConfig.WaterParamsSettings oldParams =
                RTXSettingsConfig.getWaterParamsSettings();
        RTXSettingsConfig.WaterParamsSettings params = new RTXSettingsConfig.WaterParamsSettings(
                crestBacklit, dispersion, depthDiffusion, oldParams.physicalPhase());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterParams(params.toParamArray())) {
            RTXSettingsConfig.setWaterParamsSettings(params);
        }

        RTXSettingsConfig.WaterSssSettings oldSss = RTXSettingsConfig.getWaterSssSettings();
        RTXSettingsConfig.WaterSssSettings sss = new RTXSettingsConfig.WaterSssSettings(
                oldSss.nonlinear(), sssBoost, oldSss.phaseG(), sssStrength);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterSssParams(sss.toParamArray())) {
            RTXSettingsConfig.setWaterSssSettings(sss);
        }

        RTXSettingsConfig.WaterFftSettings oldFft = RTXSettingsConfig.getWaterFftSettings();
        RTXSettingsConfig.WaterFftSettings fft = new RTXSettingsConfig.WaterFftSettings(
                oldFft.enabled(), fftAmplitude, fftChoppiness, oldFft.tilePeriod(),
                fftDetail, fftSwellWeight, fftDetailWeight, fftWindSpeed);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setWaterFftParams(fft.toParamArray())) {
            RTXSettingsConfig.setWaterFftSettings(fft);
        }

        this.loadWaterStylePendings(style);
        this.loadWaterWaveSettings(wave);
        this.loadWaterParamsSettings(params);
        this.loadWaterSssSettings(sss);
        this.loadWaterFftSettings(fft);
        this.waterStyleIndex = i;
        RTXSettingsConfig.flush();
        this.scheduleRebuild();
    }

    // —— 光照风格（顾问 §4.2 D 表） ——

    private void applyLightStylePreset(int i) {
        final float scatter;
        final float ozone;
        final float groundAerial;
        final float sunDisk;
        final float sunAngle;
        final float noDirect;
        final float localLightBrightness;
        final float localLightRange;
        final float star;
        final float aurora;
        final float nightFloor;
        switch(i) {
            case 1 -> { scatter = 0.65f; ozone = 0.90f; groundAerial = 0.90f;
                sunDisk = 1.20f; sunAngle = 0.53f; noDirect = 0.0120f;
                localLightBrightness = 3.00f; localLightRange = 1.00f;
                star = 1.20f; aurora = 0.25f; nightFloor = 0.80f; }
            case 2 -> { scatter = 0.75f; ozone = 0.70f; groundAerial = 1.10f;
                sunDisk = 2.20f; sunAngle = 0.85f; noDirect = 0.0180f;
                localLightBrightness = 3.50f; localLightRange = 1.15f;
                star = 1.00f; aurora = 0.00f; nightFloor = 0.90f; }
            case 3 -> { scatter = 0.40f; ozone = 1.80f; groundAerial = 0.80f;
                sunDisk = 1.20f; sunAngle = 0.53f; noDirect = 0.0060f;
                localLightBrightness = 4.00f; localLightRange = 1.25f;
                star = 2.00f; aurora = 1.60f; nightFloor = 0.65f; }
            default -> { scatter = 0.50f; ozone = 1.50f; groundAerial = 2.00f;
                sunDisk = 1.92f; sunAngle = 0.71f; noDirect = 0.0219f;
                localLightBrightness = 4.00f; localLightRange = 1.00f;
                star = 2.00f; aurora = 0.50f; nightFloor = 1.00f; }
        }

        RTXSettingsConfig.AtmosphereSettings oldAtmo =
                RTXSettingsConfig.getAtmosphereSettings();
        RTXSettingsConfig.AtmosphereSettings atmo = new RTXSettingsConfig.AtmosphereSettings(
                star, oldAtmo.sunTintStrength(), oldAtmo.physicalSkyEnabled(),
                scatter, ozone);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setAtmosphereSettings(
                atmo.starIntensity(), atmo.sunTintStrength(), atmo.physicalSkyEnabled(),
                atmo.scatterIntensity(), atmo.ozoneStrength())) {
            RTXSettingsConfig.setAtmosphereSettings(atmo);
        }

        RTXSettingsConfig.CloudAerialSettings oldAerial =
                RTXSettingsConfig.getCloudAerialSettings();
        RTXSettingsConfig.CloudAerialSettings aerial = new RTXSettingsConfig.CloudAerialSettings(
                oldAerial.cloudAerialStrength(), groundAerial,
                oldAerial.groundAerialOcclusion());
        if(!NativeVulkan.isLoaded() || NativeVulkan.setCloudAerialParams(aerial.toParamArray())) {
            RTXSettingsConfig.setCloudAerialSettings(aerial);
        }

        RTXSettingsConfig.SunVisualSettings oldSun = RTXSettingsConfig.getSunVisualSettings();
        RTXSettingsConfig.SunVisualSettings sun = new RTXSettingsConfig.SunVisualSettings(
                sunDisk, oldSun.haloStrength(), sunAngle);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setSunVisualSettings(
                sun.diskLuminance(), sun.haloStrength(), sun.angularDiameterDegrees())) {
            RTXSettingsConfig.setSunVisualSettings(sun);
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setNoDirectLightAmbient(noDirect)) {
            RTXSettingsConfig.setNoDirectLightAmbient(noDirect);
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setLocalLightIntensity(localLightBrightness)) {
            RTXSettingsConfig.setLocalLightIntensity(localLightBrightness);
        }
        if(!NativeVulkan.isLoaded() || NativeVulkan.setLocalLightMaxDistance(localLightRange)) {
            RTXSettingsConfig.setLocalLightMaxDistance(localLightRange);
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setAuroraBrightness(aurora)) {
            RTXSettingsConfig.setAuroraBrightness(aurora);
        }

        if(!NativeVulkan.isLoaded() || NativeVulkan.setSkyModelSettings(
                RTXSettingsConfig.getSkyModel(), RTXSettingsConfig.getSkyModelScale(),
                nightFloor, RTXSettingsConfig.getRainSkyDimScale())) {
            RTXSettingsConfig.setNightSkyAmbientScale(nightFloor);
        }

        this.pendingScatterIntensity = atmo.scatterIntensity();
        this.pendingOzoneStrength = atmo.ozoneStrength();
        this.pendingStarIntensity = atmo.starIntensity();
        this.pendingSunDiskLuminance = sun.diskLuminance();
        this.pendingSunAngularDiameterDegrees = sun.angularDiameterDegrees();
        this.pendingNoDirectLightAmbient = noDirect;
        this.pendingLocalLightIntensity = localLightBrightness;
        this.pendingLocalLightMaxDistance = localLightRange;
        this.pendingAuroraBrightness = aurora;
        this.pendingNightSkyAmbientScale = nightFloor;
        this.groundAerialStrength = groundAerial;
        this.lightStyleIndex = i;
        RTXSettingsConfig.flush();
        this.scheduleRebuild();
    }

    // —— 卷云（顾问 §2.2） ——

    private void applyCirrusStylePreset(int i) {
        final boolean enabled;
        final float coverage;
        final float brightness;
        final float heightOffset;
        final float scale;
        final float speed;
        switch(i) {
            case 1 -> { enabled = false; coverage = 0.74f; brightness = 1.35f;
                heightOffset = 6500f; scale = 1.40f; speed = 1.00f; }
            case 2 -> { enabled = true; coverage = 0.35f; brightness = 1.10f;
                heightOffset = 6500f; scale = 1.68f; speed = 0.80f; }
            case 3 -> { enabled = true; coverage = 0.92f; brightness = 1.55f;
                heightOffset = 5850f; scale = 1.05f; speed = 1.40f; }
            default -> { enabled = true; coverage = 0.74f; brightness = 1.35f;
                heightOffset = 6500f; scale = 1.40f; speed = 1.00f; }
        }

        RTXSettingsConfig.PlaneCloudSettings old = RTXSettingsConfig.getPlaneCloudSettings();
        RTXSettingsConfig.PlaneCloudSettings cirrus =
                new RTXSettingsConfig.PlaneCloudSettings(
                        enabled, heightOffset, scale, speed, old.density(),
                        coverage, old.octaves(), old.fadeDistance(), brightness);
        if(!NativeVulkan.isLoaded() || NativeVulkan.setPlaneCloudParams(cirrus.toParamArray())) {
            RTXSettingsConfig.setPlaneCloudSettings(cirrus);
        }

        this.pendingPlaneCloudEnabled = cirrus.enabled();
        this.pendingPlaneCloudCoverage = cirrus.coverage();
        this.pendingPlaneCloudBrightness = cirrus.brightness();
        this.pendingPlaneCloudHeightOffset = cirrus.heightOffset();
        this.pendingPlaneCloudScale = cirrus.scale();
        this.pendingPlaneCloudSpeed = cirrus.speed();
        this.cirrusStyleIndex = i;
        RTXSettingsConfig.flush();
        this.scheduleRebuild();
    }

    private void loadWaterStylePendings(RTXSettingsConfig.WaterStyleSettings style) {
        this.pendingWaterStyleTintMix = style.tintMix();
        this.pendingWaterStyleDensityScale = style.densityScale();
        this.pendingWaterStyleCausticStrength = style.causticStrength();
        this.pendingWaterStyleWaveScale = style.waveScale();
    }

    private void applyColorGradePreset(int preset) {
        float[] grade = RTXSettingsConfig.DEFAULT_COLOR_GRADE.clone();

        if(preset == PRESET_ICELAND) {
            grade = GRADE_ICELAND.clone();
        } else if(preset == PRESET_GREENLAND) {
            grade = GRADE_GREENLAND.clone();
        } else if(preset == PRESET_MIST) {
            grade = GRADE_MIST.clone();
        }

        RTXSettingsConfig.ColorGradeSettings updated =
                new RTXSettingsConfig.ColorGradeSettings(grade);
        if(!NativeVulkan.isLoaded()
                || NativeVulkan.setColorGradeParams(updated.toParamArray())) {
            RTXSettingsConfig.setColorGradeSettings(updated);
        }

        this.scheduleRebuild();
    }

    private static Component postTonemapLabel(int mode) {

        switch (mode) {
            case 1: return Component.literal("AgX");
            case 2: return Component.literal("PBR 中性");
            case 3: return Component.literal("ACES");
            case 4: return Component.literal("Uncharted 2");
            case 5: return Component.literal("Reinhard");
            case 6: return Component.literal("ACES 2.0");
            default: return Component.literal("Uchimura / GT");
        }
    }

    private void loadPostProcessingSettings(RTXSettingsConfig.PostProcessingSettings post) {
        this.postTonemapMode = post.tonemapMode();
        this.postExposureStrength = post.exposureStrength();
        this.postContrast = post.contrast();
        this.postSaturation = post.saturation();
        this.postGamma = post.gamma();
        this.postMirrorBounceCount = post.mirrorBounceCount();
        this.postTlasCullDistance = post.tlasCullDistance();
        this.postTlasFrustumCullMargin = post.tlasFrustumCullMargin();
        this.postRrFarCloudDistance = post.rrFarCloudDistance();
        this.postBloomStrength = RTXSettingsConfig.getPostBloomStrength();
        this.postGainR = RTXSettingsConfig.getPostGainR();
        this.postGainG = RTXSettingsConfig.getPostGainG();
        this.postGainB = RTXSettingsConfig.getPostGainB();
    }

    private void loadWaterWaveShapeSettings(RTXSettingsConfig.WaterWaveShapeSettings shape) {
        this.waterWaveLengthScale = shape.lengthScale();
        this.waterWaveDirection = shape.directionDegrees();
        this.waterWaveCrestSharpness = shape.crestSharpness();
        this.waterWaveWarpStrength = shape.warpStrength();
    }

    private void loadWaterWaveAflSettings(RTXSettingsConfig.WaterWaveAflSettings afl) {
        this.waterWaveAflScale = afl.wavelength();
        this.waterWaveAflLayers = afl.octaves();
    }

    private void loadWaterSssSettings(RTXSettingsConfig.WaterSssSettings sss) {
        this.waterSssNonlinear = sss.nonlinear();
        this.waterSssBoost = sss.boost();
        this.waterSssPhaseG = sss.phaseG();
        this.waterSssStrength = sss.strength();
    }

    /**
     * 「紧凑预览」开关。开启后：世界继续模拟（isPauseScreen=false）+ 设置列表
     * 压到屏幕底部条带 + 不再绘制原版全屏背景 ⇒ 调滑条时能直接看到画面变化，
     * 类似多数游戏的画面设置界面。
     */
    /**
     * 折叠开关的通用实现：翻转静态标志、改按钮文案、整页重建。
     * 三处折叠（实验闸 / 云 HP / 精细分级）共用，避免三份同样的样板。
     *
     * 这些标志是**界面级**静态字段（和 compactPreview 一样），不进存档：
     * 它们只决定"这一屏显示哪些行"，不改变任何渲染参数。
     */
    private AbstractWidget foldToggleButton(
            String title,
            boolean expanded,
            Runnable flip
    ) {
        final String state = expanded ? "已展开（按此收起）" : "已收起（按此展开）";

        return Button.builder(
                        Component.literal(title + "：" + state),
                        button -> {
                            flip.run();
                            this.scheduleRebuild();
                        })
                .bounds(0, 0, 310, 20)
                .build();
    }

    /**
     * 重新构建整页 —— 必须走 {@link #rebuildPage()}，不能裸调
     * {@code rebuildWidgets()}，而且必须延到下一 tick。
     *
     * <p>原因（2026-11-16 修「折叠展开后滚轮划不动」）：</p>
     * <ul>
     *   <li>{@code OptionsSubScreen.layout} 是 {@code final} 字段，
     *       {@code rebuildWidgets()} 只清屏幕控件、不清 layout。于是
     *       {@code init()} 会往同一个 layout 里再 {@code addTitle()} /
     *       {@code addContents()} / {@code addFooter()} 一遍，屏幕上叠出
     *       **两份列表**。</li>
     *   <li>旧列表在下、内容少、{@code maxScrollAmount()==0}，但它先注册，
     *       而 {@code AbstractScrollArea.mouseScrolled()} 只要
     *       {@code visible} 就返回 true —— 滚轮事件被旧列表吃掉，
     *       新列表永远收不到，表现就是「展开后滚轮/滚动条都划不动」。</li>
     *   <li>顺带：旧列表的控件位置与新列表完全重合且先参与命中测试，
     *       点击会落到旧控件上。所以除了滚动，其它交互也会逐渐错位。</li>
     * </ul>
     *
     * <p>{@code rebuildPage()} 会先 {@code layout.removeChildren()}，
     * 这样每次重建都只有一份标题/列表/页脚。</p>
     */
    private void scheduleRebuild() {
        if(this.minecraft != null) {
            this.minecraft.execute(this::rebuildPage);
        } else {
            this.rebuildPage();
        }
    }

    private AbstractWidget advancedGatesToggle() {
        return foldToggleButton("高级 / 实验选项", showAdvancedGates,
                () -> showAdvancedGates = !showAdvancedGates);
    }

    private AbstractWidget cloudHpToggleButton() {
        return foldToggleButton("HP 全参数", cloudHpExpanded,
                () -> cloudHpExpanded = !cloudHpExpanded);
    }

    private AbstractWidget colorGradeToggleButton() {
        return foldToggleButton("精细分级参数（24 项 + 预设）", colorGradeExpanded,
                () -> colorGradeExpanded = !colorGradeExpanded);
    }

    // 2026-11-21 精简批：六个合并页的「高级选项」折叠按钮。
    private AbstractWidget upscalerAdvancedToggle() {
        return foldToggleButton("高级选项（性能采样 / 剔除 / FG 细项）", showUpscalerAdvanced,
                () -> showUpscalerAdvanced = !showUpscalerAdvanced);
    }

    private AbstractWidget skyAdvancedToggle() {
        return foldToggleButton("高级选项（月光 / 天气压暗 / 日冕 / 云内月光）", showSkyAdvanced,
                () -> showSkyAdvanced = !showSkyAdvanced);
    }

    // 2026-11-22 统一风场折叠组（风向 / 风强 / 阵风 → 云雾水波植物）
    private AbstractWidget windAdvancedToggle() {
        return foldToggleButton("风场（风向 / 风强 / 阵风 → 云雾水波）", showWindAdvanced,
                () -> showWindAdvanced = !showWindAdvanced);
    }

    private AbstractWidget cloudAdvancedToggle() {
        return foldToggleButton("高级选项（软化 / 步进 / 受光 / UE 八项 / 时域）", showCloudAdvanced,
                () -> showCloudAdvanced = !showCloudAdvanced);
    }

    private AbstractWidget fogAdvancedToggle() {
        return foldToggleButton("高级选项（雾高度 / 平流雾 / 雾色 / 雨线 / 雪 / 墙面）", showFogAdvanced,
                () -> showFogAdvanced = !showFogAdvanced);
    }

    private AbstractWidget waterAdvancedToggle() {
        return foldToggleButton("高级选项（FFT / 交互波 / SSS / 材质细项）", showWaterAdvanced,
                () -> showWaterAdvanced = !showWaterAdvanced);
    }

    private AbstractWidget postAdvancedToggle() {
        return foldToggleButton("高级选项（曝光强度 / 通道增益 / 泛光）", showPostAdvanced,
                () -> showPostAdvanced = !showPostAdvanced);
    }

    private AbstractWidget compactToggleButton() {
        return Button.builder(
                        Component.literal(compactPreview ? "紧凑预览：开（按此关闭）" : "紧凑预览：关（按此开启）"),
                        button -> {
                            compactPreview = !compactPreview;
                            button.setMessage(Component.literal(
                                    compactPreview ? "紧凑预览：开（按此关闭）" : "紧凑预览：关（按此开启）"));
                            this.scheduleRebuild();
                        })
                .bounds(0, 0, 150, 20)
                .build();
    }

    /**
     * 把设置列表压到底部条带。
     *
     * 用反射而不是直接调用：本机没有 fabric-loom 的 mapped jar 可以核对
     * {@code AbstractSelectionList} 在这个版本里的方法名/签名。反射在方法缺失时
     * 只会静默失败（界面退回整屏列表），不会让编译或运行崩掉。
     */
    private void applyCompactListBounds() {
        if(this.list == null) {
            return;
        }

        if(!compactPreview) {
            return;
        }

        final int bottom = this.height - 28;
        final int stripHeight = Math.max(60, Math.min(COMPACT_STRIP_HEIGHT, this.height - 80));
        final int top = bottom - stripHeight;

        try {
            // 本版本签名（26.3 客户端 jar 反编译核对）：
            //   updateSizeAndPosition(int width, int height, int y)
            //      -> updateSizeAndPosition(width, height, 0, y)
            //      -> setSize(width, height); setPosition(0, y)
            // 旧代码找的是 4 参重载并传 (width, stripHeight, top, bottom)，
            // 但 4 参是 (width, height, x, y)，于是列表被摆到
            // (x=top, y=bottom) —— 偏到右下角且压到屏幕外。改为 3 参：
            // 高度 = 条带高度，y = 条带顶部。
            this.list.getClass()
                    .getMethod("updateSizeAndPosition", int.class, int.class, int.class)
                    .invoke(this.list, this.width, stripHeight, top);
            return;
        } catch (Throwable ignored) {
            // 继续尝试下一种签名
        }

        try {
            this.list.getClass()
                    .getMethod("repositionEntries")
                    .invoke(this.list);
        } catch (Throwable ignored) {
            // 该版本没有可用接口 ⇒ 保持原布局（不影响其它功能）
        }
    }

    @Override
    protected void init() {
        super.init();
        applyCompactListBounds();
    }

    /**
     * 2026-11-13：紧凑预览时**不暂停世界**，这样滑条的效果能立刻看到。
     * 普通整屏模式下保持原版行为（暂停）。
     */
    @Override
    public boolean isPauseScreen() {
        return !compactPreview;
    }

    /**
     * 紧凑模式下不画原版的整屏背景（模糊/泥土纹理），只给底部条带铺一层
     * 半透明底，其余整幅画面留给世界 —— 否则"腾出画面看效果"就无从谈起。
     * 普通模式原样交给 super。
     */
    @Override
    public void extractBackground(GuiGraphicsExtractor guiGraphics, int mouseX, int mouseY, float partialTick) {
        if(!compactPreview) {
            super.extractBackground(guiGraphics, mouseX, mouseY, partialTick);
            return;
        }

        final int bottom = this.height - 28;
        final int stripHeight = Math.max(60, Math.min(COMPACT_STRIP_HEIGHT, this.height - 80));
        final int top = Math.max(0, bottom - stripHeight);

        guiGraphics.fill(0, top - 6, this.width, this.height, 0x99000000);
    }

    private void addOptionRow(AbstractWidget widget) {
        if(this.section != this.page) {
            return;
        }
        widget.setWidth(150);
        this.optionRow.add(widget);
        if(this.optionRow.size() == 2) {
            this.list.addSmall(this.optionRow.get(0), this.optionRow.get(1));
            this.optionRow.clear();
        }
    }

    private void flushOptionRow() {
        if(!this.optionRow.isEmpty()) {
            this.list.addBig(this.optionRow.get(0));
            this.optionRow.clear();
        }
    }

    private void addSectionHeader(String title) {
        if(this.section != this.page) {
            return;
        }
        this.flushOptionRow();
        Button header = Button.builder(Component.literal(title), button -> {})
                .bounds(0, 0, 310, 20)
                .build();
        header.active = false;
        this.list.addBig(header);
    }

    private AbstractSliderButton cloudSlider(
            String label,
            double minimum,
            double maximum,
            double increment,
            float current,
            String format,
            DoubleConsumer setter
    ) {
        return cloudSliderWithZero(
                label, minimum, maximum, increment, current, format, null, setter);
    }

    // 2026-11-21 精简批（顾问 §3.2 0 值契约）：最左 0 槽不显示裸数字，
    // 改停靠语义文字（自动 / 关 / 跟随质量档）。仅改显示，存档与下发不变。
    private AbstractSliderButton cloudSliderWithZero(
            String label,
            double minimum,
            double maximum,
            double increment,
            float current,
            String format,
            String zeroText,
            DoubleConsumer setter
    ) {
        return new AbstractSliderButton(
                0,
                0,
                310,
                20,
                Component.empty(),
                (current - minimum) / (maximum - minimum)
        ) {
            {
                this.updateMessage();
            }

            private double snapped() {
                return Math.round((minimum + this.value * (maximum - minimum)) / increment) * increment;
            }

            @Override
            protected void updateMessage() {
                final double shown = this.snapped();
                this.setMessage(Component.literal(label + ": " + (
                        shown < 0.0
                                ? "内置默认"
                                : zeroText != null && shown <= 0.0
                                        ? zeroText
                                        : String.format(
                                                java.util.Locale.ROOT, format, shown))));
            }

            @Override
            protected void applyValue() {
                double value = Math.clamp(this.snapped(), minimum, maximum);
                this.value = (value - minimum) / (maximum - minimum);
                setter.accept(value);
                this.updateMessage();
            }
        };
    }

    @Override
    protected void addFooter() {

        LinearLayout footer =
                LinearLayout.horizontal()
                        .spacing(
                                8
                        );


        if(this.page == PAGE_HUB) {
            footer.addChild(
                    Button.builder(
                                    Component.literal(
                                            "确认"
                                    ),
                                    button ->
                                            this.applyPendingSettingsSafe()
                            )
                            .width(
                                    150
                            )
                            .build()
            );


            footer.addChild(
                    Button.builder(
                                    Component.literal(
                                            "返回"
                                    ),
                                    button ->
                                            this.onClose()
                            )
                            .width(
                                    150
                            )
                            .build()
            );
        } else {
            footer.addChild(
                    Button.builder(
                                    Component.literal(
                                            "应用"
                                    ),
                                    button ->
                                            this.applyPendingSettingsSafe()
                            )
                            .width(
                                    150
                            )
                            .build()
            );


            footer.addChild(
                    Button.builder(
                                    Component.literal(
                                            "返回"
                                    ),
                                    button ->
                                            this.openPage(this.parentPage())
                            )
                            .width(
                                    150
                            )
                            .build()
            );
        }


        this.layout.addToFooter(
                footer
        );
    }


    private int parentPage() {
        // 2026-11-16：色彩分级已并回后处理页，所有子页的返回目标都是主页。
        return PAGE_HUB;
    }


    private void openPage(int target) {
        this.page = target;
        this.scheduleRebuild();
    }


    private void rebuildPage() {
        this.layout.removeChildren();
        this.rebuildWidgets();
    }


    private void addHubButtons() {
        // 2026-11-15：主页最上面放「性能模式」一键开关。
        this.addOptionRow(this.performanceModeButton());
        // 2026-11-21 精简批（顾问 §4.1）：11 页合并为 6 页。
        //   性能与超分 ← 超分与帧生成 + 光追性能 + 性能与剔除
        //   天空与天体 ← 天空与大气 + 天体与夜光
        //   体积云     ← 不变（云隙光从雾页归位到此）
        //   雾与天气   ← 体积雾与体积光 + 降雨
        //   水面与材质 ← 水面 + 材质与画面
        //   后处理     ← 不变
        this.addOptionRow(this.hubButton("性能与超分", PAGE_UPSCALER));
        this.addOptionRow(this.hubButton("天空与天体", PAGE_SKY));
        this.addOptionRow(this.hubButton("体积云", PAGE_CLOUD));
        this.addOptionRow(this.hubButton("雾与天气", PAGE_FOG));
        this.addOptionRow(this.hubButton("水面与材质", PAGE_WATER));
        this.addOptionRow(this.hubButton("后处理", PAGE_POST));
        this.flushOptionRow();
    }


    private AbstractWidget hubButton(String label, int target) {
        return Button.builder(
                Component.literal(label),
                button -> this.openPage(target)
        ).width(150).build();
    }


    /**
     * 2026-11-15「性能模式」一键开关。
     *
     * <p>语义：主光追只到 6 区块以内，6~8 区块交给 SSPT，8 区块以外交给 SSR。
     * 落地为四项联动：</p>
     * <ul>
     *   <li>ssptRange = 8（SSPT 管到 8 区块）</li>
     *   <li>ssrRange = 48（SSR 射程上限）</li>
     *   <li>rtGlossyEnabled = false（RT 不再跑 GGX 反射射线）</li>
     *   <li>ssHybridFloor = 8（SSR 的区块下限从 12 降到 8）</li>
     * </ul>
     * 关闭时四项回默认（SSPT/SSR 关、RT 光泽射线开、下限 12）。
     */
    private AbstractWidget performanceModeButton() {
        return Button.builder(
                        Component.literal(
                                RTXSettingsConfig.isPerformanceMode()
                                        ? "性能模式：开（按此关闭）"
                                        : "性能模式：关（按此开启）"),
                        button -> {
                            final boolean next =
                                    !RTXSettingsConfig.isPerformanceMode();

                            RTXSettingsConfig.setPerformanceMode(next);

                            if(NativeVulkan.isLoaded()) {
                                final boolean ok =
                                        NativeVulkan.setSSPTRange(
                                                RTXSettingsConfig.getSsptRange())
                                        && NativeVulkan.setSSRRange(
                                                RTXSettingsConfig.getSsrRange())
                                        && NativeVulkan.setRTGlossyEnabled(
                                                RTXSettingsConfig.isRTGlossyEnabled())
                                        && NativeVulkan.setSsHybridFloor(
                                                next
                                                        ? RTXSettingsConfig.PERF_MODE_SSR_FLOOR_CHUNKS
                                                        : 12.0f)
                                        && NativeVulkan.setPerformanceMode(next);

                                if(!ok) {
                                    System.err.println(
                                            "[RTX] performance mode: native apply failed");
                                }
                            }

                            // 2026-11-21 持久化修复：这个一键开关直接改配置，
                            // 此前不 flush ⇒ 本次会话有效、重启回旧档（除非之后
                            // 在设置页点过「确认」把 dirty 一起 flush）。补落盘。
                            RTXSettingsConfig.flush();

                            button.setMessage(Component.literal(
                                    RTXSettingsConfig.isPerformanceMode()
                                            ? "性能模式：开（按此关闭）"
                                            : "性能模式：关（按此开启）"));

                            this.scheduleRebuild();
                        })
                .bounds(0, 0, 150, 20)
                .build();
    }


}
