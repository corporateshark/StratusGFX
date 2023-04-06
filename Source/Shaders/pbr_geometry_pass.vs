STRATUS_GLSL_VERSION

#extension GL_ARB_bindless_texture : require

#include "mesh_data.glsl"
#include "common.glsl"

layout (std430, binding = 13) readonly buffer SSBO3 {
    mat4 modelMatrices[];
};

layout (std430, binding = 14) readonly buffer SSBO4 {
    mat4 prevModelMatrices[];
};

uniform mat4 projection;
uniform mat4 view;
uniform mat4 projectionView;
uniform mat4 prevProjectionView;

uniform int viewWidth;
uniform int viewHeight;

/**
 * Information about the camera
 */
uniform vec3 viewPosition;

uniform vec2 jitter;

smooth out vec3 fsPosition;
//smooth out vec3 fsViewSpacePos;
out vec3 fsNormal;
smooth out vec2 fsTexCoords;

// Made using the tangent, bitangent and normal
out mat3 fsTbnMatrix;
out mat4 fsModel;
out mat3 fsModelNoTranslate;
// Unjittered
out vec4 fsCurrentClipPos;
out vec4 fsPrevClipPos;

flat out int fsDrawID;

void main() {
    //mat4 model = modelMats[gl_InstanceID];
    vec4 pos = modelMatrices[gl_DrawID] * vec4(getPosition(gl_VertexID), 1.0);
    //vec4 pos = vec4(getPosition(gl_VertexID), 1.0);

    //vec4 viewSpacePos = view * pos;
    fsPosition = pos.xyz;
    //fsViewSpacePos = viewSpacePos.xyz;
    fsTexCoords = getTexCoord(gl_VertexID);

    fsModelNoTranslate = mat3(modelMatrices[gl_DrawID]);
    fsNormal = normalize(fsModelNoTranslate * getNormal(gl_VertexID));

    // @see https://learnopengl.com/Advanced-Lighting/Normal-Mapping
    // Also see the tangent space and bump mapping section in "Foundations of Game Engine Development: Rendering"
    // tbn matrix transforms from normal map space to world space
    mat3 normalMatrix = mat3(modelMatrices[gl_DrawID]);
    vec3 n = normalize(getNormal(gl_VertexID)); //normalize(normalMatrix * getNormal(gl_VertexID));
    vec3 t = getTangent(gl_VertexID); //normalize(normalMatrix * getTangent(gl_VertexID));
    // re-orthogonalize T with respect to N - see end of https://learnopengl.com/Advanced-Lighting/Normal-Mapping
    // this is also called Graham-Schmidt
    t = normalize(t - dot(t, n) * n);
    // then retrieve perpendicular vector B and do the same
    //vec3 b = normalize(cross(n, t));
    vec3 b = getBitangent(gl_VertexID);
    b = normalize(b - dot(b, n) * n - dot(b, t) * t);
    fsTbnMatrix = mat3(t, b, n);

    fsModel = modelMatrices[gl_DrawID];

    fsDrawID = gl_DrawID;
    
    fsPrevClipPos = prevProjectionView * prevModelMatrices[gl_DrawID] * vec4(getPosition(gl_VertexID), 1.0);
    vec4 clip = projectionView * pos;
    fsCurrentClipPos = clip;

    //clip.xy += jitter * clip.w;

    mat4 jitterProjection = projection;
    jitterProjection[3][0] += jitter.x;
    jitterProjection[3][1] += jitter.y;
    clip = jitterProjection * view * pos;

    gl_Position = clip;
}