// Single-header library implementations. Compiled once into patina_thirdparty.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_EASY_FONT_IMPLEMENTATION_UNUSED
#include "stb_easy_font.h"

// Expose stb_easy_font through a non-static symbol so C++ code can link to it.
int patina_easy_font_print(float x, float y, const char *text, unsigned char color[4], void *vertex_buffer, int vbuf_size) {
  return stb_easy_font_print(x, y, (char *)text, color, vertex_buffer, vbuf_size);
}
