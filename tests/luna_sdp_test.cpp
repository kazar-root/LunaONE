// SPDX-License-Identifier: GPL-3.0-only

#include "luna/luna_sdp.hpp"

#include <cassert>
#include <string>

int main()
{
    using namespace lunanx::luna;

    const std::string candidate = "a=candidate:1 1 UDP 2122194687 192.0.2.10 5000 typ host";
    const auto transport = ParseLocalTransport(
        "v=0\r\na=ice-ufrag:example\r\na=ice-pwd:example-password\r\n"
        "a=fingerprint:sha-256 AA:BB\r\n" + candidate + "\r\n");
    assert(transport.ice_ufrag == "example");
    assert(transport.ice_pwd == "example-password");
    assert(transport.fingerprint == "sha-256 AA:BB");
    assert(transport.candidates.size() == 1 && transport.candidates.front() == candidate);

    const auto offer = BuildLunaOffer(transport);
    assert(offer.find("a=group:BUNDLE 0 1 2\r\n") != std::string::npos);
    assert(offer.find(candidate + "\r\n") != std::string::npos);
    assert(offer.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos);
    assert(offer.find("m=audio 9 UDP/TLS/RTP/SAVPF 63 111\r\n") != std::string::npos);
    assert(offer.find("a=rtpmap:63 red/48000/2\r\na=fmtp:63 111/111\r\n") != std::string::npos);
    assert(offer.find("a=sctp-port:5000\r\n") != std::string::npos);
    assert(AnsweredH264PayloadType(offer) == 96);
    assert(offer.find("profile-level-id=42e020") != std::string::npos);
    assert(BuildLunaOffer(transport, 720, 30).find("profile-level-id=42e01f") != std::string::npos);
    assert(BuildLunaOffer(transport, 1080, 60).find("profile-level-id=42e02a") != std::string::npos);
    assert(offer.find("m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n") != std::string::npos);

    // High first on 96, Constrained Baseline kept on 98, each with its RTX.
    const auto high = BuildLunaOffer(transport, 1080, 60, OfferedProfile::High);
    assert(high.find("m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99\r\n") != std::string::npos);
    assert(high.find("a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=64002a\r\n") !=
           std::string::npos);
    assert(high.find("a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e02a\r\n") !=
           std::string::npos);
    assert(high.find("a=fmtp:97 apt=96\r\n") != std::string::npos && high.find("a=fmtp:99 apt=98\r\n") != std::string::npos);
    assert(high.find("a=rtcp-fb:98 nack pli\r\n") != std::string::npos);
    assert(AnsweredH264PayloadType(high) == 96);
    // Main: 4d on 96, Baseline still on 98.
    const auto main = BuildLunaOffer(transport, 720, 60, OfferedProfile::Main);
    assert(main.find("profile-level-id=4d0020\r\n") != std::string::npos &&
           main.find("a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e020\r\n") !=
               std::string::npos);

    // Transport-wide congestion control: the extension in both media
    // sections (same id across the bundle) and the feedback on every codec.
    const auto twcc = BuildLunaOffer(transport, 1080, 60, OfferedProfile::High, true);
    const std::string twcc_extension =
        "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n";
    const auto first_extension = twcc.find(twcc_extension);
    assert(first_extension != std::string::npos && first_extension < twcc.find("m=audio"));
    assert(twcc.find(twcc_extension, twcc.find("m=audio")) != std::string::npos);
    assert(twcc.find("a=rtcp-fb:96 transport-cc\r\n") != std::string::npos);
    assert(twcc.find("a=rtcp-fb:98 transport-cc\r\n") != std::string::npos);
    assert(high.find("rmcat-transport-wide-cc") == std::string::npos);

    // Ultra mode: Opus at its highest rate.
    const auto full = BuildLunaOffer(transport, 1080, 60, OfferedProfile::High, true, true);
    assert(full.find("useinbandfec=1;maxaveragebitrate=510000;maxplaybackrate=48000\r\n") != std::string::npos);
    assert(twcc.find("maxaveragebitrate") == std::string::npos);

    const auto answer = NormalizeLunaAnswer(
        "v=0\na=rtpmap:98 red/90000\na=rtpmap:111 opus/48000/2\n"
        "a=rtpmap:63 red/48000/2\na=rtpmap:104 VP9/90000\na=rtpmap:102 H264/90000\n");
    assert(answer.find("red/90000") == std::string::npos);
    assert(answer.find("a=rtpmap:63 red/48000/2\r\n") != std::string::npos);
    assert(answer.ends_with("a=rtpmap:102 H264/90000\r\n"));
    assert(AnsweredH264PayloadType(answer) == 102);
    assert(AnsweredH264PayloadType("a=rtpmap:111 opus/48000/2\r\n") == -1);
    assert(AnsweredH264PayloadType("a=rtpmap:128 H264/90000\r\n") == -1);
}
