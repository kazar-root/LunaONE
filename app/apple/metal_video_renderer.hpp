// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#import <QuartzCore/QuartzCore.h>
#include <CoreVideo/CoreVideo.h>

#include <memory>

namespace lunaone::apple
{

// The picture path of LunaNX and NxNow on Metal. The decoder's 4:2:0 planes
// reach the GPU without a copy and are converted with the BT.601, BT.709 or
// BT.2020 matrix read as limited range (Luna can flag full range for limited
// samples). A spatial pass then smooths low-contrast compression blocks and
// restores bounded edge detail. It adds no buffered frame.
class MetalVideoRenderer
{
  public:
    struct Filter
    {
        float denoise;
        float sharpen;
    };

    // Returns null when Metal or the shader is unavailable.
    static std::unique_ptr<MetalVideoRenderer> Create(Filter filter);
    ~MetalVideoRenderer();
    MetalVideoRenderer(const MetalVideoRenderer&) = delete;
    MetalVideoRenderer& operator=(const MetalVideoRenderer&) = delete;

    CALayer* Layer() const;

    // Main thread. Draws the picture aspect-fit at the layer's pixel size and
    // presents it at the next display refresh. False when nothing was shown.
    bool Render(CVPixelBufferRef image);

  private:
    struct State;
    explicit MetalVideoRenderer(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace lunaone::apple
