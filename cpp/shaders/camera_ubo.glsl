
    mat4 viewInverse;
    mat4 projInverse;
    uvec2 vertexBufferAddr;
    uint frameCount;
    uint padding;

    vec4 cameraParams;

    uint historyReadIndex;
    uint historyValid;
    uint historyPadding0;
    uint historyPadding1;

    uvec4 perfPrimary;
    uvec4 perfSecondary;

    mat4 prevViewInverse;
    mat4 prevProjInverse;

    vec4 historyParams;

    vec4 playerPositionAndWater;
    vec4 playerVelocityAndSpeed;

    vec4 waterSimSurfaceAndOrigin;
    uvec4 waterSimState;
    vec4 waterSimImpulse;

    vec4 weatherParams;

    vec4 cloudParams0;
    vec4 cloudParams1;
    vec4 cloudParams2;
    vec4 cloudParams3;
    vec4 cloudParams4;
    vec4 cloudParams5;
    vec4 cloudParams6;
    vec4 cloudParams7;
    vec4 cloudParams8;
    vec4 cloudParams9;
    vec4 skyParams0;
    vec4 skyParams1;

    // godRayParams1 = 贴地薄雾四车道（2026-11-04 复活，消费方 volumetric_fog.glsl）：
    // x=总开关 y=高度标尺(×6=e 折叠格数) z=消光(×0.05=每格) w=距相机淡出半径(格)
    vec4 godRayParams0;
    vec4 godRayParams1;
    vec4 godRayParams2;

    vec4 rainWallParams0;
    vec4 rainWallParams1;
    vec4 waterStyleParams0;
    vec4 waterStyleParams1;

    uvec4 perfSerAb;

    vec4 dhFarFieldParams;
    mat4 dhFarFieldInvMvp;
    vec4 dhFarFieldLight;
    mat4 dhFarFieldMvp;

    vec4 sunShadowFarParams;

    vec4 highCloudParams0;
    vec4 highCloudParams1;
    vec4 highCloudParams2;
    vec4 highCloudParams3;
    vec4 highCloudParams4;

    vec4 cloudMsParams;
    vec4 cloudMarchParams;
    vec4 waterParams;

    vec4 cloudPhiParams;
    vec4 cloudLightParams;
    vec4 cloudShapeParams;
    vec4 cloudMoonParams;
    vec4 cloudShape2Params;

    vec4 cloudAmbientParams;
    vec4 cloudMsParams2;
    vec4 cloudLightParams2;
    vec4 cloudProfileParams;
    vec4 cloudDetailParams;
    vec4 waterWaveParams;
    vec4 waterWaveShapeParams;
    vec4 waterSssParams;
    vec4 waterFftParams;
    vec4 waterFftParams2;

    vec4 auroraParams;
    vec4 rtSampleParams;
    uvec4 perfAblationStage;
    vec4 galaxyParams;

    vec4 fogExtrasParams;
    vec4 cloudTemporalParams;

    vec4 cloudAerialParams;

    uvec4 sharcBuffersLo;
    uvec4 sharcBuffersHi;

    uvec4 sharcCacheParams;

    vec4 cloudSkyHueParams;

    vec4 cloudPhysicsParams;

    vec4 pomParams;

    vec4 sssParams;

    vec4 ueCloudA;
    vec4 ueCloudB;

    vec4 giHalfParams;

    vec4 giRayParams;

    vec4 hpShapeA;
    vec4 hpShapeB;
    vec4 hpShapeC;
    vec4 hpShapeD;
    vec4 hpLightA;
    vec4 hpLightB;
    vec4 hpMiscA;

    vec4 sparseParams;

    // ── skyModelParams ──（2026-11-01 追加在末尾：std140 允许块提前结束，
    // 但中间插入会让后面所有字段整体平移，所以只能追加）
    //   x = 天空散射模型：0 = Bruneton 预计算 LUT（默认），1 = Hillaire 结构
    //       （逐像素 march + 解析多次散射，见 sky_hillaire_common.glsl）
    //   y = Hillaire 分支的辐亮度标定倍率（1 = 出厂；两模型量纲不完全一致，
    //       留这个旋钮做 A/B 对齐，不必重编）
    //   z/w = 保留（写 0）
    vec4 skyModelParams;

    // ── ssHybridParams ──（2026-11-02 追加在末尾，理由同上：中间插入会平移）
    //   x = SSPT 射程（区块，0 = 关；UI 滑条 0..12）
    //   y = SSR 射程上限（区块，0 = 关；UI 硬性下限 12 ⇒ 只要 >0 就按 12 起算）
    //   z/w = 保留（写 0；内部渲染分辨率走 ss_hybrid 自己的 push constant，
    //         不用 UBO 是因为 UBO 在 updateCamera 里写，重尺寸那一帧会滞后）
    // 消费方：cpp/shaders/ss_hybrid.comp
    vec4 ssHybridParams;
