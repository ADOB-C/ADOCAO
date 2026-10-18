#version 330 core
// 全屏三角形（无 VBO）：gl_VertexID 0/1/2 → (-1,-1) (3,-1) (-1,3)
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    gl_Position = vec4(p, 0.0, 1.0);
}
