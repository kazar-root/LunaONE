// LunaNX-Origin: KazaR-RooT | urn:uuid:57e9877e-564c-4275-ae2a-a70f276bfbed | unit=07f7a89ba116f08358c9
// Project origin: https://github.com/kazar-root/LunaNX (third-party notices remain applicable).
#include "core/source_origin.hpp"
LUNANX_SOURCE_ORIGIN(u07f7a89ba116f08358c9, "07f7a89ba116f08358c9")

#include "luna_sdp.hpp"

#include <charconv>
#include <cstdio>
#include <random>
#include <sstream>
#include <string_view>

namespace lunanx::luna
{
namespace
{

std::vector<std::string> SplitLines(const std::string& sdp)
{
    std::vector<std::string> lines;
    std::stringstream stream(sdp);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(line);
    }
    return lines;
}

bool StartsWith(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string Value(const std::string& line, const char* prefix)
{
    return line.substr(std::char_traits<char>::length(prefix));
}

unsigned long RandomSsrc()
{
    thread_local std::mt19937 rng(std::random_device {}());
    return std::uniform_int_distribution<unsigned long>(1u, 0x7fffffffu)(rng);
}

void Append(std::string& sdp, const std::string& line)
{
    sdp += line;
    sdp += "\r\n";
}

void AppendTransport(std::string& sdp, const LocalTransport& t, bool with_candidates)
{
    Append(sdp, "a=ice-ufrag:" + t.ice_ufrag);
    Append(sdp, "a=ice-pwd:" + t.ice_pwd);
    Append(sdp, "a=ice-options:trickle");
    Append(sdp, "a=fingerprint:" + t.fingerprint);
    Append(sdp, "a=setup:actpass");
    if (with_candidates)
        for (const auto& candidate : t.candidates)
            Append(sdp, candidate);
}

} // namespace

LocalTransport ParseLocalTransport(const std::string& libpeer_sdp)
{
    LocalTransport transport;
    for (const auto& line : SplitLines(libpeer_sdp))
    {
        if (StartsWith(line, "a=ice-ufrag:"))
            transport.ice_ufrag = Value(line, "a=ice-ufrag:");
        else if (StartsWith(line, "a=ice-pwd:"))
            transport.ice_pwd = Value(line, "a=ice-pwd:");
        else if (StartsWith(line, "a=fingerprint:"))
            transport.fingerprint = Value(line, "a=fingerprint:");
        else if (StartsWith(line, "a=candidate:"))
            transport.candidates.push_back(line);
    }
    return transport;
}

constexpr const char* kTransportWideCc =
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";

std::string BuildLunaOffer(const LocalTransport& t, int video_height, int fps, OfferedProfile profile, bool transport_cc,
                           bool full_audio)
{
    const bool fallback = profile != OfferedProfile::ConstrainedBaseline;
    const unsigned long audio_ssrc = RandomSsrc();
    std::string sdp;
    char origin[96];
    std::snprintf(origin, sizeof(origin), "o=- %lu%06lu 2 IN IP4 127.0.0.1",
                  RandomSsrc(), RandomSsrc() % 1000000ul);
    Append(sdp, "v=0");
    Append(sdp, origin);
    Append(sdp, "s=-");
    Append(sdp, "t=0 0");
    Append(sdp, "a=extmap-allow-mixed");
    Append(sdp, "a=msid-semantic: WMS lunanx");
    Append(sdp, "a=group:BUNDLE 0 1 2");

    // H.264 Constrained Baseline with the level required for the requested picture size.
    // H.264 Main or High comes first on 96 and Constrained Baseline stays
    // available on 98: the server picks the one it can encode.
    Append(sdp, fallback ? "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99" : "m=video 9 UDP/TLS/RTP/SAVPF 96 97");
    Append(sdp, "c=IN IP4 0.0.0.0");
    Append(sdp, "a=rtcp:9 IN IP4 0.0.0.0");
    AppendTransport(sdp, t, true);
    Append(sdp, "a=mid:0");
    Append(sdp, "a=extmap:1 urn:ietf:params:rtp-hdrext:toffset");
    Append(sdp, "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
    // Transport-wide sequence numbers, shared by every stream of the bundle:
    // the arrival feedback lets the server raise its bitrate as Chrome does.
    if (transport_cc)
        Append(sdp, std::string("a=extmap:3 ") + kTransportWideCc);
    Append(sdp, "a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay");
    Append(sdp, "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid");
    Append(sdp, "a=recvonly");
    Append(sdp, "a=rtcp-mux");
    Append(sdp, "a=rtcp-rsize");
    const bool sixty = fps > 30;
    const char* level = video_height >= 2160 ? (sixty ? "34" : "33") :
                        video_height >= 1440 ? (sixty ? "33" : "32") :
                        video_height >= 1080 ? (sixty ? "2a" : "28") : (sixty ? "20" : "1f");
    const auto append_h264 = [&](int pt, const char* profile) {
        const auto payload = std::to_string(pt);
        Append(sdp, "a=rtpmap:" + payload + " H264/90000");
        Append(sdp, "a=rtcp-fb:" + payload + " goog-remb");
        if (transport_cc)
            Append(sdp, "a=rtcp-fb:" + payload + " transport-cc");
        Append(sdp, "a=rtcp-fb:" + payload + " ccm fir");
        Append(sdp, "a=rtcp-fb:" + payload + " nack");
        Append(sdp, "a=rtcp-fb:" + payload + " nack pli");
        Append(sdp, "a=fmtp:" + payload + " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=" +
                    profile + level);
        Append(sdp, "a=rtpmap:" + std::to_string(pt + 1) + " rtx/90000");
        Append(sdp, "a=fmtp:" + std::to_string(pt + 1) + " apt=" + payload);
    };
    // Main adds CABAC; High adds the 8x8 transform too: far fewer blocks and
    // less noise in dark or fast scenes for the same bitrate.
    if (fallback)
    {
        append_h264(96, profile == OfferedProfile::High ? "6400" : "4d00");
        append_h264(98, "42e0");
    }
    else
    {
        append_h264(96, "42e0");
    }

    // Audio : Opus stéréo. Le serveur ouvre la voie en sendrecv (micro) ; on n'envoie rien.
    Append(sdp, "m=audio 9 UDP/TLS/RTP/SAVPF 63 111");
    Append(sdp, "c=IN IP4 0.0.0.0");
    Append(sdp, "a=rtcp:9 IN IP4 0.0.0.0");
    AppendTransport(sdp, t, false);
    Append(sdp, "a=mid:1");
    Append(sdp, "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
    if (transport_cc)
        Append(sdp, std::string("a=extmap:3 ") + kTransportWideCc);
    Append(sdp, "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid");
    Append(sdp, "a=sendrecv");
    Append(sdp, "a=msid:lunanx lunanx-audio");
    Append(sdp, "a=rtcp-mux");
    Append(sdp, "a=rtcp-rsize");
    Append(sdp, "a=rtpmap:63 red/48000/2");
    Append(sdp, "a=fmtp:63 111/111");
    Append(sdp, "a=rtpmap:111 opus/48000/2");
    Append(sdp, "a=rtcp-fb:111 transport-cc");
    Append(sdp, std::string("a=fmtp:111 minptime=10;stereo=1;sprop-stereo=1;useinbandfec=1") +
                    (full_audio ? ";maxaveragebitrate=510000;maxplaybackrate=48000" : ""));
    Append(sdp, "a=ssrc:" + std::to_string(audio_ssrc) + " cname:lunanx");
    Append(sdp, "a=ssrc:" + std::to_string(audio_ssrc) + " msid:lunanx lunanx-audio");

    // Data channels (manette, affichage, contrôle).
    Append(sdp, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel");
    Append(sdp, "c=IN IP4 0.0.0.0");
    AppendTransport(sdp, t, false);
    Append(sdp, "a=mid:2");
    Append(sdp, "a=sctp-port:5000");
    Append(sdp, "a=max-message-size:262144");
    return sdp;
}

std::string NormalizeLunaAnswer(const std::string& answer_sdp)
{
    std::string out;
    for (const auto& line : SplitLines(answer_sdp))
    {
        // The audio path supports RED for Opus on 63. Leave video
        // redundancy codecs out of the description passed to libpeer.
        if ((line.find(" red/") != std::string::npos && line != "a=rtpmap:63 red/48000/2") ||
            line.find(" ulpfec/") != std::string::npos ||
            line.find(" flexfec") != std::string::npos)
            continue;
        Append(out, line);
    }
    return out;
}

int AnsweredH264PayloadType(const std::string& answer_sdp)
{
    // Adapted for LunaCore on 2026-11-18. Match the codec as well as the payload number.
    constexpr std::string_view prefix = "a=rtpmap:";
    for (const auto& line : SplitLines(answer_sdp))
    {
        if (!line.starts_with(prefix))
            continue;
        int pt = -1;
        const char* end = line.data() + line.size();
        const auto [stop, error] = std::from_chars(line.data() + prefix.size(), end, pt);
        if (error == std::errc{} && pt >= 0 && pt <= 127 &&
            std::string_view(stop, end) == " H264/90000")
            return pt;
    }
    return -1;
}

} // namespace lunanx::luna
