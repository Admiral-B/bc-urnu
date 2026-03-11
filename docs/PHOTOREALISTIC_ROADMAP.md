# Photorealistic Ship Simulator Roadmap

**Branch:** `upgrade/graphics-and-simulation-overhaul`
**Created:** February 2026

Goal: transform Bridge Command into a photorealistic maritime simulator with accurate weather rendering and high-fidelity ship physics, competitive with commercial systems (Kongsberg K-Sim, Wartsila, BMT REMBRANDT).

## Current State Summary

**Working:** WickedEngine PBR rendering (DX12), GPU FFT ocean (512x512 Phillips), MMG 3-DOF physics (8 ships), ImGui HUD, OSM procedural buildings, S-57 chart world generation, multi-point collision, Isherwood wind, procedural lighthouses, engine audio with diesel synthesis.

**Scaffolded but not integrated:** Multi-cascade ocean (headers ready), multi-window bridge (class ready), VR stereo (class ready).

**Missing entirely:** 6-DOF buoyancy, volumetric atmosphere, dynamic sky, rain/spray particles, PBR terrain splatting, ship model PBR upgrades, radar rendering, underwater caustics, screen-space water interaction.

## Hard Constraints

- 2 networked PCs, 6 monitors. Never break this.
- ENet protocol byte-compatible.
- All 13 scenarios, 6 worlds, 17 ownships, 41 AI ships must work.
- Windows primary, macOS/Linux secondary.
- Performance: 60fps at 1080p per viewport on GTX 1070-class GPU.

---

## Phase 1: Ocean Realism (Weeks 1-4)

The ocean is 80% of what the user sees. This is the highest-impact work.

### Phase 1 Status -- BLOCKED on shader work, pivoting to Phase 2

**Completed:**
- `WickedMultiCascadeOcean` wired into game loop, replacing `WickedWater` (2-line swap in WickedMain.cpp)
- `OceanMath.hpp` extracted: pure-math Beaufort mapping, JONSWAP/TMA/Phillips spectra, cascade weights (26 tests, 69 assertions)
- Beaufort-to-ocean parameter mapping with amplitude table, choppy scale, wind direction conversion
- Water color tuned for North Sea/Atlantic look (dark murky green-grey, reduced reflectivity)

**Key findings from parameter tuning (4 iterations):**
- WE has a **hard `patch_length=50` limit** -- crashes at other values
- Single 50m FFT cascade creates **visible tiling every 50m** that no parameter combination can hide
- Phillips spectrum wind_speed must be **capped at 1500 cm/s** (15 m/s) to prevent aliasing (dominant wavelength must fit in 50m patch)
- Choppy_scale above ~0.8 creates **repeating grid foam** from tiled Jacobian folds
- Choppy_scale below ~0.6 produces **no whitecaps at all**
- There is no sweet spot -- the fundamental issue is the 50m tile repetition

**What's needed to fix ocean (requires WE shader work):**
1. **JONSWAP H(0) override** (Phase 1.2) -- fork `wiOcean.cpp` H(0) init, replace Phillips with JONSWAP per frequency bin. Changes wave *shape* not just size. Medium effort.
2. **Normal map overlay** -- bind a large-scale (500m+) tiling normal map to the ocean surface shader, breaking up the 50m repetition. Low-medium effort.
3. **Multi-cascade shader blending** (Phase 1.1c) -- run auxiliary `wi::Ocean` instances at 50m patch but with different spectrum parameters, blend displacement textures in a custom ocean pixel shader. High effort.

### 1.1 Multi-Cascade Ocean

Wire `WickedMultiCascadeOcean` into `WickedMain.cpp` game loop, replacing the single `WickedWater` instance.

| Cascade | Patch | FFT | Content |
|---------|-------|-----|---------|
| 0 (far) | 50m | 256x256 | Long-period swells (different spectrum seed) |
| 1 (mid) | 50m | 512x512 | Wind waves (current system) |
| 2 (near) | 50m | 256x256 | Capillary ripples (different spectrum seed) |

**UPDATED:** All cascades must use 50m patch (WE hard limit). Tiling breakup comes from different spectrum parameters and random seeds per cascade, not different patch sizes. Requires custom ocean pixel shader to sample and blend all 3 displacement/gradient textures.

**Files:** `WickedMultiCascadeOcean.hpp/cpp`, `WickedMain.cpp`, custom ocean shader (HLSL)

### 1.2 JONSWAP Spectrum Integration

Replace Phillips spectrum initialization with JONSWAP for cascades 0-1, TMA for cascade 2 (shallow water). The `WickedOceanSpectrum.hpp` already has the math. Need to hook `JONSWAPAmplitude()` into WE's H(0) texture initialization.

**Approach:** WE generates H(0) in `wiOcean.cpp` using Phillips. Fork the H(0) compute shader (or override via `OceanParameters` if WE exposes a callback). Write a BC-specific H(0) init that calls `JONSWAPAmplitude()` per frequency bin, upload as the initial spectrum texture.

**Parameters from weather:**
- Beaufort 0-3: fetch=10km (harbour), gamma=3.3
- Beaufort 4-7: fetch=100km (coastal), gamma=3.3
- Beaufort 8-12: fetch=500km (open ocean), gamma=1.0 (fully developed)

**Files:** `WickedOceanSpectrum.hpp`, new `WickedOceanH0.cpp` (compute shader dispatch)

### 1.3 Foam and Whitecap Enhancement

Current foam uses Jacobian fold detection (single threshold). Upgrade to two-layer foam:

1. **Surface foam (white):** Jacobian < threshold, intensity scales with Beaufort. Use a tiled foam detail texture (256x256 procedural Worley noise) with UV animation. Fade with distance to camera.
2. **Subsurface bubbles:** Where foam was recently active, darken water albedo and add parallax-offset bubble texture below surface. Use an accumulation buffer that decays over 2-3 seconds.
3. **Shore foam:** At terrain-water intersection, generate foam line from depth gradient. Width scales with wave amplitude. Use screen-space depth comparison in the water pixel shader.

**Reference technique:** Crest (Unity) two-layer foam with parallax bubble depth, adapted for WE's shader pipeline.

**Files:** Custom water pixel shader override, foam texture assets

### 1.4 Kelvin Wake Integration -- COMPLETE

**Implemented:** Full physics-based ship wake system with both visual and physical effects.

1. **3D vertex displacement** (`oceanSurfaceVS.hlsl`): Noblesse (2008) bow wave (`Z_b = 0.25*V^2/g`), stern depression, transverse + divergent far-field waves with 1/sqrt(r) decay and cusp-line Airy enhancement. Planing suppression above Fn~0.5. Both live ship data (`wakeShips[8]`) and persistent trail breadcrumbs (`wakeTrail[128]`).

2. **Surface foam** (`oceanSurfacePS.hlsl`): Centerline wake foam from trail segments with Gaussian lateral falloff. Kelvin V-arm envelope as continuous port/starboard lines (not per-point rays). Intensity dissipation + cubic distance fade.

3. **Ship handling** (`WickedMultiCascadeOcean::computeWakeHeightAt()`): CPU mirror of VS wake physics. `getWaveHeight()` includes wake contribution so ships experience heave/pitch/roll from other ships' wakes.

**Files:** `ShaderInterop_Ocean.h`, `wiOcean.h`, `oceanSurfaceVS.hlsl`, `oceanSurfacePS.hlsl`, `WickedMultiCascadeOcean.cpp`, `WickedMain.cpp`

### 1.5 Underwater Rendering

When camera submerges (camera Y < water surface Y):
- Switch to underwater color grading (exponential fog with extinction color from `OceanParameters`)
- Disable sky, enable caustics projection on terrain
- God rays via light shaft effect (WE has `setLightShaftsEnabled`)
- Surface seen from below: Snell's window (circular bright patch overhead)

This is lower priority but completes the water system.

---

## Phase 2: Atmosphere and Weather (Weeks 3-6)

Weather is the second most important visual system for a maritime simulator. Real mariners train in fog, rain, and heavy seas.

### 2.1 Volumetric Atmosphere (Sky) -- COMPLETE

Realistic sky with Rayleigh + Mie scattering configured. Sun position from scenario time-of-day. Mie scattering scales dynamically with Beaufort. Aerial perspective enabled. All in `WickedMain.cpp` init (lines 1614-1660) and per-frame update (lines 3164-3258).

### 2.2 Volumetric Clouds -- COMPLETE

Cloud coverage, wind-driven motion, and shadow casting configured. Coverage and cloud base scale with Beaufort. Wind angle/speed drives cloud movement. All in `WickedMain.cpp`.

### 2.3 Volumetric Fog and Visibility -- COMPLETE

Fog density mapped from BC `VisibilityRange` (`0.01 / vis`). Fog start at 30% of visibility range. Disabled when visibility > 5km. Per-frame update in `WickedMain.cpp`.

### 2.4 Rain and Spray Particles

1. **Rain:** GPU particle system (WE's `EmittedParticleSystem`). Particles fall from cloud base, stretch by velocity. Density proportional to weather. Splash particles on water surface and deck.

2. **Spray:** At Beaufort 6+, horizontal spray particles blown off wave crests. Emit from foam regions, travel with wind. Use low-opacity streaks.

3. **Deck wetness:** When raining, increase roughness of all ship materials (simulate wet surfaces). Darken albedo by 15%.

4. **Windshield rain (stretch):** Screen-space raindrop overlay on bridge windows. UV-distorted refraction through droplets. Wiper animation clears drops in arc pattern.

**Files:** New `WeatherParticles.hpp/cpp`, rain particle texture assets

### 2.5 Lightning (Storm Conditions)

At Beaufort 10+, occasional lightning flashes:
- Brief (0.1s) omni-directional point light at random position in cloud layer
- Illuminate clouds from inside (bright spot in volumetric cloud)
- Optional: bolt geometry (line strip from cloud to sea surface)
- Thunder sound with distance-based delay

Low priority but dramatic effect.

---

## Phase 3: Ship Physics Upgrade (Weeks 4-8)

Most of Phase 3 is complete. Remaining: azimuth drive MMG support (3.6).

### 3.1 Wave-Excited 6-DOF Extension -- COMPLETE

**Implemented:** `WaveMotionModel.hpp` -- second-order damped harmonic oscillators for heave, pitch, roll. Driven by wave surface sampling at hull points. Parameters from boat.ini (GM, RollPeriod, PitchPeriod, damping ratios) or auto-estimated from ship dimensions. Semi-implicit Euler integration. Wavelength reduction (sinc filter), added resistance in waves (Stawave-1/ITTC), rudder sea state degradation.

**Files:** `WaveMotionModel.hpp`, `OwnShip.cpp` (5-point path), `WickedMain.cpp` (15-point path)

### 3.2 Multi-Point Buoyancy -- COMPLETE

**Implemented:** 15-point hull grid (5 longitudinal x 3 transverse) with elliptical waterplane footprint. Distributed buoyancy computes net heave, pitch moment, and roll moment from wave heights across hull. Gives parametric rolling in beam seas, bow slamming in head seas, broaching in following seas.

**Files:** `WaveMotionModel.hpp` (computeHullGrid, computeBuoyancy, updateMultiPoint), `WickedMain.cpp` (15-point sampling loop)

### 3.3 Applied Squat -- COMPLETE

Barras squat computed and applied in `OwnShip.cpp:2481-2493`. Sinkage subtracted from Y position, bow-down trim applied. Comprehensive test coverage in `test_mmg_physics.cpp`.

### 3.4 Current Forces -- COMPLETE

Force-based tidal current in MMG. Body-frame current components passed to `PhysicsInput`, relative velocity (`u_rel = u - currentSurge`) used for all hydrodynamic forces. Gives proper drift, crab angle, current-induced yaw. Tested.

### 3.5 Propeller Walk -- COMPLETE

Implemented in both legacy physics (`LegacyPhysicsModel.cpp`) and own ship physics (`OwnShip.cpp`). Configurable via `PropWalkAhead`/`PropWalkAstern` in `boat.ini`. Single and twin-screw support.

### 3.6 Azimuth Drive Support for MMG -- COMPLETE

**Implemented:** `computeAzimuthForces()` in MMGPhysicsModel uses K_T propeller model for thrust magnitude, decomposes into body-frame surge/sway via cos/sin of azimuth angle. Yaw moment from differential axial thrust (propeller spacing) plus lateral thrust at lever arm. Replaces propeller + rudder forces when `PhysicsInput::isAzimuthDrive` is set. Clutch gating in OwnShip.cpp passes zero engine when declutched.

**Files:** `PhysicsModel.hpp` (azimuth fields), `MMGPhysicsModel.hpp/cpp` (computeAzimuthForces, step branch), `OwnShip.cpp` (removed !azimuthDrive restriction, passes azimuth data), `ShetlandTrader/boat.ini` and `3111_Tug/boat.ini` (MMGMode=1 enabled)

---

## Phase 4: Terrain and Environment (Weeks 5-10)

### 4.1 PBR Terrain Splatting

Replace single satellite texture with multi-layer PBR terrain:

1. **Splat map:** 4-channel RGBA texture encoding blend weights for 4 material layers
2. **Material layers** (each has albedo + normal + roughness):
   - Rock/cliff (grey, high roughness)
   - Grass/vegetation (green, medium roughness)
   - Sand/beach (tan, medium roughness)
   - Mud/wetland (brown, high roughness)
3. **Blending:** Height-based priority (rock on slopes >45 deg, sand below 3m elevation, grass above)
4. **Satellite underlay:** Blend satellite texture at 30-40% opacity beneath procedural layers (existing approach, refined)

**Approach:** Generate splat map during world generation based on elevation, slope, land-use classification, and distance to coast. At runtime, WE's terrain shader samples all 4 material layers and blends by splat weight.

**Files:** `TerrainTextureBlender.cpp` (generate splat map), `WickedTerrainNode.cpp` (multi-material setup)

### 4.2 Tri-Planar Mapping for Cliffs

Coastal cliffs currently stretch textures on steep slopes. Fix:

1. In terrain vertex shader, compute world-space normal
2. Blend between XY, XZ, YZ texture projections weighted by abs(normal) components
3. Only activate on slopes >30 deg (avoid cost on flat terrain)

WE's material system supports custom shaders per material. Apply tri-planar only to the rock layer.

**Files:** Custom terrain shader override

### 4.3 Fractal Coastline Detail

Coastlines are rasterized at ~10m/pixel, losing fine detail. Add:

1. **Pre-generation:** Fractal subdivision of coastline polygons (recursive midpoint displacement, +-3m amplitude, 3 iterations)
2. **Runtime:** Domain-warped noise in terrain shader displaces sampling coordinates near coast, giving organic irregular edges
3. **Vertex displacement:** If using WE terrain chunks, add per-vertex noise displacement in coastal strip (within 200m of waterline)

**Files:** `HeightmapGenerator.cpp`, terrain shader

### 4.4 Thermal Erosion Pass

Run 50-100 iterations of thermal erosion on heightmap during world generation:
- Material moves from steep cells to lower neighbors
- Creates gullies, sediment fans, realistic cliff faces
- Only apply to land above 5m (don't erode seabed)
- Talus angle threshold: 35 deg

**Files:** New `ThermalErosion.hpp/cpp`, called from `HeightmapGenerator.cpp`

### 4.5 Vegetation (Grass and Trees) -- DONE (trees)

**Implemented:**
1. **VegetationPlacer** (`editor/VegetationPlacer.hpp/cpp`): Generates tree placements from OSM land use grid + heightmap. Density varies by land use type (Forest=80/ha, Heath=15, Residential=8, Grass=5, Farmland=2). Slope >35 deg rejected. Max 15000 trees per world.
2. **4-species system:** Deciduous (broadleaf), Conifer (tall/narrow), Shrub (low/wide), Palm (tropical). Species selected by land use type + latitude (palms <35 deg, more conifers >55 deg).
3. **2x2 atlas texture:** Procedurally generated 512x512 RGBA billboard atlas with all 4 species. Alpha-tested cutoff for transparency.
4. **Multi-species rendering:** WickedMain reads Species(N) from trees.ini, maps to UV sub-regions in the atlas. Per-species base dimensions (conifer 5x16m, deciduous 8x12m, shrub 6x4m, palm 4x14m).
5. **Terrain-relative placement:** Trees snap to actual terrain height via `terrainNode->getHeightAt()`. Trees in water (height <0.3m) are skipped.
6. **Legacy compatibility:** Worlds with old trees.ini (no Species field) render with full UV range as before.

**Integrated into world generation pipeline** (EditorApp.cpp): runs after land use grid is populated, outputs trees.ini + tree_billboard.png.

**Remaining:**
- Grass (WE hair particles) -- deferred due to rain particle crash risk (same particle system)
- Wind animation for tree billboards
- LOD billboards at distance
- OSM `natural=tree` individual tree placement

### 4.6 WE Native Terrain Migration (Stretch)

WE has a built-in terrain system with 67x67 chunks, LOD, virtual texturing, grass, and physics collision. Migrating to it would give:
- Automatic LOD (dense near camera, sparse far away)
- Virtual texture streaming (no memory limit on texture resolution)
- Built-in grass/prop scattering per chunk
- Physics heightfield collision (Bullet/Jolt)

**Risk:** WE's terrain is designed for its editor, not for BC's chart-generated heightmaps. May need significant adapter code.

**Recommendation:** Defer until Phase 1-3 are stable. Evaluate whether the adaptive LOD benefit justifies the integration cost.

---

## Phase 5: Ship and Object Visuals (Weeks 6-12)

### 5.1 PBR Ship Model Upgrade

Current ship models are .3ds/.x format with simple diffuse textures. For photorealism:

1. **Priority ships** (most-used): ProtisSingleScrew, Protis, VIC56, Puffer -- create PBR texture sets:
   - Albedo (hull paint, rust, waterline fouling)
   - Normal map (panel lines, rivets, deck planking)
   - Roughness map (smooth paint vs rough deck vs corroded metal)
   - Metalness map (hull=0, railings/winches=1)
   - AO map (baked ambient occlusion for recesses)

2. **Model format:** Convert to glTF 2.0 with embedded PBR textures. BC's glTF importer already handles this.

3. **Ship detail geometry:**
   - Bridge windows (glass material: high metalness, low roughness, high transparency)
   - Mast/antenna detail
   - Deck equipment (winches, bollards, liferafts)
   - Running rigging for sail vessels

4. **Weathering:** Rust streaks below scuppers, waterline fouling band, deck wear patterns. All baked into PBR textures.

**Effort:** This is primarily art/content work. Each ship needs 4-8 hours of texturing in Substance Painter or similar. Consider commissioning or finding CC0 maritime PBR texture packs.

### 5.2 Navigation Aid Upgrades

1. **Buoy models:** Replace simple cylinder/cone with detailed IALA buoys:
   - Lateral buoys with retroreflective tape bands
   - Cardinal buoys with proper topmarks (already partially done procedurally)
   - Safe water buoys, isolated danger marks, special marks
   - Light character visualization (flash patterns visible at correct range)

2. **Lighthouse models:** Procedural generation is good. Add:
   - Glass lantern with fresnel lens visible up close
   - Point light with correct range/intensity
   - Flash sequence from light.ini

3. **Beacons and day marks:** Simple geometry from chart data (already in landobject system)

### 5.3 Dynamic Ship Lighting

**Status: BLOCKED -- lens flare approach failed, needs new technique**

**Failed approaches:**

1. **Emissive UV spheres:** Look like glowing balls, not point lights. Distance-based scaling hacks, no volumetric fog interaction.

2. **WE lens flare billboards (current):** `lensFlareRimTextures` on `LightComponent` renders screen-space textured quads. Fundamental problems:
   - Billboards are always rectangular -- look like colored blocks, not point lights
   - Large billboards (16px) bleed through bridge window frames (glass is alpha-blended, doesn't write to depth buffer, so per-pixel depth testing in lensFlarePS.hlsl cannot clip frame overlap)
   - Small billboards (4px) don't bleed but still look like colored squares, not realistic lights
   - WE bloom post-process doesn't produce sufficient glow from small additive billboards at typical nav light brightness levels
   - The lens flare system is designed for sun decorative effects (starburst, hex bokeh), not for simulating distant point lights

**What's needed:** A rendering technique that produces a sub-pixel bright point with natural radial falloff (Airy disk / atmospheric scatter appearance). Options to investigate:
- Custom screen-space point sprite pass with radial alpha gradient and proper depth testing against opaque geometry only
- Emissive material on a tiny camera-facing quad with HDR emissive intensity (>>1.0) to drive bloom naturally
- Custom compute shader that writes directly to the HDR render target at the light's screen position with a Gaussian splat
- Investigate how other WE-based projects or commercial maritime sims render distant point lights

**Current state in code:** `WickedMain.cpp` has working Allard's Law intensity, COLREG arc visibility, flash sequences, and per-frame light management. The light entity infrastructure is solid -- only the visual representation technique needs replacing.

**Files:** `WickedMain.cpp` (light creation + update loop), `lensFlareVS/PS.hlsl` (modified with per-pixel depth test, kept for reference)

### 5.4 PBR Building Materials -- PARTIALLY DONE

**Implemented:**

1. **Material classification:** `classifyBuildingMaterial()` maps OSM building type + height to 6 material classes: Brick, Concrete, Stone, Industrial, Glass, Default. Residential/houses -> brick, commercial/office -> concrete (or glass if >20m), churches -> stone, warehouses -> industrial, tall buildings -> glass.
2. **Per-building vertex color tinting:** Each building gets a unique RGBA vertex color based on its material class with deterministic random variation (Knuth hash from outline coords). WE materials use `SetUseVertexColors(true)` to multiply texture by vertex color. Breaks up uniform appearance even when all buildings share one atlas texture.
3. **Glass building batch:** Tall commercial/office buildings (Glass class) are batched separately with distinct PBR properties: roughness 0.15, metalness 0.3 for reflective curtain-wall look.
4. **Roof color variety:** Roof vertex colors vary by material class (dark slate for brick, lighter for concrete, etc.)

**Not yet done:**

- Window placement (procedural rectangles with glass/emissive material)
- Age/weathering overlay (dirt/stain on lower floors)
- Expanded texture atlas (16+ facade types instead of 2x2)

**Files:** `BuildingGenerator.hpp/cpp`, `WickedMain.cpp` (createBuildingMeshEntity + runtime generation loop)

---

## Phase 6: Multi-View and Platform (Weeks 8-14)

### 6.1 Multi-Window Bridge Rendering -- COMPLETE

**Implemented:** `WickedMultiView` creates borderless fullscreen windows on extra monitors (auto-detected via `EnumDisplayMonitors`). Each gets its own `SwapChain`, `RenderPath3D`, and `CameraComponent` with configurable yaw offset. Camera updates use ship quaternion for proper pitch/roll coupling. Secondary mode also works via ENet network with `look_angle` offset.

**Config (bc5.ini):** `wicked_views=3`, `wicked_view_offset_1=-60`, `wicked_view_offset_2=60`, `view_angle=90`

**Files:** `WickedMultiView.hpp/cpp`, `WickedMain.cpp` (lines 1612-1640 init, 4793-4802 per-frame)

### 6.2 Radar Rendering -- COMPLETE

**Implemented:** Full radar simulation with `RadarCalculation` (360-degree sweep, RCS, sea/rain clutter, noise, STC). ARPA tracking with CPA/TCPA. ImGui fullscreen PPI display (`RadarDisplay`, R key toggle) with range rings, controls, and ARPA contact table. 3D console display scaffolded but disabled (geometry clipping).

**Files:** `RadarCalculation.hpp/cpp`, `RadarScreen.hpp/cpp`, `RadarDisplay.hpp/cpp`, `RadarData.hpp`, `WickedMain.cpp`

### 6.3 VR Support (OpenXR)

`WickedVRView` has stereo camera scaffolding. Complete:

1. OpenXR session setup (WE supports DX12, need to confirm OpenXR interop)
2. Per-eye render targets from `XrSwapchainImageD3D12KHR`
3. Asymmetric FOV from `XrFovf` (varies per frame with eye tracking)
4. Head tracking: apply 6-DOF head pose to bridge camera
5. Hand tracking: optional for throttle/wheel interaction
6. Reprojection: WE at 45fps + ASW/motion smoothing to 90fps headset

**Risk:** WE's OpenXR support may be incomplete. Budget time for debugging.

**Files:** `WickedVRView.cpp`, `WickedMain.cpp`

---

## Phase 7: Audio and Immersion (Weeks 10-14)

### 7.1 Environmental Audio -- PARTIALLY DONE

Extend existing PortAudio/OpenAL system:

1. **Wave sounds:** DONE. Bwave.wav volume scales with Beaufort (silent at B0, full at B6+). Driven per-frame via `ISound::setEnvironment()`.
2. **Wind:** DONE. Procedural wind noise synthesis in PortAudio callback: LP-filtered white noise (broadband) + BP tonal howl + slow gust modulation. Audible from B3, full intensity at B7+. Cutoff frequency rises with wind speed (200-2000 Hz).
3. **Engine character:** DONE. `ISound::setEngineCharacter(maxRPM, cylinders, stroke)` auto-classifies vessels into 5 engine classes from MaxRevs in boat.ini. Tunes LP filter cutoff, diesel synthesis mix, playback rate range, firing frequency (2/4-stroke aware), pulse width, and sub-harmonic rumble per class. Slow-speed (<=200 RPM) engines get heavy individual thumps and sub-bass; high-speed (3000+ RPM) get mostly WAV-driven pitch shift with minimal synthesis.
4. **Rain:** Deferred (rain particles disabled due to WE crash).
5. **Fog horn:** Own ship horn works (user-triggered). AI ship fog horn intervals: not yet implemented.
6. **Harbour ambient:** Not yet implemented.

**Future engine audio improvements:**
- Exhaust waveguide resonance model (delay line per exhaust path creates formant peaks, the biggest differentiator between engine types). Reference: Antonio-R1/engine-sound-generator, Manes et al. SIVE 2015.
- Turbocharger whine: additive sine at blade-pass frequency (2-5 kHz), amplitude proportional to load.
- Mechanical noise layer: injector clicks, piston slap synchronized to crank angle.
- Granular synthesis from RPM-sweep recordings (CrankcaseAudio REV approach) for highest fidelity.

### 7.2 Crew and Bridge Sounds

1. **Helm acknowledgment:** "Starboard twenty" etc. when helm orders given (pre-recorded or TTS)
2. **Telegraph bell:** Ding-ding when engine order changed
3. **Gyro tick:** Subtle tick at each degree on gyro compass repeater
4. **VHF radio:** Static/squelch sounds, maritime radio chatter ambient

### 7.3 Vibration (Haptic)

If VR controllers available, haptic feedback for:
- Engine vibration (proportional to RPM)
- Wave impact (sudden heave acceleration)
- Collision (large impulse)

---

## Phase 8: Polish and Integration (Weeks 12-16)

### 8.1 Post-Processing Pipeline

Enable additional WE post-processing:
- **SSAO:** Ambient occlusion in building recesses, ship details
- **Temporal AA:** Upgrade from FXAA for smoother edges with less blur
- **Color grading:** Maritime-appropriate LUT (slightly desaturated, cool blue shadows)
- **Chromatic aberration:** Subtle, for binocular/window refraction effect
- **Vignette:** Slight darkening at screen edges for binocular feel

### 8.2 Time-of-Day Transitions

Smooth transitions through:
- **Dawn:** Orange/pink horizon, stars fade, clouds illuminate from below
- **Day:** Full lighting, shadows, cloud shadows on water
- **Dusk:** Golden hour, long shadows, warm light on ship/buildings
- **Night:** Moonlight on water, navigation lights prominent, city glow on horizon
- **Blue hour:** 20-minute transition with characteristic deep blue

All driven by sun/moon position calculation from lat/lon/date/time.

### 8.3 Performance Optimization

Target: 60fps per viewport at 1080p on GTX 1070 or equivalent.

1. **Occlusion culling:** WE built-in (hierarchical Z-buffer). Verify it's enabled.
2. **LOD:** WE auto-LOD for models. Ensure ship models have 3 LOD levels (full/medium/billboard).
3. **Draw call batching:** Buildings already batched. Ensure buoys/lights are instanced.
4. **Texture streaming:** WE's texture streaming for large satellite textures.
5. **Half-res effects:** Render volumetric fog and SSR at half resolution, bilateral upsample.
6. **Ocean cascade budget:** If cascade 2 (ripples) is too expensive, disable beyond 100m from camera.

### 8.4 Quality Presets

| Preset | Ocean | Clouds | Fog | Terrain | Buildings | Target GPU |
|--------|-------|--------|-----|---------|-----------|------------|
| Low | 1 cascade, 256 FFT | 2D skybox | Height fog only | Single texture | Color-only | GTX 960 |
| Medium | 2 cascades, 512 FFT | Volumetric (half-res) | Volumetric (half-res) | 2-layer splat | Basic PBR | GTX 1070 |
| High | 3 cascades, 512 FFT | Volumetric (full-res) | Volumetric + patchy | 4-layer splat + triplanar | Full PBR + windows | RTX 3060 |
| Ultra | 3 cascades, 512 FFT + SSR | Volumetric + raytraced shadows | Full volumetric | Virtual texture | Full PBR + weathering | RTX 4070 |

---

## Priority Order (What to Build First)

This ordering maximizes visual impact per unit of effort:

| Priority | Item | Phase | Impact | Effort | Status |
|----------|------|-------|--------|--------|--------|
| ~1~ | ~Atmosphere + sun~ | ~2.1~ | ~Huge~ | ~Medium~ | DONE |
| ~2~ | ~Volumetric clouds~ | ~2.2~ | ~High~ | ~Medium~ | DONE |
| ~3~ | ~Volumetric fog~ | ~2.3~ | ~High~ | ~Medium~ | DONE |
| 4 | JONSWAP H(0) override | 1.2 | High | Medium | BLOCKED (shader) |
| 5 | Ocean normal map overlay | 1.1+ | High | Low-Medium | BLOCKED (shader) |
| 6 | Multi-cascade blending | 1.1c | Huge | High | BLOCKED (shader) |
| 7 | PBR terrain splatting | 4.1 | High | High | BLOCKED (shader) |
| ~8~ | ~Kelvin wakes (shader-based)~ | ~1.4~ | ~Medium~ | ~Medium~ | DONE |
| ~9~ | ~6-DOF seakeeping~ | ~3.1~ | ~High~ | ~High~ | DONE |
| ~10~ | ~Multi-window bridge~ | ~6.1~ | ~Critical~ | ~Medium~ | DONE |
| ~11~ | ~Multi-point buoyancy~ | ~3.2~ | ~High~ | ~High~ | DONE |
| ~12~ | ~Radar rendering~ | ~6.2~ | ~Critical~ | ~High~ | DONE |
| 13 | Nav light visuals | 5.3 | Medium | Medium | BLOCKED (technique) |
| 14 | PBR ship models | 5.1 | High | High (art) | Not started |
| 15 | Rain particles | 2.4 | Medium | Medium | BLOCKED (WE crash) |
| ~16~ | ~Azimuth drive MMG~ | ~3.6~ | ~Medium~ | ~Medium~ | DONE |
| ~17~ | ~Applied squat~ | ~3.3~ | ~Medium~ | ~Low~ | DONE |
| ~18~ | ~Current forces~ | ~3.4~ | ~Medium~ | ~Low~ | DONE |
| ~19~ | ~Propeller walk~ | ~3.5~ | ~Medium~ | ~Low~ | DONE |
| 20 | Environmental audio | 7.1 | Medium | Medium | DONE (wave + wind) |
| 21 | PBR buildings | 5.4 | Medium | Medium | DONE (vertex color tint + glass) |
| 22 | Vegetation | 4.5 | Medium | High | DONE (trees, 4-species atlas) |
| 23 | VR support | 6.3 | Low (niche) | High | Not started |

---

## Key Technical Risks

| Risk | Mitigation |
|------|-----------|
| WE `patch_length` was thought to be hard-limited at 50m | **RESOLVED.** Crash was sizeof(Scene) mismatch, not WE limitation. Now using 1000m patch. |
| H(0) spectrum override requires WE source modification | Fork `wiOcean.cpp` H(0) init. Minimal change (replace Phillips call with JONSWAP). |
| 6-DOF stability with large waves | RK2 at 50Hz should handle wave periods >5s. Add implicit damping if needed. Cap heave/roll/pitch rates. |
| Multi-swapchain on varied hardware | Test on Intel/NVIDIA/AMD. Fallback to single-framebuffer split via display driver. |
| Performance with 3 cascades + volumetric clouds + fog | Profile early. Budget: ocean 3ms, clouds 2ms, fog 1ms, terrain 2ms, ships 1ms, post 2ms = 11ms (90fps). |
| PBR ship models require art pipeline | Use free CC0 PBR texture libraries (ambientCG, FreePBR). Automate roughness/normal generation from photos where possible. |

## Research References

- [WickedEngine GitHub](https://github.com/turanszkij/WickedEngine) -- engine source, terrain system, volumetric clouds, ocean
- [WickedEngine Material System (DeepWiki)](https://deepwiki.com/turanszkij/WickedEngine/5.3-material-system-and-pbr) -- 15 texture slots, sheen, clearcoat, anisotropy
- [WickedEngine Terrain System (DeepWiki)](https://deepwiki.com/turanszkij/WickedEngine/6-physics-system) -- chunks, virtual texturing, grass
- [Ocean Rendering Part 1 (Robert Ryan, 2025)](https://rtryan98.github.io/2025/10/04/ocean-rendering-part-1.html) -- multi-cascade FFT, JONSWAP, foam
- [Ocean Simulation with FFT and WebGPU (Barth Paleologue)](https://barthpaleologue.github.io/Blog/posts/ocean-simulation-webgpu/) -- modern FFT ocean reference
- [Crest Ocean Renderer (Unity)](https://github.com/JJarvis89/crest-oceanrender) -- two-layer foam, subsurface scattering, LOD ocean mesh
- [GPU-Accelerated 3D Nonlinear Kelvin Wake Simulation](https://www.mdpi.com/2076-3417/13/22/12148) -- CUDA-based Kelvin wake patterns
- [6-DOF Maneuvering Model (ScienceDirect)](https://www.sciencedirect.com/science/article/pii/S0029801820310416) -- rapid estimation deep/shallow water
- [Real-Time Buoyancy via Convex Hull (arXiv 2025)](https://arxiv.org/html/2509.03804v1) -- submerged volume calculation for buoyancy
- [Kongsberg K-Sim Navigation](https://www.kongsberg.com/maritime/products/simulation/k-sim-navigation/) -- commercial reference for visual fidelity target
- [BMT REMBRANDT](https://www.bmt.org/innovations/bmt-rembrandt/) -- commercial ship simulation benchmark
- [Volumetric Cloudscapes of Horizon Zero Dawn (SIGGRAPH 2015)](https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf) -- foundational volumetric cloud technique
- [Real-Time Ray Marching for Volumetric Worlds](https://www.daydreamsoft.com/blog/real-time-ray-marching-for-volumetric-worlds-and-next-gen-visual-effects) -- fog, clouds, optimization
- [SIGGRAPH 2025 Advances in Real-Time Rendering](https://advances.realtimerendering.com/s2025/index.html) -- latest rendering techniques
- [Cliff Terrain Shader (Harry Alisavakis)](https://halisavakis.com/my-take-on-shaders-cliff-terrain-shader/) -- tri-planar cliff texturing
- [FreePBR](https://freepbr.com/) and [ambientCG](https://ambientcg.com/) -- CC0 PBR texture sources
