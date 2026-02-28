# Photorealistic Ship Simulator Roadmap

**Branch:** `upgrade/graphics-and-simulation-overhaul`
**Created:** February 2026

Goal: transform Bridge Command into a photorealistic maritime simulator with accurate weather rendering and high-fidelity ship physics, competitive with commercial systems (Kongsberg K-Sim, Wartsila, BMT REMBRANDT).

## Current State Summary

**Working:** WickedEngine PBR rendering (DX12), GPU FFT ocean (512x512 Phillips), MMG 3-DOF physics (8 ships), ImGui HUD, OSM procedural buildings, S-57 chart world generation, multi-point collision, Isherwood wind, procedural lighthouses, engine audio with diesel synthesis.

**Scaffolded but not integrated:** Multi-cascade ocean (headers ready), JONSWAP/TMA spectra (headers ready), Kelvin wake (class ready), multi-window bridge (class ready), VR stereo (class ready).

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

### 1.4 Kelvin Wake Integration -- IN PROGRESS

**Approach:** Shader-based. Ship wake data (position, heading, speed) is passed to the ocean constant buffer (`OceanCB`). The ocean pixel shader (`oceanSurfacePS.hlsl`) computes Kelvin V-pattern foam analytically per-pixel, integrated into the existing 3-layer foam system (shore/shallow/whitecap). Foam renders ON the ocean surface, moves with waves, uses same noise/lighting. No separate mesh or render pass.

Previous mesh-based approach (`WickedKelvinWake` class) abandoned -- flat polygons above the ocean cannot look realistic regardless of material/blending.

**Files:** `ShaderInterop_Ocean.h` (wake struct in CB), `wiOcean.h/cpp` (OceanParameters extension), `oceanSurfacePS.hlsl` (foam_wake computation), `WickedMain.cpp` (per-frame ship data)

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

The MMG 3-DOF model is solid for maneuvering. The gap is wave-induced motion: ships currently use sinusoidal roll/pitch independent of actual waves. This breaks immersion.

### 3.1 Wave-Excited 6-DOF Extension

Extend MMG from 3-DOF (surge/sway/yaw) to 6-DOF (add heave/roll/pitch).

**Two-timescale approach** (standard in maritime simulation, per Fossen 2011):
- **Low frequency (maneuvering):** Existing MMG handles surge/sway/yaw. Timescale: seconds to minutes.
- **High frequency (seakeeping):** New module handles heave/roll/pitch from wave excitation. Timescale: wave period (5-15s).

**Heave model:**
```
m_z * z_ddot = F_hydrostatic + F_wave_excitation + F_damping
F_hydrostatic = -rho * g * A_wp * z   (waterplane area restoring force)
F_wave_excitation = rho * g * A_wp * eta(x,y,t)  (wave elevation at CG)
F_damping = -B_33 * z_dot  (heave damping, ~5-10% critical)
```
Sample `eta(x,y,t)` from `WickedWater::getWaveHeight()` at ship CG.

**Roll model (critical for realism):**
```
I_xx * phi_ddot = -K_roll * phi - B_roll * phi_dot + M_wave + M_wind
K_roll = rho * g * V * GM_T  (hydrostatic restoring, GM from boat.ini)
B_roll = 2 * zeta * sqrt(K_roll * I_xx)  (damping ratio zeta ~0.05-0.15)
M_wave = rho * g * V * GM_T * slope_y(x,y,t)  (wave slope excitation)
M_wind = Y_wind * z_wind_center  (wind heeling moment)
```
Sample wave slope from `WickedWater::getLocalNormals()` (already implemented).

**Pitch model:** Same structure as roll with longitudinal metacentric height GM_L and wave slope in x-direction.

**New boat.ini parameters:**
```
GM_T=1.5          ; Transverse metacentric height (meters)
GM_L=100.0        ; Longitudinal metacentric height (meters)
RollDamping=0.08  ; Damping ratio (fraction of critical)
WaterplaneArea=0  ; Auto-estimate from L*B*Cw if 0
```

**Integration:** RK2 (same as existing MMG), 50Hz. The 6-DOF state feeds into camera (heave/roll/pitch applied) and into ship model transform.

**Files:** New `SeakeepingModel.hpp/cpp`, modify `OwnShip.cpp` (replace sinusoidal pitch/roll), modify `boat.ini` parser

### 3.2 Multi-Point Buoyancy

Replace single-CG wave height lookup with distributed buoyancy sampling:

1. Define N buoyancy sample points on the hull waterplane (typically 5x3 grid = 15 points)
2. Each frame, query wave height at each point's world position
3. Compute net force and moments from submerged volume approximation
4. Feed into 6-DOF equations as `F_wave_excitation` and `M_wave`

This gives:
- **Parametric rolling** in beam seas (wave slope varies along hull length)
- **Bow slamming** in head seas (bow rides up on crest, drops into trough)
- **Broaching** in following seas (stern lifted by overtaking wave)

**Performance:** 15 wave height queries per ship per frame at 60fps = 900 queries/s. `getWaveHeight()` uses CPU readback from GPU (async, 2-3 frame latency). Acceptable if we batch the queries.

**Files:** New `BuoyancySampler.hpp/cpp`, modify `SeakeepingModel.cpp`

### 3.3 Applied Squat -- COMPLETE

Barras squat computed and applied in `OwnShip.cpp:2481-2493`. Sinkage subtracted from Y position, bow-down trim applied. Comprehensive test coverage in `test_mmg_physics.cpp`.

### 3.4 Current Forces -- COMPLETE

Force-based tidal current in MMG. Body-frame current components passed to `PhysicsInput`, relative velocity (`u_rel = u - currentSurge`) used for all hydrodynamic forces. Gives proper drift, crab angle, current-induced yaw. Tested.

### 3.5 Propeller Walk -- COMPLETE

Implemented in both legacy physics (`LegacyPhysicsModel.cpp`) and own ship physics (`OwnShip.cpp`). Configurable via `PropWalkAhead`/`PropWalkAstern` in `boat.ini`. Single and twin-screw support.

### 3.6 Azimuth Drive Support for MMG

Currently azimuth drives (VIC56_360) must use legacy physics. Extend MMG to handle vectored thrust:

1. Add `thrustAngle` to `PhysicsInput` (azimuth angle, -180 to 180 deg)
2. Compute thrust components: `Xp = T*cos(thrustAngle)`, `Yp = T*sin(thrustAngle)`
3. Moment from offset: `Np = Yp * x_prop * L`
4. Skip rudder model when azimuth drive active

**Files:** `MMGPhysicsModel.cpp`, `PhysicsModel.hpp`, `OwnShip.cpp`

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

### 4.5 Vegetation (Grass and Trees)

WE's hair particle system supports grass rendering:

1. **Grass:** Scatter on terrain where splat_grass_weight > 0.5 and slope < 30 deg. Use WE hair particles with wind animation. Density falls off with camera distance.
2. **Trees:** Place tree models at strategic locations from OSM `natural=tree` or procedurally along roads/parks. Use LOD billboards at distance. Start with 3-4 species (oak, pine, birch, palm depending on latitude).
3. **Wind animation:** WE's built-in wind affects hair particles and tree foliage. Tie wind speed/direction to BC weather system.

**Files:** New `VegetationPlacer.hpp/cpp`, WE terrain chunk setup

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

### 5.4 PBR Building Materials

Current buildings use flat color or basic texture atlas. Upgrade:

1. **Material variety:** During world generation, assign building materials from a library:
   - Brick (red, yellow, brown variants) with mortar-line normal maps
   - Concrete (smooth, rough, weathered)
   - Glass curtain wall (reflective, high metalness)
   - Stone (limestone, granite)
   - Cladding (metal panels, wood)

2. **Roof materials:** Slate, tile, flat/membrane (already partially done)

3. **Window placement:** Procedurally place window rectangles on wall faces with glass material (emissive at night)

4. **Age/weathering:** Random dirt/stain overlay on lower floors

**Files:** `BuildingGenerator.cpp`, material texture atlas expansion

---

## Phase 6: Multi-View and Platform (Weeks 8-14)

### 6.1 Multi-Window Bridge Rendering

Wire `WickedMultiView` into `WickedMain`. The 3-monitor bridge setup needs:

1. **3 render targets:** Left (-60 deg yaw offset), Center (0 deg), Right (+60 deg)
2. **Shared scene:** All 3 cameras render the same WE scene
3. **Per-camera:** Yaw offset applied to ship heading, independent FOV
4. **Sync:** All 3 views update from same physics tick (no frame tearing between monitors)

**Approach:** WE supports multiple swapchains. Create 3 `RenderPath3D` instances, each with its own camera entity. Share the same `Scene`. On multi-GPU systems, consider SLI/NVLink for distributing viewports.

**Fallback:** If multi-swapchain is problematic, render all 3 views to a single large framebuffer (e.g., 5760x1080 for 3x 1920x1080) and split output via NVIDIA Surround / AMD Eyefinity.

**Files:** `WickedMultiView.cpp`, `WickedMain.cpp`

### 6.2 Radar Rendering

Currently stubbed (`setRenderTarget()` and `draw2DImage()` TODO in `WickedRenderer.cpp`). Implement:

1. **Render target:** Create WE texture as render target (512x512 or 1024x1024)
2. **Radar sweep:** Rotate camera 360 deg, project to polar coordinates
3. **Target detection:** Ray-cast from own ship to terrain/other ships, compute signal return
4. **Display:** Green-on-black PPI display rendered via ImGui to secondary monitor
5. **Features:** Range rings, bearing cursor, EBL/VRM, guard zones, ARPA tracking

**Alternative approach:** Render depth buffer from top-down camera, threshold for radar returns. Simpler than ray-casting, gives terrain/ship echoes automatically.

**Files:** `WickedRenderer.cpp`, new `RadarRenderer.hpp/cpp`

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

### 7.1 Environmental Audio

Extend existing PortAudio/OpenAL system:

1. **Wave sounds:** Continuous ocean ambient, intensity scales with Beaufort. Low-frequency rumble for heavy seas.
2. **Wind:** Procedural wind noise (filtered white noise), pitch increases with speed. Whistle through rigging at high wind.
3. **Rain:** Stochastic rain impact sounds when weather includes rain. Intensity from `Rain` parameter.
4. **Fog horn:** Own ship horn (user-triggered), AI ship horns at interval in restricted visibility
5. **Harbour ambient:** Gulls, distant traffic, port machinery when near land

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
| 8 | Kelvin wakes (shader-based) | 1.4 | Medium | Medium | IN PROGRESS |
| 9 | 6-DOF seakeeping | 3.1 | High | High | Not started |
| 10 | Multi-window bridge | 6.1 | Critical | Medium | Not started |
| 11 | Multi-point buoyancy | 3.2 | High | High | Not started |
| 12 | Radar rendering | 6.2 | Critical | High | Not started |
| 13 | Nav light visuals | 5.3 | Medium | Medium | BLOCKED (technique) |
| 14 | PBR ship models | 5.1 | High | High (art) | Not started |
| 15 | Rain particles | 2.4 | Medium | Medium | BLOCKED (WE crash) |
| 16 | Azimuth drive MMG | 3.6 | Medium | Medium | Not started |
| ~17~ | ~Applied squat~ | ~3.3~ | ~Medium~ | ~Low~ | DONE |
| ~18~ | ~Current forces~ | ~3.4~ | ~Medium~ | ~Low~ | DONE |
| ~19~ | ~Propeller walk~ | ~3.5~ | ~Medium~ | ~Low~ | DONE |
| 20 | Environmental audio | 7.1 | Medium | Medium | Not started |
| 21 | PBR buildings | 5.4 | Medium | Medium | Not started |
| 22 | Vegetation | 4.5 | Medium | High | Not started |
| 23 | VR support | 6.3 | Low (niche) | High | Not started |

---

## Key Technical Risks

| Risk | Mitigation |
|------|-----------|
| WE `patch_length` hard limit at 50m | **CONFIRMED.** Crashes at other values. Multi-cascade must use same patch size with different spectrum seeds. Custom shader required to blend. |
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
