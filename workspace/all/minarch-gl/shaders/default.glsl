#if defined(VERTEX)
attribute vec2 VertexCoord;
attribute vec2 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 vTexCoord;

void main() {
    // Flip Y-axis in the vertex shader (CPU-convention textures: v=0 = top)
    vTexCoord = vec2(TexCoord.x, 1.0 - TexCoord.y);
    gl_Position = MVPMatrix * vec4(VertexCoord, 0.0, 1.0);
}
#endif

#if defined(FRAGMENT)
precision mediump float;
uniform sampler2D Texture;
varying vec2 vTexCoord;

void main() {
    gl_FragColor = texture2D(Texture, vTexCoord);
}
#endif
