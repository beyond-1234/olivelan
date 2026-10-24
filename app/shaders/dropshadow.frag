uniform sampler2D tex_in;
uniform vec4 color_in;
uniform float distance_in;
uniform float angle_in;
uniform float radius_in;
uniform float opacity_in;
uniform vec2 resolution_in;
uniform sampler2D previous_iteration_in;
uniform bool fast_in;

uniform int ove_iteration;

in vec2 ove_texcoord;
out vec4 frag_color;

// Gaussian function uses PI
#define M_PI 3.1415926535897932384626433832795

// Single gaussian formula (unused, mainly here for documentation/just in case)
//float gaussian(float x, float sigma) {
//    return (1.0/(sigma*sqrt(2.0*M_PI)))*exp(-0.5*pow(x/sigma, 2.0));
//}

// Double gaussian formula, actually used in the code below
// Should be faster than the single gaussian above since it doesn't need sqrt()
float gaussian2(float x, float y, float sigma) {
  return (1.0/((sigma*sigma)*2.0*M_PI))*exp(-0.5*(((x*x) + (y*y))/(sigma*sigma)));
}

// 1D Gaussian formula for separated 1D passes (normalized integral)
float gaussian1d(float x, float sigma) {
  return (1.0 / (sigma * sqrt(2.0 * M_PI))) * exp(-0.5 * (x * x) / (sigma * sigma));
}

void main(void) {
  if (ove_iteration == 2 || radius_in == 0.0) {

    // Merge step
    vec4 composite = texture(tex_in, ove_texcoord);
    if (composite.a < 1.0) {
      // Convert degrees to radians
      float shadow_angle = ((angle_in + 90.0)*M_PI)/180.0;

      vec2 shadow_offset = vec2(cos(shadow_angle) * distance_in, sin(shadow_angle) * distance_in);
      shadow_offset /= resolution_in;
      shadow_offset += ove_texcoord;

      vec4 shadow_color = texture(previous_iteration_in, shadow_offset);

      shadow_color.rgb = color_in.rgb * shadow_color.a;
      shadow_color *= 1.0 - composite.a;
      shadow_color *= opacity_in;

      composite += shadow_color;
    }
    frag_color = composite;

  } else {
    // We only sample on hard pixels, so we don't accept decimal radii
    float real_radius = ceil(radius_in);

    vec4 composite = vec4(0.0);

    float sigma = real_radius;
    if (!fast_in) {
      // Using (radius = 3 * sigma) because 3 standard deviations covers 97% of the blur
      real_radius *= 3.0;
    }

    float step_size = max(1.0, floor(real_radius / 48.0));
    float half_step = step_size * 0.5;
    float total_weight = 0.0;

    if (fast_in) {
      // Fast mode: unweighted box decimation
      for (float i = -real_radius + half_step; i <= real_radius; i += step_size) {
        vec2 pixel_coord = ove_texcoord;
        vec4 tex_col;
        if (ove_iteration == 0) {
          pixel_coord.x += i / resolution_in.x;
          tex_col = texture(tex_in, pixel_coord);
        } else {
          pixel_coord.y += i / resolution_in.y;
          tex_col = texture(previous_iteration_in, pixel_coord);
        }
        composite += tex_col;
        total_weight += 1.0;
      }
    } else {
      // High-quality Bilinear Gaussian Decimation
      float curr = -real_radius + half_step;
      while (curr <= real_radius) {
        float x1 = curr;
        float w1 = gaussian1d(x1, sigma) * step_size;
        curr += step_size;

        float tap_offset = x1;
        float tap_weight = w1;

        if (curr <= real_radius) {
          float x2 = curr;
          float w2 = gaussian1d(x2, sigma) * step_size;
          curr += step_size;

          float pair_weight = w1 + w2;
          if (pair_weight > 0.000001) {
            tap_offset = (x1 * w1 + x2 * w2) / pair_weight;
            tap_weight = pair_weight;
          }
        }

        vec2 pixel_coord = ove_texcoord;
        vec4 tex_col;
        if (ove_iteration == 0) {
          pixel_coord.x += tap_offset / resolution_in.x;
          tex_col = texture(tex_in, pixel_coord);
        } else {
          pixel_coord.y += tap_offset / resolution_in.y;
          tex_col = texture(previous_iteration_in, pixel_coord);
        }

        composite += tex_col * tap_weight;
        total_weight += tap_weight;
      }
    }

    if (total_weight > 0.0) {
      composite /= total_weight;
    }

    frag_color = composite;
  }
}
