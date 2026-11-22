// SPDX-License-Identifier: GPL-3.0-only
#include "artwork.hpp"
#include "core/http_client.hpp"
#include "ui/image_url_policy.hpp"

#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace lunaone::windows
{
using Microsoft::WRL::ComPtr;

HBITMAP LoadArtwork(const std::string& address, int width, int height)
{
    if (!address.starts_with("https://"))
        return nullptr;
    const auto slash = address.find('/', 8);
    const auto host = address.substr(8, slash - 8);
    if (!host.ends_with(".media-amazon.com") && !host.ends_with(".images-amazon.com"))
        return nullptr;
    const auto response = lunanx::HttpClient{}.Get(lunanx::ResizeAmazonImageUrl(address, width > 480 ? width : 480), "LunaONE", {}, 3, 5);
    if (response.status_code != 200 || response.body.empty() || response.body.size() > 5 * 1024 * 1024)
        return nullptr;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf())) ||
        FAILED(stream->Write(response.body.data(), static_cast<ULONG>(response.body.size()), nullptr)) ||
        FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr)))
        return nullptr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conversion;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(conversion.GetAddressOf())) ||
        FAILED(conversion->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                      nullptr, 0, WICBitmapPaletteTypeCustom)))
        return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap && SUCCEEDED(conversion->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(pixels))))
        return bitmap;
    if (bitmap) DeleteObject(bitmap);
    return nullptr;
}
}
