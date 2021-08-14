#version 410 core

/**
 * @see https://learnopengl.com/PBR/Theory
 *
 * The equation we're working off of is Lo(p,ωo)=Integral [(kd(c/π)+DFG/(4(ωo⋅n)(ωi⋅n)))*Li(p,ωi)n⋅ωi] dωi
 *
 * We effectively discretize the function by just calculating it from the perspective of each light rather than
 * trying to solve the indefinite integral (which from what I understand has no analytical solution). This is also
 * known as a numerical solution where a finite number of steps approximate the real solution.
 *
 * Lo(p, wo) effectively stands for the light intensity (radiance) at point p when viewed from wo.
 *
 * kd is equal to 1.0 - ks, where ks is the specular component (roughly F in the above)
 *
 * (c/π) is equal to (surface color aka diffuse / pi), where the division by pi normalizes the color.
 *
 * DFG is three separate functions D, F, G:
 *      D = normal distribution function = α^2 / (π ((n⋅h) ^ 2 (α ^ 2 − 1)+1) ^ 2)
          ==> Approximates the total surface area of the microfacets which are perfectly aligned with the half angle vector
          ==> a is the surface's roughness
          ==> n is the surface normal
          ==> h is the half angle vector = normalize (l + v) = (l + v) / || l + v ||
          ==> Since n and h are normalized, n dot h is equal to cos(angle between them)
        F = Fresnel (Freh-nel) equation = F0+(1−F0)(1−(h⋅v)) ^ 5
          ==> Describes the ratio of the light that gets reflected (specular) over the light that gets refracted (diffuse)
          ==> A good approximation of specular component from earlier lighting approximations such as Blinn-Phong
          ==> F0 represents the base reflectivity of the surface calculated using indices of refraction (IOR)
              as the viewing angle gets closer and closer to 90 degrees, the stronger the Fresnel and thus reflections
              (demonstrated by looking at even a rough surface from a 90 degree angle - generally you will see some shine)
          ==> https://refractiveindex.info/ is a good place for F0
          ==> h*v is the dot product of normalized half angle vector and normalized view vector, so it is cos(angle between them)
        G = G(n,v,l,k)=Gsub(n,v,k)Gsub(n,l,k)
          ==> Approximates the amount of surface area where parts of the geometry occlude other parts of the surface (self-shadowing)
          ==> n is the surface normal
          ==> v is the view direction
          ==> k is a remapping of roughness based on whether we're using direct or IBL lighting
              (direct) = (a + 1) ^ 2 / 8
              (IBL)    = a ^ 2 / 2
          ==> l is the light direction vector
        Gsub = G_Schlick_GGX = n⋅v / ((n⋅v)(1−k)+k) (see above for definition of terms)

 * Li(p, wi) is the radiance of point p when viewed from light Wi. It is equal to (light_color * attenuation)
          ==> attenuation when using quadratic attenuation is equal to 1.0 / distance ^ 2
          ==> distance = length(light_pos - world_pos)

 * n*wi is equal to cos(angle between n and light wi) since both are normalized
 * wi = normalize(light_pos - world_pos)
 */

#define MAX_LIGHTS 256
// Apple limits us to 16 total samplers active in the pipeline :(
#define MAX_SHADOW_LIGHTS 11
#define SPECULAR_MULTIPLIER 128.0
#define POINT_LIGHT_AMBIENT_INTENSITY 0.03
//#define AMBIENT_INTENSITY 0.00025
#define PI 3.14159265359
#define PREVENT_DIV_BY_ZERO 0.00001

uniform float ambientIntensity = 0.00025;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedo;
uniform sampler2D gBaseReflectivity;
uniform sampler2D gRoughnessMetallicAmbient;

/**
 * Information about the camera
 */
uniform vec3 viewPosition;

/**
 * Lighting information. All values related
 * to positions should be in world space.
 */
uniform vec3 lightPositions[MAX_LIGHTS];
uniform vec3 lightColors[MAX_LIGHTS];
uniform float lightRadii[MAX_LIGHTS];
uniform samplerCube shadowCubeMaps[MAX_SHADOW_LIGHTS];
uniform float lightFarPlanes[MAX_SHADOW_LIGHTS];
// Since max lights is an upper bound, this can
// tell us how many lights are actually present
uniform int numLights = 0;
uniform int numShadowLights = 0;

in vec2 fsTexCoords;
out vec4 fsColor;

float calculateShadowValue(vec3 fragPos, vec3 lightPos, int lightIndex, float lightNormalDotProduct) {
    // Not required for fragDir to be normalized
    vec3 fragDir = fragPos - lightPos;
    float currentDepth = length(fragDir);
    // It's very important to multiply by lightFarPlane. The recorded depth
    // is on the range [0, 1] so we need to convert it back to what it was originally
    // or else our depth comparison will fail.
    float calculatedDepth = texture(shadowCubeMaps[lightIndex], fragDir).r * lightFarPlanes[lightIndex];
    // This bias was found through trial and error... it was originally
    // 0.05 * (1.0 - max...)
    // Part of this came from GPU Gems
    // @see http://developer.download.nvidia.com/books/HTML/gpugems/gpugems_ch12.html
    float bias = (currentDepth * max(0.5 * (1.0 - max(lightNormalDotProduct, 0.0)), 0.05));// - texture(shadowCubeMap, fragDir).r;
    //float bias = max(0.75 * (1.0 - max(lightNormalDotProduct, 0.0)), 0.05);
    //float bias = max(0.85 * (1.0 - lightNormalDotProduct), 0.05);
    //bias = bias < 0.0 ? bias * -1.0 : bias;
    // Now we use a sampling-based method to look around the current pixel
    // and blend the values for softer shadows (introduces some blur). This falls
    // under the category of Percentage-Closer Filtering (PCF) algorithms.
    float shadow = 0.0;
    float samples = 2.0;
    float totalSamples = samples * samples * samples; // 64 if samples is set to 4.0
    float offset = 0.1;
    float increment = offset / (samples * 0.5);
    for (float x = -offset; x < offset; x += increment) {
        for (float y = -offset; y < offset; y += increment) {
            for (float z = -offset; z < offset; z += increment) {
                float depth = texture(shadowCubeMaps[lightIndex], fragDir + vec3(x, y, z)).r;
                // Perform this operation to go from [0, 1] to
                // the original value
                depth = depth * lightFarPlanes[lightIndex];
                if ((currentDepth - bias) > depth) {
                    shadow = shadow + 1.0;
                }
            }
        }
    }

    //float bias = 0.005 * tan(acos(max(lightNormalDotProduct, 0.0)));
    //bias = clamp(bias, 0, 0.01);
    //return (currentDepth - bias) > calculatedDepth ? 1.0 : 0.0;
    return shadow / totalSamples;
}

float normalDistribution(const float NdotH, const float roughness) {
    float roughnessSquared = roughness * roughness;
    float denominator = (NdotH * NdotH) * (roughnessSquared - 1) + 1;
    denominator = PI * (denominator * denominator);
    return roughnessSquared / max(denominator, PREVENT_DIV_BY_ZERO);
}

vec3 fresnel(vec3 albedo, float HdotV, vec3 baseReflectivity, float metallic) {
    vec3 F0 = mix(baseReflectivity, albedo, metallic);
    return F0 + (1.0 - F0) * pow(1.0 - HdotV, 5);
}

float geometrySchlickGGX(float NdotX, float k) {
    return NdotX / max(NdotX * (1 - k) + k, PREVENT_DIV_BY_ZERO);
}

float geometry(vec3 normal, vec3 viewDir, vec3 lightDir, const float roughness) {
    float k = pow(roughness + 1, 2) / 8.0;
    float NdotV = max(dot(normal, viewDir), 0.0);
    float NdotL = max(dot(normal, lightDir), 0.0);
    return geometrySchlickGGX(NdotV, k) * geometrySchlickGGX(NdotL, k);
}

vec3 calculatePointAmbient(vec3 fragPosition, vec3 baseColor, int lightIndex, const float ao) {
    vec3 lightPos   = lightPositions[lightIndex];
    vec3 lightColor = lightColors[lightIndex];
    vec3 lightDir   = lightPos - fragPosition;
    float lightDist = length(lightDir);
    float attenuationFactor = 1.0 / (1.0 + lightDist * lightDist);
    vec3 ambient = baseColor * ao * lightColor * POINT_LIGHT_AMBIENT_INTENSITY;
    return attenuationFactor * ambient;
}

vec3 calculatePointLighting(vec3 fragPosition, vec3 baseColor, vec3 normal, vec3 viewDir, int lightIndex, const float roughness, const float metallic, const float ao, const float shadowFactor, vec3 baseReflectivity) {
    vec3 lightPos   = lightPositions[lightIndex];
    vec3 lightColor = lightColors[lightIndex];
    vec3 lightDir   = lightPos - fragPosition;
    float lightDist = length(lightDir);

    vec3 V = viewDir;
    vec3 L = normalize(lightDir);
    vec3 H = normalize(V + L);
    vec3 N = normal;
    // Linear attenuation
    float attenuationFactor = 1.0 / (1.0 + lightDist * lightDist);

    float NdotH    = max(dot(N, H), 0.0);
    float HdotV    = max(dot(H, V), 0.0);
    float W0dotN   = max(dot(V, N), 0.0);
    float WidotN   = max(dot(L, N), 0.0);
    float NdotWi   = max(dot(N, L), 0.0);
    vec3 F         = fresnel(baseColor, clamp(HdotV, 0.0, 1.0), baseReflectivity, metallic);
    vec3 kS        = F;
    // We multiply by inverse of metallic since we only want non-metals to have diffuse lighting
    vec3 kD        = (vec3(1.0) - kS) * (1.0 - metallic);
    float D        = normalDistribution(NdotH, roughness);
    float G        = geometry(N, V, L, roughness);
    vec3 diffuse   = lightColor; // * attenuationFactor;
    vec3 specular  = (D * F * G) / max((4 * W0dotN * WidotN), PREVENT_DIV_BY_ZERO);

    vec3 ambient = baseColor * ao * lightColor * POINT_LIGHT_AMBIENT_INTENSITY; // * attenuationFactor;

    //return (1.0 - shadowFactor) * ((kD * baseColor / PI + specular) * diffuse * NdotWi);
    return attenuationFactor * (ambient + (1.0 - shadowFactor) * ((kD * baseColor / PI + specular) * diffuse * NdotWi));
}

void main() {
    vec2 texCoords = fsTexCoords;
    vec3 fragPos = texture(gPosition, texCoords).rgb;
    vec3 viewDir = normalize(viewPosition - fragPos);

    vec3 baseColor = texture(gAlbedo, texCoords).rgb;
    vec3 normal = texture(gNormal, texCoords).rgb;
    float roughness = texture(gRoughnessMetallicAmbient, texCoords).r;
    float metallic = texture(gRoughnessMetallicAmbient, texCoords).g;
    float ambient = texture(gRoughnessMetallicAmbient, texCoords).b;
    vec3 baseReflectivity = texture(gBaseReflectivity, texCoords).rgb;

    vec3 color = vec3(0.0);
    for (int i = 0; i < numShadowLights; ++i) {
        // calculate distance between light source and current fragment
        float distance = length(lightPositions[i] - fragPos);
        if(distance < lightRadii[i]) {
            //if (i >= numLights) break;
            // We need to perform shadow calculations in world space
            float shadowFactor = calculateShadowValue(fragPos, lightPositions[i], i, dot(lightPositions[i] - fragPos, normal));
            color = color + calculatePointLighting(fragPos, baseColor, normal, viewDir, i, roughness, metallic, ambient, shadowFactor, baseReflectivity);
        }
        else if (distance < 2 * lightRadii[i]) {
            color = color + calculatePointAmbient(fragPos, baseColor, i, ambient);
        }
    }

    for (int i = numShadowLights; i < numLights; ++i) {
        //if (i >= numLights) break;
        // calculate distance between light source and current fragment
        float distance = length(lightPositions[i] - fragPos);
        if(distance < lightRadii[i]) {
            color = color + calculatePointLighting(fragPos, baseColor, normal, viewDir, i, roughness, metallic, ambient, 0.0, baseReflectivity);
        }
        else if (distance < 2 * lightRadii[i]) {
            color = color + calculatePointAmbient(fragPos, baseColor, i, ambient);
        }
    }

    color = color + baseColor * ambient * ambientIntensity;
    fsColor = vec4(color, 1.0);
}