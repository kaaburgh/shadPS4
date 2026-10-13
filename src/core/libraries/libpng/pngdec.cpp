// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <csetjmp>
#include <cstring>
#include <png.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/libpng/pngdec.h"
#include "core/libraries/libpng/pngdec_error.h"
#include "core/libraries/libs.h"

namespace Libraries::PngDec {

// The decoder work memory whose size scePngDecQueryMemorySize reports. Every call creates its own
// libpng state, so a handle stays usable after a failed decode and for any number of images.
struct PngHandler {
    u64 reserved[2];
};

struct PngStruct {
    const u8* data;
    size_t size;
    u64 offset;
};

static inline OrbisPngDecColorSpace MapPngColor(int color) {
    switch (color) {
    case PNG_COLOR_TYPE_GRAY:
        return OrbisPngDecColorSpace::Grayscale;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
        return OrbisPngDecColorSpace::GrayscaleAlpha;
    case PNG_COLOR_TYPE_PALETTE:
        return OrbisPngDecColorSpace::Clut;
    case PNG_COLOR_TYPE_RGB:
        return OrbisPngDecColorSpace::Rgb;
    case PNG_COLOR_TYPE_RGB_ALPHA:
        return OrbisPngDecColorSpace::Rgba;
    }
    UNREACHABLE_MSG("unknown png color type");
}

void PngDecError(png_structp png_ptr, png_const_charp error_message) {
    LOG_ERROR(Lib_Png, "PNG error {}", error_message);
}

void PngDecWarning(png_structp png_ptr, png_const_charp error_message) {
    LOG_ERROR(Lib_Png, "PNG warning {}", error_message);
}

static void PngReadFn(png_structp png_ptr, png_bytep data, png_size_t len) {
    auto pngdata = (PngStruct*)png_get_io_ptr(png_ptr);
    if (len > pngdata->size - pngdata->offset) {
        png_error(png_ptr, "read past the end of the PNG data");
    }
    ::memcpy(data, pngdata->data + pngdata->offset, len);
    pngdata->offset += len;
}

static bool HasPngSignature(const u8* data, u32 size) {
    return size >= 8 && png_sig_cmp(data, 0, 8) == 0;
}

static void FillImageInfo(png_structp png_ptr, png_infop info_ptr,
                          OrbisPngDecImageInfo* imageInfo) {
    imageInfo->image_width = png_get_image_width(png_ptr, info_ptr);
    imageInfo->image_height = png_get_image_height(png_ptr, info_ptr);
    imageInfo->color_space = MapPngColor(png_get_color_type(png_ptr, info_ptr));
    imageInfo->bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    imageInfo->image_flag = OrbisPngDecImageFlag::None;
    if (png_get_interlace_type(png_ptr, info_ptr) == 1) {
        imageInfo->image_flag |= OrbisPngDecImageFlag::Adam7Interlace;
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        imageInfo->image_flag |= OrbisPngDecImageFlag::TrnsChunkExist;
    }
}

// libpng reports errors by longjmp back into these two functions, so their frames must not own
// objects with destructors. The callers create and destroy the libpng state.
static s32 ReadHeader(png_structp png_ptr, png_infop info_ptr, PngStruct* pngdata,
                      OrbisPngDecImageInfo* imageInfo) {
    if (setjmp(png_jmpbuf(png_ptr))) {
        return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    }
    png_set_read_fn(png_ptr, pngdata, PngReadFn);
    png_read_info(png_ptr, info_ptr);
    FillImageInfo(png_ptr, info_ptr, imageInfo);
    return ORBIS_OK;
}

static s32 DecodeImage(png_structp png_ptr, png_infop info_ptr, PngStruct* pngdata,
                       const OrbisPngDecDecodeParam* param, OrbisPngDecImageInfo* imageInfo) {
    if (setjmp(png_jmpbuf(png_ptr))) {
        return ORBIS_PNG_DEC_ERROR_DECODE_ERROR;
    }
    png_set_read_fn(png_ptr, pngdata, PngReadFn);
    png_read_info(png_ptr, info_ptr);
    const u32 width = png_get_image_width(png_ptr, info_ptr);
    const u32 height = png_get_image_height(png_ptr, info_ptr);
    const auto color_type = MapPngColor(png_get_color_type(png_ptr, info_ptr));
    const auto bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    if (imageInfo != nullptr) {
        FillImageInfo(png_ptr, info_ptr, imageInfo);
    }

    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == OrbisPngDecColorSpace::Clut) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == OrbisPngDecColorSpace::Grayscale && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if (color_type == OrbisPngDecColorSpace::Grayscale ||
        color_type == OrbisPngDecColorSpace::GrayscaleAlpha) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (param->pixel_format == OrbisPngDecPixelFormat::B8G8R8A8) {
        png_set_bgr(png_ptr);
    }
    if (color_type == OrbisPngDecColorSpace::Rgb ||
        color_type == OrbisPngDecColorSpace::Grayscale ||
        color_type == OrbisPngDecColorSpace::Clut) {
        png_set_add_alpha(png_ptr, param->alpha_value, PNG_FILLER_AFTER);
    }

    const s32 pass = png_set_interlace_handling(png_ptr);
    png_read_update_info(png_ptr, info_ptr);

    // Row size after the transforms above. Every row must fit in the output buffer before any
    // is written.
    const u64 row_bytes = png_get_rowbytes(png_ptr, info_ptr);
    const u64 stride = param->image_pitch > 0 ? param->image_pitch : row_bytes;
    if (stride < row_bytes || (height - 1) * stride + row_bytes > param->image_mem_size) {
        LOG_ERROR(Lib_Png, "Output does not fit: {}x{}, pitch = {}, imageMemSize = {}", width,
                  height, param->image_pitch, param->image_mem_size);
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    }

    for (int j = 0; j < pass; j++) {
        auto ptr = reinterpret_cast<png_bytep>(param->image_mem_addr);
        for (int y = 0; y < height; y++) {
            png_read_row(png_ptr, ptr, nullptr);
            ptr += stride;
        }
    }
    // Read the chunks after the image data, so a stream cut before IEND is an error.
    png_read_end(png_ptr, nullptr);

    return (width > 32767 || height > 32767) ? 0 : (width << 16) | height;
}

s32 PS4_SYSV_ABI scePngDecCreate(const OrbisPngDecCreateParam* param, void* memoryAddress,
                                 u32 memorySize, OrbisPngDecHandle* handle) {
    if (param == nullptr || param->attribute > 1) {
        LOG_ERROR(Lib_Png, "Invalid param!");
        return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    }
    if (memoryAddress == nullptr || handle == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid memory address!");
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }
    if (param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Png, "Invalid size! width = {}", param->max_image_width);
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    }
    auto pngh = (PngHandler*)memoryAddress;
    *pngh = {};
    *handle = pngh;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecDecode(OrbisPngDecHandle handle, const OrbisPngDecDecodeParam* param,
                                 OrbisPngDecImageInfo* imageInfo) {
    if (handle == nullptr) {
        LOG_ERROR(Lib_Png, "invalid handle!");
        return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    }
    if (param == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid param!");
        return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    }
    if (param->png_mem_addr == nullptr || param->image_mem_addr == nullptr) {
        LOG_ERROR(Lib_Png, "invalid image address!");
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }
    LOG_TRACE(Lib_Png,
              "pngMemSize = {} , imageMemSize = {} , pixelFormat = {} , alphaValue = {} , "
              "imagePitch = {}",
              param->png_mem_size, param->image_mem_size, int(param->pixel_format),
              param->alpha_value, param->image_pitch);

    if (!HasPngSignature(param->png_mem_addr, param->png_mem_size)) {
        LOG_ERROR(Lib_Png, "Memory doesn't contain a valid png file");
        return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    }

    auto png_ptr =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, PngDecError, PngDecWarning);
    if (png_ptr == nullptr) {
        return ORBIS_PNG_DEC_ERROR_FATAL;
    }
    auto info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        return ORBIS_PNG_DEC_ERROR_FATAL;
    }

    auto pngdata = PngStruct{
        .data = param->png_mem_addr,
        .size = param->png_mem_size,
        .offset = 0,
    };
    const s32 result = DecodeImage(png_ptr, info_ptr, &pngdata, param, imageInfo);
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    return result;
}

s32 PS4_SYSV_ABI scePngDecDecodeWithInputControl() {
    LOG_ERROR(Lib_Png, "(STUBBED)called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecDelete(OrbisPngDecHandle handle) {
    if (handle == nullptr) {
        LOG_ERROR(Lib_Png, "invalid handle!");
        return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecParseHeader(const OrbisPngDecParseParam* param,
                                      OrbisPngDecImageInfo* imageInfo) {
    if (param == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid param!");
        return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    }
    if (param->png_mem_addr == nullptr || imageInfo == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid address!");
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }

    // Check if the header indicates a valid PNG file
    if (!HasPngSignature(param->png_mem_addr, param->png_mem_size)) {
        LOG_ERROR(Lib_Png, "Memory doesn't contain a valid png file");
        return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    }
    // Create a libpng structure, also pass our custom error/warning functions
    auto png_ptr =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, PngDecError, PngDecWarning);
    if (png_ptr == nullptr) {
        return ORBIS_PNG_DEC_ERROR_FATAL;
    }

    // Create a libpng info structure
    auto info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        return ORBIS_PNG_DEC_ERROR_FATAL;
    }

    auto pngdata = PngStruct{
        .data = param->png_mem_addr,
        .size = param->png_mem_size,
        .offset = 0,
    };
    const s32 result = ReadHeader(png_ptr, info_ptr, &pngdata, imageInfo);
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    if (result != ORBIS_OK) {
        LOG_ERROR(Lib_Png, "Invalid png header");
        return result;
    }
    LOG_TRACE(
        Lib_Png,
        "imageWidth = {} , imageHeight = {} , colorSpace = {} , bitDepth = {} , imageFlag = {}",
        imageInfo->image_width, imageInfo->image_height, int(imageInfo->color_space),
        imageInfo->bit_depth, int(imageInfo->image_flag));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecQueryMemorySize(const OrbisPngDecCreateParam* param) {
    if (param == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid param!");
        return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    }
    if (param->attribute > 1) {
        LOG_ERROR(Lib_Png, "Invalid attribute! attribute = {}", param->attribute);
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }
    if (param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Png, "Invalid size! width = {}", param->max_image_width);
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    }
    return sizeof(PngHandler);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("m0uW+8pFyaw", "libScePngDec", 1, "libScePngDec", scePngDecCreate);
    LIB_FUNCTION("WC216DD3El4", "libScePngDec", 1, "libScePngDec", scePngDecDecode);
    LIB_FUNCTION("cJ--1xAbj-I", "libScePngDec", 1, "libScePngDec", scePngDecDecodeWithInputControl);
    LIB_FUNCTION("QbD+eENEwo8", "libScePngDec", 1, "libScePngDec", scePngDecDelete);
    LIB_FUNCTION("U6h4e5JRPaQ", "libScePngDec", 1, "libScePngDec", scePngDecParseHeader);
    LIB_FUNCTION("-6srIGbLTIU", "libScePngDec", 1, "libScePngDec", scePngDecQueryMemorySize);
    LIB_FUNCTION("cJ--1xAbj-I", "libScePngDec_jvm", 1, "libScePngDec",
                 scePngDecDecodeWithInputControl);
};

} // namespace Libraries::PngDec
