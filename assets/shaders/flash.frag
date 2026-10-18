#version 330 core
// Flash 叠层：直接输出 uniform 颜色 × alpha，混合由调用方设置。
uniform vec3  uColor;
uniform float uAlpha;
out vec4 fragColor;
void main() { fragColor = vec4(uColor, uAlpha); }
