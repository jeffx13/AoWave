#version 440
// Liquid in a horizontal capsule. Coordinates are in item heights, so a 6px pipe and a 32px
// pill keep the same proportions.
// turbulent 0 = the progress pipe, 1 = the title-bar pill's shoreline wash.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float time;
    float progress;
    float buffered;
    float aspect;
    float turbulent;   // 1: wash and froth (pill); 0: clean boundary (pipe)
    float intensity;
    float bubbles;     // master alpha; 0 switches the field off
    float bubbleScale; // radius as a multiple of the default, so a 6px pipe can show them
    vec4 waterRgba;
    vec4 trackRgba;
};

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { v += a * noise(p); p *= 2.03; a *= 0.5; }
    return v;
}

vec4 over(vec4 base, vec3 rgb, float a) {
    a = clamp(a, 0.0, 1.0);
    float outA = a + base.a * (1.0 - a);
    vec3 outRgb = outA > 0.0 ? (rgb * a + base.rgb * base.a * (1.0 - a)) / outA : vec3(0.0);
    return vec4(outRgb, outA);
}

// Uprush is quick, backwash a long slide, so `up` sits well under half. `seed` varies the reach.
vec2 wash(float phase, float up, float seed) {
    float cyc = fract(phase);
    float reach = 0.65 + 0.50 * hash(vec2(floor(phase), seed));
    float rush  = smoothstep(0.0, up, cyc);
    float drain = 1.0 - smoothstep(up, 1.0, cyc);
    return vec2(rush * drain * reach, rush * (1.0 - smoothstep(up, up + 0.30, cyc)));
}

// Incommensurable rates, so the pattern never repeats.
// Item heights: about a fifth of a 32px pill's width. Anything smaller is invisible.
vec2 washSet(float t) {
    vec2 a = wash(t * 1.20,         0.30,  3.0);
    vec2 b = wash(t * 0.82 + 0.41,  0.24,  9.0);
    vec2 c = wash(t * 1.75 + 0.17,  0.34, 17.0);
    return vec2(a.x * 0.240 + b.x * 0.160 + c.x * 0.100,
                clamp(a.y * 0.70 + b.y * 0.50 + c.y * 0.40, 0.0, 1.0));
}

void main() {
    float w = max(aspect, 1.0);
    vec2 p = vec2(qt_TexCoord0.x * w, qt_TexCoord0.y);
    float t = time;
    bool pill = turbulent > 0.5;

    vec2 c = vec2(clamp(p.x, 0.5, w - 0.5), 0.5);
    float d = length(p - c) - 0.5;
    float aa = max(fwidth(d), 1e-4);
    float inside = 1.0 - smoothstep(-aa, aa, d);
    if (inside <= 0.0) { fragColor = vec4(0.0); return; }

    vec4 col = vec4(trackRgba.rgb, trackRgba.a);

    float bufEnd = clamp(buffered, 0.0, 1.0) * w;
    col = over(col, vec3(1.0), 0.10 * (1.0 - smoothstep(bufEnd - aa, bufEnd + aa, p.x)));

    float P = clamp(progress, 0.0, 1.0) * w;
    // Wider than the wash travels, or a full capsule sloshes out of its cap.
    float envelope = smoothstep(0.0, 0.70, P) * smoothstep(0.0, 1.10, w - P);

    // Surge, sway and irregular deformation, so the tongue is not a ruled line.
    vec2 set = washSet(t);
    float surge  = set.x * envelope;
    float energy = set.y;

    float sway = (sin(t * 2.00) * 0.085
                + sin(t * 3.20 + 1.7) * 0.050
                + sin(t * 5.00 + 0.4) * 0.025) * envelope;

    // Sampled against y and drifting, which is what makes it read as fluid.
    float wobble = ((fbm(vec2(p.y * 2.3 + 11.0, t * 1.00)) - 0.5) * 0.110
                  + sin(p.y * 7.1 - t * 3.2) * 0.022) * envelope;

    float tongue = sin(p.y * 3.14159) * (0.040 + 0.110 * energy) * envelope;

    float edge = pill ? P + sway + surge + wobble + tongue : P;
    float soft = pill ? 0.018 : aa;
    float fill = (1.0 - smoothstep(edge - soft, edge + soft, p.x)) * step(0.001, progress);

    if (fill > 0.0) {
        vec3 base = waterRgba.rgb;
        if (pill) {
            // Flat, lit a little from above.
            vec3 body = mix(mix(base, vec3(1.0), 0.17), base * 0.88, p.y);
            col = over(col, body, intensity * fill);
        } else {
            vec3 lit = mix(base, vec3(1.0), 0.30);
            vec3 deep = base * 0.72;
            vec3 body = mix(lit, deep, smoothstep(0.0, 1.0, p.y));
            float caustic = noise(vec2(p.x * 2.6 - t * 1.6, p.y * 4.5 + t * 0.9))
                          * noise(vec2(p.x * 4.3 + t * 1.1, p.y * 2.4 - t * 0.7));
            body += (caustic - 0.25) * 0.28 * mix(base, vec3(1.0), 0.5);
            col = over(col, body, intensity * fill);

            float ribbon = 0.0;
            for (int i = 0; i < 2; ++i) {
                float fi = float(i);
                float y0 = 0.30 + fi * 0.36 + sin(p.x * (1.4 + fi * 0.5) - t * (1.1 + fi * 0.4)) * 0.09
                         + sin(p.x * 0.6 + t * 0.7 + fi) * 0.03;
                ribbon += (1.0 - smoothstep(0.0, 0.045, abs(p.y - y0))) * (0.55 - fi * 0.2);
            }
            col = over(col, vec3(1.0), ribbon * 0.20 * fill);
        }

        // Sized off bubbleScale, or a 6px pipe draws them a pixel across.
        if (bubbles > 0.001) {
            float field = 0.0;
            float rim = 0.0;
            for (int i = 0; i < 14; ++i) {
                float fi = float(i) + 1.0;
                float pace  = 0.110 + 0.150 * hash(vec2(fi, 1.0));
                float cycle = fract(hash(vec2(fi, 2.0)) + t * pace);
                float bx = cycle * max(P - 0.5, 0.0) + 0.26
                         + sin(t * (1.9 + 0.7 * fi) + fi) * 0.055;
                float by = 0.30 + 0.52 * hash(vec2(fi, 3.0))
                         + sin(t * (2.4 + 0.6 * fi) + fi * 1.7) * 0.075;
                bool hero = hash(vec2(fi, 12.0)) > 0.72;
                float r = (hero ? 0.16 : 0.075 + 0.075 * hash(vec2(fi, 4.0))) * max(bubbleScale, 0.05);

                vec2 dd = p - vec2(bx, by);
                // A round bubble reads as a dot.
                float ang = atan(dd.y, dd.x);
                float wob = 1.0 + 0.10 * sin(ang * 3.0 + t * 2.3 + fi)
                                + 0.06 * sin(ang * 5.0 - t * 1.7 + fi * 2.1);
                float dist = length(dd) / max(wob, 0.4);

                float life = smoothstep(0.0, 0.10, cycle) * (1.0 - smoothstep(0.86, 1.0, cycle));
                float shell = smoothstep(r, r * 0.72, dist) - smoothstep(r * 0.66, r * 0.20, dist) * 0.7;
                float glint = 1.0 - smoothstep(0.0, r * 0.30, length(dd - vec2(r * 0.34, r * 0.34)));
                field += (shell * 0.34 + glint * 0.52) * life;

                if (hero) {
                    float crescent = smoothstep(r * 0.96, r * 0.74, dist)
                                   * smoothstep(-0.2, 0.9, dot(normalize(dd + 1e-5), vec2(0.6, 0.8)));
                    rim += crescent * 0.42 * life;
                }
            }
            col = over(col, vec3(1.0), clamp(field, 0.0, 1.0) * fill * bubbles);
            col = over(col, vec3(1.0), clamp(rim, 0.0, 1.0) * fill * bubbles);
        }
    }

    // Sampled against the edge, so the pattern travels with the water.

    if (pill && progress > 0.001) {
        float settle = smoothstep(0.999, 0.94, progress);
        float behind = edge - p.x;

        // Elongated along the edge, so the froth breaks into pieces rather than speckles.
        float grain = fbm(vec2((p.x - edge) * 13.0, p.y * 3.4 + t * 0.80));
        float broken = smoothstep(0.34, 0.80, grain);

        float bandW = 0.060 + 0.110 * energy;
        float band = 1.0 - smoothstep(0.0, bandW, abs(behind));
        col = over(col, vec3(1.0, 0.997, 0.99),
                   band * (0.22 + 0.62 * broken) * (0.40 + 0.60 * energy) * settle);

        float churn = smoothstep(0.38, 0.0, behind) * step(0.0, behind) * fill;
        col = over(col, vec3(1.0, 0.995, 0.98), churn * (0.10 + 0.30 * grain) * settle);

        // The water goes back, the foam does not.
        float strand = P + sway + max(surge, washSet(t - 0.30).x * envelope) + wobble + tongue;
        float span = max(strand - edge, 0.02);
        float ahead = p.x - edge;
        float lace = step(0.0, ahead) * (1.0 - smoothstep(0.0, span, ahead));
        float drying = (1.0 - energy) * (1.0 - energy);
        col = over(col, vec3(1.0, 0.996, 0.985),
                   lace * smoothstep(0.42, 0.86, grain) * 0.40 * drying * settle);
    }

    float gloss = (1.0 - smoothstep(0.0, 0.10, abs(p.y - 0.17)))
                * smoothstep(0.35, 0.6, p.x) * smoothstep(w - 0.35, w - 0.6, p.x);
    col = over(col, vec3(1.0), gloss * mix(0.20, 0.10, turbulent) * (1.0 - fill * 0.55));

    fragColor = vec4(col.rgb * col.a, col.a) * inside * qt_Opacity;
}
