#version 330 core
in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0 - vColor, 1.0);  // inverted track color
}
