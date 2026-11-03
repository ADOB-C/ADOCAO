#version 330 core
in vec2 vUv;
uniform float uCompletion;
uniform vec3 uColor;
uniform float uOpacity;
out vec4 FragColor;
void main() { FragColor = vec4(1.0, 0.0, 1.0, 1.0); }
