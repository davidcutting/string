// Shared procedural sky model, imported by the sky background pass (sky_shader.slang) and the lit
// shader's image-based ambient so the visible sky and the ambient it casts stay consistent.
//
// A smooth zenith -> horizon -> ground gradient in the ray's up component, plus a soft sun disc and
// a wide glow around the sun direction. Returns LINEAR HDR radiance (the composite tonemaps it).

static const float SKY_PI = 3.14159265359;

// Pale horizon band: brighter and desaturated relative to the zenith. Scale-INVARIANT (brief
// 07 units: colors are radiance in kilo-nits now, so the old absolute 0.85 constant would
// invert into a dark band).
float3 sky_horizon(float3 zenith)
{
    float lum = dot(zenith, float3(0.2126, 0.7152, 0.0722));
    return lerp(zenith, float3(lum), 0.6) * 2.0;
}

// Ground-band radiance from a Lambertian ground plane (brief 07 TOD fix). The old constant ground
// radiance kept radiating noon-warm at dusk/night — and the ground band dominates the SH irradiance
// seen by ceilings, so interior vaults glowed gold at blue hour. Instead the ground is a constant
// ALBEDO and its radiance is lit dynamically: L = albedo/pi * (E_sun + E_sky), where the sun term
// is the current sun illuminance projected onto the ground plane (dims/warms through sunset, zero
// with the sun below the horizon) and the sky term is the irradiance of the current zenith/horizon
// gradient (E = pi * mean hemisphere radiance, approximated by the zenith/horizon average) — so a
// moonless night ground goes near-black. Used by both the visible sky and the IBL capture.
public float3 sky_ground_radiance(float3 sun_dir, float3 sun_color, float sun_intensity,
                                  float3 zenith, float3 ground_albedo)
{
    float3 e_sun = sun_color * sun_intensity * clamp(normalize(sun_dir).y, 0.0, 1.0);
    float3 e_sky = SKY_PI * 0.5 * (zenith + sky_horizon(zenith));
    return ground_albedo / SKY_PI * (e_sun + e_sky);
}

// Just the sky gradient (no sun) — used for diffuse ambient, so the sun isn't double-counted (the
// lit shader already has an explicit, shadowed sun term). `ground` is the ground-band RADIANCE
// (see sky_ground_radiance; callers pass the dynamically lit value, not the albedo).
public float3 sky_gradient(float3 dir, float3 zenith, float3 ground)
{
    float up = normalize(dir).y;
    float3 horizon = sky_horizon(zenith);
    float3 upper = lerp(horizon, zenith, pow(clamp(up, 0.0, 1.0), 0.5));  // horizon -> zenith going up
    return lerp(upper, ground, clamp(-up * 4.0, 0.0, 1.0));      // horizon -> ground going down
}

// Environment variant for the IBL capture (brief 07): gradient + the broad glow, NO sun disc.
// The sun is an analytic, shadowed direct light — capturing its disc into the prefiltered
// environment would double-count it (and alias badly in the GGX prefilter). The glow is kept so
// rough reflections warm toward the sun; it is broad and prefilters cleanly.
public float3 sky_env(float3 dir, float3 sun_dir, float3 zenith, float3 ground_albedo,
                      float3 sun_color, float sun_intensity)
{
    float3 d = normalize(dir);
    float3 ground = sky_ground_radiance(sun_dir, sun_color, sun_intensity, zenith, ground_albedo);
    float3 col = sky_gradient(d, zenith, ground);
    float c = max(dot(d, normalize(sun_dir)), 0.0);
    float glow = pow(c, 256.0) * 0.5 + pow(c, 8.0) * 0.1;
    float above = smoothstep(-0.05, 0.05, d.y);
    return col + sun_color * glow * 20.0 * above;   // kilo-nit units (brief 07 M4)
}

// Full sky: gradient plus a sun disc and glow. Used for the visible background and for specular
// reflections (a real sun glint on smooth/metallic surfaces).
public float3 sky(float3 dir, float3 sun_dir, float3 zenith, float3 ground_albedo,
                  float3 sun_color, float sun_intensity)
{
    float3 d = normalize(dir);
    float3 ground = sky_ground_radiance(sun_dir, sun_color, sun_intensity, zenith, ground_albedo);
    float3 col = sky_gradient(d, zenith, ground);

    float c = max(dot(d, normalize(sun_dir)), 0.0);
    float disc = smoothstep(0.9995, 0.9998, c);             // ~few-degree disc
    float glow = pow(c, 256.0) * 0.5 + pow(c, 8.0) * 0.1;   // tight + broad glow
    float above = smoothstep(-0.05, 0.05, d.y);             // fade the sun out below the horizon
    // Kilo-nit units (brief 07 M4): the visible disc is clamped far below the real ~1.6e6 knits
    // (it tonemaps to pure white regardless; fp16 headroom is the constraint), the glow matches
    // sky_env so the visible halo and the captured environment agree.
    return col + sun_color * (disc * 2000.0 + glow * 20.0) * above;
}
