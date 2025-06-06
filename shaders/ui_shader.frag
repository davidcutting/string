#version 450

layout(push_constant) uniform PushConstants {
    vec2 screen_size;
    uint num_shapes;
    float delta_time;
} pc;

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

void main() {
    // note: set up basic colors
    vec3 color = black;
    // note: draw circle sdf
    float radius = 0.3;
    // radius = 3.0;
    vec2 center = vec2(0.0, 0.0);
    // center = vec2(sin(2.0 * u_time), 0.0);
    float distanceToCircle = sdfCircle(uv - center, radius);

    //fragColor = circle_waves(radius, center, distanceToCircle);

    fragColor = circle_no_fill_outlined(radius, center, distanceToCircle, 0.1);

    //fragColor = vec4(color, 1.0);
}