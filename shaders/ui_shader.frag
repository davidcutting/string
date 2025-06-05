#version 450

// Push constants
layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    uint primitiveCount;
} pc;

// SDF Primitive structure (must match C++ struct exactly)
struct SDFPrimitive {
    vec2 center;        // 16-byte aligned
    vec2 size;          // 16-byte aligned  
    vec4 color;         // 16-byte aligned
    float cornerRadius; // 4 bytes
    float strokeWidth;  // 4 bytes
    uint shapeType;     // 4 bytes
    uint flags;         // 4 bytes
};

// 3D Camera, yeah this doesn't work lol
layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

// Storage buffer containing all SDF primitives
layout(set = 0, binding = 1, std430) readonly buffer PrimitiveBuffer {
    SDFPrimitive primitives[];
};

// Input from vertex shader
layout(location = 0) in vec2 screenPos;

// Output color
layout(location = 0) out vec4 fragColor;

// Shape type constants
const uint SHAPE_RECTANGLE = 0u;
const uint SHAPE_CIRCLE = 1u;
const uint SHAPE_ROUNDED_RECT = 2u;
const uint SHAPE_LINE = 3u;

// Flag constants
const uint FLAG_FILLED = 1u;
const uint FLAG_STROKED = 2u;
const uint FLAG_ANTIALIASED = 4u;

// SDF functions
float sdBox(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float sdCircle(vec2 p, float r) {
    return length(p) - r;
}

float sdLineSegment(vec2 p, vec2 a, vec2 b, float thickness) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - thickness;
}

// Get distance for a primitive
float getPrimitiveDistance(SDFPrimitive prim, vec2 localPos) {
    float dist = 1000.0; // Large default distance
    
    if (prim.shapeType == SHAPE_RECTANGLE) {
        dist = sdBox(localPos, prim.size * 0.5);
    }
    else if (prim.shapeType == SHAPE_CIRCLE) {
        dist = sdCircle(localPos, prim.size.x * 0.5);
    }
    else if (prim.shapeType == SHAPE_ROUNDED_RECT) {
        dist = sdRoundedBox(localPos, prim.size * 0.5, prim.cornerRadius);
    }
    else if (prim.shapeType == SHAPE_LINE) {
        // For lines, size.x is length, size.y is thickness
        vec2 start = vec2(-prim.size.x * 0.5, 0.0);
        vec2 end = vec2(prim.size.x * 0.5, 0.0);
        dist = sdLineSegment(localPos, start, end, prim.size.y * 0.5);
    }
    
    return dist;
}

// Convert distance to alpha with antialiasing
float distanceToAlpha(float dist) {
    return 1.0 - smoothstep(0.0, fwidth(dist), dist);
}

// Process stroke rendering
float processStroke(float dist, float strokeWidth) {
    if (strokeWidth <= 0.0) return 0.0;
    
    float outerDist = dist;
    float innerDist = dist + strokeWidth;
    
    float outerAlpha = distanceToAlpha(outerDist);
    float innerAlpha = distanceToAlpha(innerDist);
    
    return outerAlpha - innerAlpha;
}

void main() {
    vec4 finalColor = vec4(0.0);
    
    // Iterate through all primitives
    for (uint i = 0u; i < pc.primitiveCount; i++) {
        SDFPrimitive prim = primitives[i];
        
        // Transform screen position to primitive-local space
        vec2 localPos = screenPos - prim.center;
        
        // Get distance to primitive
        float dist = getPrimitiveDistance(prim, localPos);
        
        float alpha = 0.0;
        vec4 primColor = prim.color;
        
        // Handle filled rendering
        if ((prim.flags & FLAG_FILLED) != 0u) {
            alpha = distanceToAlpha(dist);
        }
        
        // Handle stroke rendering
        if ((prim.flags & FLAG_STROKED) != 0u) {
            float strokeAlpha = processStroke(dist, prim.strokeWidth);
            alpha = max(alpha, strokeAlpha);
        }
        
        // Apply alpha to color
        primColor.a *= alpha;
        
        // Blend with existing color (over operator)
        if (primColor.a > 0.0) {
            finalColor.rgb = mix(finalColor.rgb, primColor.rgb, primColor.a);
            finalColor.a = finalColor.a + primColor.a * (1.0 - finalColor.a);
        }
    }
    
    fragColor = finalColor;
}