#version 150 core

smooth in vec2 fsTexCoords;

//uniform vec3 diffuseColor;
uniform sampler2D diffuseTexture;

out vec4 color;

void main() {
    vec3 texColor = texture(diffuseTexture, fsTexCoords).xyz;
    color = vec4(texColor * 2.0, 1.0);
}