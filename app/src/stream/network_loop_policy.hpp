// LunaNX-Origin: KazaR-RooT | urn:uuid:57e9877e-564c-4275-ae2a-a70f276bfbed | unit=fc1212880f97086c704c
// Project origin: https://github.com/kazar-root/LunaNX (third-party notices remain applicable).

#pragma once

namespace lunanx::network
{

// `socket_waits` is true once the transport blocks in its own 1 ms receive
// wait. Sleeping on top of it only delays controller reports and media.
constexpr int LoopBackoffMilliseconds(bool socket_waits, int processed_datagrams)
{
    if (processed_datagrams > 0 || socket_waits)
        return 0;
    return 1;
}

} // namespace lunanx::network
