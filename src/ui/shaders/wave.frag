#version 440
// A swell rolling left to right, horizon level.
// Gerstner is parametric, so the per-pixel fill inverts it with Newton. Speed goes as sqrt(k),
// which is how deep-water gravity waves disperse.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float time;
    float aspect;
    float flow;    // radians below the horizontal; 0 rolls straight right
    float foam;
    vec4 tintA;
    vec4 tintB;
};

float hash11(float p) { return fract(sin(p * 127.1) * 43758.5453); }
float hash(vec2 p)    { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { v += a * noise(p); p = p * 2.03 + 17.0; a *= 0.5; }
    return v;
}

vec4 over(vec4 base, vec3 rgb, float a) {
    a = clamp(a, 0.0, 1.0);
    float outA = a + base.a * (1.0 - a);
    vec3 outRgb = outA > 0.0 ? (rgb * a + base.rgb * base.a * (1.0 - a)) / outA : vec3(0.0);
    return vec4(outRgb, outA);
}

struct Surface {
    float h;
    float steep;
    float lift;    // 0..1
};

Surface gerstner(float s, vec3 amp, vec3 k, vec3 w, vec3 ph, float rest) {
    // A fixed point converges at ~sum(A*k) per pass, visibly wrong at breaking steepness.
    float u = s;
    vec3 theta;
    for (int i = 0; i < 4; ++i) {
        theta = k * u - w * time + ph;
        float d = dot(amp, sin(theta));
        float dsdu = 1.0 - dot(amp * k, cos(theta));
        // dsdu reaches zero at breaking; the floor keeps the step finite.
        u -= (u - d - s) / (abs(dsdu) < 0.08 ? (dsdu < 0.0 ? -0.08 : 0.08) : dsdu);
    }
    theta = k * u - w * time + ph;
    vec3 c = cos(theta), sn = sin(theta);

    float total = amp.x + amp.y + amp.z;
    float lift = dot(amp, c);
    float dhdu = dot(amp * k, sn);
    float dsdu = 1.0 - dot(amp * k, c);

    Surface surf;
    surf.h = rest - lift;
    // ~0.76 on the breaking face, ~0.26 over the flats, which the foam thresholds assume.
    surf.steep = clamp(abs(dhdu) / max(abs(dsdu), 0.08), 0.0, 1.0);
    surf.lift = clamp((lift + total) / (2.0 * total + 1e-4), 0.0, 1.0);
    return surf;
}

// How deep below its own crest the layer paints.
vec4 waveLayer(vec4 col, float s, float h, vec3 amp, vec3 k, vec3 w, vec3 ph,
               float rest, vec3 tint, float bodyA, float foamA, float reach, float haze) {
    Surface surf = gerstner(s, amp, k, w, ph, rest);
    float e = max(fwidth(h) * 1.5, 0.0035);
    float below = h - surf.h;
    float body = smoothstep(-e, e, below) * smoothstep(reach + 0.25, reach, below);
    if (body <= 0.0 && below > 0.0) return col;

    if (body > 0.0) {
        // Beer-Lambert in spirit.
        float depth = clamp(below / 0.55, 0.0, 1.0);
        vec3 shallow = mix(tint, vec3(1.0), 0.42);
        vec3 deep = tint * 0.30;
        vec3 water = mix(shallow, deep, depth * depth * 0.85 + depth * 0.15);

        float sss = surf.lift * (1.0 - smoothstep(0.0, 0.34, below));
        water = mix(water, mix(tint, vec3(1.0), 0.72), sss * 0.55);

        float caustic = noise(vec2(s * 3.1 - time * 0.55, below * 7.0 + time * 0.3))
                      * noise(vec2(s * 1.7 + time * 0.3, below * 3.0));
        water += (caustic - 0.28) * 0.13 * mix(tint, vec3(1.0), 0.6);

        col = over(col, mix(water, vec3(1.0), haze * 0.35), body * bodyA);

        float ribbon = (1.0 - smoothstep(0.0, 0.035, abs(below - 0.16 - sin(s * 1.9 - time * 0.9) * 0.04)))
                     + (1.0 - smoothstep(0.0, 0.028, abs(below - 0.38 - sin(s * 1.3 + time * 0.7) * 0.05))) * 0.6;
        col = over(col, vec3(1.0), ribbon * 0.10 * body);
    }

    // A dense cap at the crest, trails feathered down the face.
    float breaking = smoothstep(0.38, 0.70, surf.steep) * smoothstep(0.28, 0.70, surf.lift);
    if (breaking > 0.001) {
        float fingers = fbm(vec2(s * 16.0 - time * 1.4, time * 0.6));
        float capDepth = (0.045 + 0.075 * fingers) * (0.5 + 0.5 * breaking);
        float cap = smoothstep(capDepth, capDepth * 0.25, below) * step(-e, below);
        col = over(col, vec3(1.0, 0.995, 0.98), cap * breaking * 0.95 * foamA);

        float lip = 1.0 - smoothstep(0.0, e * 5.0 + 0.012, abs(below + 0.035 * breaking));
        col = over(col, vec3(1.0), lip * breaking * 0.85 * foamA);

        float trail = fbm(vec2(s * 7.0 - time * 2.2, below * 9.0 + time * 0.5));
        col = over(col, vec3(1.0),
                   body * smoothstep(0.42, 0.0, below) * breaking
                   * smoothstep(0.45, 0.85, trail) * 0.55 * foamA);
    }

    float rim = 1.0 - smoothstep(0.0, e * 3.0, abs(below));
    col = over(col, mix(tint, vec3(1.0), 0.75), rim * 0.28 * (1.0 - haze * 0.5));
    return col;
}

// Screen space: spray is thrown along travel and falls straight down, and that mismatch is
    // what tells the eye which way is down.
float spray(vec2 xy, vec2 dir, float rest, float amp) {
    float acc = 0.0;
    for (int i = 0; i < 14; ++i) {
        float fi = float(i) + 1.0;
        float rate = 0.30 + 0.30 * hash11(fi * 3.7);
        float life = fract(hash11(fi * 1.3) + time * rate);
        float along = hash11(fi * 7.1) * (aspect + 1.0);
        vec2 origin = dir * along + vec2(-dir.y, dir.x) * (rest - amp * 1.25);
        vec2 at = origin
                + dir * (0.18 + 0.45 * hash11(fi * 5.3)) * life * 1.6
                + vec2(0.0, -0.75 * life + 1.45 * life * life);
        float r = 0.006 + 0.014 * hash11(fi * 11.7);
        float fade = smoothstep(0.0, 0.12, life) * (1.0 - smoothstep(0.55, 1.0, life));
        acc += smoothstep(r, 0.0, length(xy - at)) * fade;
    }
    return clamp(acc, 0.0, 1.0);
}

void main() {
    // x scaled by aspect, so the rotation is not sheared.
    vec2 xy = vec2(qt_TexCoord0.x * max(aspect, 0.001), qt_TexCoord0.y);

    float cs = cos(flow), sn = sin(flow);
    vec2 dir  = vec2(cs, sn);
    vec2 into = vec2(-sn, cs);
    float s = dot(xy, dir);
    float h = dot(xy, into);

    vec4 col = vec4(0.0);

    // sum(A*k) near 0.8 - the Gerstner steepness, which must stay under 1.
    const float g = 0.55;

    col = waveLayer(col, s, h,
                    vec3(0.030, 0.009, 0.002), vec3(9.0, 18.0, 34.0),
                    g * sqrt(vec3(9.0, 18.0, 34.0)), vec3(0.0, 2.3, 4.1),
                    0.113, tintB.rgb, 0.34, 0.40, 0.20, 0.55);

    col = waveLayer(col, s, h,
                    vec3(0.055, 0.014, 0.004), vec3(7.0, 14.5, 27.0),
                    g * sqrt(vec3(7.0, 14.5, 27.0)), vec3(1.7, 3.9, 0.6),
                    0.263, mix(tintA.rgb, tintB.rgb, 0.4), 0.60, 0.75, 0.32, 0.22);

    col = waveLayer(col, s, h,
                    vec3(0.085, 0.020, 0.005), vec3(5.5, 11.5, 22.0),
                    g * sqrt(vec3(5.5, 11.5, 22.0)), vec3(4.2, 0.9, 2.8),
                    0.423, tintA.rgb, 0.94, 1.00, 3.00, 0.0);

    col = over(col, vec3(1.0), spray(xy, dir, 0.423, 0.110) * 0.75 * foam);

    float edges = smoothstep(0.0, 0.10, qt_TexCoord0.x) * smoothstep(1.0, 0.90, qt_TexCoord0.x);
    fragColor = vec4(col.rgb * col.a, col.a) * edges * qt_Opacity;
}
