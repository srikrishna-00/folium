// SPDX-License-Identifier: Apache-2.0
//
// pdf-engine/writer/src/stb_impl.cpp — the ONE translation unit in the whole
// program that instantiates stb_image.h / stb_image_write.h's actual
// function bodies. Defining STB_IMAGE_IMPLEMENTATION /
// STB_IMAGE_WRITE_IMPLEMENTATION in more than one .cpp is a duplicate-symbol
// link error, so every other file in this project includes these headers
// declaration-only and this file's only job is to exist exactly once.
//
// STBI_NO_STDIO / STBI_WRITE_NO_STDIO: this project always has bytes already
// in memory (an uploaded file, a rendered page) and never a filesystem path
// to hand stb directly, so the fopen()-based half of each API
// (stbi_load/stbi_load_from_file, stbi_write_jpg) is compiled out, keeping
// only the memory-buffer and callback-based entry points this module
// actually calls.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
