// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <csetjmp>
#include <png.h>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libpng/pngenc.h"
#include "core/libraries/libs.h"

#include "pngenc_error.h"

namespace Libraries::PngEnc {

// The encoder work memory whose size scePngEncQueryMemorySize reports. Every call creates its own
// libpng state, so a handle stays usable after a failed encode and for any number of images.
struct PngHandler {
    u64 reserved[2];
};

struct PngWriter {
    u8* cursor;
    u8* start;
    size_t capacity;
    bool cancel_write;
};

static inline int MapPngFilter(u16 filter) {
    if (filter == (u16)OrbisPngEncFilterType::All) {
        return PNG_ALL_FILTERS;
    }

    int f = 0;

    if (filter & (u16)OrbisPngEncFilterType::None)
        f |= PNG_FILTER_NONE;
    if (filter & (u16)OrbisPngEncFilterType::Sub)
        f |= PNG_FILTER_SUB;
    if (filter & (u16)OrbisPngEncFilterType::Up)
        f |= PNG_FILTER_UP;
    if (filter & (u16)OrbisPngEncFilterType::Average)
        f |= PNG_FILTER_AVG;
    if (filter & (u16)OrbisPngEncFilterType::Paeth)
        f |= PNG_FILTER_PAETH;

    return f;
}

void PngWriteFn(png_structp png_ptr, png_bytep data, size_t length) {
    PngWriter* ctx = (PngWriter*)png_get_io_ptr(png_ptr);

    if ((size_t)(ctx->cursor - ctx->start) + length > ctx->capacity) {
        LOG_ERROR(Lib_Png, "PNG output buffer too small");
        ctx->cancel_write = true;
        return;
    }

    memcpy(ctx->cursor, data, length);
    ctx->cursor += length;
}

void PngFlushFn(png_structp png_ptr) {}

void PngEncError(png_structp png_ptr, png_const_charp error_message) {
    LOG_ERROR(Lib_Png, "PNG error {}", error_message);
}

void PngEncWarning(png_structp png_ptr, png_const_charp error_message) {
    LOG_ERROR(Lib_Png, "PNG warning {}", error_message);
}

s32 PS4_SYSV_ABI scePngEncCreate(const OrbisPngEncCreateParam* param, void* memoryAddress,
                                 u32 memorySize, OrbisPngEncHandle* handle) {
    if (param == nullptr || param->attribute != 0) {
        LOG_ERROR(Lib_Png, "Invalid param");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (memoryAddress == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid memory address");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Png, "Invalid Size, width = {}", param->max_image_width);
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    }

    if (handle == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid handle address");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    auto pngh = (PngHandler*)memoryAddress;
    *pngh = {};
    *handle = pngh;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngEncDelete(OrbisPngEncHandle handle) {
    if (handle == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid handle");
        return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    }
    return ORBIS_OK;
}

// libpng reports errors by longjmp back into this function, so its frame must not own objects
// with destructors. The caller creates and destroys the libpng state and the RGB row buffer.
static s32 EncodeImage(png_structp png_ptr, png_infop info_ptr, PngWriter* writer, u8* rgb_row,
                       const OrbisPngEncEncodeParam* param, u64 src_pitch,
                       OrbisPngEncOutputInfo* outputInfo) {
    if (setjmp(png_jmpbuf(png_ptr))) {
        LOG_ERROR(Lib_Png, "LibPNG aborted encode");
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }

    int png_color_type = PNG_COLOR_TYPE_RGB;

    if (param->color_space == OrbisPngEncColorSpace::RGBA) {
        png_color_type |= PNG_COLOR_MASK_ALPHA;
    }

    int png_interlace_type = PNG_INTERLACE_NONE;
    int png_compression_type = PNG_COMPRESSION_TYPE_DEFAULT;
    int png_filter_method = PNG_FILTER_TYPE_DEFAULT;

    png_set_write_fn(png_ptr, writer, PngWriteFn, PngFlushFn);

    png_set_IHDR(png_ptr, info_ptr, param->image_width, param->image_height, param->bit_depth,
                 png_color_type, png_interlace_type, png_compression_type, png_filter_method);

    if (param->pixel_format == OrbisPngEncPixelFormat::B8G8R8A8) {
        png_set_bgr(png_ptr);
    }

    png_set_compression_level(png_ptr, std::clamp<u16>(param->compression_level, 0, 9));
    png_set_filter(png_ptr, 0, MapPngFilter(param->filter_type));

    png_write_info(png_ptr, info_ptr);

    uint32_t processed_height = 0;

    if (param->color_space == OrbisPngEncColorSpace::RGBA) {
        for (; processed_height < param->image_height; ++processed_height) {
            png_bytep row = (png_bytep)param->image_mem_addr + processed_height * src_pitch;
            png_write_row(png_ptr, row);

            if (outputInfo != nullptr) {
                outputInfo->processed_height = processed_height;
            }

            if (writer->cancel_write) {
                LOG_ERROR(Lib_Png, "Ran out of room to write PNG");
                return ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW;
            }
        }
    } else {
        // our input data is always rgba but when outputting without an alpha channel, libpng
        // expects the input to not have alpha either, i couldn't find a way around this easily?
        // png_strip_alpha is for reading and set_background wasn't working, this seems fine...?
        for (; processed_height < param->image_height; ++processed_height) {
            const unsigned char* src = param->image_mem_addr + processed_height * src_pitch;

            uint8_t* dst = rgb_row;

            for (uint32_t x = 0; x < param->image_width; ++x) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                src += 4; // skip reading alpha channel
                dst += 3;
            }

            png_write_row(png_ptr, rgb_row);

            if (outputInfo != nullptr) {
                outputInfo->processed_height = processed_height;
            }

            if (writer->cancel_write) {
                LOG_ERROR(Lib_Png, "Ran out of room to write PNG");
                return ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW;
            }
        }
    }

    png_write_flush(png_ptr);

    png_write_end(png_ptr, info_ptr);

    if (writer->cancel_write) {
        LOG_ERROR(Lib_Png, "Ran out of room to write PNG");
        return ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW;
    }

    if (outputInfo != nullptr) {
        outputInfo->data_size = writer->cursor - writer->start;
        outputInfo->processed_height = processed_height;
    }

    return writer->cursor - writer->start;
}

s32 PS4_SYSV_ABI scePngEncEncode(OrbisPngEncHandle handle, const OrbisPngEncEncodeParam* param,
                                 OrbisPngEncOutputInfo* outputInfo) {
    if (handle == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid handle");
        return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    }

    if (param == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid param");
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    }

    LOG_TRACE(Lib_Png, "called png addr = {}, image addr = {}, image size = {}",
              (void*)param->png_mem_addr, (void*)param->image_mem_addr, param->image_mem_size);

    if (param->image_mem_addr == nullptr || param->png_mem_addr == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid input or output address");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (param->png_mem_size == 0 || param->image_mem_size == 0 || param->image_height == 0 ||
        param->image_width == 0) {
        LOG_ERROR(Lib_Png, "Invalid Size");
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    }

    // The source is always 8-bit RGBA rows, also when the PNG drops the alpha channel. A pitch
    // of 0 means tightly packed rows. Every row must lie within the source buffer.
    if (param->bit_depth != 8) {
        LOG_ERROR(Lib_Png, "Unsupported bit depth {}", param->bit_depth);
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    }
    const u64 row_bytes = u64(param->image_width) * 4;
    const u64 src_pitch = param->image_pitch > 0 ? param->image_pitch : row_bytes;
    if (src_pitch < row_bytes ||
        (param->image_height - 1) * src_pitch + row_bytes > param->image_mem_size) {
        LOG_ERROR(Lib_Png, "Source does not fit: {}x{}, pitch = {}, imageMemSize = {}",
                  param->image_width, param->image_height, param->image_pitch,
                  param->image_mem_size);
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    }

    auto png_ptr =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, PngEncError, PngEncWarning);
    if (png_ptr == nullptr) {
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }
    auto info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }

    PngWriter writer{};
    writer.cursor = param->png_mem_addr;
    writer.start = param->png_mem_addr;
    writer.capacity = param->png_mem_size;

    std::vector<u8> rgb_row(
        param->color_space == OrbisPngEncColorSpace::RGBA ? 0 : param->image_width * 3);

    const s32 result =
        EncodeImage(png_ptr, info_ptr, &writer, rgb_row.data(), param, src_pitch, outputInfo);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return result;
}

s32 PS4_SYSV_ABI scePngEncQueryMemorySize(const OrbisPngEncCreateParam* param) {
    if (param == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid Address");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (param->attribute != 0 || param->max_filter_number > 5) {
        LOG_ERROR(Lib_Png, "Invalid Param, attribute = {}, max_filter_number = {}",
                  param->attribute, param->max_filter_number);
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    }

    if (param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Png, "Invalid Size, width = {}", param->max_image_width);
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    }

    return sizeof(PngHandler);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("7aGTPfrqT9s", "libScePngEnc", 1, "libScePngEnc", scePngEncCreate);
    LIB_FUNCTION("RUrWdwTWZy8", "libScePngEnc", 1, "libScePngEnc", scePngEncDelete);
    LIB_FUNCTION("xgDjJKpcyHo", "libScePngEnc", 1, "libScePngEnc", scePngEncEncode);
    LIB_FUNCTION("9030RnBDoh4", "libScePngEnc", 1, "libScePngEnc", scePngEncQueryMemorySize);
};

} // namespace Libraries::PngEnc