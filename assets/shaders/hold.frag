#version 330 core
in vec2 vUv;
uniform sampler2D uMap;      // 程序生成的彩虹带：UV.x 横跨带宽（中间白核）、UV.y 沿路径
uniform float uCompletion;   // 0..1 填充进度
uniform vec3 uColor;         // 移动球的颜色
uniform float uOpacity;
out vec4 FragColor;
void main() {
    vec4 tex = texture(uMap, vUv);
    float filled = vUv.y <= uCompletion ? 1.0 : 0.0;
    // 未填充也保持鲜艳（只压暗到 0.55），填充段更亮并轻微带球色 —— 与参考同构。
    vec3 col = tex.rgb * mix(0.55, 1.15, filled);
    col = mix(col, col * uColor, 0.35 * filled);
    // 中央白线：沿带宽方向的高亮核，填充后更亮
    float core = smoothstep(0.4, 0.0, abs(vUv.x - 0.5));
    col = mix(col, vec3(1.0), core * mix(0.25, 0.5, filled));
    float a = tex.a * uOpacity;
    if (a <= 0.003) discard;
    FragColor = vec4(col, a);
}
