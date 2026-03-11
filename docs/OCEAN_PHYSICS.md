# Ocean Physics -- WickedEngine Integration

## WE Ocean Pipeline

WE uses a GPU-accelerated FFT ocean based on the NVIDIA OceanCS/CUDA oceanFFT samples. The Phillips spectrum generates initial wave heights H(0), which are evolved in frequency domain and transformed to spatial displacement maps each frame.

Key: WE's internal physics uses **CGS units** (gravity = 981 cm/s^2, wind_speed in cm/s). The `spectrumCallback` override replaces the built-in Phillips spectrum with PhillipsHasselmann, which uses consistent SI units internally and pre-computed normalization to achieve a target Hs.

## Ocean Configuration: patch_length = 1000m

`patch_length` controls the FFT tile size. Using 1000m (up from the WE default 50m) eliminates visible geometric tiling at ship scale. Previous "crashes at other values" was a sizeof(Scene) mismatch, not a WE limitation.

**CGS/SI dispersion fix**: WE computes omega=sqrt(981*|K|) with K in rad/m but g in cm/s^2, making waves 10x too fast. `time_scale=0.1` exactly compensates.

## Hs-Normalized Spectrum

The spectrum is no longer driven by `wave_amplitude`. Instead:

1. `OceanMath::beaufortToOceanParams()` maps Beaufort to a **target Hs** (WMO significant wave height)
2. `OceanSpectrum::PhillipsHasselmann()` computes spectrum shape using Pierson-Moskowitz alpha (8.1e-3) with Hasselmann frequency-dependent directional spreading, L computed in **meters** (not cm)
3. `OceanSpectrum::computeSpectrumScale()` pre-sums energy over the FFT grid with scaleFactor=1, then derives `scale = Hs / (4 * sqrt(energySum / 2))` to normalize the spectrum to produce exactly the target Hs

This eliminates the impossible task of finding a single `wave_amplitude` that works across all Beaufort numbers.

## Scenario -> Ocean Wiring

All weather parameters flow from `environment.ini` through `SimulationModel` -> `SimBridge` -> `WickedMain.cpp` -> `WickedWater::update()`.

| Scenario Field | SimBridge Getter | Ocean/Sky Effect |
|---|---|---|
| `Weather=` (B0-12) | `getWeather()` | `wave_amplitude`, `choppy_scale`, cloud coverage |
| `WindSpeed=` (knots) | `getWindSpeed()` | `wind_speed` (cm/s), cloud drift speed |
| `WindDirection=` (deg) | `getWindDirection()` | `wind_dir` (wave propagation), cloud drift angle |
| `VisibilityRange=` (nm) | `getVisibility()` | `fogStart`, `fogDensity` |
| `RainIntensity=` (0-10) | `getRain()` | Cloud darkening only (WE rain particles disabled -- crash bug) |

**Wind direction**: BC meteorological (FROM) is flipped +180 deg to get wave propagation direction. Low wind (<0.3 m/s) defaults to north.

**Cloud coverage**: Derived from Beaufort: `cloudCoverage = min(1, beaufort / 8)`. No independent CloudCover field in scenario files. Wind drives cloud drift angle and speed.

**Spectrum recreation**: `ocean.Create()` only called when wind speed changes >30% OR direction changes >30 deg. Prevents visual pop from random phase regeneration.

## Parameter Reference

| Parameter | Description | Range |
|---|---|---|
| `patch_length` | FFT tile size in meters. | 1000 |
| `wave_amplitude` | Set to 1.0 (placeholder). Actual energy from `spectrumCallback` + `computeSpectrumScale()`. | 1.0 |
| `wind_speed` | Wind velocity in cm/s. Drives dominant wavelength. Capped at 2000 cm/s (20 m/s). | 30-2000 |
| `choppy_scale` | Gerstner horizontal displacement. 0.1 at B0, ramps to 1.3 at B12. | 0.1-1.3 |
| `dmap_dim` | FFT resolution. Must be power of 2. | 512 |
| `time_scale` | Wave animation speed multiplier. **0.1 required** (compensates CGS dispersion). | 0.1 |
| `wind_dependency` | Anti-wind wave energy. Unused when spectrumCallback set. | 0.07 |
| `surfaceDetail` | Mesh LOD level. 3 = 480x270 screen-space grid. | 3 |
| `surfaceDisplacementTolerance` | Max vertex displacement for LOD culling. | 2 |

## Beaufort Mapping

Spectrum energy is Hs-normalized (see above). `wave_amplitude` is set to 1.0 (placeholder). `choppy_scale` = 0.1 at B0, ramps linearly to 1.3 at B12. `wind_speed` from knots -> cm/s, capped at 2000 cm/s.

| B | Wind (cm/s) | Target Hs (m) | choppy_scale |
|---|---|---|---|
| 0 | 30 (floor) | 0.0 | 0.10 |
| 1 | 103 | 0.1 | 0.20 |
| 3 | 440 | 0.6 | 0.40 |
| 5 | 980 | 2.0 | 0.60 |
| 7 | 1540 | 5.5 | 0.80 |
| 9 | 2000 (cap) | 9.0 | 1.00 |
| 12 | 2000 (cap) | 14.0 | 1.30 |

## Foam / Whitecap System

Four foam sources in `oceanSurfacePS.hlsl`:

1. **Shore foam** (`foam_shore`): Depth buffer comparison. `exp(-depth_diff * 0.7)` -- visible surf zone ~4m wide at waterline.

2. **Shallow water breaking** (`foam_shallow`): `exp(-water_depth * 0.25) * 0.5` -- broad foam zone where seabed < ~5m below surface. Uses refraction pass depth (zero cost in deep water where water_depth = FLT_MAX).

3. **Open ocean whitecaps** (`foam_wave`): `pow(saturate(crest_steepness), 2)` from gradient steepness (`length(gradient.rg) * 0.15`). Wave slope-based, not Jacobian fold.

4. **Ship wake foam** (`foam_wake` + `foam_kelvin`): Two components:
   - **Centerline wake** (`foam_wake`): Trail-based. Per-segment closest-point test, Gaussian lateral falloff (`exp(-perpDist^2 / hw^2)`), intensity dissipation, cubic distance fade (`distFade^3`).
   - **Kelvin V-arm envelope** (`foam_kelvin`): Continuous port/starboard lines offset from trail centerline by `distFromBow * tan(19.47deg)`. Rendered as segment pairs with Gaussian falloff around each arm line. NOT per-point V-rays (causes millipede pattern).

All natural foam modulated by 3 octaves each of simplex + Voronoi noise for patchy appearance. Wake foam uses `max(foam_wake, foam_kelvin) * 0.5`.

**Jacobian fold**: `J = (1 + Dx.x)(1 + Dy.y) - Dx.y * Dy.x` where Dx/Dy = displacement derivatives * choppy_scale * gridLen. At patch=1000m, fold values are routinely 1-10+ even in moderate seas. Direct use for foam requires very high thresholds or a different approach.

## Anti-Tiling System

FFT tiles every 250m. Three techniques break visible repetition in `oceanSurfacePS.hlsl`:

1. **Gradient re-sampling**: Gradient map at UV * 0.37 (135m period) + UV offset, 40% weight. Reuses FFT normals at non-harmonic period. One texture sample.

2. **Simplex noise**: Three octaves of `noise_simplex_2D` on gradient normals at 125m, 20m, 5m scales, animated. Non-periodic, organic.

3. **Near/far gradient blend** (WE built-in): Full UV near camera, UV * 0.125 (400m period) at distance > 1000m.

**Do NOT use sine waves for perturbation** -- they create visible stripes/checkerboard. Simplex noise or gradient re-sampling only.

## Shallow Water Effects (Pixel-Shader Only)

WE's FFT ocean is a deep-water model -- no bathymetry awareness. True shallow water physics (shoaling, refraction, breaking) would require either a depth-dependent spectrum (TMA) or a separate shallow water simulation. Instead, we fake the visual effects entirely in `oceanSurfacePS.hlsl` using depth data from the refraction pass.

**Constraint**: The vertex shader has NO access to scene depth. All shallow water effects are normal/foam changes in the pixel shader. No geometry changes near shore.

### Depth Sources

| Variable | Source | Available When |
|---|---|---|
| `shallow_depth_est` | `texture_lineardepth[pixel] * z_far - lineardepth` | Always (early in PS) |
| `water_depth` | Refraction pass plane equation + displacement | After refraction block |

### Effects

1. **Normal steepening** (`shallow_factor`): Gradient amplified up to 2.5x in water < 8m deep. Simulates wave energy conservation during shoaling.

2. **Wavelength compression**: Gradient map re-sampled at UV*2.0 near shore (UV*1.0 in deep water). Simulates phase velocity decrease.

3. **Shore wave lines** (`water_depth < 15m`): Two sets of animated foam bands using `frac(water_depth * freq - time * speed)`. Bands are automatically parallel to depth contours. Noise-modulated.

4. **Shallow water foam** (`foam_shallow`): Broad foam zone where seabed < ~5m below surface.

### Limitations

- No geometric displacement changes near shore (VS can't see depth)
- Shore wave lines use `water_depth` as distance proxy
- No wave refraction toward shore
- No breaking wave geometry

## Horizon Distance Fade

All custom perturbations are faded to zero beyond ~2km via `custom_fade`. At the horizon, the ocean mesh is extremely coarse and high-frequency noise creates aliasing.

```
custom_fade = saturate(1 - saturate(dist * 0.0005 - 0.5) * 2.0)  // 1.0 at <1km, 0.0 at >2km
```

## Wind Dependency

`wind_dependency` controls energy in anti-wind direction only:
```
if (Kcos < 0) phillips *= dir_depend;
```
- 0.07 = very directional
- 0.35 = significant opposing wave energy

## Ship Wake Physics

Three layers: vertex displacement (VS), normal perturbation (PS), and foam (PS). Two data sources: live ship positions (`wakeShips[8]`) and persistent trail breadcrumbs (`wakeTrail[128]`).

### Bow Wave (Noblesse 2008)

Near the bow (-0.4L to +0.15L along ship axis):

```
Z_b = 0.25 * V^2 / g * planingFactor
planingFactor = (Fn > 0.5) ? (0.5/Fn)^2.5 : 1.0
```

Capped at 3.0m. Lateral Gaussian envelope: `exp(-across^2 / (L^2 * 0.02))`. Longitudinal profile: `sin(bowT * PI)` where bowT = 0..1 over the bow zone.

### Stern Depression (Noblesse)

Behind the stern: `Z = -0.6 * Z_b * exp(-along^2 / L^2) * lateralEnvelope`. Creates the trough immediately aft of the hull.

### Far-Field Transverse Waves (Kelvin Theory)

Beyond 0.2L behind stern. Base amplitude: 30% of bow wave height. Decay: `1/sqrt(along/L)` (energy conservation). Wavelength: `lambda = 2*PI*V^2/g`. Lateral envelope: Gaussian within Kelvin half-angle.

### Far-Field Divergent Waves

Along Kelvin arms at 19.47deg half-angle. Amplitude: 60% of transverse. Cusp-line Airy enhancement: 1.5x at the arm boundary. Wavelength: 70% of transverse. Phase: `length(along, absAcross) * 2PI / divWL`.

### Planing Suppression

Above Fn~0.5 (planing regime), wake amplitude REDUCES as `(0.5/Fn)^2.5`. Maximum wake occurs at pre-planing hump speed.

### Trail Persistence

Trail breadcrumbs carry `intensity` (0..1, decaying over time) and `distFromBow` (distance from the bow position at time of deposit). Same physics as live wake but with `intensity` multiplier for dissipation and `halfWidth * 5` as estimated ship length.

### Normal Perturbation (PS)

The VS displaces vertices but the FFT gradient map has no wake information, so surface normals stay flat even where geometry is displaced. The PS computes analytical wake wave gradients and adds them to the FFT gradient before the surface normal is built:

```
gradient of sin(along * 2PI/WL) = cos(phase) * 2PI/WL
projected to world xz via ship heading direction
```

Both live ship and trail loops mirror the VS transverse wave math. Added to `gradient.rg` before `surface.N = normalize(float3(gradient.x, 0.20, gradient.y))`.

### CPU Mirror for Ship Handling

`WickedMultiCascadeOcean::computeWakeHeightAt()` replicates the VS physics on CPU so that `getWaveHeight()` includes wake contributions. Ships sailing through another ship's wake experience real heave/pitch/roll excitation.

### Visual Limitations

**Physically correct amplitudes are small relative to FFT ocean waves.** At typical speeds:

| Ship | Speed | Fn | bowWaveH | Far-field | Notes |
|---|---|---|---|---|---|
| Powerboat (10m) | 20kn | 1.04 | 0.43m | 0.13m | Planing suppresses to 16% |
| Type 45 (152m) | 10kn | 0.13 | 0.67m | 0.20m | Displacement regime, full amplitude |
| Cargo (200m) | 15kn | 0.17 | 1.51m | 0.45m | Most visible case |

These amplitudes are correct per Noblesse (2008) but largely invisible against B3-4 seas (Hs 0.6-1.0m). The VS displacement primarily serves ship handling (CPU mirror) rather than visual impact.

**Amplifying amplitudes causes sawtooth artifacts.** WE's screen-space ocean grid (`surfaceDetail=3` = 480x270 vertices) maps to ~0.8m vertex spacing at 500m from camera. Wake displacement exceeding vertex spacing creates sharp triangle edges. Tested with 2x coefficient (0.5) and reduced planing -- produced severe sawtooth patterns near the powerboat.

**Future improvement: dedicated wake mesh.** A separate higher-resolution mesh around each ship, decoupled from the main ocean grid, would allow larger displacements without artifacts. Requires a second render pass with its own vertex density.

## Ship Motion Physics

### Overview

Ship heave, pitch, and roll are driven by the FFT ocean surface via second-order damped harmonic oscillators. Each DOF:

```
x'' + 2*zeta*omega_n*x' + omega_n^2*x = omega_n^2 * excitation(t)
```

This replaces the previous fake sinusoidal motion (`weather * angle * sin(t * 2PI / period)`) that was unrelated to the actual wave surface.

### Wave Excitation

- **Heave**: `getWaveHeight(shipX, shipZ)` at ship CG
- **Pitch**: `atan2(h_bow - h_stern, L)` -- wave slope along ship length
- **Roll**: `atan2(h_port - h_stbd, B)` -- wave slope across ship beam

Five wave height samples per frame (CG, bow, stern, port, stbd).

### Natural Periods

- **Heave**: `T = 2*PI*sqrt(2*T_draught/g)` (~6.4s for 5m draft)
- **Roll**: From `RollPeriod` in boat.ini, or `2*PI * 0.38*B / sqrt(g*GM)`
- **Pitch**: From `PitchPeriod` in boat.ini, or `0.8 * T_roll`

### Damping Ratios

| DOF | Zeta | Notes |
|-----|------|-------|
| Heave | 0.30 | Heavily damped by waterplane area |
| Pitch | 0.20 | Can override via `PitchDamping` in boat.ini |
| Roll | 0.10 | Lowest damping, most oscillatory. Override via `RollDamping` |

### Ship Length vs Wavelength Reduction

Large ships bridge over short waves. Excitation is multiplied by a sinc filter:

```
reduction = |sin(PI * L_eff * cos(mu) / lambda) / (PI * L_eff * cos(mu) / lambda)|
```

Where `L_eff = L` for pitch, `B` for roll. A 200m ferry in 60m waves sees pitch reduction of ~0.15. A 15m boat sees ~1.0.

### Weather Effects on Controls

**Added Resistance in Waves (Stawave-1, ITTC)**:
```
RAW = 0.25 * rho * g * Hs^2 * B * cos^2(mu) / sqrt(L)
```
Head seas: maximum resistance. Following seas: minimal. At B7, a 100m ship loses ~30% calm-water speed in head seas.

**Directional yaw buffeting**: Beam seas cause more yaw disturbance than head/following:
```
beamSeaFactor = |sin(mu)|
rateOfTurn += buffet * weather * beamSeaFactor * noise * dt
```

**Rudder sea state factor**: `1 - 0.03 * weather * 12`. At B5 rudder is 85% effective, at B8 76%.

### boat.ini Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `Swell` | Base swell magnitude (legacy, still used for amplitude context) | 0 |
| `RollPeriod` | Natural roll period (s) | auto from GM |
| `PitchPeriod` | Natural pitch period (s) | 0.8 * T_roll |
| `Buffet` | Yaw buffeting magnitude | 0 |
| `GM` | Metacentric height (m) | B * 0.06 |
| `RollDamping` | Roll damping ratio | 0.10 |
| `PitchDamping` | Pitch damping ratio | 0.20 |

## Runtime Shader Compilation

WE compiles shaders at runtime via DXC. Edit the `.hlsl` and delete any `.cso`/`.wishadermeta` files. No separate build step.

**HLSL half-precision**: Use `f` suffix on all float literals (e.g. `3.0f` not `3.0`). Bare literals cause compile failures with half4 types.

## Common Mistakes

**choppy_scale + high amplitude**: At patch=250m, energy is much higher. Keep amplitude in 2-50 range.

**time_scale = 1.0**: Looks unnaturally fast. WE default 0.3 looks natural.

**Sine waves for normal perturbation**: Periodic functions create visible checkerboard. Use simplex noise instead.

**Frequent Create() calls**: Regenerates H(0) with new random phases = visual pop. Only when wind changes >30% speed or >30 deg direction.

**Jacobian fold for foam**: At patch=250m, fold values are 1-10+ even in moderate seas. Simple thresholding produces either scum everywhere or all-white ocean. Use gradient steepness instead.

**Bare float literals in HLSL**: Must use `f` suffix (e.g. `3.0f`). Bare `3.0` causes DXC compile failure with half precision types. "Shaders invalid" console message results.

## Phillips Spectrum (WE Implementation)

```
P(K) = A * exp(-1/(L^2 * K^2)) / K^6 * (K . W)^2
```

A = wave_amplitude * 1e-7, L = V^2/g (cm), K = wave vector, W = unit wind direction. K^6 with unnormalized dot = standard K^4 with cos^2(theta).

## Files

- `src/OceanMath.hpp` -- Beaufort mapping, Hs-normalized spectrum math (pure math, testable)
- `src/graphics/wicked/WickedOceanSpectrum.hpp` -- PhillipsHasselmann spectrum, computeSpectrumScale()
- `src/WaveMotionModel.hpp` -- Ship motion oscillators, wavelength reduction, added resistance
- `src/graphics/wicked/WickedWater.cpp` -- Primary ocean setup and per-frame update
- `src/graphics/wicked/WickedWater.hpp` -- Interface
- `src/graphics/wicked/WickedMultiCascadeOcean.cpp` -- Multi-cascade framework + CPU wake height (`computeWakeHeightAt`)
- `src/WickedMain.cpp` -- Wake data upload (wakeShips, wakeTrail), weather-to-ocean wiring
- `WickedEngine/wiOcean.cpp` -- FFT ocean implementation (upstream, spectrumCallback hook)
- `WickedEngine/wiOcean.h` -- OceanParameters struct, WakeShip/WakeTrailPoint statics
- `WickedEngine/shaders/ShaderInterop_Ocean.h` -- OceanWakeShip, OceanWakeTrailPoint CB structs
- `WickedEngine/shaders/oceanSurfaceVS.hlsl` -- 3D wake displacement (Noblesse/Kelvin, live + trail)
- `WickedEngine/shaders/oceanSurfacePS.hlsl` -- Foam + anti-tiling + wake foam + Kelvin V-arm envelope
- `WickedEngine/shaders/oceanUpdateGradientFoldingCS.hlsl` -- Jacobian fold computation
