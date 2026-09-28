// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Guest sendmsg/recvmsg go through PosixSocket::SendMessage/ReceiveMessage with a guest
// OrbisNetMsghdr, which uses the FreeBSD layout: 48 bytes, int msg_iovlen, socklen_t
// msg_controllen and a BSD sockaddr (sa_len, sa_family) in msg_name. These tests check that the
// guest-visible semantics of that structure are preserved on the host.

#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "core/libraries/network/net.h"
#include "core/libraries/network/sockets.h"

#ifndef _WIN32

namespace {

using Libraries::Net::OrbisNetIovec;
using Libraries::Net::OrbisNetMsghdr;
using Libraries::Net::OrbisNetSockaddr;
using Libraries::Net::OrbisNetSockaddrIn;
using Libraries::Net::PosixSocket;

// FreeBSD <sys/socket.h> values, as seen by the guest.
constexpr u8 kOrbisAfInet = 2;
constexpr int kOrbisMsgTrunc = 0x10;

OrbisNetSockaddrIn MakeLoopbackAddr(u16 port_be) {
    OrbisNetSockaddrIn addr{};
    addr.sin_len = sizeof(OrbisNetSockaddrIn);
    addr.sin_family = kOrbisAfInet;
    addr.sin_port = port_be;
    addr.sin_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

class PosixSocketMsgTest : public ::testing::Test {
protected:
    void SetUp() override {
        receiver = std::make_unique<PosixSocket>(AF_INET, SOCK_DGRAM, 0);
        sender = std::make_unique<PosixSocket>(AF_INET, SOCK_DGRAM, 0);
        ASSERT_TRUE(receiver->IsValid());
        ASSERT_TRUE(sender->IsValid());

        OrbisNetSockaddrIn bind_addr = MakeLoopbackAddr(0);
        ASSERT_EQ(
            receiver->Bind(reinterpret_cast<OrbisNetSockaddr*>(&bind_addr), sizeof(bind_addr)), 0);
        ASSERT_EQ(sender->Bind(reinterpret_cast<OrbisNetSockaddr*>(&bind_addr), sizeof(bind_addr)),
                  0);

        // Read the host-assigned ports directly so the test does not depend on the conversion
        // helpers under test.
        receiver_port = LocalPort(*receiver);
        sender_port = LocalPort(*sender);
        ASSERT_NE(receiver_port, 0);
        ASSERT_NE(sender_port, 0);
    }

    void TearDown() override {
        receiver->Close();
        sender->Close();
    }

    static u16 LocalPort(PosixSocket& socket) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (getsockname(socket.sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return 0;
        }
        return addr.sin_port;
    }

    void SendTo(PosixSocket& to, const char* data, size_t size) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = &to == receiver.get() ? receiver_port : sender_port;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(
            sendto(sender->sock, data, size, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
            static_cast<ssize_t>(size));
    }

    std::unique_ptr<PosixSocket> receiver;
    std::unique_ptr<PosixSocket> sender;
    u16 receiver_port{};
    u16 sender_port{};
};

TEST_F(PosixSocketMsgTest, ReceiveMessageDoesNotWritePastGuestMsghdr) {
    SendTo(*receiver, "hello", 5);

    struct {
        OrbisNetMsghdr msg;
        u32 canary;
    } guest{};
    static_assert(sizeof(OrbisNetMsghdr) == 48);
    guest.canary = 0xdeadbeef;

    std::array<char, 16> data{};
    OrbisNetIovec iov{data.data(), data.size()};
    guest.msg.msg_iov = &iov;
    guest.msg.msg_iovlen = 1;

    EXPECT_EQ(receiver->ReceiveMessage(&guest.msg, 0), 5);
    EXPECT_EQ(std::memcmp(data.data(), "hello", 5), 0);
    EXPECT_EQ(guest.canary, 0xdeadbeefU);
}

TEST_F(PosixSocketMsgTest, ReceiveMessageReturnsOrbisSourceAddress) {
    SendTo(*receiver, "hello", 5);

    OrbisNetSockaddrIn from{};
    std::array<char, 16> data{};
    OrbisNetIovec iov{data.data(), data.size()};
    OrbisNetMsghdr msg{};
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ASSERT_EQ(receiver->ReceiveMessage(&msg, 0), 5);
    EXPECT_EQ(from.sin_family, kOrbisAfInet);
    EXPECT_EQ(from.sin_port, sender_port);
    EXPECT_EQ(from.sin_addr, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(msg.msg_namelen, sizeof(OrbisNetSockaddrIn));
}

TEST_F(PosixSocketMsgTest, ReceiveMessageReportsTruncation) {
    SendTo(*receiver, "12345678", 8);

    std::array<char, 4> data{};
    OrbisNetIovec iov{data.data(), data.size()};
    OrbisNetMsghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    EXPECT_EQ(receiver->ReceiveMessage(&msg, 0), 4);
    EXPECT_NE(msg.msg_flags & kOrbisMsgTrunc, 0);
}

TEST_F(PosixSocketMsgTest, SendMessageToOrbisSockaddr) {
    char payload[] = "hello";
    OrbisNetIovec iov{payload, 5};
    OrbisNetSockaddrIn to = MakeLoopbackAddr(receiver_port);
    OrbisNetMsghdr msg{};
    msg.msg_name = &to;
    msg.msg_namelen = sizeof(to);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    EXPECT_EQ(sender->SendMessage(&msg, 0), 5);

    std::array<char, 16> data{};
    EXPECT_EQ(recv(receiver->sock, data.data(), data.size(), MSG_DONTWAIT), 5);
}

TEST_F(PosixSocketMsgTest, SendMessageIgnoresUninitializedGuestFields) {
    // FreeBSD ignores msg_flags on send and never reads the struct padding, so guests may leave
    // both uninitialized. Use a connected socket so no address conversion is involved.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = receiver_port;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connect(sender->sock, reinterpret_cast<sockaddr*>(&to), sizeof(to)), 0);

    char payload[] = "hello";
    OrbisNetIovec iov{payload, 5};
    OrbisNetMsghdr msg;
    std::memset(&msg, 0xaa, sizeof(msg));
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = nullptr;
    msg.msg_controllen = 0;

    EXPECT_EQ(sender->SendMessage(&msg, 0), 5);
}

} // Anonymous namespace

#endif
