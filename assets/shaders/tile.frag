#version 330 core
in vec3 vColor;
in float vOpacity;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, vOpacity);
}
