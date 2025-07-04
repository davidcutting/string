#pragma once

#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cstdint>
#include <memory>
#include <system_error>
#include <tuple>
#include <unistd.h>

namespace net
{

enum class AddressFamily : std::uint8_t
{
    IPv4 = AF_INET,
    IPv6 = AF_INET6,
    Unspecified = AF_UNSPEC
};

enum class SocketProtocol : std::uint8_t
{
    TCP = SOCK_STREAM,
    UDP = SOCK_DGRAM
};

enum class OperationType : std::uint8_t
{
    Read = 1,
    Write = 2,
    Bidirectional = 3
};

struct ResolveInfo
{
    std::string address;
    std::string port;
    AddressFamily address_family;
    SocketProtocol socket_protocol;
    OperationType operation_type;
};

using AddressInfoPtr = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;
using Socket = std::uint32_t;

template<typename T>
using Result = std::tuple<T, std::error_condition>;
using SocketResult = Result<Socket>;

std::error_condition get_last_error() noexcept
{
    return std::error_code(errno, std::system_category()).default_error_condition();
}

AddressInfoPtr resolve(const ResolveInfo& info) noexcept
{
    addrinfo hints{};
    hints.ai_family = static_cast<int>(info.address_family);
    hints.ai_socktype = static_cast<int>(info.socket_protocol);
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    int status = ::getaddrinfo(
        info.address.empty() ? nullptr : info.address.c_str(),
        info.port.c_str(), 
        &hints, 
        &result
    );

    if (status == 0) {
        return AddressInfoPtr(result, &freeaddrinfo);
    }
    
    return AddressInfoPtr(nullptr, &freeaddrinfo);
}

SocketResult bind(const AddressInfoPtr& addr_info) noexcept
{
    int fd = ::socket(addr_info->ai_family, addr_info->ai_socktype, addr_info->ai_protocol);
    
    if (fd < 0) {
        return {Socket{}, get_last_error()};
    }

    // Set SO_REUSEADDR by default for server sockets
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (::bind(fd, addr_info->ai_addr, addr_info->ai_addrlen) == -1) {
        ::close(fd);
        return {Socket{}, get_last_error()};
    }

    return {fd, std::error_condition{}};
}

SocketResult connect(const AddressInfoPtr& addr_info) noexcept
{
    int fd = ::socket(addr_info->ai_family, addr_info->ai_socktype, addr_info->ai_protocol);
    
    if (fd < 0) {
        return {Socket{}, get_last_error()};
    }

    if (::connect(fd, addr_info->ai_addr, addr_info->ai_addrlen) == -1) {
        ::close(fd);
        return {Socket{}, get_last_error()};
    }

    return {fd, std::error_condition{}};
}

std::error_condition listen(const Socket& socket, int backlog = 128) noexcept
{
    if (socket < 0) {
        return std::make_error_condition(std::errc::bad_file_descriptor);
    }
    
    if (::listen(socket, backlog) == -1) {
        return get_last_error();
    }
    
    return {};
}

std::error_condition close(const Socket& socket) noexcept
{
    if (socket < 0) {
        return {};
    }
    
    if (::close(socket) == -1) {
        return get_last_error();
    }
    
    return {};
}

}