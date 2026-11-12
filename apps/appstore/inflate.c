/* The shared HTTP client can decode compressed catalogs without an HTML engine. */
#include <stdbool.h>
#include <stdint.h>
#define STBI_ONLY_PNG
#define STBI_NO_PNG
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "stb_image.h"
#pragma GCC diagnostic pop
#include "../browser/inflate.h"
