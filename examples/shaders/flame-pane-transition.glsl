// Dramatic upward-burning pane transition. Configure this through the
// dedicated pane enter/exit shader keys; ghostty-qt phase-gates it otherwise.

float flameHash(vec2 point)
{
    point = fract(point * vec2(123.34, 456.21));
    point += dot(point, point + 45.32);
    return fract(point.x * point.y);
}

float flameNoise(vec2 point)
{
    vec2 cell = floor(point);
    vec2 local = fract(point);
    local = local * local * (3.0 - 2.0 * local);
    return mix(
        mix(flameHash(cell), flameHash(cell + vec2(1.0, 0.0)), local.x),
        mix(flameHash(cell + vec2(0.0, 1.0)),
            flameHash(cell + vec2(1.0, 1.0)), local.x),
        local.y);
}

float flameFbm(vec2 point)
{
    float value = 0.0;
    float amplitude = 0.5;
    mat2 rotateAndScale = mat2(1.62, 1.18, -1.18, 1.62);
    for (int octave = 0; octave < 5; ++octave) {
        value += amplitude * flameNoise(point);
        point = rotateAndScale * point + vec2(13.7, 9.2);
        amplitude *= 0.5;
    }
    return value;
}

vec3 flamePalette(float heat)
{
    vec3 ember = vec3(0.45, 0.015, 0.002);
    vec3 orange = vec3(1.0, 0.18, 0.005);
    vec3 yellow = vec3(1.0, 0.72, 0.06);
    vec3 whiteHot = vec3(1.0, 0.96, 0.72);
    vec3 color = mix(ember, orange, smoothstep(0.05, 0.42, heat));
    color = mix(color, yellow, smoothstep(0.36, 0.76, heat));
    return mix(color, whiteHot, smoothstep(0.76, 1.0, heat));
}

float risingEmbers(vec2 uv, float time, float front)
{
    vec2 grid = vec2(uv.x * 72.0, (uv.y - time * 0.28) * 48.0);
    vec2 cell = floor(grid);
    float random = flameHash(cell);
    vec2 center = vec2(fract(random * 7.13), fract(random * 19.71));
    float particle = 1.0 - smoothstep(0.04, 0.18,
                                      length(fract(grid) - center));
    float heightAboveFront = uv.y - front;
    float nearFire = (1.0 - smoothstep(0.02, 0.24, heightAboveFront))
        * smoothstep(-0.01, 0.025, heightAboveFront);
    return particle * step(0.925, random) * nearFire;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 resolution = max(iResolution.xy, vec2(1.0));
    vec2 uv = fragCoord / resolution;
    vec4 terminal = texture(iChannel0, uv);

    float progress = clamp(iPaneTransition.x, 0.0, 1.0);
    bool entering = iPaneTransition.y == float(PANETRANSITION_ENTER);
    if (progress <= 0.0) {
        fragColor = entering ? vec4(0.002, 0.001, 0.001, 1.0) : terminal;
        return;
    }
    if (progress >= 1.0) {
        fragColor = entering ? terminal : vec4(0.002, 0.001, 0.001, 1.0);
        return;
    }
    progress = progress * progress * (3.0 - 2.0 * progress);
    float time = iPaneTransition.z;

    // Suppress contour displacement at the endpoints so enter finishes with
    // the exact terminal and exit finishes with a completely burned pane.
    float endpointEnvelope = 4.0 * progress * (1.0 - progress);
    float contour = flameFbm(vec2(uv.x * 8.0 - time * 0.35,
                                  time * 2.4));
    contour += 0.35 * sin(uv.x * 31.0 + time * 5.0);
    float front = progress + (contour - 0.58) * 0.085 * endpointEnvelope;

    float edgeSoftness = 1.5 / resolution.y;
    float belowFront = 1.0 - smoothstep(front - edgeSoftness,
                                        front + edgeSoftness, uv.y);
    float contentMask = entering ? belowFront : 1.0 - belowFront;

    // Scorch the surviving terminal pixels immediately adjacent to the front.
    float visibleDistance = entering ? front - uv.y : uv.y - front;
    float charBand = (1.0 - smoothstep(0.0, 0.055, visibleDistance))
        * step(0.0, visibleDistance);
    vec3 charredTerminal = terminal.rgb
        * mix(1.0, 0.08 + 0.12 * contour, charBand);

    // Flame tongues always rise above the burn front. Layered moving noise
    // gives each tongue a different reach while retaining a white-hot base.
    float aboveFront = uv.y - front;
    float turbulence = flameFbm(vec2(uv.x * 11.0 + time * 0.45,
                                     uv.y * 5.5 - time * 3.2));
    float fineTurbulence = flameNoise(vec2(uv.x * 37.0 - time,
                                           uv.y * 13.0 - time * 5.0));
    float flameHeight = 0.035 + 0.16 * pow(turbulence, 2.2);
    flameHeight *= 0.72 + 0.42 * fineTurbulence;
    float tongue = (1.0 - smoothstep(0.0, flameHeight,
                                     max(aboveFront, 0.0)))
        * smoothstep(-0.025, 0.004, aboveFront);
    float endpointFade = smoothstep(0.0, 0.035, progress)
        * (1.0 - smoothstep(0.965, 1.0, progress));
    tongue *= endpointFade;

    float normalizedHeight = clamp(aboveFront / max(flameHeight, 0.001),
                                   0.0, 1.0);
    float heat = tongue * (1.0 - 0.78 * normalizedHeight);
    heat *= 0.78 + 0.34 * fineTurbulence;
    vec3 fire = flamePalette(clamp(heat, 0.0, 1.0));

    float glow = exp(-abs(aboveFront) * 28.0) * endpointFade;
    vec3 burnedBackground = vec3(0.002, 0.001, 0.001);
    vec3 color = mix(burnedBackground, charredTerminal, contentMask);
    color += vec3(0.55, 0.055, 0.004) * glow * 0.42;
    color = mix(color, fire, clamp(tongue, 0.0, 1.0));

    float embers = risingEmbers(uv, time, front) * endpointFade;
    color += vec3(1.0, 0.25, 0.015) * embers * 1.8;

    // Wisps linger above the front without obscuring the flames.
    float smokeRegion = smoothstep(0.02, 0.22, aboveFront)
        * (1.0 - smoothstep(0.22, 0.48, aboveFront));
    float smoke = smokeRegion
        * flameFbm(vec2(uv.x * 5.0 + time * 0.16,
                        uv.y * 3.0 - time * 0.7));
    color = mix(color, vec3(0.055, 0.045, 0.04), smoke * 0.20 * endpointFade);

    fragColor = vec4(color, mix(1.0, terminal.a, contentMask));
}
