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

    float progress = clamp(iPaneTransition.x, 0.0, 1.0);
    // Quintic easing has zero velocity and acceleration at both ends. This
    // avoids a visible kick when the pane is attached or finally removed.
    progress = progress * progress * progress
        * (progress * (progress * 6.0 - 15.0) + 10.0);
    float reveal = phase == float(PANETRANSITION_ENTER)
        ? progress
        : 1.0 - progress;

    // Turning off collapses height first, then pulls the remaining raster line
    // into the center. Turning on naturally performs the inverse sequence.
    float halfWidth = 0.5 * smoothstep(0.0, 0.42, reveal);
    float halfHeight = 0.5 * smoothstep(0.08, 1.0, reveal);
    vec2 centered = uv - vec2(0.5);

    // Feather the moving boundary by roughly 1.5 physical pixels. A hard
    // branch makes the band advance one whole row at a time, which looks
    // jerky even when the transition clock itself is updating every frame.
    vec2 feather = 1.5 / max(iResolution.xy, vec2(1.0));
    float horizontalMask = 1.0 - smoothstep(
        max(0.0, halfWidth - feather.x), halfWidth + feather.x,
        abs(centered.x));
    float verticalMask = 1.0 - smoothstep(
        max(0.0, halfHeight - feather.y), halfHeight + feather.y,
        abs(centered.y));
    float rasterMask = horizontalMask * verticalMask;
    if (rasterMask <= 0.0) {
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
    fragColor = mix(vec4(0.0, 0.0, 0.0, 1.0), source, rasterMask);
}
