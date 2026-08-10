// Lifecycle-only CRT raster collapse. Configure this through the dedicated
// pane enter/exit keys; ghostty-qt phase-gates it outside those transitions.
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    float phase = iPaneTransition.y;
    if (phase == float(PANETRANSITION_STABLE)) {
        fragColor = texture(iChannel0, uv);
        return;
    }

    float progress = smoothstep(0.0, 1.0, iPaneTransition.x);
    float reveal = phase == float(PANETRANSITION_ENTER)
        ? progress
        : 1.0 - progress;

    // Turning off collapses height first, then pulls the remaining raster line
    // into the center. Turning on naturally performs the inverse sequence.
    float halfWidth = 0.5 * smoothstep(0.0, 0.28, reveal);
    float halfHeight = 0.5 * smoothstep(0.18, 1.0, reveal);
    vec2 centered = uv - vec2(0.5);
    if (abs(centered.x) > halfWidth || abs(centered.y) > halfHeight) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 sourceUv = vec2(
        centered.x / max(2.0 * halfWidth, 0.0001) + 0.5,
        centered.y / max(2.0 * halfHeight, 0.0001) + 0.5);
    vec4 source = texture(iChannel0, clamp(sourceUv, 0.0, 1.0));

    float raster = 0.90 + 0.10 * sin(fragCoord.y * 3.14159265);
    float lineGlow = exp(-abs(centered.y) * iResolution.y * 0.10)
        * (1.0 - smoothstep(0.18, 0.65, reveal));
    source.rgb = source.rgb * raster
        + vec3(0.32, 0.70, 0.48) * lineGlow;
    fragColor = source;
}
