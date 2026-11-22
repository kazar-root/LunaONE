// SPDX-License-Identifier: GPL-3.0-only

#include "video_decoder.hpp"
#include "stream/DecodeQueuePolicy.hpp"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "core/runtime_journal.hpp"
#include "core/playback_preferences.hpp"

namespace lunaone::windows
{
namespace
{
using Microsoft::WRL::ComPtr;

class DecodeOutputError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

template <class Error = std::runtime_error>
void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(static_cast<DWORD>(hr)));
        throw Error(std::string(operation) + " (" + code + ").");
    }
}

class MediaFoundation
{
  public:
    MediaFoundation()
    {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialisation de COM");
        try
        {
            module_ = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module_)
                throw std::runtime_error("Media Foundation est indisponible. Vérifie que les composants multimédias Windows sont installés.");
            const auto startup = Load<decltype(&MFStartup)>("MFStartup");
            shutdown = Load<decltype(&MFShutdown)>("MFShutdown");
            media_type = Load<decltype(&MFCreateMediaType)>("MFCreateMediaType");
            sample = Load<decltype(&MFCreateSample)>("MFCreateSample");
            aligned_buffer = Load<decltype(&MFCreateAlignedMemoryBuffer)>("MFCreateAlignedMemoryBuffer");
            Check(startup(MF_VERSION, MFSTARTUP_LITE), "Initialisation de Media Foundation");
        }
        catch (...)
        {
            if (module_)
                FreeLibrary(module_);
            CoUninitialize();
            throw;
        }
    }
    ~MediaFoundation()
    {
        shutdown();
        FreeLibrary(module_);
        CoUninitialize();
    }

    decltype(&MFCreateMediaType) media_type{};
    decltype(&MFCreateSample) sample{};
    decltype(&MFCreateAlignedMemoryBuffer) aligned_buffer{};

  private:
    template <class Function> Function Load(const char* name)
    {
        const auto address = GetProcAddress(module_, name);
        if (!address)
            throw std::runtime_error(std::string("Fonction Media Foundation indisponible ") + name);
        return reinterpret_cast<Function>(address);
    }

    HMODULE module_{};
    decltype(&MFShutdown) shutdown{};
};

class Transform
{
  public:
    Transform(const MediaFoundation& system, const VideoDecoder::FrameReady& ready, bool compatibility = false)
        : system_(system), ready_(ready), compatibility_(compatibility)
    {
        Check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(decoder_.GetAddressOf())), "Ouverture du décodeur H.264 Windows");
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(decoder_->GetAttributes(attributes.GetAddressOf())))
        {
            constexpr GUID acceleration{STATIC_CODECAPI_AVDecVideoAcceleration_H264};
            attributes->SetUINT32(acceleration, FALSE);
            if (!compatibility_)
                attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        ComPtr<IMFMediaType> type;
        Check(system_.media_type(type.GetAddressOf()), "Création du format vidéo");
        Check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Configuration du type vidéo");
        Check(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Configuration H.264");
        // Start with the dimensions and frame rate requested from Luna. The MFT can
        // still signal a different output size once it has read the SPS.
        const auto profile = lunanx::PlaybackDisplay(lunanx::LoadPlaybackPreferences());
        Check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, profile.width, profile.height), "Taille d’entrée H.264");
        fps_ = profile.fps;
        Check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_, 1), "Cadence d’entrée H.264");
        Check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Rapport de pixel H.264");
        Check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
              "Configuration du balayage vidéo");
        Check(decoder_->SetInputType(0, type.Get(), 0), "Configuration de l’entrée H.264");
        SelectOutput();
        Check(decoder_->GetInputStreamInfo(0, &input_), "Configuration des tampons H.264");
        Check(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Préparation du décodeur");
        Check(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Démarrage du décodeur");
    }

    void Reset()
    {
        if (first_packet_)
            return;
        Check(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "Réinitialisation du décodeur");
        Check(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Démarrage du décodeur");
        first_packet_ = true;
    }

    void Submit(std::span<const std::uint8_t> bytes, std::uint32_t timestamp, bool keyframe)
    {
        if (first_packet_)
            first_timestamp_ = timestamp;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        Check(system_.sample(sample.GetAddressOf()), "Création de l’échantillon H.264");
        Check(system_.aligned_buffer(std::max<DWORD>(input_.cbSize, static_cast<DWORD>(bytes.size())),
              input_.cbAlignment ? input_.cbAlignment - 1 : 0, buffer.GetAddressOf()),
              "Allocation de l’échantillon H.264");
        BYTE* data;
        Check(buffer->Lock(&data, nullptr, nullptr), "Accès à l’échantillon H.264");
        std::memcpy(data, bytes.data(), bytes.size());
        buffer->Unlock();
        Check(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())), "Taille de l’échantillon H.264");
        Check(sample->AddBuffer(buffer.Get()), "Assemblage de l’échantillon H.264");
        const auto ticks = static_cast<std::int32_t>(timestamp - first_timestamp_);
        Check(sample->SetSampleTime(static_cast<LONGLONG>(ticks) * 10'000'000 / 90'000), "Horodatage H.264");
        Check(sample->SetSampleDuration(10'000'000 / fps_), "Durée de l’échantillon H.264");
        if (keyframe)
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (first_packet_)
            sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);

        HRESULT result = decoder_->ProcessInput(0, sample.Get(), 0);
        if (result == MF_E_NOTACCEPTING)
        {
            Drain();
            result = decoder_->ProcessInput(0, sample.Get(), 0);
        }
        Check(result, "Décodage H.264");
        first_packet_ = false;
        Drain();
    }

  private:
    void SelectOutput()
    {
        const auto& preferred = compatibility_ ? MFVideoFormat_YV12 : MFVideoFormat_NV12;
        ComPtr<IMFMediaType> fallback;
        ComPtr<IMFMediaType> selected;
        for (DWORD index = 0;; ++index)
        {
            ComPtr<IMFMediaType> type;
            const auto result = decoder_->GetOutputAvailableType(0, index, type.GetAddressOf());
            if (result == MF_E_NO_MORE_TYPES)
                break;
            Check(result, "Sélection du format vidéo décodé");
            GUID format{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &format)))
                continue;
            if (format == preferred)
            {
                selected = std::move(type);
                break;
            }
            if (!fallback && (format == MFVideoFormat_NV12 || format == MFVideoFormat_YV12))
                fallback = std::move(type);
        }
        if (!selected)
            selected = std::move(fallback);
        if (!selected)
            throw std::runtime_error("Aucun format de sortie H.264 pris en charge par LunaCore.");
        Check(decoder_->SetOutputType(0, selected.Get(), 0), "Configuration de la sortie vidéo");
        GUID format{};
        Check(selected->GetGUID(MF_MT_SUBTYPE, &format), "Format de la sortie vidéo");
        planar_ = format == MFVideoFormat_YV12;
        Check(MFGetAttributeSize(selected.Get(), MF_MT_FRAME_SIZE, &width_, &height_), "Dimensions de la vidéo");
        if (!width_ || !height_ || width_ > 4096 || height_ > 2304 || width_ % 2 || height_ % 2)
            throw std::runtime_error("Dimensions de sortie vidéo invalides.");
        UINT32 stride = width_;
        selected->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
        stride_ = stride >= width_ && stride <= 8192 && stride % 2 == 0 ? stride : width_;
        MFT_OUTPUT_STREAM_INFO output{};
        Check(decoder_->GetOutputStreamInfo(0, &output), "Configuration des tampons vidéo");
        output_sample_.Reset();
        output_buffer_.Reset();
        if (!(output.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
        {
            Check(system_.sample(output_sample_.GetAddressOf()), "Création de la sortie vidéo");
            const DWORD bytes = std::max<DWORD>(output.cbSize, stride_ * height_ * 3 / 2);
            Check(system_.aligned_buffer(bytes, output.cbAlignment ? output.cbAlignment - 1 : 0,
                  output_buffer_.GetAddressOf()), "Allocation de la sortie vidéo");
            Check(output_sample_->AddBuffer(output_buffer_.Get()), "Assemblage de la sortie vidéo");
        }
        lunanx::LogRuntimeEvent("windows", "decoder_format",
            std::string(planar_ ? "YV12" : "NV12") + " " + std::to_string(width_) + "x" + std::to_string(height_) +
            " stride=" + std::to_string(stride_) + " flags=" + std::to_string(output.dwFlags));
    }

    void Drain()
    {
        for (;;)
        {
            MFT_OUTPUT_DATA_BUFFER output{};
            output.pSample = output_sample_.Get();
            if (output_buffer_)
                Check(output_buffer_->SetCurrentLength(0), "Réutilisation du tampon de sortie");
            DWORD status = 0;
            const HRESULT result = decoder_->ProcessOutput(0, 1, &output, &status);
            ComPtr<IMFCollection> events;
            events.Attach(output.pEvents);
            ComPtr<IMFSample> provided;
            if (output.pSample != output_sample_.Get())
                provided.Attach(output.pSample);
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                SelectOutput();
                continue;
            }
            if (result == E_FAIL || result == MF_E_INVALID_STREAM_DATA)
                Check<DecodeOutputError>(result, "Lecture de l’image décodée");
            Check(result, "Lecture de l’image décodée");
            if (output.pSample && (output.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) != MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE)
                Publish(output.pSample);
        }
    }

    void Publish(IMFSample* sample)
    {
        ComPtr<IMFMediaBuffer> buffer;
        Check(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()), "Accès au tampon vidéo");
        ComPtr<IMF2DBuffer> planes;
        auto frame = std::make_shared<VideoFrame>();
        frame->width = static_cast<int>(width_);
        frame->height = static_cast<int>(height_);
        frame->pixels.resize(static_cast<std::size_t>(width_) * height_);
        if (SUCCEEDED(buffer.As(&planes)))
        {
            DWORD bytes;
            Check(planes->GetContiguousLength(&bytes), "Taille des plans NV12");
            if (bytes < width_ * height_ * 3 / 2)
                throw std::runtime_error("Image décodée incomplète.");
            contiguous_.resize(bytes);
            Check(planes->ContiguousCopyTo(contiguous_.data(), bytes), "Copie des plans vidéo");
            Convert(contiguous_.data(), width_, *frame);
        }
        else
        {
            BYTE* bytes;
            DWORD count;
            Check(buffer->Lock(&bytes, nullptr, &count), "Lecture des plans vidéo");
            const UINT32 stride = count >= stride_ * height_ * 3 / 2 ? stride_ : width_;
            if (count >= stride * height_ * 3 / 2)
                Convert(bytes, stride, *frame);
            buffer->Unlock();
            if (count < stride * height_ * 3 / 2)
                throw std::runtime_error("Image décodée incomplète.");
        }
        ready_(std::move(frame));
    }

    void Convert(const BYTE* data, UINT32 stride, VideoFrame& frame) const
    {
        const BYTE* chroma = data + stride * frame.height;
        const BYTE* second_plane = chroma + stride * frame.height / 4;
        for (int y = 0; y < frame.height; ++y)
        {
            const BYTE* luma = data + stride * y;
            const BYTE* uv = chroma + (planar_ ? stride / 2 : stride) * (y / 2);
            const BYTE* u_plane = second_plane + (planar_ ? stride / 2 * (y / 2) : 0);
            auto* out = frame.pixels.data() + static_cast<std::size_t>(y) * frame.width;
            for (int x = 0; x < frame.width; x += 2)
            {
                const int u = (planar_ ? u_plane[x / 2] : uv[x]) - 128;
                const int v = (planar_ ? uv[x / 2] : uv[x + 1]) - 128;
                const int red = 459 * v;
                const int green = -55 * u - 136 * v;
                const int blue = 541 * u;
                for (int offset = 0; offset < 2; ++offset)
                {
                    const int level = 298 * (luma[x + offset] - 16) + 128;
                    const auto channel = [level](int color) {
                        return static_cast<std::uint32_t>(std::clamp((level + color) >> 8, 0, 255));
                    };
                    out[x + offset] = channel(blue) | (channel(green) << 8) | (channel(red) << 16);
                }
            }
        }
    }

    const MediaFoundation& system_;
    const VideoDecoder::FrameReady& ready_;
    ComPtr<IMFTransform> decoder_;
    ComPtr<IMFSample> output_sample_;
    ComPtr<IMFMediaBuffer> output_buffer_;
    MFT_INPUT_STREAM_INFO input_{};
    std::vector<BYTE> contiguous_;
    UINT32 width_ = 0, height_ = 0, stride_ = 0, fps_ = 60;
    std::uint32_t first_timestamp_ = 0;
    bool first_packet_ = true;
    bool planar_ = false;
    bool compatibility_ = false;
};

std::size_t NextNal(std::span<const std::uint8_t> bytes, std::size_t from)
{
    for (std::size_t i = from; i + 3 < bytes.size(); ++i)
        if (bytes[i] == 0 && bytes[i + 1] == 0 &&
            (bytes[i + 2] == 1 || (bytes[i + 2] == 0 && bytes[i + 3] == 1)))
            return i;
    return bytes.size();
}
} // namespace

VideoDecoder::VideoDecoder(FrameReady ready, int fps)
    : frame_ready_(std::move(ready)), queue_limit_(lunanx::video::MaximumBurstAccessUnits(fps)),
      worker_(&VideoDecoder::Decode, this)
{
}

VideoDecoder::~VideoDecoder()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    available_.notify_one();
    worker_.join();
}

bool VideoDecoder::Submit(std::span<const std::uint8_t> bytes, std::uint32_t timestamp)
{
    std::lock_guard lock(mutex_);
    if (!error_.empty())
        return false;
    bool idr = false, slice = false, sps = false, pps = false;
    for (auto start = NextNal(bytes, 0); start < bytes.size();)
    {
        const auto header = start + (bytes[start + 2] == 1 ? 3 : 4);
        if (header >= bytes.size())
            break;
        const auto next = NextNal(bytes, header + 1);
        const auto type = bytes[header] & 31;
        if (type == 7 || type == 8)
        {
            auto& parameter_set = type == 7 ? sps_ : pps_;
            const auto nal = bytes.subspan(start, next - start);
            if (nal.size() <= 4096 && !std::ranges::equal(parameter_set, nal))
            {
                parameter_set.assign(nal.begin(), nal.end());
                packets_.clear();
                queued_bytes_ = 0;
                needs_keyframe_ = true;
            }
            (type == 7 ? sps : pps) = true;
        }
        idr |= type == 5;
        slice |= type >= 1 && type <= 5;
        start = next;
    }
    if (!slice)
        return !needs_keyframe_;
    const auto now = std::chrono::steady_clock::now();
    if (packets_.size() >= queue_limit_ ||
        bytes.size() + sps_.size() + pps_.size() > lunanx::video::MaximumBurstQueueBytes - queued_bytes_ ||
        (!packets_.empty() && now - packets_.front().arrival >
            std::chrono::milliseconds(lunanx::video::MaximumBurstQueueDelayMs)))
    {
        packets_.clear();
        queued_bytes_ = 0;
        needs_keyframe_ = true;
    }
    if (bytes.size() + sps_.size() + pps_.size() > lunanx::video::MaximumBurstQueueBytes)
        return false;
    if (needs_keyframe_ && (!idr || sps_.empty() || pps_.empty()))
        return false;
    Packet packet{{}, timestamp, now, idr, needs_keyframe_};
    if (idr && (!sps || !pps) && !sps_.empty() && !pps_.empty())
    {
        packet.bytes.insert(packet.bytes.end(), sps_.begin(), sps_.end());
        packet.bytes.insert(packet.bytes.end(), pps_.begin(), pps_.end());
    }
    packet.bytes.insert(packet.bytes.end(), bytes.begin(), bytes.end());
    queued_bytes_ += packet.bytes.size();
    packets_.push_back(std::move(packet));
    needs_keyframe_ = false;
    available_.notify_one();
    return true;
}

std::string VideoDecoder::Error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

void VideoDecoder::Decode()
{
    try
    {
        const MediaFoundation system;
        auto decoder = std::make_unique<Transform>(system, frame_ready_);
        bool compatibility = false;
        ready_ = true;
        for (;;)
        {
            std::unique_lock lock(mutex_);
            available_.wait(lock, [this] { return stopping_ || !packets_.empty(); });
            if (stopping_)
                return;
            Packet packet = std::move(packets_.front());
            packets_.pop_front();
            queued_bytes_ -= packet.bytes.size();
            lock.unlock();
            try
            {
                if (packet.reset)
                    decoder->Reset();
                decoder->Submit(packet.bytes, packet.timestamp, packet.keyframe);
            }
            catch (const DecodeOutputError& error)
            {
                if (compatibility)
                    throw;
                compatibility = true;
                lunanx::LogRuntimeEvent("windows", "decoder_compatibility", error.what());
                decoder = std::make_unique<Transform>(system, frame_ready_, true);
                if (packet.keyframe)
                    decoder->Submit(packet.bytes, packet.timestamp, true);
                else
                {
                    lock.lock();
                    packets_.clear();
                    queued_bytes_ = 0;
                    needs_keyframe_ = true;
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(mutex_);
        error_ = error.what();
    }
}
} // namespace lunaone::windows
