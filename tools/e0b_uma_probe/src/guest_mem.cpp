// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common.h"
#include "guest_mem.h"

namespace e0b {

static std::string Errno(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

GuestMemory::~GuestMemory() {
    if (gva_reservation_) {
        munmap(gva_reservation_, gva_reservation_size_);
    }
    if (base_reservation_) {
        munmap(base_reservation_, base_reservation_size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool GuestMemory::Create(uint64_t backing_size, uint64_t va_span, std::string* err) {
    fd_ = memfd_create("e0b_backing_dmem", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd_ < 0) {
        *err = Errno("memfd_create");
        return false;
    }
    if (ftruncate(fd_, static_cast<off_t>(backing_size)) != 0) {
        *err = Errno("ftruncate");
        return false;
    }
    size_ = backing_size;
    if (fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK) == 0) {
        sealed_ = true;
    } else {
        seal_error_ = Errno("F_ADD_SEALS(F_SEAL_SHRINK)");
    }
    // Canonical mapping, 2 MiB aligned so host-pointer imports never trip over
    // minImportedHostPointerAlignment.
    constexpr uint64_t kAlign = 2 * MiB;
    base_reservation_size_ = size_ + kAlign;
    void* res = mmap(nullptr, base_reservation_size_, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) {
        *err = Errno("mmap(backing reservation)");
        return false;
    }
    base_reservation_ = static_cast<uint8_t*>(res);
    auto* aligned = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(res) + kAlign - 1) &
                                               ~uintptr_t(kAlign - 1));
    void* b = mmap(aligned, size_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd_, 0);
    if (b == MAP_FAILED) {
        *err = Errno("mmap(backing)");
        return false;
    }
    base_ = static_cast<uint8_t*>(b);

    // Reserve the guest VA window, aligned to 2 MiB so guest VA offsets and absolute
    // addresses share alignment up to that size.
    gva_reservation_size_ = va_span + kAlign;
    void* r = mmap(nullptr, gva_reservation_size_, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) {
        *err = Errno("mmap(guest va reservation)");
        return false;
    }
    gva_reservation_ = static_cast<uint8_t*>(r);
    const auto addr = reinterpret_cast<uintptr_t>(r);
    gva_ = reinterpret_cast<uint8_t*>((addr + kAlign - 1) & ~(kAlign - 1));
    va_span_ = va_span;
    return true;
}

uint8_t* GuestMemory::Map(uint64_t va_off, uint64_t pa, uint64_t len) {
    if (va_off + len > va_span_ || pa + len > size_) {
        throw std::runtime_error("GuestMemory::Map out of range");
    }
    void* p = mmap(gva_ + va_off, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd_,
                   static_cast<off_t>(pa));
    if (p == MAP_FAILED) {
        throw std::runtime_error(Errno("mmap(guest alias)"));
    }
    Forget(va_off, len);
    maps_[va_off] = {pa, len};
    return static_cast<uint8_t*>(p);
}

uint8_t* GuestMemory::MapFd(uint64_t va_off, int fd, uint64_t off, uint64_t len) {
    if (va_off + len > va_span_) {
        throw std::runtime_error("GuestMemory::MapFd out of range");
    }
    void* p = mmap(gva_ + va_off, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
                   static_cast<off_t>(off));
    if (p == MAP_FAILED) {
        throw std::runtime_error(Errno("mmap(exported fd at guest va)"));
    }
    Forget(va_off, len);
    maps_[va_off] = {kForeign, len};
    return static_cast<uint8_t*>(p);
}

void GuestMemory::Unmap(uint64_t va_off, uint64_t len) {
    mmap(gva_ + va_off, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1,
         0);
    Forget(va_off, len);
}

void GuestMemory::Forget(uint64_t va_off, uint64_t len) {
    const uint64_t end = va_off + len;
    auto it = maps_.upper_bound(va_off);
    if (it != maps_.begin()) {
        --it;
    }
    while (it != maps_.end() && it->first < end) {
        const uint64_t s = it->first;
        const Mapping m = it->second;
        const uint64_t e = s + m.len;
        if (e <= va_off) {
            ++it;
            continue;
        }
        it = maps_.erase(it);
        if (s < va_off) { // keep the part before the range
            maps_[s] = {m.pa, va_off - s};
        }
        if (e > end) { // keep the part after the range
            maps_[end] = {m.pa == kForeign ? kForeign : m.pa + (end - s), e - end};
        }
    }
}

bool GuestMemory::Pieces(uint64_t va_off, uint64_t len,
                         std::vector<std::pair<uint64_t, uint64_t>>* out, std::string* why) const {
    out->clear();
    uint64_t va = va_off;
    const uint64_t end = va_off + len;
    while (va < end) {
        auto it = maps_.upper_bound(va);
        if (it == maps_.begin()) {
            *why = "guest va " + Hex(va) + " is not mapped";
            return false;
        }
        --it;
        const uint64_t s = it->first;
        const Mapping& m = it->second;
        if (va >= s + m.len) {
            *why = "guest va " + Hex(va) + " is not mapped";
            return false;
        }
        if (m.pa == kForeign) {
            *why = "guest va " + Hex(va) + " is not backed by the guest memfd";
            return false;
        }
        const uint64_t pa = m.pa + (va - s);
        const uint64_t n = std::min(end, s + m.len) - va;
        if (!out->empty() && out->back().first + out->back().second == pa) {
            out->back().second += n;
        } else {
            out->push_back({pa, n});
        }
        va += n;
    }
    return true;
}

DmaBufSource::~DmaBufSource() {
    if (dev_fd_ >= 0) {
        close(dev_fd_);
    }
}

void DmaBufSource::Init(const GuestMemory& gm, bool fake) {
    gm_ = &gm;
    fake_ = fake;
    if (fake_) {
        why_ = "self-test mode: memfd handed out as dma-buf (no udmabuf involved)";
        return;
    }
    if (!gm.Sealed()) {
        why_ = "memfd could not be sealed: " + gm.SealError();
        return;
    }
    dev_fd_ = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev_fd_ < 0) {
        why_ = Errno("open(/dev/udmabuf)");
    }
}

DmaBuf DmaBufSource::Single(uint64_t pa, uint64_t size) const {
    DmaBuf d;
    if (fake_) {
        if (pa + size > gm_->Size()) {
            d.error = "range outside backing";
            return d;
        }
        d.fd = fcntl(gm_->Fd(), F_DUPFD_CLOEXEC, 0);
        if (d.fd < 0) {
            d.error = Errno("dup(memfd)");
            return d;
        }
        d.offset = pa;
        d.size = gm_->Size();
        return d;
    }
    if (dev_fd_ < 0) {
        d.error = why_;
        return d;
    }
    udmabuf_create c{};
    c.memfd = static_cast<__u32>(gm_->Fd());
    c.flags = UDMABUF_FLAGS_CLOEXEC;
    c.offset = pa;
    c.size = size;
    const int fd = ioctl(dev_fd_, UDMABUF_CREATE, &c);
    if (fd < 0) {
        d.error = Errno("UDMABUF_CREATE");
        return d;
    }
    d.fd = fd;
    d.offset = 0;
    d.size = size;
    return d;
}

DmaBuf DmaBufSource::List(const std::vector<std::pair<uint64_t, uint64_t>>& pieces) const {
    DmaBuf d;
    if (!SupportsList()) {
        d.error = fake_ ? "UDMABUF_CREATE_LIST is not available in self-test mode" : why_;
        return d;
    }
    const size_t bytes = sizeof(udmabuf_create_list) + pieces.size() * sizeof(udmabuf_create_item);
    std::vector<uint8_t> storage(bytes);
    auto* head = reinterpret_cast<udmabuf_create_list*>(storage.data());
    head->flags = UDMABUF_FLAGS_CLOEXEC;
    head->count = static_cast<__u32>(pieces.size());
    uint64_t total = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        head->list[i].memfd = static_cast<__u32>(gm_->Fd());
        head->list[i].__pad = 0;
        head->list[i].offset = pieces[i].first;
        head->list[i].size = pieces[i].second;
        total += pieces[i].second;
    }
    const int fd = ioctl(dev_fd_, UDMABUF_CREATE_LIST, head);
    if (fd < 0) {
        d.error = Errno("UDMABUF_CREATE_LIST");
        return d;
    }
    d.fd = fd;
    d.offset = 0;
    d.size = total;
    return d;
}

int DmaBufSync(int fd, bool start, bool read, bool write) {
    dma_buf_sync s{};
    s.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | (read ? DMA_BUF_SYNC_READ : 0) |
              (write ? DMA_BUF_SYNC_WRITE : 0);
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0 ? 0 : errno;
}

std::string ReadFileTrim(const std::string& path, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) {
            *err = std::strerror(errno);
        }
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

} // namespace e0b
