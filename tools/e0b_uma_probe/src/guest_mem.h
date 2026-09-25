// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace e0b {

/**
 * Reproduces shadPS4's Linux guest memory model (core/address_space.cpp):
 *  - all guest "physical" memory is one memfd;
 *  - `base` is the canonical MAP_SHARED mapping of the whole memfd (backing_base);
 *  - guest virtual mappings are MAP_SHARED|MAP_FIXED mmaps of the same memfd at PA offsets
 *    inside a reserved range (`gva`), so the same PA can appear at several VAs (aliases).
 * Unlike shadPS4 today, the memfd is created with MFD_ALLOW_SEALING and sealed with
 * F_SEAL_SHRINK, which udmabuf requires.
 */
class GuestMemory {
public:
    ~GuestMemory();

    bool Create(uint64_t backing_size, uint64_t va_span, std::string* err);

    /// Maps [pa, pa+len) of the memfd at guest VA offset va_off (MAP_FIXED).
    uint8_t* Map(uint64_t va_off, uint64_t pa, uint64_t len);
    /// Maps [off, off+len) of an arbitrary fd (e.g. exported Vulkan memory) at guest VA va_off.
    uint8_t* MapFd(uint64_t va_off, int fd, uint64_t off, uint64_t len);
    /// Returns [va_off, va_off+len) to an inaccessible reservation.
    void Unmap(uint64_t va_off, uint64_t len);

    uint8_t* Gva(uint64_t va_off) const {
        return gva_ + va_off;
    }
    uint8_t* Canonical(uint64_t pa) const {
        return base_ + pa;
    }

    int Fd() const {
        return fd_;
    }
    uint64_t Size() const {
        return size_;
    }
    uint64_t VaSpan() const {
        return va_span_;
    }
    bool Sealed() const {
        return sealed_;
    }
    const std::string& SealError() const {
        return seal_error_;
    }

private:
    int fd_ = -1;
    uint64_t size_ = 0;
    uint8_t* base_ = nullptr;
    uint8_t* base_reservation_ = nullptr;
    uint64_t base_reservation_size_ = 0;
    uint8_t* gva_reservation_ = nullptr;
    uint64_t gva_reservation_size_ = 0;
    uint8_t* gva_ = nullptr;
    uint64_t va_span_ = 0;
    bool sealed_ = false;
    std::string seal_error_;
};

/// A dma-buf describing guest physical memory. `offset` is where the requested first PA
/// starts inside the dma-buf, i.e. the VkDeviceMemory offset to bind from.
struct DmaBuf {
    int fd = -1;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string error;
    explicit operator bool() const {
        return fd >= 0;
    }
};

/**
 * Creates dma-bufs over the guest memfd through /dev/udmabuf.
 * In --selftest-memfd-as-dmabuf mode (lavapipe only) the memfd itself is handed out as a
 * "dma-buf" covering the whole backing, which exercises the import/bind code paths but says
 * nothing about udmabuf; list (stitched) creation is not available in that mode.
 */
class DmaBufSource {
public:
    ~DmaBufSource();
    void Init(const GuestMemory& gm, bool fake);

    bool Available() const {
        return fake_ || dev_fd_ >= 0;
    }
    bool IsFake() const {
        return fake_;
    }
    bool SupportsList() const {
        return !fake_ && dev_fd_ >= 0;
    }
    const std::string& Why() const {
        return why_;
    }

    DmaBuf Single(uint64_t pa, uint64_t size) const;
    DmaBuf List(const std::vector<std::pair<uint64_t, uint64_t>>& pieces) const;

private:
    const GuestMemory* gm_ = nullptr;
    int dev_fd_ = -1;
    bool fake_ = false;
    std::string why_;
};

/// DMA_BUF_IOCTL_SYNC wrapper; returns errno (0 on success).
int DmaBufSync(int fd, bool start, bool read, bool write);

std::string ReadFileTrim(const std::string& path, std::string* err = nullptr);

} // namespace e0b
