#version 330 core
in vec2 vUv;
uniform float uCompletion;
uniform vec3 uColor;
uniform float uOpacity;
out vec4 FragColor;
void main() {
    // 官方用一张彩虹贴图；我们没有那张资产，用"球色 + 中央白线"近似（差异记在 docs/rendering.md）。
    float filled = vUv.y <= uCompletion ? 1.0 : 0.0;
    vec3 col = uColor * mix(0.55, 1.15, filled);
    float core = smoothstep(0.4, 0.0, abs(vUv.x - 0.5));
    col = mix(col, vec3(1.0), core * mix(0.25, 0.5, filled));
    float a = uOpacity;
    if (a <= 0.003) discard;
    FragColor = vec4(col, a);
}
