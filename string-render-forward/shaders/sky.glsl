// Shared procedural sky model, #included by the sky background pass (sky.frag) and the lit shader's
// image-based ambient (3d_shader.frag) so the visible sky and the ambient it casts stay consistent.
//
// A smooth zenith -> horizon -> ground gradient in the ray's up component, plus a soft sun disc and
// a wide glow around the sun direction. Returns LINEAR HDR radiance (the composite tonemaps it).
#ifndef SKY_GLSL
#define SKY_GLSL

// Just the sky gradient (no sun) — used for diffuse ambient, so the sun isn't double-counted (the
// lit shader already has an explicit, shadowed sun term).
vec3 sky_gradient(vec3 dir, vec3 zenith, vec3 ground) {
    float up = normalize(dir).y;
    vec3 horizon = mix(zenith, vec3(0.85), 0.35);          // pale (not white) band at the horizon
    vec3 upper = mix(horizon, zenith, pow(clamp(up, 0.0, 1.0), 0.5));  // horizon -> zenith going up
    return mix(upper, ground, clamp(-up * 4.0, 0.0, 1.0));  // horizon -> ground going down
}

// Full sky: gradient plus a sun disc and glow. Used for the visible background and for specular
// reflections (a real sun glint on smooth/metallic surfaces).
vec3 sky(vec3 dir, vec3 sun_dir, vec3 zenith, vec3 ground, vec3 sun_color) {
    vec3 d = normalize(dir);
    vec3 col = sky_gradient(d, zenith, ground);

    float c = max(dot(d, normalize(sun_dir)), 0.0);
    float disc = smoothstep(0.9995, 0.9998, c);             // ~few-degree disc
    float glow = pow(c, 256.0) * 0.5 + pow(c, 8.0) * 0.1;   // tight + broad glow
    float above = smoothstep(-0.05, 0.05, d.y);             // fade the sun out below the horizon
    return col + sun_color * (disc * 40.0 + glow) * above;
}

#endif
