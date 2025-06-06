#version 450

layout(push_constant) uniform PushConstants {
    vec2 screen_size;
    uint num_shapes;
    float delta_time;
} pc;

struct Element
{
    vec4 fill;
    vec4 stroke;
    vec2 position;
    float radius;
    float stroke_width;
};

layout(set = 0, binding = 0) uniform Camera2D {
    vec2 position;
    float zoom;
    float rotation;
} camera;

layout(set = 0, binding = 1) readonly buffer Elements {
    Element data[];
} elements;

// A coordinate in image pixel space
layout(location = 0) in vec2 pixel_loc;
// A normalized coordinate from the screen center in clip space
layout(location = 1) in vec2 uv;

layout(location = 0) out vec4 fragColor;

// https://iquilezles.org/articles/distfunctions2d/
float sdfCircle(vec2 p, float r) {
    // note: sqrt(pow(p.x, 2.0) + pow(p.y, 2.0)) - r;
    return length(p) - r;
}

vec3 black = vec3(0.0);
vec3 white = vec3(1.0);
vec3 red = vec3(1.0, 0.0, 0.0);
vec3 blue = vec3(0.65, 0.85, 1.0);
vec3 orange = vec3(0.9, 0.6, 0.3);

vec4 circle_waves(float radius, vec2 center, float distance_to_cicle)
{
    vec3 color = black;
    color = vec3(uv.x, uv.y, 0.0);
    color = distance_to_cicle > 0.0 ? white : white;
    // note: adding a black outline to the circle
    // color = color * exp(distance_to_cicle);
    // color = color * exp(2.0 * distance_to_cicle);
    // color = color * exp(-2.0 * abs(distance_to_cicle));
    // color = color * (1.0 - exp(-2.0 * abs(distance_to_cicle)));
    // color = color * (1.0 - exp(-5.0 * abs(distance_to_cicle)));
    color = color * (1.0 - exp(-5.0 * abs(distance_to_cicle)));

    // note: adding waves
    // color = color * 0.8 + color * 0.2;
    // color = color * 0.8 + color * 0.2 * sin(distance_to_cicle);
    // color = color * 0.8 + color * 0.2 * sin(50.0 * distance_to_cicle);
    color = color * 0.8 + color * 0.2 * sin(50.0 * distance_to_cicle - 4.0 * pc.delta_time);

    // note: adding white border to the circle
    // color = mix(white, color, step(0.1, distance_to_cicle));
    // color = mix(white, color, step(0.1, abs(distance_to_cicle)));
    color = mix(white, color, smoothstep(0.0, 0.1, abs(distance_to_cicle)));

    // note: thumbnail?
    // color = mix(white, color, abs(distance_to_cicle));
    // color = mix(white, color, 2.0 * abs(distance_to_cicle));
    // color = mix(white, color, 4.0 * abs(distance_to_cicle));
    return vec4(color, 1.0);
}

vec4 circle_no_fill_outlined(float radius, vec2 center, float distance_to_cicle, float outline_width)
{
    vec4 color = vec4(white, 1.0);

    //float vibey_radius = radius * sin(pc.delta_time * pc.delta_time);
    float vibey_radius = radius * sin(pc.delta_time * 1000);

    color = distance_to_cicle > vibey_radius && distance_to_cicle < vibey_radius + outline_width ? vec4(white, 1.0) : vec4(0.0, 0.0, 0.0, 0.0);

    return color;
}

vec4 render_circle(Element element, vec2 frag_coord) {
    // Calculate distance from fragment to circle center in pixel space
    float dist = sdfCircle(frag_coord - element.position, element.radius);

    // Start with transparent
    vec4 color = vec4(0.0, 0.0, 0.0, 0.0);

    // Fill the circle first
    if (dist <= 0.0) {
        color = element.fill;
    }

    // Add stroke if stroke_width > 0
    if (element.stroke_width > 0.0) {
        // Check if we're in the stroke area (ring around the circle)
        float stroke_outer = element.radius + element.stroke_width * 0.5;
        float stroke_inner = element.radius - element.stroke_width * 0.5;
        float distance_to_center = length(frag_coord - element.position);
        
        if (distance_to_center <= stroke_outer && distance_to_center >= stroke_inner) {
            color = element.stroke;
        }
    }

    return color;
}

void main() {
    //float distanceToCircle = sdfCircle(uv - center, radius);

    //fragColor = circle_waves(radius, center, distanceToCircle);

    //fragColor = circle_no_fill_outlined(radius, center, distanceToCircle, 0.1);

    //fragColor = vec4(color, 1.0);

    // Test 1: Is num_shapes > 0?
    if (pc.num_shapes == 0u) {
        fragColor = vec4(1.0, 0.0, 0.0, 1.0); // Red if no shapes
        return;
    }
    
    // Test 2: Can we access first element?
    //Element first = elements.data[0];
    //fragColor = vec4(0.0, 1.0, 0.0, 1.0); // Green if we can access
    //return;
    
    // Test 3: Show first element's fill color
    //fragColor = first.fill;
    //return;

    // Test 4: Show pixel coordinates as colors to verify pixel_loc is working
    //vec2 normalized_pixel = pixel_loc / pc.screen_size;
    //fragColor = vec4(normalized_pixel.x, normalized_pixel.y, 0.0, 1.0);
    //return;

    // Test 5: Raw pixel coordinates (see black in top left, yellow in bottom right)
    //fragColor = vec4(pixel_loc.x / 800.0, pixel_loc.y / 800.0, 0.0, 1.0);
    //return;

    // Test 6: Build a circle manually :(
    //vec2 screenCenter = pc.screen_size * 0.5; // Should be (400, 400)
    //float distToCenter = distance(pixel_loc, screenCenter);
    //if (distToCenter < 100.0) {
    //    fragColor = vec4(1.0, 0.0, 0.0, 1.0); // Red circle
    //    return;
    //}
    //fragColor = vec4(0.0, 0.0, 1.0, 1.0); // Blue background
    //return;

    vec4 final_color = vec4(0.0, 0.0, 0.0, 0.0); // Transparent back_cround
    
    // Iterate through all elements
    for (uint i = 0u; i < pc.num_shapes; i++) {
        Element element = elements.data[i];

        // Render this element
        vec4 element_color = render_circle(element, pixel_loc);
        
        if (element_color.a > 0.0)
        {
            // Alpha blending: src over dst
            final_color = element_color + final_color * (1.0 - element_color.a);
        }
    }

    fragColor = final_color;
}
