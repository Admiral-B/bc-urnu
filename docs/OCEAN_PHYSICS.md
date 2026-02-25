# Ocean Physics -- WickedEngine Integration

## WE Ocean Pipeline

WE uses a GPU-accelerated FFT ocean based on the NVIDIA OceanCS/CUDA oceanFFT samples. The Phillips spectrum generates initial wave heights H(0), which are evolved in frequency domain and transformed to spatial displacement maps each frame.

Key: WE's internal physics uses **CGS units** (gravity = 981 cm/s^2, wind_speed in cm/s). The `1e-7` scaling on `wave_amplitude` absorbs the unit conversion between the CGS physics and world-space rendering.

## Critical Constraint: patch_length = 50

**patch_length MUST stay at 50** (WE default). WE's adaptive ocean mesh has 2^surfaceDetail subdivisions per patch side. With surfaceDetail=4 that's 16 quads per side. At patch_length=200+, each quad spans 12+ meters causing extreme vertex spacing and catastrophic mesh breakdown (mountain-like artifacts). Tested and confirmed:

- patch_length=50: correct geometry
- patch_length=200: visible artifacts, oversized choppy waves
- patch_length=500: complete mesh breakdown
- patch_length=1000: complete mesh breakdown

Max representable wavelength at patch_length=50 is 50m, which covers dominant waves up to ~Beaufort 5.

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
| `patch_length` | FFT tile size. **Must be 50.** | 50 (fixed) |
| `wave_amplitude` | Phillips spectrum constant A, scaled by 1e-7 internally. NOT wave height. | 2-3500 |
| `wind_speed` | Wind velocity in cm/s. Drives dominant wavelength L = V^2/g. | 30-3500 |
| `choppy_scale` | Gerstner horizontal displacement. Drives foam via Jacobian fold. | 0.8-2.0 |
| `dmap_dim` | FFT resolution. Must be power of 2. | 256, 512 |
| `time_scale` | Wave animation speed multiplier. **0.3 recommended** (WE default). | 0.3-1.0 |
| `wind_dependency` | Anti-wind wave energy. 0 = directional, 1 = omnidirectional. | 0.07-0.35 |
| `surfaceDetail` | Mesh LOD level. 2^n quads per patch side. | 3-5 |
| `surfaceDisplacementTolerance` | Max vertex displacement for LOD culling. | 1-4 |

## Beaufort Mapping

`wave_amplitude` scales via lookup table. `choppy_scale` = 0.8 + weather * 0.1. `wind_speed` from Beaufort midpoint knots -> m/s -> cm/s.

| B | Wind (cm/s) | wave_amplitude | choppy_scale | time_scale |
|---|---|---|---|---|
| 0 | 30 (floor) | 2 | 0.80 | 0.3 |
| 1 | 103 | 8 | 0.90 | 0.3 |
| 3 | 440 | 40 | 1.10 | 0.3 |
| 5 | 980 | 200 | 1.30 | 0.3 |
| 7 | 1540 | 500 | 1.50 | 0.3 |
| 9 | 2260 | 1200 | 1.70 | 0.3 |
| 12 | 3500 | 3500 | 2.00 | 0.3 |

## Foam / Whitecap System

Three foam sources in `oceanSurfacePS.hlsl`:

1. **Shore foam** (`foam_shore`): Depth buffer comparison. `exp(-depth_diff * 0.7)` -- visible surf zone ~4m wide at waterline.

2. **Shallow water breaking** (`foam_shallow`): `exp(-water_depth * 0.25) * 0.5` -- broad foam zone where seabed < ~5m below surface. Uses refraction pass depth (zero cost in deep water where water_depth = FLT_MAX).

3. **Open ocean whitecaps** (`foam_wave`): `pow(fold, 3)` from Jacobian fold factor. No depth suppression -- appears anywhere fold > 0 (B3+). Reduced from WE default `pow(fold, 4)` for slightly earlier onset.

All three are modulated by 3 octaves each of simplex + Voronoi noise for patchy appearance.

**Jacobian fold**: `J = (1 + Dx.x)(1 + Dy.y) - Dx.y * Dy.x` where Dx/Dy = displacement derivatives \* choppy\_scale \* gridLen. `fold = max(1 - J, 0)`.

## Anti-Tiling System

FFT tiles every 50m. Three techniques break visible repetition in `oceanSurfacePS.hlsl`:

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

1. **Normal steepening** (`shallow_factor`): Gradient amplified up to 2.5x in water < 8m deep. Simulates wave energy conservation during shoaling -- waves grow taller as they slow down.

2. **Wavelength compression**: Gradient map re-sampled at UV\*2.0 near shore (UV\*1.0 in deep water). Simulates phase velocity decrease causing wavelength to shorten. Blended 50% with original gradients to avoid aliasing.

3. **Shore wave lines** (`water_depth < 15m`): Two sets of animated foam bands using `frac(water_depth * freq - time * speed)`. Bands are automatically parallel to depth contours (= parallel to coastline). Noise-modulated to avoid uniform rings.

4. **Shallow water foam** (`foam_shallow`): Broad foam zone where seabed < ~5m below surface. `exp(-water_depth * 0.25) * 0.5`.

### Limitations

- No geometric displacement changes near shore (VS can't see depth)
- Shore wave lines use `water_depth` as distance proxy -- irregular bathymetry can cause irregular spacing
- No wave refraction toward shore (would need per-pixel wave direction from bathymetry gradient)
- No actual breaking wave geometry (Horizon Forbidden West uses dedicated curve-based mesh deformation for this)

### Future: Depth Cache for Vertex Shader

The Crest Ocean System approach: render terrain from above into an orthographic depth texture, bind to VS, attenuate displacement in shallow water. Would solve waves-clipping-through-terrain and enable geometric shoaling. Requires C++ pipeline changes (new render pass + texture binding).

## Horizon Distance Fade

All custom perturbations (gradient re-sampling, simplex noise, foam detail noise) are faded to zero beyond ~2km via `custom_fade`. At the horizon, the ocean mesh is extremely coarse and high-frequency noise creates aliasing artifacts. Only WE's built-in near/far gradient blend survives at distance.

```
custom_fade = saturate(1 - saturate(dist * 0.0005 - 0.5) * 2.0)  // 1.0 at <1km, 0.0 at >2km
```

Simplex noise and foam detail noise are branched out entirely when `custom_fade < 0.01` (saves GPU).

## Wind Dependency

`wind_dependency` controls energy in anti-wind direction only:
```
if (Kcos < 0) phillips *= dir_depend;
```
- 0.07 = very directional (almost no opposing waves)
- 0.35 = significant opposing wave energy, breaks up uniform pattern
- Does NOT affect cross-wind energy -- always suppressed by Phillips cos^2(theta)

## Ship Physics Integration

`OwnShip.cpp` queries `getWaveHeight()` for vertical displacement and applies:
- `waveHeightFiltered`: EMA-filtered wave height added to ship Y
- `pitch`/`roll`: Sinusoidal oscillation scaled by `Swell * weather`
- `buffet`: Random heading disturbance

For large vessels (ferries): set `Buffet` very low (0.02) in boat.ini.

## Runtime Shader Compilation

WE compiles shaders at runtime if `.hlsl` is newer than `.cso`. Edit the `.hlsl`, delete the `.cso`, and the game recompiles on launch via `dxcompiler.dll`. No separate build step.

## Common Mistakes

**patch_length > 50**: Breaks the mesh. Mountain artifacts. Do not increase.

**choppy_scale + high amplitude**: Values > 1.0 CAN cause mesh fold-over when combined with high wave_amplitude. At low Beaufort, choppy_scale up to 2.0 is safe.

**time_scale = 1.0**: Looks unnaturally fast. WE default 0.3 looks natural.

**Sine waves for normal perturbation**: Periodic functions create visible checkerboard. Use simplex noise instead.

**Frequent Create() calls**: Regenerates H(0) with new random phases = visual pop. Only when wind changes >30% speed or >30 deg direction.

## Phillips Spectrum (WE Implementation)

```
P(K) = A * exp(-1/(L^2 * K^2)) / K^6 * (K . W)^2
```

A = wave_amplitude * 1e-7, L = V^2/g (cm), K = wave vector, W = unit wind direction. K^6 with unnormalized dot = standard K^4 with cos^2(theta).

## Files

- `src/graphics/wicked/WickedWater.cpp` -- Primary ocean setup and per-frame update
- `src/graphics/wicked/WickedWater.hpp` -- Interface
- `src/graphics/wicked/WickedMultiCascadeOcean.cpp` -- Multi-cascade framework (auxiliary cascades not yet active)
- `WickedEngine/wiOcean.cpp` -- FFT ocean implementation (upstream)
- `WickedEngine/wiOcean.h` -- OceanParameters struct (upstream)
- `WickedEngine/shaders/oceanUpdateGradientFoldingCS.hlsl` -- Jacobian fold computation
- `WickedEngine/shaders/oceanSurfacePS.hlsl` -- Foam + anti-tiling + normal perturbation (modified)
