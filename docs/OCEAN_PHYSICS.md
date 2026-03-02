# Ocean Physics -- WickedEngine Integration

## WE Ocean Pipeline

WE uses a GPU-accelerated FFT ocean based on the NVIDIA OceanCS/CUDA oceanFFT samples. The Phillips spectrum generates initial wave heights H(0), which are evolved in frequency domain and transformed to spatial displacement maps each frame.

Key: WE's internal physics uses **CGS units** (gravity = 981 cm/s^2, wind_speed in cm/s). The `1e-7` scaling on `wave_amplitude` absorbs the unit conversion between the CGS physics and world-space rendering.

## Ocean Configuration: patch_length = 250m

`patch_length` controls the FFT tile size. Using 250m (up from the WE default 50m) eliminates visible geometric tiling at ship scale.

Physics impact: At patch=250m, K_min drops from 0.126 to 0.025 rad/m. Phillips K^-6 scaling gives ~15000x more energy per mode at K_min. Total FFT energy is ~1000x higher than at patch=50m, so `wave_amplitude` values are ~15x lower to compensate. Wind speed (via Phillips) carries the Beaufort energy scaling.

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
| `patch_length` | FFT tile size in meters. | 250 |
| `wave_amplitude` | Phillips spectrum constant A, scaled by 1e-7 internally. NOT wave height. | 2-50 |
| `wind_speed` | Wind velocity in cm/s. Drives dominant wavelength L = V^2/g. Capped at 2000. | 30-2000 |
| `choppy_scale` | Gerstner horizontal displacement. 3x multiplied to compensate GridLen reduction. | 0.4-1.3 |
| `dmap_dim` | FFT resolution. Must be power of 2. | 256, 512 |
| `time_scale` | Wave animation speed multiplier. **0.3 recommended** (WE default). | 0.3-1.0 |
| `wind_dependency` | Anti-wind wave energy. 0 = directional, 1 = omnidirectional. | 0.07-0.35 |
| `surfaceDetail` | Mesh LOD level. 2^n quads per patch side. | 3-5 |
| `surfaceDisplacementTolerance` | Max vertex displacement for LOD culling. | 2-3 |

## Beaufort Mapping

`wave_amplitude` scales via lookup table in `OceanMath.hpp`. `choppy_scale` = 0.4 + beaufort * 0.07, capped at 1.3. `wind_speed` from actual knots -> cm/s, capped at 2000 cm/s.

| B | Wind (cm/s) | wave_amplitude | choppy_scale |
|---|---|---|---|
| 0 | 30 (floor) | 2 | 0.40 |
| 1 | 103 | 3 | 0.47 |
| 3 | 440 | 8 | 0.61 |
| 5 | 980 | 16 | 0.75 |
| 7 | 1540 | 25 | 0.89 |
| 9 | 2000 (cap) | 35 | 1.03 |
| 12 | 2000 (cap) | 50 | 1.24 |

## Foam / Whitecap System

Three foam sources in `oceanSurfacePS.hlsl`:

1. **Shore foam** (`foam_shore`): Depth buffer comparison. `exp(-depth_diff * 0.7)` -- visible surf zone ~4m wide at waterline.

2. **Shallow water breaking** (`foam_shallow`): `exp(-water_depth * 0.25) * 0.5` -- broad foam zone where seabed < ~5m below surface. Uses refraction pass depth (zero cost in deep water where water_depth = FLT_MAX).

3. **Open ocean whitecaps** (`foam_wave`): `pow(saturate(crest_steepness), 2)` from gradient steepness (`length(gradient.rg) * 0.15`). This is wave slope-based, not Jacobian fold -- the fold values at current choppy_scale are 1-10+ (too broad for thresholding).

All three are modulated by 3 octaves each of simplex + Voronoi noise for patchy appearance.

**Jacobian fold**: `J = (1 + Dx.x)(1 + Dy.y) - Dx.y * Dy.x` where Dx/Dy = displacement derivatives * choppy_scale * gridLen. At patch=250m, fold values are routinely 1-10+ even in moderate seas. Direct use for foam requires very high thresholds or a different approach (Burst 40).

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

- `src/OceanMath.hpp` -- Beaufort mapping, spectrum math, cascade blending (pure math, testable)
- `src/WaveMotionModel.hpp` -- Ship motion oscillators, wavelength reduction, added resistance
- `src/graphics/wicked/WickedWater.cpp` -- Primary ocean setup and per-frame update
- `src/graphics/wicked/WickedWater.hpp` -- Interface
- `src/graphics/wicked/WickedMultiCascadeOcean.cpp` -- Multi-cascade framework
- `WickedEngine/wiOcean.cpp` -- FFT ocean implementation (upstream)
- `WickedEngine/wiOcean.h` -- OceanParameters struct (upstream)
- `WickedEngine/shaders/oceanUpdateGradientFoldingCS.hlsl` -- Jacobian fold computation
- `WickedEngine/shaders/oceanSurfacePS.hlsl` -- Foam + anti-tiling + normal perturbation (modified)
