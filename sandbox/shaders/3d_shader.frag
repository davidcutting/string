#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Forward metallic-roughness PBR: one directional sun + hemispheric ambient, normal-mapped. All
// textures live in the bindless combined-image-sampler array (set 0, binding 1); the per-draw slots
// arrive from the vertex stage. Output is linear HDR — tonemapping happens in the composite pass.
layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(push_constant) uniform Push {
    mat4 view_proj;
    uvec2 vertex_address;   // buffer_reference (unused here; keeps the block matching the vertex stage)
    uint drawdata_slot;
    uint _pad0;
    vec3 camera_pos;      float _pad1;
    vec3 sun_dir;         float sun_intensity;
    vec3 sun_color;       float _pad2;
    vec3 ambient_sky;     float _pad3;
    vec3 ambient_ground;  float _pad4;
    mat4 light_view_proj;
    uint shadow_slot;
    float shadow_texel;
    float shadow_bias;
    float shadow_normal_offset;
} pc;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec3 fragWorldNormal;
layout(location = 3) in flat vec4 fragBaseColor;
layout(location = 4) in flat uint fragBaseSlot;
layout(location = 5) in flat uint fragNormalSlot;
layout(location = 6) in flat uint fragMrSlot;
layout(location = 7) in flat vec2 fragMetalRough;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// Perturb the geometric normal by a tangent-space normal map WITHOUT precomputed tangents, deriving
// the TBN from screen-space derivatives of position and uv (Christian Schüler's cotangent frame).
vec3 apply_normal_map(vec3 N, vec3 world_pos, vec2 uv, vec3 n_tangent) {
    vec3 dp1 = dFdx(world_pos);
    vec3 dp2 = dFdy(world_pos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float inv_max = inversesqrt(max(dot(T, T), dot(B, B)));
    mat3 TBN = mat3(T * inv_max, B * inv_max, N);
    return normalize(TBN * n_tangent);
}

// GGX/Trowbridge-Reitz normal distribution.
float distribution_ggx(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Smith geometry with Schlick-GGX, height-correlated-ish (separable) for direct lighting.
float geometry_smith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Fraction of the fragment in shadow (0 = lit, 1 = fully shadowed) via 3x3 PCF against the
// directional shadow map. Reverse-Z: nearer = larger depth, so a fragment is shadowed when it is
// FARTHER (smaller depth) than the recorded occluder, minus a slope-scaled bias.
float shadow_factor(vec3 world_pos, vec3 geom_n, float NdotL) {
    // Normal-offset bias: sample from a point pushed off the surface along its geometric normal,
    // which removes acne on grazing surfaces far better than depth bias alone (and avoids the
    // peter-panning a large depth bias would cause).
    vec3 p = world_pos + geom_n * pc.shadow_normal_offset;
    vec4 lp = pc.light_view_proj * vec4(p, 1.0);
    vec3 ndc = lp.xyz / lp.w;                 // ortho: w == 1
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;  // outside the map -> lit
    float frag_depth = ndc.z;
    float bias = pc.shadow_bias * (1.0 + 2.0 * (1.0 - NdotL));
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder = texture(textures[nonuniformEXT(pc.shadow_slot)],
                                     uv + vec2(x, y) * pc.shadow_texel).r;
            shadow += (frag_depth < occluder - bias) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

void main() {
    // Material inputs. Base color is sRGB (decoded in hardware); MR + normal maps are linear data.
    vec4 base = texture(textures[nonuniformEXT(fragBaseSlot)], fragTexCoord) * fragBaseColor;
    vec3 albedo = base.rgb;

    vec3 mr = texture(textures[nonuniformEXT(fragMrSlot)], fragTexCoord).rgb;
    float roughness = clamp(mr.g * fragMetalRough.y, 0.04, 1.0);   // glTF: roughness = green
    float metallic = clamp(mr.b * fragMetalRough.x, 0.0, 1.0);     //       metallic  = blue

    vec3 n_tangent = texture(textures[nonuniformEXT(fragNormalSlot)], fragTexCoord).xyz * 2.0 - 1.0;
    vec3 N = normalize(fragWorldNormal);
    // Two-sided: face the geometric normal toward the viewer before perturbing.
    vec3 V = normalize(pc.camera_pos - fragWorldPos);
    if (dot(N, V) < 0.0) N = -N;
    N = apply_normal_map(N, fragWorldPos, fragTexCoord, n_tangent);

    vec3 L = normalize(pc.sun_dir);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance specular for the directional sun.
    float D = distribution_ggx(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3 F = fresnel_schlick(VdotH, F0);
    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);

    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / PI;
    // The directional sun is occluded by the shadow map; ambient (below) is unshadowed fill. Bias
    // the lookup off the geometric (unperturbed) normal.
    float shadow = shadow_factor(fragWorldPos, normalize(fragWorldNormal), NdotL);
    vec3 direct = (diffuse + specular) * pc.sun_color * pc.sun_intensity * NdotL * (1.0 - shadow);

    // Hemispheric ambient (cheap sky/ground fill; ao = 1 until an occlusion map is added).
    vec3 ambient = mix(pc.ambient_ground, pc.ambient_sky, N.y * 0.5 + 0.5) * albedo;

    outColor = vec4(direct + ambient, base.a);
}
