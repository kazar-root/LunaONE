// SPDX-License-Identifier: GPL-3.0-only
#include "platform.hpp"

#if defined(__FreeBSD__)
#include <array>
#include <climits>
#include <cstring>
#include <memory>
#include <vector>

#include <sys/socket.h>
#include <sys/sysctl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>
#endif

namespace lunaone::linux_desktop
{
std::filesystem::path LogoPath()
{
#if defined(__FreeBSD__)
    const std::array mib{CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    std::array<char, PATH_MAX> path{};
    std::size_t size = path.size();
    if (sysctl(mib.data(), mib.size(), path.data(), &size, nullptr, 0) == 0 && path.front())
        return std::filesystem::path(path.data()).parent_path() / "LunaONE.png";
    return "LunaONE.png";
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path() / "LunaONE.png";
#endif
}

#if defined(__FreeBSD__)
namespace
{
unsigned DefaultRouteInterface(int family)
{
    const std::array mib{CTL_NET, PF_ROUTE, 0, family, NET_RT_DUMP, 0};
    std::size_t size = 0;
    if (sysctl(mib.data(), mib.size(), nullptr, &size, nullptr, 0) != 0 || size == 0)
        return 0;
    std::vector<char> routes(size);
    if (sysctl(mib.data(), mib.size(), routes.data(), &size, nullptr, 0) != 0)
        return 0;

    for (std::size_t offset = 0; size - offset >= sizeof(rt_msghdr);)
    {
        rt_msghdr route{};
        std::memcpy(&route, routes.data() + offset, sizeof(route));
        if (route.rtm_msglen < sizeof(route) || route.rtm_msglen > size - offset)
            break;
        const auto* address = routes.data() + offset + sizeof(route);
        const auto address_size = route.rtm_msglen - sizeof(route);
        offset += route.rtm_msglen;
        if (route.rtm_version != RTM_VERSION || !(route.rtm_addrs & RTA_DST) ||
            (route.rtm_flags & (RTF_UP | RTF_GATEWAY)) != (RTF_UP | RTF_GATEWAY))
            continue;

        if (family == AF_INET && address_size >= sizeof(sockaddr_in))
        {
            sockaddr_in destination{};
            std::memcpy(&destination, address, sizeof(destination));
            if (destination.sin_family == AF_INET && destination.sin_addr.s_addr == INADDR_ANY)
                return route.rtm_index;
        }
        if (family == AF_INET6 && address_size >= sizeof(sockaddr_in6))
        {
            sockaddr_in6 destination{};
            std::memcpy(&destination, address, sizeof(destination));
            if (destination.sin6_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&destination.sin6_addr))
                return route.rtm_index;
        }
    }
    return 0;
}
}

std::string NetworkLabel()
{
    auto index = DefaultRouteInterface(AF_INET);
    if (index == 0) index = DefaultRouteInterface(AF_INET6);
    if (index == 0) return {};

    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return {};
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> interfaces(addresses, freeifaddrs);
    for (auto* entry = interfaces.get(); entry; entry = entry->ifa_next)
    {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_LINK) continue;
        const auto* link = reinterpret_cast<const sockaddr_dl*>(entry->ifa_addr);
        if (link->sdl_index != index) continue;
        if (link->sdl_type == IFT_IEEE80211) return "Wi-Fi";
        if (link->sdl_type == IFT_ETHER) return "LAN";
    }
    return {};
}

int BatteryPercent()
{
    int value = -1;
    std::size_t size = sizeof(value);
    if (sysctlbyname("hw.acpi.battery.life", &value, &size, nullptr, 0) != 0 ||
        size != sizeof(value) || value < 0 || value > 100)
        return -1;
    return value;
}
#endif
}
