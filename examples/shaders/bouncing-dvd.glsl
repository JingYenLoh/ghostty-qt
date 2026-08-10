// Classic bouncing-DVD screensaver as a persistent custom shader.
// Use `custom-shader-animation = always` so iTime advances continuously.

float sdSegment(vec2 point, vec2 start, vec2 end)
{
    vec2 offset = point - start;
    vec2 segment = end - start;
    float along = clamp(dot(offset, segment) / dot(segment, segment), 0.0, 1.0);
    return length(offset - segment * along);
}

float dvdWordDistance(vec2 point)
{
    float distanceToWord = 1000.0;

    // First D.
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(-0.82, 0.02), vec2(-0.82, 0.48)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(-0.82, 0.48), vec2(-0.55, 0.48)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(-0.55, 0.48), vec2(-0.42, 0.38)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(-0.42, 0.38), vec2(-0.42, 0.12)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(-0.42, 0.12), vec2(-0.55, 0.02)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(-0.55, 0.02), vec2(-0.82, 0.02)));

    // V.
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(-0.30, 0.48), vec2(0.0, 0.02)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(0.0, 0.02), vec2(0.30, 0.48)));

    // Second D.
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(0.42, 0.02), vec2(0.42, 0.48)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(0.42, 0.48), vec2(0.69, 0.48)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(0.69, 0.48), vec2(0.82, 0.38)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(0.82, 0.38), vec2(0.82, 0.12)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(0.82, 0.12), vec2(0.69, 0.02)));
    distanceToWord = min(distanceToWord,
        sdSegment(point, vec2(0.69, 0.02), vec2(0.42, 0.02)));

    return distanceToWord;
}

float dvdLogoMask(vec2 point, float antialias)
{
    float word = 1.0 - smoothstep(0.070, 0.070 + antialias,
                                  dvdWordDistance(point));

    // The familiar flattened disc and its offset shine underneath the word.
    vec2 discPoint = (point - vec2(0.0, -0.23)) / vec2(0.78, 0.18);
    float discDistance = abs(length(discPoint) - 1.0) * 0.18;
    float disc = 1.0 - smoothstep(0.025, 0.025 + antialias, discDistance);
    float shine = 1.0 - smoothstep(
        0.035, 0.035 + antialias,
        sdSegment(point, vec2(-0.50, -0.18), vec2(0.46, -0.30)));

    return clamp(max(word, max(disc, shine)), 0.0, 1.0);
}

float pingPong(float distance, float span)
{
    if (span <= 0.0) return 0.0;
    float periodPosition = mod(distance, 2.0 * span);
    return span - abs(periodPosition - span);
}

vec3 hueToRgb(float hue)
{
    vec3 wave = abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0))
                    * 6.0 - 3.0);
    return clamp(wave - 1.0, 0.0, 1.0);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 resolution = max(iResolution.xy, vec2(1.0));
    vec2 uv = fragCoord / resolution;
    vec4 terminal = texture(iChannel0, uv);

    // Physical-pixel dimensions keep the mark legible and the velocity stable
    // on high-DPI displays. Clamp for very small split panes.
    vec2 naturalLogoSize = vec2(250.0, 125.0);
    float logoFit = min(1.0, min(resolution.x / naturalLogoSize.x,
                                 resolution.y / naturalLogoSize.y));
    vec2 logoSize = naturalLogoSize * logoFit;
    vec2 travel = max(resolution - logoSize, vec2(0.0));
    vec2 velocity = vec2(155.0, 113.0);
    vec2 phaseOffset = vec2(0.37, 0.73) * travel;
    vec2 distanceTravelled = velocity * iTime + phaseOffset;
    vec2 logoOrigin = vec2(
        pingPong(distanceTravelled.x, travel.x),
        pingPong(distanceTravelled.y, travel.y));

    float logoUnit = max(min(0.5 * logoSize.x, logoSize.y / 1.1), 1.0);
    vec2 logoPoint = (fragCoord - logoOrigin - 0.5 * logoSize) / logoUnit;
    float antialias = 1.5 / logoUnit;
    float logo = dvdLogoMask(logoPoint, antialias);

    // Change hue only when an edge collision occurs.
    vec2 bounce = floor(distanceTravelled / max(travel, vec2(1.0)));
    float hue = fract(0.11 + bounce.x * 0.173 + bounce.y * 0.317);
    vec3 logoColor = 0.25 + 0.75 * hueToRgb(hue);

    // A dim terminal remains visible beneath the screensaver. Replace 0.16
    // with 0.0 for the fully black classic background.
    vec4 background = vec4(terminal.rgb * 0.16, terminal.a);
    fragColor = mix(background, vec4(logoColor, 1.0), logo);
}
