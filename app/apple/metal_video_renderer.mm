// SPDX-License-Identifier: GPL-3.0-only

#include "metal_video_renderer.hpp"
#include "core/runtime_journal.hpp"

#import <Metal/Metal.h>
#include <CoreVideo/CVMetalTextureCache.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace lunaone::apple
{
namespace
{
// Same shader as LunaNX and NxNow (texture_fsh.glsl), in Metal.
constexpr char kShader[] = R"(
#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4 column0;
    float4 column1;
    float4 column2;
    float4 offset;
    float4 quality; // xy: luma texel size, z: denoise, w: sharpen
};

struct Varyings
{
    float4 position [[position]];
    float2 uv;
};

vertex Varyings video_vertex(uint corner [[vertex_id]])
{
    const float2 positions[4] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0)};
    const float2 coordinates[4] = {float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0)};
    Varyings out;
    out.position = float4(positions[corner], 0.0, 1.0);
    out.uv = coordinates[corner];
    return out;
}

fragment float4 video_fragment(Varyings in [[stage_in]],
                               texture2d<float> luma [[texture(0)]],
                               texture2d<float> chroma [[texture(1)]],
                               constant Uniforms& u [[buffer(0)]])
{
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    const float2 uv = in.uv;
    const float center = luma.sample(s, uv).r;
    float y = center;
    if (u.quality.z + u.quality.w > 0.0)
    {
        const float2 texel = u.quality.xy;
        const float north = luma.sample(s, uv - float2(0.0, texel.y)).r;
        const float south = luma.sample(s, uv + float2(0.0, texel.y)).r;
        const float west = luma.sample(s, uv - float2(texel.x, 0.0)).r;
        const float east = luma.sample(s, uv + float2(texel.x, 0.0)).r;
        const float average = (north + south + west + east) * 0.25;
        const float local_min = min(center, min(min(north, south), min(west, east)));
        const float local_max = max(center, max(max(north, south), max(west, east)));
        const float local_range = local_max - local_min;

        // Smooth low-contrast quantization blocks, then restore only bounded
        // high-contrast detail. This is spatial and adds no buffered frame.
        const float flat_weight = 1.0 - smoothstep(0.025, 0.10, local_range);
        const float cleaned = mix(center, average, u.quality.z * flat_weight);
        const float edge_weight = smoothstep(0.045, 0.20, local_range);
        y = cleaned + (cleaned - average) * u.quality.w * edge_weight;
        y = clamp(y, local_min - 0.008, local_max + 0.008);
    }
    const float3 yuv = float3(y, chroma.sample(s, uv).rg) - u.offset.xyz;
    const float3x3 yuv_to_rgb = float3x3(u.column0.xyz, u.column1.xyz, u.column2.xyz);
    return float4(clamp(yuv_to_rgb * yuv, 0.0, 1.0), 1.0);
}
)";

struct Uniforms
{
    float column0[4];
    float column1[4];
    float column2[4];
    float offset[4];
    float quality[4];
};
static_assert(sizeof(Uniforms) == 80, "Must match the Metal Uniforms layout.");

// Limited-range matrices of LunaNX, by column.
constexpr float kBt601[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f, -0.3917f, 2.0172f, 1.5960f, -0.8129f, 0.0f};
constexpr float kBt709[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f, -0.2132f, 2.1124f, 1.7927f, -0.5329f, 0.0f};
constexpr float kBt2020[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f, -0.1874f, 2.1418f, 1.6781f, -0.6505f, 0.0f};

const float* MatrixFor(CVPixelBufferRef image)
{
    // Like LunaNX, BT.601 unless the stream names another matrix.
    const float* matrix = kBt601;
    if (const auto value = CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr))
    {
        if (CFEqual(value, kCVImageBufferYCbCrMatrix_ITU_R_709_2))
            matrix = kBt709;
        else if (CFEqual(value, kCVImageBufferYCbCrMatrix_ITU_R_2020))
            matrix = kBt2020;
        CFRelease(value);
    }
    return matrix;
}
} // namespace

struct MetalVideoRenderer::State
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline;
    CAMetalLayer* layer;
    CVMetalTextureCacheRef cache = nullptr;
    Filter filter {};

    ~State()
    {
        if (cache)
            CFRelease(cache);
    }
};

MetalVideoRenderer::MetalVideoRenderer(std::unique_ptr<State> state) : state_(std::move(state)) {}

MetalVideoRenderer::~MetalVideoRenderer() = default;

std::unique_ptr<MetalVideoRenderer> MetalVideoRenderer::Create(Filter filter)
{
    @autoreleasepool
    {
        auto state = std::make_unique<State>();
        state->filter = filter;
        state->device = MTLCreateSystemDefaultDevice();
        if (!state->device)
            return nullptr;
        state->queue = [state->device newCommandQueue];
        NSError* error = nil;
        id<MTLLibrary> library = [state->device newLibraryWithSource:[NSString stringWithUTF8String:kShader]
                                                             options:nil
                                                               error:&error];
        if (!library)
        {
            lunanx::LogRuntimeEvent("apple", "metal_unavailable",
                std::string("shader=") + (error.localizedDescription.UTF8String ?: "?"));
            return nullptr;
        }
        auto descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"video_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"video_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        state->pipeline = [state->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!state->queue || !state->pipeline ||
            CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, state->device, nullptr, &state->cache) !=
                kCVReturnSuccess)
        {
            lunanx::LogRuntimeEvent("apple", "metal_unavailable", "pipeline");
            return nullptr;
        }

        auto layer = [CAMetalLayer layer];
        layer.device = state->device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
        layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        // Two drawables: a picture is never queued behind another one.
        layer.maximumDrawableCount = 2;
        // The shader writes BT.709-encoded values; let the system match them
        // to the display as AVFoundation does for video.
        if (const auto space = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709))
        {
            layer.colorspace = space;
            CGColorSpaceRelease(space);
        }
#if TARGET_OS_OSX
        layer.displaySyncEnabled = YES;
#endif
        state->layer = layer;
        lunanx::LogRuntimeEvent("apple", "picture_renderer",
            "metal device=" + std::string(state->device.name.UTF8String ?: "?") +
            " denoise=" + std::to_string(filter.denoise) + " sharpen=" + std::to_string(filter.sharpen));
        return std::unique_ptr<MetalVideoRenderer>(new MetalVideoRenderer(std::move(state)));
    }
}

CALayer* MetalVideoRenderer::Layer() const
{
    return state_->layer;
}

bool MetalVideoRenderer::Render(CVPixelBufferRef image)
{
    @autoreleasepool
    {
        auto& state = *state_;
        const OSType format = CVPixelBufferGetPixelFormatType(image);
        if (format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
            format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
            return false;
        const auto width = CVPixelBufferGetWidthOfPlane(image, 0);
        const auto height = CVPixelBufferGetHeightOfPlane(image, 0);
        const auto chroma_width = CVPixelBufferGetWidthOfPlane(image, 1);
        const auto chroma_height = CVPixelBufferGetHeightOfPlane(image, 1);
        if (!width || !height || !chroma_width || !chroma_height)
            return false;

        const CGSize bounds = state.layer.bounds.size;
        const CGFloat scale = state.layer.contentsScale;
        const CGSize size = CGSizeMake(std::round(bounds.width * scale), std::round(bounds.height * scale));
        if (size.width < 1 || size.height < 1)
            return false;
        if (!CGSizeEqualToSize(state.layer.drawableSize, size))
            state.layer.drawableSize = size;

        CVMetalTextureRef luma = nullptr;
        CVMetalTextureRef chroma = nullptr;
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, state.cache, image, nullptr,
                MTLPixelFormatR8Unorm, width, height, 0, &luma) != kCVReturnSuccess)
            return false;
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, state.cache, image, nullptr,
                MTLPixelFormatRG8Unorm, chroma_width, chroma_height, 1, &chroma) != kCVReturnSuccess)
        {
            CFRelease(luma);
            return false;
        }
        id<CAMetalDrawable> drawable = [state.layer nextDrawable];
        if (!drawable)
        {
            CFRelease(luma);
            CFRelease(chroma);
            return false;
        }

        Uniforms uniforms {};
        const float* matrix = MatrixFor(image);
        std::memcpy(uniforms.column0, matrix, 3 * sizeof(float));
        std::memcpy(uniforms.column1, matrix + 3, 3 * sizeof(float));
        std::memcpy(uniforms.column2, matrix + 6, 3 * sizeof(float));
        // Limited range even when full range is flagged, as LunaNX does.
        uniforms.offset[0] = 16.0f / 255.0f;
        uniforms.offset[1] = 128.0f / 255.0f;
        uniforms.offset[2] = 128.0f / 255.0f;
        uniforms.quality[0] = 1.0f / static_cast<float>(width);
        uniforms.quality[1] = 1.0f / static_cast<float>(height);
        uniforms.quality[2] = state.filter.denoise;
        uniforms.quality[3] = state.filter.sharpen;

        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        id<MTLCommandBuffer> commands = [state.queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        const double fit = std::min(size.width / static_cast<double>(width), size.height / static_cast<double>(height));
        const double shown_width = std::round(width * fit);
        const double shown_height = std::round(height * fit);
        [encoder setViewport:(MTLViewport){std::floor((size.width - shown_width) / 2),
                                           std::floor((size.height - shown_height) / 2),
                                           shown_width, shown_height, 0.0, 1.0}];
        [encoder setRenderPipelineState:state.pipeline];
        [encoder setFragmentTexture:CVMetalTextureGetTexture(luma) atIndex:0];
        [encoder setFragmentTexture:CVMetalTextureGetTexture(chroma) atIndex:1];
        [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
        [commands presentDrawable:drawable];

        // The decoded surface must outlive the GPU work that samples it.
        CVPixelBufferRetain(image);
        [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
            CFRelease(luma);
            CFRelease(chroma);
            CVPixelBufferRelease(image);
        }];
        [commands commit];
        return true;
    }
}

} // namespace lunaone::apple
