//
// Copyright (c) 2012-2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// renderer11_utils.cpp: Conversion functions and other utility routines
// specific to the D3D11 renderer.

#include "libANGLE/renderer/d3d/d3d11/renderer11_utils.h"

#include <algorithm>

#include "common/debug.h"
#include "libANGLE/Buffer.h"
#include "libANGLE/Context.h"
#include "libANGLE/Framebuffer.h"
#include "libANGLE/Program.h"
#include "libANGLE/State.h"
#include "libANGLE/VertexArray.h"
#include "libANGLE/formatutils.h"
#include "libANGLE/renderer/d3d/BufferD3D.h"
#include "libANGLE/renderer/d3d/FramebufferD3D.h"
#include "libANGLE/renderer/d3d/d3d11/Context11.h"
#include "libANGLE/renderer/d3d/d3d11/RenderTarget11.h"
#include "libANGLE/renderer/d3d/d3d11/Renderer11.h"
#include "libANGLE/renderer/d3d/d3d11/dxgi_support_table.h"
#include "libANGLE/renderer/d3d/d3d11/formatutils11.h"
#include "libANGLE/renderer/d3d/d3d11/texture_format_table.h"
#include "libANGLE/renderer/driver_utils.h"
#include "platform/FeaturesD3D.h"
#include "platform/Platform.h"

namespace rx
{

namespace d3d11_gl
{
namespace
{
// Standard D3D sample positions from
// https://msdn.microsoft.com/en-us/library/windows/desktop/ff476218.aspx
using SamplePositionsArray                                            = std::array<float, 32>;
static constexpr std::array<SamplePositionsArray, 5> kSamplePositions = {
    {{{0.5f, 0.5f}},
     {{0.75f, 0.75f, 0.25f, 0.25f}},
     {{0.375f, 0.125f, 0.875f, 0.375f, 0.125f, 0.625f, 0.625f, 0.875f}},
     {{0.5625f, 0.3125f, 0.4375f, 0.6875f, 0.8125f, 0.5625f, 0.3125f, 0.1875f, 0.1875f, 0.8125f,
       0.0625f, 0.4375f, 0.6875f, 0.9375f, 0.9375f, 0.0625f}},
     {{0.5625f, 0.5625f, 0.4375f, 0.3125f, 0.3125f, 0.625f,  0.75f,   0.4375f,
       0.1875f, 0.375f,  0.625f,  0.8125f, 0.8125f, 0.6875f, 0.6875f, 0.1875f,
       0.375f,  0.875f,  0.5f,    0.0625f, 0.25f,   0.125f,  0.125f,  0.75f,
       0.0f,    0.5f,    0.9375f, 0.25f,   0.875f,  0.9375f, 0.0625f, 0.0f}}}};

// TODO(xinghua.cao@intel.com): Get a more accurate limit.
static D3D_FEATURE_LEVEL kMinimumFeatureLevelForES31 = D3D_FEATURE_LEVEL_11_0;

// Helper functor for querying DXGI support. Saves passing the parameters repeatedly.
class DXGISupportHelper : angle::NonCopyable
{
  public:
    DXGISupportHelper(ID3D11Device *device, D3D_FEATURE_LEVEL featureLevel)
        : mDevice(device), mFeatureLevel(featureLevel)
    {}

    bool query(DXGI_FORMAT dxgiFormat, UINT supportMask)
    {
        if (dxgiFormat == DXGI_FORMAT_UNKNOWN)
            return false;

        auto dxgiSupport = d3d11::GetDXGISupport(dxgiFormat, mFeatureLevel);

        UINT supportedBits = dxgiSupport.alwaysSupportedFlags;

        if ((dxgiSupport.optionallySupportedFlags & supportMask) != 0)
        {
            UINT formatSupport;
            if (SUCCEEDED(mDevice->CheckFormatSupport(dxgiFormat, &formatSupport)))
            {
                supportedBits |= (formatSupport & supportMask);
            }
            else
            {
                // TODO(jmadill): find out why we fail this call sometimes in FL9_3
                // ERR() << "Error checking format support for format 0x" << std::hex << dxgiFormat;
            }
        }

        return ((supportedBits & supportMask) == supportMask);
    }

  private:
    ID3D11Device *mDevice;
    D3D_FEATURE_LEVEL mFeatureLevel;
};

gl::TextureCaps GenerateTextureFormatCaps(gl::Version maxClientVersion,
                                          GLenum internalFormat,
                                          ID3D11Device *device,
                                          const Renderer11DeviceCaps &renderer11DeviceCaps)
{
    gl::TextureCaps textureCaps;

    DXGISupportHelper support(device, renderer11DeviceCaps.featureLevel);
    const d3d11::Format &formatInfo = d3d11::Format::Get(internalFormat, renderer11DeviceCaps);

    const gl::InternalFormat &internalFormatInfo = gl::GetSizedInternalFormatInfo(internalFormat);

    UINT texSupportMask = D3D11_FORMAT_SUPPORT_TEXTURE2D;
    if (internalFormatInfo.depthBits == 0 && internalFormatInfo.stencilBits == 0)
    {
        texSupportMask |= D3D11_FORMAT_SUPPORT_TEXTURECUBE;
        if (maxClientVersion.major > 2)
        {
            texSupportMask |= D3D11_FORMAT_SUPPORT_TEXTURE3D;
        }
    }

    textureCaps.texturable = support.query(formatInfo.texFormat, texSupportMask);
    textureCaps.filterable =
        support.query(formatInfo.srvFormat, D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
    textureCaps.textureAttachment =
        (support.query(formatInfo.rtvFormat, D3D11_FORMAT_SUPPORT_RENDER_TARGET)) ||
        (support.query(formatInfo.dsvFormat, D3D11_FORMAT_SUPPORT_DEPTH_STENCIL));
    textureCaps.renderbuffer = textureCaps.textureAttachment;

    DXGI_FORMAT renderFormat = DXGI_FORMAT_UNKNOWN;
    if (formatInfo.dsvFormat != DXGI_FORMAT_UNKNOWN)
    {
        renderFormat = formatInfo.dsvFormat;
    }
    else if (formatInfo.rtvFormat != DXGI_FORMAT_UNKNOWN)
    {
        renderFormat = formatInfo.rtvFormat;
    }
    if (renderFormat != DXGI_FORMAT_UNKNOWN &&
        support.query(renderFormat, D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET))
    {
        // Assume 1x
        textureCaps.sampleCounts.insert(1);

        for (unsigned int sampleCount = 2; sampleCount <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT;
             sampleCount *= 2)
        {
            UINT qualityCount = 0;
            if (SUCCEEDED(device->CheckMultisampleQualityLevels(renderFormat, sampleCount,
                                                                &qualityCount)))
            {
                // Assume we always support lower sample counts
                if (qualityCount == 0)
                {
                    break;
                }
                textureCaps.sampleCounts.insert(sampleCount);
            }
        }
    }

    return textureCaps;
}

bool GetNPOTTextureSupport(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return true;

        // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476876.aspx
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return false;

        default:
            UNREACHABLE();
            return false;
    }
}

float GetMaximumAnisotropy(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_MAX_MAXANISOTROPY;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_MAX_MAXANISOTROPY;

        // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476876.aspx
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
            return 16;

        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_DEFAULT_MAX_ANISOTROPY;

        default:
            UNREACHABLE();
            return 0;
    }
}

bool GetOcclusionQuerySupport(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return true;

        // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476150.aspx
        // ID3D11Device::CreateQuery
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
            return true;
        case D3D_FEATURE_LEVEL_9_1:
            return false;

        default:
            UNREACHABLE();
            return false;
    }
}

bool GetEventQuerySupport(D3D_FEATURE_LEVEL featureLevel)
{
    // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476150.aspx
    // ID3D11Device::CreateQuery

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return true;

        default:
            UNREACHABLE();
            return false;
    }
}

bool GetInstancingSupport(D3D_FEATURE_LEVEL featureLevel)
{
    // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476150.aspx
    // ID3D11Device::CreateInputLayout

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return true;

        // Feature Level 9_3 supports instancing, but slot 0 in the input layout must not be
        // instanced.
        // D3D9 has a similar restriction, where stream 0 must not be instanced.
        // This restriction can be worked around by remapping any non-instanced slot to slot
        // 0.
        // This works because HLSL uses shader semantics to match the vertex inputs to the
        // elements in the input layout, rather than the slots.
        // Note that we only support instancing via ANGLE_instanced_array on 9_3, since 9_3
        // doesn't support OpenGL ES 3.0
        case D3D_FEATURE_LEVEL_9_3:
            return true;

        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return false;

        default:
            UNREACHABLE();
            return false;
    }
}

bool GetFramebufferMultisampleSupport(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return true;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return false;

        default:
            UNREACHABLE();
            return false;
    }
}

bool GetFramebufferBlitSupport(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return true;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return false;

        default:
            UNREACHABLE();
            return false;
    }
}

bool GetDerivativeInstructionSupport(D3D_FEATURE_LEVEL featureLevel)
{
    // http://msdn.microsoft.com/en-us/library/windows/desktop/bb509588.aspx states that
    // shader model
    // ps_2_x is required for the ddx (and other derivative functions).

    // http://msdn.microsoft.com/en-us/library/windows/desktop/ff476876.aspx states that
    // feature level
    // 9.3 supports shader model ps_2_x.

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
        case D3D_FEATURE_LEVEL_9_3:
            return true;
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return false;

        default:
            UNREACHABLE();
            return false;
    }
}

bool GetShaderTextureLODSupport(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return true;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return false;

        default:
            UNREACHABLE();
            return false;
    }
}

size_t GetMaximumSimultaneousRenderTargets(D3D_FEATURE_LEVEL featureLevel)
{
    // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476150.aspx
    // ID3D11Device::CreateInputLayout

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT;

        case D3D_FEATURE_LEVEL_9_3:
            return D3D_FL9_3_SIMULTANEOUS_RENDER_TARGET_COUNT;
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_SIMULTANEOUS_RENDER_TARGET_COUNT;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximum2DTextureSize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        case D3D_FEATURE_LEVEL_9_3:
            return D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumCubeMapTextureSize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_TEXTURECUBE_DIMENSION;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_REQ_TEXTURECUBE_DIMENSION;

        case D3D_FEATURE_LEVEL_9_3:
            return D3D_FL9_3_REQ_TEXTURECUBE_DIMENSION;
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_REQ_TEXTURECUBE_DIMENSION;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximum2DTextureArraySize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximum3DTextureSize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumViewportSize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_VIEWPORT_BOUNDS_MAX;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_VIEWPORT_BOUNDS_MAX;

        // No constants for D3D11 Feature Level 9 viewport size limits, use the maximum
        // texture sizes
        case D3D_FEATURE_LEVEL_9_3:
            return D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumDrawIndexedIndexCount(D3D_FEATURE_LEVEL featureLevel)
{
    // D3D11 allows up to 2^32 elements, but we report max signed int for convenience since
    // that's what's
    // returned from glGetInteger
    static_assert(D3D11_REQ_DRAWINDEXED_INDEX_COUNT_2_TO_EXP == 32,
                  "Unexpected D3D11 constant value.");
    static_assert(D3D10_REQ_DRAWINDEXED_INDEX_COUNT_2_TO_EXP == 32,
                  "Unexpected D3D11 constant value.");

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return std::numeric_limits<GLint>::max();

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
            return D3D_FL9_2_IA_PRIMITIVE_MAX_COUNT;
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_IA_PRIMITIVE_MAX_COUNT;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumDrawVertexCount(D3D_FEATURE_LEVEL featureLevel)
{
    // D3D11 allows up to 2^32 elements, but we report max signed int for convenience since
    // that's what's
    // returned from glGetInteger
    static_assert(D3D11_REQ_DRAW_VERTEX_COUNT_2_TO_EXP == 32, "Unexpected D3D11 constant value.");
    static_assert(D3D10_REQ_DRAW_VERTEX_COUNT_2_TO_EXP == 32, "Unexpected D3D11 constant value.");

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return std::numeric_limits<GLint>::max();

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
            return D3D_FL9_2_IA_PRIMITIVE_MAX_COUNT;
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_IA_PRIMITIVE_MAX_COUNT;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumVertexInputSlots(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_STANDARD_VERTEX_ELEMENT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
            return D3D10_1_STANDARD_VERTEX_ELEMENT_COUNT;
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_STANDARD_VERTEX_ELEMENT_COUNT;

        // From http://http://msdn.microsoft.com/en-us/library/windows/desktop/ff476876.aspx
        // "Max Input Slots"
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 16;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumVertexUniformVectors(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;

        // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476149.aspx
        // ID3D11DeviceContext::VSSetConstantBuffers
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 255 - d3d11_gl::GetReservedVertexUniformVectors(featureLevel);

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumVertexUniformBlocks(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT -
                   d3d11::RESERVED_CONSTANT_BUFFER_SLOT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT -
                   d3d11::RESERVED_CONSTANT_BUFFER_SLOT_COUNT;

        // Uniform blocks not supported on D3D11 Feature Level 9
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetReservedVertexOutputVectors(D3D_FEATURE_LEVEL featureLevel)
{
    // According to The OpenGL ES Shading Language specifications
    // (Language Version 1.00 section 10.16, Language Version 3.10 section 12.21)
    // built-in special variables (e.g. gl_FragCoord, or gl_PointCoord)
    // which are statically used in the shader should be included in the variable packing
    // algorithm.
    // Therefore, we should not reserve output vectors for them.

    switch (featureLevel)
    {
        // We must reserve one output vector for dx_Position.
        // We also reserve one for gl_Position, which we unconditionally output on Feature
        // Levels 10_0+,
        // even if it's unused in the shader (e.g. for transform feedback). TODO: This could
        // be improved.
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return 2;

        // Just reserve dx_Position on Feature Level 9, since we don't ever need to output
        // gl_Position.
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 1;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumVertexOutputVectors(D3D_FEATURE_LEVEL featureLevel)
{
    static_assert(gl::IMPLEMENTATION_MAX_VARYING_VECTORS == D3D11_VS_OUTPUT_REGISTER_COUNT,
                  "Unexpected D3D11 constant value.");

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_VS_OUTPUT_REGISTER_COUNT - GetReservedVertexOutputVectors(featureLevel);

        case D3D_FEATURE_LEVEL_10_1:
            return D3D10_1_VS_OUTPUT_REGISTER_COUNT - GetReservedVertexOutputVectors(featureLevel);
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_VS_OUTPUT_REGISTER_COUNT - GetReservedVertexOutputVectors(featureLevel);

        // Use Shader Model 2.X limits
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 8 - GetReservedVertexOutputVectors(featureLevel);

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumVertexTextureUnits(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT;

        // Vertex textures not supported on D3D11 Feature Level 9 according to
        // http://msdn.microsoft.com/en-us/library/windows/desktop/ff476149.aspx
        // ID3D11DeviceContext::VSSetSamplers and ID3D11DeviceContext::VSSetShaderResources
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumPixelUniformVectors(D3D_FEATURE_LEVEL featureLevel)
{
    // TODO(geofflang): Remove hard-coded limit once the gl-uniform-arrays test can pass
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return 1024;  // D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return 1024;  // D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;

        // From http://msdn.microsoft.com/en-us/library/windows/desktop/ff476149.aspx
        // ID3D11DeviceContext::PSSetConstantBuffers
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 32 - d3d11_gl::GetReservedFragmentUniformVectors(featureLevel);

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumPixelUniformBlocks(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT -
                   d3d11::RESERVED_CONSTANT_BUFFER_SLOT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT -
                   d3d11::RESERVED_CONSTANT_BUFFER_SLOT_COUNT;

        // Uniform blocks not supported on D3D11 Feature Level 9
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumPixelInputVectors(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_PS_INPUT_REGISTER_COUNT - GetReservedVertexOutputVectors(featureLevel);

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_PS_INPUT_REGISTER_COUNT - GetReservedVertexOutputVectors(featureLevel);

        // Use Shader Model 2.X limits
        case D3D_FEATURE_LEVEL_9_3:
            return 8 - GetReservedVertexOutputVectors(featureLevel);
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 8 - GetReservedVertexOutputVectors(featureLevel);

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumPixelTextureUnits(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT;

        // http://msdn.microsoft.com/en-us/library/windows/desktop/ff476149.aspx
        // ID3D11DeviceContext::PSSetShaderResources
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 16;

        default:
            UNREACHABLE();
            return 0;
    }
}

std::array<GLuint, 3> GetMaxComputeWorkGroupCount(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return {{D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION,
                     D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION,
                     D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION}};
            break;
        default:
            return {{0, 0, 0}};
    }
}

std::array<GLuint, 3> GetMaxComputeWorkGroupSize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return {{D3D11_CS_THREAD_GROUP_MAX_X, D3D11_CS_THREAD_GROUP_MAX_Y,
                     D3D11_CS_THREAD_GROUP_MAX_Z}};
            break;
        default:
            return {{0, 0, 0}};
    }
}

size_t GetMaxComputeWorkGroupInvocations(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
        default:
            return 0;
    }
}

unsigned int GetMaxComputeSharedMemorySize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        // In D3D11 the maximum total size of all variables with the groupshared storage class is
        // 32kb.
        // https://docs.microsoft.com/en-us/windows/desktop/direct3dhlsl/dx-graphics-hlsl-variable-syntax
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return 32768u;
        default:
            return 0u;
    }
}

size_t GetMaximumComputeUniformVectors(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
        default:
            return 0;
    }
}

size_t GetMaximumComputeUniformBlocks(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT -
                   d3d11::RESERVED_CONSTANT_BUFFER_SLOT_COUNT;
        default:
            return 0;
    }
}

size_t GetMaximumComputeTextureUnits(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        default:
            return 0;
    }
}

void SetUAVRelatedResourceLimits(D3D_FEATURE_LEVEL featureLevel, gl::Caps *caps)
{
    ASSERT(caps);

    GLuint reservedUAVsForAtomicCounterBuffers = 0u;

    // For pixel shaders, the render targets and unordered access views share the same resource
    // slots when being written out.
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ff476465(v=vs.85).aspx
    GLuint maxNumRTVsAndUAVs = 0u;

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
            // Currently we allocate 4 UAV slots for atomic counter buffers on feature level 11_1.
            reservedUAVsForAtomicCounterBuffers = 4u;
            maxNumRTVsAndUAVs                   = D3D11_1_UAV_SLOT_COUNT;
            break;
        case D3D_FEATURE_LEVEL_11_0:
            // Currently we allocate 1 UAV slot for atomic counter buffers on feature level 11_0.
            reservedUAVsForAtomicCounterBuffers = 1u;
            maxNumRTVsAndUAVs                   = D3D11_PS_CS_UAV_REGISTER_COUNT;
            break;
        default:
            return;
    }

    // Set limits on atomic counter buffers in fragment shaders and compute shaders.
    caps->maxCombinedAtomicCounterBuffers = reservedUAVsForAtomicCounterBuffers;
    caps->maxShaderAtomicCounterBuffers[gl::ShaderType::Compute] =
        reservedUAVsForAtomicCounterBuffers;
    caps->maxShaderAtomicCounterBuffers[gl::ShaderType::Fragment] =
        reservedUAVsForAtomicCounterBuffers;
    caps->maxAtomicCounterBufferBindings = reservedUAVsForAtomicCounterBuffers;

    // Setting MAX_COMPUTE_ATOMIC_COUNTERS to a conservative number of 1024 * the number of UAV
    // reserved for atomic counters. It could theoretically be set to max buffer size / 4 but that
    // number could cause problems.
    caps->maxCombinedAtomicCounters = reservedUAVsForAtomicCounterBuffers * 1024;
    caps->maxShaderAtomicCounters[gl::ShaderType::Compute] = caps->maxCombinedAtomicCounters;

    // See
    // https://docs.microsoft.com/en-us/windows/desktop/direct3d11/overviews-direct3d-11-resources-limits
    // Resource size (in MB) for any of the preceding resources is min(max(128,0.25f * (amount of
    // dedicated VRAM)), 2048) MB. So we set it to 128MB to keep same with GL backend.
    caps->maxShaderStorageBlockSize =
        D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024 * 1024;

    // Allocate the remaining slots for images and shader storage blocks.
    // The maximum number of fragment shader outputs depends on the current context version, so we
    // will not set it here. See comments in Context11::initialize().
    caps->maxCombinedShaderOutputResources =
        maxNumRTVsAndUAVs - reservedUAVsForAtomicCounterBuffers;

    // Set limits on images and shader storage blocks in fragment shaders and compute shaders.
    caps->maxCombinedShaderStorageBlocks                   = caps->maxCombinedShaderOutputResources;
    caps->maxShaderStorageBlocks[gl::ShaderType::Compute]  = caps->maxCombinedShaderOutputResources;
    caps->maxShaderStorageBlocks[gl::ShaderType::Fragment] = caps->maxCombinedShaderOutputResources;
    caps->maxShaderStorageBufferBindings                   = caps->maxCombinedShaderOutputResources;

    caps->maxImageUnits                                    = caps->maxCombinedShaderOutputResources;
    caps->maxCombinedImageUniforms                         = caps->maxCombinedShaderOutputResources;
    caps->maxShaderImageUniforms[gl::ShaderType::Compute]  = caps->maxCombinedShaderOutputResources;
    caps->maxShaderImageUniforms[gl::ShaderType::Fragment] = caps->maxCombinedShaderOutputResources;

    // On feature level 11_1, UAVs are also available in vertex shaders and geometry shaders.
    if (featureLevel == D3D_FEATURE_LEVEL_11_1)
    {
        caps->maxShaderAtomicCounterBuffers[gl::ShaderType::Vertex] =
            caps->maxCombinedAtomicCounterBuffers;
        caps->maxShaderAtomicCounterBuffers[gl::ShaderType::Geometry] =
            caps->maxCombinedAtomicCounterBuffers;

        caps->maxShaderImageUniforms[gl::ShaderType::Vertex] =
            caps->maxCombinedShaderOutputResources;
        caps->maxShaderStorageBlocks[gl::ShaderType::Vertex] =
            caps->maxCombinedShaderOutputResources;
        caps->maxShaderImageUniforms[gl::ShaderType::Geometry] =
            caps->maxCombinedShaderOutputResources;
        caps->maxShaderStorageBlocks[gl::ShaderType::Geometry] =
            caps->maxCombinedShaderOutputResources;
    }
}

int GetMinimumTexelOffset(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_TEXEL_OFFSET_MAX_NEGATIVE;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_COMMONSHADER_TEXEL_OFFSET_MAX_NEGATIVE;

        // Sampling functions with offsets are not available below shader model 4.0.
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

int GetMaximumTexelOffset(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_COMMONSHADER_TEXEL_OFFSET_MAX_POSITIVE;
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D11_COMMONSHADER_TEXEL_OFFSET_MAX_POSITIVE;

        // Sampling functions with offsets are not available below shader model 4.0.
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

int GetMinimumTextureGatherOffset(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        // https://docs.microsoft.com/en-us/windows/desktop/direct3dhlsl/gather4-po--sm5---asm-
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return -32;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

int GetMaximumTextureGatherOffset(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        // https://docs.microsoft.com/en-us/windows/desktop/direct3dhlsl/gather4-po--sm5---asm-
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return 31;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumConstantBufferSize(D3D_FEATURE_LEVEL featureLevel)
{
    // Returns a size_t despite the limit being a GLuint64 because size_t is the maximum
    // size of
    // any buffer that could be allocated.

    const size_t bytesPerComponent = 4 * sizeof(float);

    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * bytesPerComponent;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * bytesPerComponent;

        // Limits from http://msdn.microsoft.com/en-us/library/windows/desktop/ff476501.aspx
        // remarks section
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 4096 * bytesPerComponent;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumStreamOutputBuffers(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_SO_BUFFER_SLOT_COUNT;

        case D3D_FEATURE_LEVEL_10_1:
            return D3D10_1_SO_BUFFER_SLOT_COUNT;
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_SO_BUFFER_SLOT_COUNT;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumStreamOutputInterleavedComponents(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return GetMaximumVertexOutputVectors(featureLevel) * 4;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumStreamOutputSeparateComponents(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return GetMaximumStreamOutputInterleavedComponents(featureLevel) /
                   GetMaximumStreamOutputBuffers(featureLevel);

        // D3D 10 and 10.1 only allow one output per output slot if an output slot other
        // than zero is used.
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return 4;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        default:
            UNREACHABLE();
            return 0;
    }
}

size_t GetMaximumRenderToBufferWindowSize(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH;
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return D3D10_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH;

        // REQ_RENDER_TO_BUFFER_WINDOW_WIDTH not supported on D3D11 Feature Level 9,
        // use the maximum texture sizes
        case D3D_FEATURE_LEVEL_9_3:
            return D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        default:
            UNREACHABLE();
            return 0;
    }
}

IntelDriverVersion GetIntelDriverVersion(const Optional<LARGE_INTEGER> driverVersion)
{
    if (!driverVersion.valid())
        return IntelDriverVersion(0);

    // According to http://www.intel.com/content/www/us/en/support/graphics-drivers/000005654.html,
    // only the fourth part is necessary since it stands for the driver specific unique version
    // number.
    WORD part = LOWORD(driverVersion.value().LowPart);
    return IntelDriverVersion(part);
}

}  // anonymous namespace

unsigned int GetReservedVertexUniformVectors(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return 0;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 3;  // dx_ViewAdjust, dx_ViewCoords and dx_ViewScale

        default:
            UNREACHABLE();
            return 0;
    }
}

unsigned int GetReservedFragmentUniformVectors(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return 0;

        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 3;

        default:
            UNREACHABLE();
            return 0;
    }
}

gl::Version GetMaximumClientVersion(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return gl::Version(3, 1);
        case D3D_FEATURE_LEVEL_10_1:
            return gl::Version(3, 0);

        case D3D_FEATURE_LEVEL_10_0:
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return gl::Version(2, 0);

        default:
            UNREACHABLE();
            return gl::Version(0, 0);
    }
}

D3D_FEATURE_LEVEL GetMinimumFeatureLevelForES31()
{
    return kMinimumFeatureLevelForES31;
}

unsigned int GetMaxViewportAndScissorRectanglesPerPipeline(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 1;
        default:
            UNREACHABLE();
            return 0;
    }
}

bool IsMultiviewSupported(D3D_FEATURE_LEVEL featureLevel)
{
    // The ANGLE_multiview extension can always be supported in D3D11 through geometry shaders.
    switch (featureLevel)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return true;
        default:
            return false;
    }
}

unsigned int GetMaxSampleMaskWords(D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
        // D3D10+ only allows 1 sample mask.
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return 1u;
        case D3D_FEATURE_LEVEL_9_3:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 0u;
        default:
            UNREACHABLE();
            return 0u;
    }
}

void GenerateCaps(ID3D11Device *device,
                  ID3D11DeviceContext *deviceContext,
                  const Renderer11DeviceCaps &renderer11DeviceCaps,
                  const angle::FeaturesD3D &features,
                  gl::Caps *caps,
                  gl::TextureCapsMap *textureCapsMap,
                  gl::Extensions *extensions,
                  gl::Limitations *limitations)
{
    D3D_FEATURE_LEVEL featureLevel  = renderer11DeviceCaps.featureLevel;
    const gl::FormatSet &allFormats = gl::GetAllSizedInternalFormats();
    for (GLenum internalFormat : allFormats)
    {
        gl::TextureCaps textureCaps = GenerateTextureFormatCaps(
            GetMaximumClientVersion(featureLevel), internalFormat, device, renderer11DeviceCaps);
        textureCapsMap->insert(internalFormat, textureCaps);

        if (gl::GetSizedInternalFormatInfo(internalFormat).compressed)
        {
            caps->compressedTextureFormats.push_back(internalFormat);
        }
    }

    // GL core feature limits
    // Reserve MAX_UINT for D3D11's primitive restart.
    caps->maxElementIndex  = static_cast<GLint64>(std::numeric_limits<unsigned int>::max() - 1);
    caps->max3DTextureSize = static_cast<GLuint>(GetMaximum3DTextureSize(featureLevel));
    caps->max2DTextureSize = static_cast<GLuint>(GetMaximum2DTextureSize(featureLevel));
    caps->maxCubeMapTextureSize = static_cast<GLuint>(GetMaximumCubeMapTextureSize(featureLevel));
    caps->maxArrayTextureLayers = static_cast<GLuint>(GetMaximum2DTextureArraySize(featureLevel));

    // Unimplemented, set to minimum required
    caps->maxLODBias = 2.0f;

    // No specific limits on render target size, maximum 2D texture size is equivalent
    caps->maxRenderbufferSize = caps->max2DTextureSize;

    // Maximum draw buffers and color attachments are the same, max color attachments could
    // eventually be increased to 16
    caps->maxDrawBuffers = static_cast<GLuint>(GetMaximumSimultaneousRenderTargets(featureLevel));
    caps->maxColorAttachments =
        static_cast<GLuint>(GetMaximumSimultaneousRenderTargets(featureLevel));

    // D3D11 has the same limit for viewport width and height
    caps->maxViewportWidth  = static_cast<GLuint>(GetMaximumViewportSize(featureLevel));
    caps->maxViewportHeight = caps->maxViewportWidth;

    // Choose a reasonable maximum, enforced in the shader.
    caps->minAliasedPointSize = 1.0f;
    caps->maxAliasedPointSize = 1024.0f;

    // Wide lines not supported
    caps->minAliasedLineWidth = 1.0f;
    caps->maxAliasedLineWidth = 1.0f;

    // Primitive count limits
    caps->maxElementsIndices  = static_cast<GLuint>(GetMaximumDrawIndexedIndexCount(featureLevel));
    caps->maxElementsVertices = static_cast<GLuint>(GetMaximumDrawVertexCount(featureLevel));

    // Program and shader binary formats (no supported shader binary formats)
    caps->programBinaryFormats.push_back(GL_PROGRAM_BINARY_ANGLE);

    caps->vertexHighpFloat.setIEEEFloat();
    caps->vertexMediumpFloat.setIEEEFloat();
    caps->vertexLowpFloat.setIEEEFloat();
    caps->fragmentHighpFloat.setIEEEFloat();
    caps->fragmentMediumpFloat.setIEEEFloat();
    caps->fragmentLowpFloat.setIEEEFloat();

    // 32-bit integers are natively supported
    caps->vertexHighpInt.setTwosComplementInt(32);
    caps->vertexMediumpInt.setTwosComplementInt(32);
    caps->vertexLowpInt.setTwosComplementInt(32);
    caps->fragmentHighpInt.setTwosComplementInt(32);
    caps->fragmentMediumpInt.setTwosComplementInt(32);
    caps->fragmentLowpInt.setTwosComplementInt(32);

    // We do not wait for server fence objects internally, so report a max timeout of zero.
    caps->maxServerWaitTimeout = 0;

    // Vertex shader limits
    caps->maxVertexAttributes = static_cast<GLuint>(GetMaximumVertexInputSlots(featureLevel));
    caps->maxVertexUniformVectors =
        static_cast<GLuint>(GetMaximumVertexUniformVectors(featureLevel));
    if (features.skipVSConstantRegisterZero.enabled)
    {
        caps->maxVertexUniformVectors -= 1;
    }
    caps->maxShaderUniformComponents[gl::ShaderType::Vertex] = caps->maxVertexUniformVectors * 4;
    caps->maxShaderUniformBlocks[gl::ShaderType::Vertex] =
        static_cast<GLuint>(GetMaximumVertexUniformBlocks(featureLevel));
    caps->maxVertexOutputComponents =
        static_cast<GLuint>(GetMaximumVertexOutputVectors(featureLevel)) * 4;
    caps->maxShaderTextureImageUnits[gl::ShaderType::Vertex] =
        static_cast<GLuint>(GetMaximumVertexTextureUnits(featureLevel));

    // Vertex Attribute Bindings are emulated on D3D11.
    caps->maxVertexAttribBindings = caps->maxVertexAttributes;
    // Experimental testing confirmed there is no explicit limit on maximum buffer offset in D3D11.
    caps->maxVertexAttribRelativeOffset = std::numeric_limits<GLint>::max();
    // Experimental testing confirmed 2048 is the maximum stride that D3D11 can support on all
    // platforms.
    caps->maxVertexAttribStride = 2048;

    // Fragment shader limits
    caps->maxFragmentUniformVectors =
        static_cast<GLuint>(GetMaximumPixelUniformVectors(featureLevel));
    caps->maxShaderUniformComponents[gl::ShaderType::Fragment] =
        caps->maxFragmentUniformVectors * 4;
    caps->maxShaderUniformBlocks[gl::ShaderType::Fragment] =
        static_cast<GLuint>(GetMaximumPixelUniformBlocks(featureLevel));
    caps->maxFragmentInputComponents =
        static_cast<GLuint>(GetMaximumPixelInputVectors(featureLevel)) * 4;
    caps->maxShaderTextureImageUnits[gl::ShaderType::Fragment] =
        static_cast<GLuint>(GetMaximumPixelTextureUnits(featureLevel));
    caps->minProgramTexelOffset = GetMinimumTexelOffset(featureLevel);
    caps->maxProgramTexelOffset = GetMaximumTexelOffset(featureLevel);

    // Compute shader limits
    caps->maxComputeWorkGroupCount = GetMaxComputeWorkGroupCount(featureLevel);
    caps->maxComputeWorkGroupSize  = GetMaxComputeWorkGroupSize(featureLevel);
    caps->maxComputeWorkGroupInvocations =
        static_cast<GLuint>(GetMaxComputeWorkGroupInvocations(featureLevel));
    caps->maxComputeSharedMemorySize = GetMaxComputeSharedMemorySize(featureLevel);
    caps->maxShaderUniformComponents[gl::ShaderType::Compute] =
        static_cast<GLuint>(GetMaximumComputeUniformVectors(featureLevel)) * 4;
    caps->maxShaderUniformBlocks[gl::ShaderType::Compute] =
        static_cast<GLuint>(GetMaximumComputeUniformBlocks(featureLevel));
    caps->maxShaderTextureImageUnits[gl::ShaderType::Compute] =
        static_cast<GLuint>(GetMaximumComputeTextureUnits(featureLevel));

    SetUAVRelatedResourceLimits(featureLevel, caps);

    // Aggregate shader limits
    caps->maxUniformBufferBindings = caps->maxShaderUniformBlocks[gl::ShaderType::Vertex] +
                                     caps->maxShaderUniformBlocks[gl::ShaderType::Fragment];
    caps->maxUniformBlockSize = GetMaximumConstantBufferSize(featureLevel);

    // TODO(oetuaho): Get a more accurate limit. For now using the minimum requirement for GLES 3.1.
    caps->maxUniformLocations = 1024;

    // With DirectX 11.1, constant buffer offset and size must be a multiple of 16 constants of 16
    // bytes each.
    // https://msdn.microsoft.com/en-us/library/windows/desktop/hh404649%28v=vs.85%29.aspx
    // With DirectX 11.0, we emulate UBO offsets using copies of ranges of the UBO however
    // we still keep the same alignment as 11.1 for consistency.
    caps->uniformBufferOffsetAlignment = 256;

    caps->maxCombinedUniformBlocks = caps->maxShaderUniformBlocks[gl::ShaderType::Vertex] +
                                     caps->maxShaderUniformBlocks[gl::ShaderType::Fragment];

    // A shader storage block will be translated to a structure in HLSL. So We reference the HLSL
    // structure packing rules
    // https://msdn.microsoft.com/en-us/library/windows/desktop/bb509632(v=vs.85).aspx. The
    // resulting size of any structure will always be evenly divisible by sizeof(four-component
    // vector).
    caps->shaderStorageBufferOffsetAlignment = 16;

    for (gl::ShaderType shaderType : gl::AllShaderTypes())
    {
        caps->maxCombinedShaderUniformComponents[shaderType] =
            static_cast<GLint64>(caps->maxShaderUniformBlocks[shaderType]) *
                static_cast<GLint64>(caps->maxUniformBlockSize / 4) +
            static_cast<GLint64>(caps->maxShaderUniformComponents[shaderType]);
    }

    caps->maxVaryingComponents =
        static_cast<GLuint>(GetMaximumVertexOutputVectors(featureLevel)) * 4;
    caps->maxVaryingVectors = static_cast<GLuint>(GetMaximumVertexOutputVectors(featureLevel));
    caps->maxCombinedTextureImageUnits = caps->maxShaderTextureImageUnits[gl::ShaderType::Vertex] +
                                         caps->maxShaderTextureImageUnits[gl::ShaderType::Fragment];

    // Transform feedback limits
    caps->maxTransformFeedbackInterleavedComponents =
        static_cast<GLuint>(GetMaximumStreamOutputInterleavedComponents(featureLevel));
    caps->maxTransformFeedbackSeparateAttributes =
        static_cast<GLuint>(GetMaximumStreamOutputBuffers(featureLevel));
    caps->maxTransformFeedbackSeparateComponents =
        static_cast<GLuint>(GetMaximumStreamOutputSeparateComponents(featureLevel));

    // Defer the computation of multisample limits to Context::updateCaps() where max*Samples values
    // are determined according to available sample counts for each individual format.
    caps->maxSamples             = std::numeric_limits<GLint>::max();
    caps->maxColorTextureSamples = std::numeric_limits<GLint>::max();
    caps->maxDepthTextureSamples = std::numeric_limits<GLint>::max();
    caps->maxIntegerSamples      = std::numeric_limits<GLint>::max();

    // Sample mask words limits
    caps->maxSampleMaskWords = GetMaxSampleMaskWords(featureLevel);

    // Framebuffer limits
    caps->maxFramebufferSamples = std::numeric_limits<GLint>::max();
    caps->maxFramebufferWidth =
        static_cast<GLuint>(GetMaximumRenderToBufferWindowSize(featureLevel));
    caps->maxFramebufferHeight = caps->maxFramebufferWidth;

    // Texture gather offset limits
    caps->minProgramTextureGatherOffset = GetMinimumTextureGatherOffset(featureLevel);
    caps->maxProgramTextureGatherOffset = GetMaximumTextureGatherOffset(featureLevel);

    // GL extension support
    extensions->setTextureExtensionSupport(*textureCapsMap);

    // Explicitly disable GL_OES_compressed_ETC1_RGB8_texture because it's emulated and never
    // becomes core. WebGL doesn't want to expose it unless there is native support.
    extensions->compressedETC1RGB8Texture = false;

    extensions->elementIndexUint            = true;
    extensions->getProgramBinary            = true;
    extensions->rgb8rgba8                   = true;
    extensions->readFormatBGRA              = true;
    extensions->pixelBufferObject           = true;
    extensions->mapBuffer                   = true;
    extensions->mapBufferRange              = true;
    extensions->textureNPOT                 = GetNPOTTextureSupport(featureLevel);
    extensions->drawBuffers                 = GetMaximumSimultaneousRenderTargets(featureLevel) > 1;
    extensions->textureStorage              = true;
    extensions->textureFilterAnisotropic    = true;
    extensions->maxTextureAnisotropy        = GetMaximumAnisotropy(featureLevel);
    extensions->occlusionQueryBoolean       = GetOcclusionQuerySupport(featureLevel);
    extensions->fence                       = GetEventQuerySupport(featureLevel);
    extensions->disjointTimerQuery          = true;
    extensions->queryCounterBitsTimeElapsed = 64;
    extensions->queryCounterBitsTimestamp =
        0;  // Timestamps cannot be supported due to D3D11 limitations
    extensions->robustness = true;
    // Direct3D guarantees to return zero for any resource that is accessed out of bounds.
    // See https://msdn.microsoft.com/en-us/library/windows/desktop/ff476332(v=vs.85).aspx
    // and https://msdn.microsoft.com/en-us/library/windows/desktop/ff476900(v=vs.85).aspx
    extensions->robustBufferAccessBehavior = true;
    extensions->blendMinMax                = true;
    // https://docs.microsoft.com/en-us/windows/desktop/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware
    extensions->floatBlend             = true;
    extensions->framebufferBlit        = GetFramebufferBlitSupport(featureLevel);
    extensions->framebufferMultisample = GetFramebufferMultisampleSupport(featureLevel);
    extensions->instancedArraysANGLE   = GetInstancingSupport(featureLevel);
    extensions->instancedArraysEXT     = GetInstancingSupport(featureLevel);
    extensions->packReverseRowOrder    = true;
    extensions->standardDerivatives    = GetDerivativeInstructionSupport(featureLevel);
    extensions->shaderTextureLOD       = GetShaderTextureLODSupport(featureLevel);
    extensions->fragDepth              = true;
    extensions->multiview              = IsMultiviewSupported(featureLevel);
    extensions->multiview2             = IsMultiviewSupported(featureLevel);
    if (extensions->multiview || extensions->multiview2)
    {
        extensions->maxViews =
            std::min(static_cast<GLuint>(gl::IMPLEMENTATION_ANGLE_MULTIVIEW_MAX_VIEWS),
                     std::min(static_cast<GLuint>(GetMaximum2DTextureArraySize(featureLevel)),
                              GetMaxViewportAndScissorRectanglesPerPipeline(featureLevel)));
    }
    extensions->textureUsage       = true;  // This could be false since it has no effect in D3D11
    extensions->discardFramebuffer = true;
    extensions->translatedShaderSource           = true;
    extensions->fboRenderMipmap                  = false;
    extensions->debugMarker                      = true;
    extensions->eglImage                         = true;
    extensions->eglImageExternal                 = true;
    extensions->eglImageExternalEssl3            = true;
    extensions->eglStreamConsumerExternal        = true;
    extensions->unpackSubimage                   = true;
    extensions->packSubimage                     = true;
    extensions->lossyETCDecode                   = true;
    extensions->syncQuery                        = GetEventQuerySupport(featureLevel);
    extensions->copyTexture                      = true;
    extensions->copyCompressedTexture            = true;
    extensions->textureStorageMultisample2DArray = true;
    extensions->multiviewMultisample     = ((extensions->multiview || extensions->multiview2) &&
                                        extensions->textureStorageMultisample2DArray);
    extensions->copyTexture3d            = true;
    extensions->textureBorderClamp       = true;
    extensions->textureMultisample       = true;
    extensions->provokingVertex          = true;
    extensions->blendFuncExtended        = true;
    extensions->maxDualSourceDrawBuffers = 1;
    extensions->texture3DOES                     = true;

    // D3D11 cannot support reading depth texture as a luminance texture.
    // It treats it as a red-channel-only texture.
    extensions->depthTextureOES = false;

    // D3D11 Feature Level 10_0+ uses SV_IsFrontFace in HLSL to emulate gl_FrontFacing.
    // D3D11 Feature Level 9_3 doesn't support SV_IsFrontFace, and has no equivalent, so can't
    // support gl_FrontFacing.
    limitations->noFrontFacingSupport =
        (renderer11DeviceCaps.featureLevel <= D3D_FEATURE_LEVEL_9_3);

    // D3D11 Feature Level 9_3 doesn't support alpha-to-coverage
    limitations->noSampleAlphaToCoverageSupport =
        (renderer11DeviceCaps.featureLevel <= D3D_FEATURE_LEVEL_9_3);

    // D3D11 Feature Levels 9_3 and below do not support non-constant loop indexing and require
    // additional
    // pre-validation of the shader at compile time to produce a better error message.
    limitations->shadersRequireIndexedLoopValidation =
        (renderer11DeviceCaps.featureLevel <= D3D_FEATURE_LEVEL_9_3);

    // D3D11 has no concept of separate masks and refs for front and back faces in the depth stencil
    // state.
    limitations->noSeparateStencilRefsAndMasks = true;

    // D3D11 cannot support constant color and alpha blend funcs together
    limitations->noSimultaneousConstantColorAndAlphaBlendFunc = true;

    // D3D11 does not support multiple transform feedback outputs writing to the same buffer.
    limitations->noDoubleBoundTransformFeedbackBuffers = true;

    // D3D11 does not support vertex attribute aliasing
    limitations->noVertexAttributeAliasing = true;

#ifdef ANGLE_ENABLE_WINDOWS_STORE
    // Setting a non-zero divisor on attribute zero doesn't work on certain Windows Phone 8-era
    // devices. We should prevent developers from doing this on ALL Windows Store devices. This will
    // maintain consistency across all Windows devices. We allow non-zero divisors on attribute zero
    // if the Client Version >= 3, since devices affected by this issue don't support ES3+.
    limitations->attributeZeroRequiresZeroDivisorInEXT = true;
#endif
}

void GetSamplePosition(GLsizei sampleCount, size_t index, GLfloat *xy)
{
    size_t indexKey = static_cast<size_t>(ceil(log2(sampleCount)));
    ASSERT(indexKey < kSamplePositions.size() &&
           (2 * index + 1) < kSamplePositions[indexKey].size());

    xy[0] = kSamplePositions[indexKey][2 * index];
    xy[1] = kSamplePositions[indexKey][2 * index + 1];
}

}  // namespace d3d11_gl

namespace gl_d3d11
{

D3D11_BLEND ConvertBlendFunc(GLenum glBlend, bool isAlpha)
{
    D3D11_BLEND d3dBlend = D3D11_BLEND_ZERO;

    switch (glBlend)
    {
        case GL_ZERO:
            d3dBlend = D3D11_BLEND_ZERO;
            break;
        case GL_ONE:
            d3dBlend = D3D11_BLEND_ONE;
            break;
        case GL_SRC_COLOR:
            d3dBlend = (isAlpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR);
            break;
        case GL_ONE_MINUS_SRC_COLOR:
            d3dBlend = (isAlpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR);
            break;
        case GL_DST_COLOR:
            d3dBlend = (isAlpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR);
            break;
        case GL_ONE_MINUS_DST_COLOR:
            d3dBlend = (isAlpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR);
            break;
        case GL_SRC_ALPHA:
            d3dBlend = D3D11_BLEND_SRC_ALPHA;
            break;
        case GL_ONE_MINUS_SRC_ALPHA:
            d3dBlend = D3D11_BLEND_INV_SRC_ALPHA;
            break;
        case GL_DST_ALPHA:
            d3dBlend = D3D11_BLEND_DEST_ALPHA;
            break;
        case GL_ONE_MINUS_DST_ALPHA:
            d3dBlend = D3D11_BLEND_INV_DEST_ALPHA;
            break;
        case GL_CONSTANT_COLOR:
            d3dBlend = D3D11_BLEND_BLEND_FACTOR;
            break;
        case GL_ONE_MINUS_CONSTANT_COLOR:
            d3dBlend = D3D11_BLEND_INV_BLEND_FACTOR;
            break;
        case GL_CONSTANT_ALPHA:
            d3dBlend = D3D11_BLEND_BLEND_FACTOR;
            break;
        case GL_ONE_MINUS_CONSTANT_ALPHA:
            d3dBlend = D3D11_BLEND_INV_BLEND_FACTOR;
            break;
        case GL_SRC_ALPHA_SATURATE:
            d3dBlend = D3D11_BLEND_SRC_ALPHA_SAT;
            break;
        case GL_SRC1_COLOR_EXT:
            d3dBlend = (isAlpha ? D3D11_BLEND_SRC1_ALPHA : D3D11_BLEND_SRC1_COLOR);
            break;
        case GL_SRC1_ALPHA_EXT:
            d3dBlend = D3D11_BLEND_SRC1_ALPHA;
            break;
        case GL_ONE_MINUS_SRC1_COLOR_EXT:
            d3dBlend = (isAlpha ? D3D11_BLEND_INV_SRC1_ALPHA : D3D11_BLEND_INV_SRC1_COLOR);
            break;
        case GL_ONE_MINUS_SRC1_ALPHA_EXT:
            d3dBlend = D3D11_BLEND_INV_SRC1_ALPHA;
            break;
        default:
            UNREACHABLE();
    }

    return d3dBlend;
}

D3D11_BLEND_OP ConvertBlendOp(GLenum glBlendOp)
{
    D3D11_BLEND_OP d3dBlendOp = D3D11_BLEND_OP_ADD;

    switch (glBlendOp)
    {
        case GL_FUNC_ADD:
            d3dBlendOp = D3D11_BLEND_OP_ADD;
            break;
        case GL_FUNC_SUBTRACT:
            d3dBlendOp = D3D11_BLEND_OP_SUBTRACT;
            break;
        case GL_FUNC_REVERSE_SUBTRACT:
            d3dBlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
            break;
        case GL_MIN:
            d3dBlendOp = D3D11_BLEND_OP_MIN;
            break;
        case GL_MAX:
            d3dBlendOp = D3D11_BLEND_OP_MAX;
            break;
        default:
            UNREACHABLE();
    }

    return d3dBlendOp;
}

UINT8 ConvertColorMask(bool red, bool green, bool blue, bool alpha)
{
    UINT8 mask = 0;
    if (red)
    {
        mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    }
    if (green)
    {
        mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    }
    if (blue)
    {
        mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    }
    if (alpha)
    {
        mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    return mask;
}

D3D11_CULL_MODE ConvertCullMode(bool cullEnabled, gl::CullFaceMode cullMode)
{
    D3D11_CULL_MODE cull = D3D11_CULL_NONE;

    if (cullEnabled)
    {
        switch (cullMode)
        {
            case gl::CullFaceMode::Front:
                cull = D3D11_CULL_FRONT;
                break;
            case gl::CullFaceMode::Back:
                cull = D3D11_CULL_BACK;
                break;
            case gl::CullFaceMode::FrontAndBack:
                cull = D3D11_CULL_NONE;
                break;
            default:
                UNREACHABLE();
        }
    }
    else
    {
        cull = D3D11_CULL_NONE;
    }

    return cull;
}

D3D11_COMPARISON_FUNC ConvertComparison(GLenum comparison)
{
    D3D11_COMPARISON_FUNC d3dComp = D3D11_COMPARISON_NEVER;
    switch (comparison)
    {
        case GL_NEVER:
            d3dComp = D3D11_COMPARISON_NEVER;
            break;
        case GL_ALWAYS:
            d3dComp = D3D11_COMPARISON_ALWAYS;
            break;
        case GL_LESS:
            d3dComp = D3D11_COMPARISON_LESS;
            break;
        case GL_LEQUAL:
            d3dComp = D3D11_COMPARISON_LESS_EQUAL;
            break;
        case GL_EQUAL:
            d3dComp = D3D11_COMPARISON_EQUAL;
            break;
        case GL_GREATER:
            d3dComp = D3D11_COMPARISON_GREATER;
            break;
        case GL_GEQUAL:
            d3dComp = D3D11_COMPARISON_GREATER_EQUAL;
            break;
        case GL_NOTEQUAL:
            d3dComp = D3D11_COMPARISON_NOT_EQUAL;
            break;
        default:
            UNREACHABLE();
    }

    return d3dComp;
}

D3D11_DEPTH_WRITE_MASK ConvertDepthMask(bool depthWriteEnabled)
{
    return depthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
}

UINT8 ConvertStencilMask(GLuint stencilmask)
{
    return static_cast<UINT8>(stencilmask);
}

D3D11_STENCIL_OP ConvertStencilOp(GLenum stencilOp)
{
    D3D11_STENCIL_OP d3dStencilOp = D3D11_STENCIL_OP_KEEP;

    switch (stencilOp)
    {
        case GL_ZERO:
            d3dStencilOp = D3D11_STENCIL_OP_ZERO;
            break;
        case GL_KEEP:
            d3dStencilOp = D3D11_STENCIL_OP_KEEP;
            break;
        case GL_REPLACE:
            d3dStencilOp = D3D11_STENCIL_OP_REPLACE;
            break;
        case GL_INCR:
            d3dStencilOp = D3D11_STENCIL_OP_INCR_SAT;
            break;
        case GL_DECR:
            d3dStencilOp = D3D11_STENCIL_OP_DECR_SAT;
            break;
        case GL_INVERT:
            d3dStencilOp = D3D11_STENCIL_OP_INVERT;
            break;
        case GL_INCR_WRAP:
            d3dStencilOp = D3D11_STENCIL_OP_INCR;
            break;
        case GL_DECR_WRAP:
            d3dStencilOp = D3D11_STENCIL_OP_DECR;
            break;
        default:
            UNREACHABLE();
    }

    return d3dStencilOp;
}

D3D11_FILTER ConvertFilter(GLenum minFilter,
                           GLenum magFilter,
                           float maxAnisotropy,
                           GLenum comparisonMode)
{
    bool comparison = comparisonMode != GL_NONE;

    if (maxAnisotropy > 1.0f)
    {
        return D3D11_ENCODE_ANISOTROPIC_FILTER(static_cast<D3D11_COMPARISON_FUNC>(comparison));
    }
    else
    {
        D3D11_FILTER_TYPE dxMin = D3D11_FILTER_TYPE_POINT;
        D3D11_FILTER_TYPE dxMip = D3D11_FILTER_TYPE_POINT;
        switch (minFilter)
        {
            case GL_NEAREST:
                dxMin = D3D11_FILTER_TYPE_POINT;
                dxMip = D3D11_FILTER_TYPE_POINT;
                break;
            case GL_LINEAR:
                dxMin = D3D11_FILTER_TYPE_LINEAR;
                dxMip = D3D11_FILTER_TYPE_POINT;
                break;
            case GL_NEAREST_MIPMAP_NEAREST:
                dxMin = D3D11_FILTER_TYPE_POINT;
                dxMip = D3D11_FILTER_TYPE_POINT;
                break;
            case GL_LINEAR_MIPMAP_NEAREST:
                dxMin = D3D11_FILTER_TYPE_LINEAR;
                dxMip = D3D11_FILTER_TYPE_POINT;
                break;
            case GL_NEAREST_MIPMAP_LINEAR:
                dxMin = D3D11_FILTER_TYPE_POINT;
                dxMip = D3D11_FILTER_TYPE_LINEAR;
                break;
            case GL_LINEAR_MIPMAP_LINEAR:
                dxMin = D3D11_FILTER_TYPE_LINEAR;
                dxMip = D3D11_FILTER_TYPE_LINEAR;
                break;
            default:
                UNREACHABLE();
        }

        D3D11_FILTER_TYPE dxMag = D3D11_FILTER_TYPE_POINT;
        switch (magFilter)
        {
            case GL_NEAREST:
                dxMag = D3D11_FILTER_TYPE_POINT;
                break;
            case GL_LINEAR:
                dxMag = D3D11_FILTER_TYPE_LINEAR;
                break;
            default:
                UNREACHABLE();
        }

        return D3D11_ENCODE_BASIC_FILTER(dxMin, dxMag, dxMip,
                                         static_cast<D3D11_COMPARISON_FUNC>(comparison));
    }
}

D3D11_TEXTURE_ADDRESS_MODE ConvertTextureWrap(GLenum wrap)
{
    switch (wrap)
    {
        case GL_REPEAT:
            return D3D11_TEXTURE_ADDRESS_WRAP;
        case GL_CLAMP_TO_EDGE:
            return D3D11_TEXTURE_ADDRESS_CLAMP;
        case GL_CLAMP_TO_BORDER:
            return D3D11_TEXTURE_ADDRESS_BORDER;
        case GL_MIRRORED_REPEAT:
            return D3D11_TEXTURE_ADDRESS_MIRROR;
        default:
            UNREACHABLE();
    }

    return D3D11_TEXTURE_ADDRESS_WRAP;
}

UINT ConvertMaxAnisotropy(float maxAnisotropy, D3D_FEATURE_LEVEL featureLevel)
{
    return static_cast<UINT>(std::min(maxAnisotropy, d3d11_gl::GetMaximumAnisotropy(featureLevel)));
}

D3D11_QUERY ConvertQueryType(gl::QueryType type)
{
    switch (type)
    {
        case gl::QueryType::AnySamples:
        case gl::QueryType::AnySamplesConservative:
            return D3D11_QUERY_OCCLUSION;
        case gl::QueryType::TransformFeedbackPrimitivesWritten:
            return D3D11_QUERY_SO_STATISTICS;
        case gl::QueryType::TimeElapsed:
            // Two internal queries are also created for begin/end timestamps
            return D3D11_QUERY_TIMESTAMP_DISJOINT;
        case gl::QueryType::CommandsCompleted:
            return D3D11_QUERY_EVENT;
        default:
            UNREACHABLE();
            return D3D11_QUERY_EVENT;
    }
}

// Get the D3D11 write mask covering all color channels of a given format
UINT8 GetColorMask(const gl::InternalFormat &format)
{
    return ConvertColorMask(format.redBits > 0, format.greenBits > 0, format.blueBits > 0,
                            format.alphaBits > 0);
}

}  // namespace gl_d3d11

namespace d3d11
{

ANGLED3D11DeviceType GetDeviceType(ID3D11Device *device)
{
    // Note that this function returns an ANGLED3D11DeviceType rather than a D3D_DRIVER_TYPE value,
    // since it is difficult to tell Software and Reference devices apart

    IDXGIDevice *dxgiDevice     = nullptr;
    IDXGIAdapter *dxgiAdapter   = nullptr;
    IDXGIAdapter2 *dxgiAdapter2 = nullptr;

    ANGLED3D11DeviceType retDeviceType = ANGLE_D3D11_DEVICE_TYPE_UNKNOWN;

    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
    if (SUCCEEDED(hr))
    {
        hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void **)&dxgiAdapter);
        if (SUCCEEDED(hr))
        {
            std::wstring adapterString;
            HRESULT adapter2hr =
                dxgiAdapter->QueryInterface(__uuidof(dxgiAdapter2), (void **)&dxgiAdapter2);
            if (SUCCEEDED(adapter2hr))
            {
                // On D3D_FEATURE_LEVEL_9_*, IDXGIAdapter::GetDesc returns "Software Adapter"
                // for the description string. Try to use IDXGIAdapter2::GetDesc2 to get the
                // actual hardware values if possible.
                DXGI_ADAPTER_DESC2 adapterDesc2;
                dxgiAdapter2->GetDesc2(&adapterDesc2);
                adapterString = std::wstring(adapterDesc2.Description);
            }
            else
            {
                DXGI_ADAPTER_DESC adapterDesc;
                dxgiAdapter->GetDesc(&adapterDesc);
                adapterString = std::wstring(adapterDesc.Description);
            }

            // Both Reference and Software adapters will be 'Software Adapter'
            const bool isSoftwareDevice =
                (adapterString.find(std::wstring(L"Software Adapter")) != std::string::npos);
            const bool isNullDevice = (adapterString == L"");
            const bool isWARPDevice =
                (adapterString.find(std::wstring(L"Basic Render")) != std::string::npos);

            if (isSoftwareDevice || isNullDevice)
            {
                ASSERT(!isWARPDevice);
                retDeviceType = ANGLE_D3D11_DEVICE_TYPE_SOFTWARE_REF_OR_NULL;
            }
            else if (isWARPDevice)
            {
                retDeviceType = ANGLE_D3D11_DEVICE_TYPE_WARP;
            }
            else
            {
                retDeviceType = ANGLE_D3D11_DEVICE_TYPE_HARDWARE;
            }
        }
    }

    SafeRelease(dxgiDevice);
    SafeRelease(dxgiAdapter);
    SafeRelease(dxgiAdapter2);

    return retDeviceType;
}

void MakeValidSize(bool isImage,
                   DXGI_FORMAT format,
                   GLsizei *requestWidth,
                   GLsizei *requestHeight,
                   int *levelOffset)
{
    const DXGIFormatSize &dxgiFormatInfo = d3d11::GetDXGIFormatSizeInfo(format);

    int upsampleCount = 0;
    // Don't expand the size of full textures that are at least (blockWidth x blockHeight) already.
    if (isImage || *requestWidth < static_cast<GLsizei>(dxgiFormatInfo.blockWidth) ||
        *requestHeight < static_cast<GLsizei>(dxgiFormatInfo.blockHeight))
    {
        while (*requestWidth % dxgiFormatInfo.blockWidth != 0 ||
               *requestHeight % dxgiFormatInfo.blockHeight != 0)
        {
            *requestWidth <<= 1;
            *requestHeight <<= 1;
            upsampleCount++;
        }
    }
    if (levelOffset)
    {
        *levelOffset = upsampleCount;
    }
}

angle::Result GenerateInitialTextureData(
    const gl::Context *context,
    GLint internalFormat,
    const Renderer11DeviceCaps &renderer11DeviceCaps,
    GLuint width,
    GLuint height,
    GLuint depth,
    GLuint mipLevels,
    gl::TexLevelArray<D3D11_SUBRESOURCE_DATA> *outSubresourceData)
{
    const d3d11::Format &d3dFormatInfo = d3d11::Format::Get(internalFormat, renderer11DeviceCaps);
    ASSERT(d3dFormatInfo.dataInitializerFunction != nullptr);

    const d3d11::DXGIFormatSize &dxgiFormatInfo =
        d3d11::GetDXGIFormatSizeInfo(d3dFormatInfo.texFormat);

    unsigned int rowPitch     = dxgiFormatInfo.pixelBytes * width;
    unsigned int depthPitch   = rowPitch * height;
    unsigned int maxImageSize = depthPitch * depth;

    angle::MemoryBuffer *scratchBuffer = nullptr;
    ANGLE_CHECK_GL_ALLOC(GetImplAs<Context11>(context),
                         context->getScratchBuffer(maxImageSize, &scratchBuffer));

    d3dFormatInfo.dataInitializerFunction(width, height, depth, scratchBuffer->data(), rowPitch,
                                          depthPitch);

    for (unsigned int i = 0; i < mipLevels; i++)
    {
        unsigned int mipWidth  = std::max(width >> i, 1U);
        unsigned int mipHeight = std::max(height >> i, 1U);

        unsigned int mipRowPitch   = dxgiFormatInfo.pixelBytes * mipWidth;
        unsigned int mipDepthPitch = mipRowPitch * mipHeight;

        outSubresourceData->at(i).pSysMem          = scratchBuffer->data();
        outSubresourceData->at(i).SysMemPitch      = mipRowPitch;
        outSubresourceData->at(i).SysMemSlicePitch = mipDepthPitch;
    }

    return angle::Result::Continue;
}

UINT GetPrimitiveRestartIndex()
{
    return std::numeric_limits<UINT>::max();
}

void SetPositionTexCoordVertex(PositionTexCoordVertex *vertex, float x, float y, float u, float v)
{
    vertex->x = x;
    vertex->y = y;
    vertex->u = u;
    vertex->v = v;
}

void SetPositionLayerTexCoord3DVertex(PositionLayerTexCoord3DVertex *vertex,
                                      float x,
                                      float y,
                                      unsigned int layer,
                                      float u,
                                      float v,
                                      float s)
{
    vertex->x = x;
    vertex->y = y;
    vertex->l = layer;
    vertex->u = u;
    vertex->v = v;
    vertex->s = s;
}

BlendStateKey::BlendStateKey()
{
    memset(this, 0, sizeof(BlendStateKey));
}

bool operator==(const BlendStateKey &a, const BlendStateKey &b)
{
    return memcmp(&a, &b, sizeof(BlendStateKey)) == 0;
}

bool operator!=(const BlendStateKey &a, const BlendStateKey &b)
{
    return !(a == b);
}

RasterizerStateKey::RasterizerStateKey()
{
    memset(this, 0, sizeof(RasterizerStateKey));
}

bool operator==(const RasterizerStateKey &a, const RasterizerStateKey &b)
{
    return memcmp(&a, &b, sizeof(RasterizerStateKey)) == 0;
}

bool operator!=(const RasterizerStateKey &a, const RasterizerStateKey &b)
{
    return !(a == b);
}

HRESULT SetDebugName(ID3D11DeviceChild *resource, const char *name)
{
#if defined(_DEBUG)
    UINT existingDataSize = 0;
    resource->GetPrivateData(WKPDID_D3DDebugObjectName, &existingDataSize, nullptr);
    // Don't check the HRESULT- if it failed then that probably just means that no private data
    // exists yet

    if (existingDataSize > 0)
    {
        // In some cases, ANGLE will try to apply two names to one object, which causes
        // a D3D SDK Layers warning. This can occur if, for example, you 'create' two objects
        // (e.g.Rasterizer States) with identical DESCs on the same device. D3D11 will optimize
        // these calls and return the same object both times.
        static const char *multipleNamesUsed = "Multiple names set by ANGLE";

        // Remove the existing name
        HRESULT hr = resource->SetPrivateData(WKPDID_D3DDebugObjectName, 0, nullptr);
        if (FAILED(hr))
        {
            return hr;
        }

        // Apply the new name
        return resource->SetPrivateData(WKPDID_D3DDebugObjectName,
                                        static_cast<unsigned int>(strlen(multipleNamesUsed)),
                                        multipleNamesUsed);
    }
    else
    {
        return resource->SetPrivateData(WKPDID_D3DDebugObjectName,
                                        static_cast<unsigned int>(strlen(name)), name);
    }
#else
    return S_OK;
#endif
}

// Keep this in cpp file where it has visibility of Renderer11.h, otherwise calling
// allocateResource is only compatible with Clang and MSVS, which support calling a
// method on a forward declared class in a template.
template <ResourceType ResourceT>
angle::Result LazyResource<ResourceT>::resolveImpl(d3d::Context *context,
                                                   Renderer11 *renderer,
                                                   const GetDescType<ResourceT> &desc,
                                                   GetInitDataType<ResourceT> *initData,
                                                   const char *name)
{
    if (!mResource.valid())
    {
        ANGLE_TRY(renderer->allocateResource(context, desc, initData, &mResource));
        mResource.setDebugName(name);
    }
    return angle::Result::Continue;
}

template angle::Result LazyResource<ResourceType::BlendState>::resolveImpl(
    d3d::Context *context,
    Renderer11 *renderer,
    const D3D11_BLEND_DESC &desc,
    void *initData,
    const char *name);
template angle::Result LazyResource<ResourceType::ComputeShader>::resolveImpl(
    d3d::Context *context,
    Renderer11 *renderer,
    const ShaderData &desc,
    void *initData,
    const char *name);
template angle::Result LazyResource<ResourceType::GeometryShader>::resolveImpl(
    d3d::Context *context,
    Renderer11 *renderer,
    const ShaderData &desc,
    const std::vector<D3D11_SO_DECLARATION_ENTRY> *initData,
    const char *name);
template angle::Result LazyResource<ResourceType::InputLayout>::resolveImpl(
    d3d::Context *context,
    Renderer11 *renderer,
    const InputElementArray &desc,
    const ShaderData *initData,
    const char *name);
template angle::Result LazyResource<ResourceType::PixelShader>::resolveImpl(d3d::Context *context,
                                                                            Renderer11 *renderer,
                                                                            const ShaderData &desc,
                                                                            void *initData,
                                                                            const char *name);
template angle::Result LazyResource<ResourceType::VertexShader>::resolveImpl(d3d::Context *context,
                                                                             Renderer11 *renderer,
                                                                             const ShaderData &desc,
                                                                             void *initData,
                                                                             const char *name);

LazyInputLayout::LazyInputLayout(const D3D11_INPUT_ELEMENT_DESC *inputDesc,
                                 size_t inputDescLen,
                                 const BYTE *byteCode,
                                 size_t byteCodeLen,
                                 const char *debugName)
    : mInputDesc(inputDesc, inputDescLen), mByteCode(byteCode, byteCodeLen), mDebugName(debugName)
{}

LazyInputLayout::~LazyInputLayout() {}

angle::Result LazyInputLayout::resolve(d3d::Context *context, Renderer11 *renderer)
{
    return resolveImpl(context, renderer, mInputDesc, &mByteCode, mDebugName);
}

LazyBlendState::LazyBlendState(const D3D11_BLEND_DESC &desc, const char *debugName)
    : mDesc(desc), mDebugName(debugName)
{}

angle::Result LazyBlendState::resolve(d3d::Context *context, Renderer11 *renderer)
{
    return resolveImpl(context, renderer, mDesc, nullptr, mDebugName);
}

void InitializeFeatures(const Renderer11DeviceCaps &deviceCaps,
                        const DXGI_ADAPTER_DESC &adapterDesc,
                        angle::FeaturesD3D *features)
{
    bool is9_3 = (deviceCaps.featureLevel <= D3D_FEATURE_LEVEL_9_3);

    features->mrtPerfWorkaround.enabled                           = true;
    features->setDataFasterThanImageUpload.enabled                = true;
    features->setDataFasterThanImageUploadOn128bitFormats.enabled = true;
    features->zeroMaxLodWorkaround.enabled                        = is9_3;
    features->useInstancedPointSpriteEmulation.enabled            = is9_3;

    // TODO(jmadill): Narrow problematic driver range.
    if (IsNvidia(adapterDesc.VendorId))
    {
        if (deviceCaps.driverVersion.valid())
        {
            WORD part1 = HIWORD(deviceCaps.driverVersion.value().LowPart);
            WORD part2 = LOWORD(deviceCaps.driverVersion.value().LowPart);

            // Disable the workaround to fix a second driver bug on newer NVIDIA.
            features->depthStencilBlitExtraCopy.enabled = (part1 <= 13u && part2 < 6881);
        }
        else
        {
            features->depthStencilBlitExtraCopy.enabled = true;
        }
    }

    // TODO(jmadill): Disable workaround when we have a fixed compiler DLL.
    features->expandIntegerPowExpressions.enabled = true;

    features->flushAfterEndingTransformFeedback.enabled = IsNvidia(adapterDesc.VendorId);
    features->getDimensionsIgnoresBaseLevel.enabled     = IsNvidia(adapterDesc.VendorId);
    features->skipVSConstantRegisterZero.enabled        = IsNvidia(adapterDesc.VendorId);
    features->forceAtomicValueResolution.enabled        = IsNvidia(adapterDesc.VendorId);

    if (IsIntel(adapterDesc.VendorId))
    {
        IntelDriverVersion capsVersion = d3d11_gl::GetIntelDriverVersion(deviceCaps.driverVersion);

        features->preAddTexelFetchOffsets.enabled           = true;
        features->useSystemMemoryForConstantBuffers.enabled = true;
        features->disableB5G6R5Support.enabled          = capsVersion < IntelDriverVersion(4539);
        features->addDummyTextureNoRenderTarget.enabled = capsVersion < IntelDriverVersion(4815);
        if (IsSkylake(adapterDesc.DeviceId))
        {
            features->callClearTwice.enabled    = capsVersion < IntelDriverVersion(4771);
            features->emulateIsnanFloat.enabled = capsVersion < IntelDriverVersion(4542);
        }
        else if (IsBroadwell(adapterDesc.DeviceId) || IsHaswell(adapterDesc.DeviceId) ||
                 IsIvyBridge(adapterDesc.DeviceId))
        {
            features->rewriteUnaryMinusOperator.enabled = capsVersion < IntelDriverVersion(4624);

            // Haswell drivers occasionally corrupt (small?) (vertex?) texture data uploads for 128bit formats.
            features->setDataFasterThanImageUploadOn128bitFormats.enabled = false;
        }
    }

    if (IsAMD(adapterDesc.VendorId))
    {
        features->disableB5G6R5Support.enabled = true;
    }

    // TODO(jmadill): Disable when we have a fixed driver version.
    features->emulateTinyStencilTextures.enabled = IsAMD(adapterDesc.VendorId);

    // The tiny stencil texture workaround involves using CopySubresource or UpdateSubresource on a
    // depth stencil texture.  This is not allowed until feature level 10.1 but since it is not
    // possible to support ES3 on these devices, there is no need for the workaround to begin with
    // (anglebug.com/1572).
    if (deviceCaps.featureLevel < D3D_FEATURE_LEVEL_10_1)
    {
        features->emulateTinyStencilTextures.enabled = false;
    }

    // If the VPAndRTArrayIndexFromAnyShaderFeedingRasterizer feature is not available, we have to
    // select the viewport / RT array index in the geometry shader.
    features->selectViewInGeometryShader.enabled =
        (deviceCaps.supportsVpRtIndexWriteFromVertexShader == false);

    // Never clear for robust resource init.  This matches Chrome's texture clearning behaviour.
    features->allowClearForRobustResourceInit.enabled = false;

    // Call platform hooks for testing overrides.
    auto *platform = ANGLEPlatformCurrent();
    platform->overrideWorkaroundsD3D(platform, features);
}

void InitConstantBufferDesc(D3D11_BUFFER_DESC *constantBufferDescription, size_t byteWidth)
{
    constantBufferDescription->ByteWidth           = static_cast<UINT>(byteWidth);
    constantBufferDescription->Usage               = D3D11_USAGE_DYNAMIC;
    constantBufferDescription->BindFlags           = D3D11_BIND_CONSTANT_BUFFER;
    constantBufferDescription->CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
    constantBufferDescription->MiscFlags           = 0;
    constantBufferDescription->StructureByteStride = 0;
}

}  // namespace d3d11

// TextureHelper11 implementation.
TextureHelper11::TextureHelper11() : mFormatSet(nullptr), mSampleCount(0) {}

TextureHelper11::TextureHelper11(TextureHelper11 &&toCopy) : TextureHelper11()
{
    *this = std::move(toCopy);
}

TextureHelper11::TextureHelper11(const TextureHelper11 &other)
    : mFormatSet(other.mFormatSet), mExtents(other.mExtents), mSampleCount(other.mSampleCount)
{
    mData = other.mData;
}

TextureHelper11::~TextureHelper11() {}

void TextureHelper11::getDesc(D3D11_TEXTURE2D_DESC *desc) const
{
    static_cast<ID3D11Texture2D *>(mData->object)->GetDesc(desc);
}

void TextureHelper11::getDesc(D3D11_TEXTURE3D_DESC *desc) const
{
    static_cast<ID3D11Texture3D *>(mData->object)->GetDesc(desc);
}

void TextureHelper11::initDesc(const D3D11_TEXTURE2D_DESC &desc2D)
{
    mData->resourceType = ResourceType::Texture2D;
    mExtents.width      = static_cast<int>(desc2D.Width);
    mExtents.height     = static_cast<int>(desc2D.Height);
    mExtents.depth      = 1;
    mSampleCount        = desc2D.SampleDesc.Count;
}

void TextureHelper11::initDesc(const D3D11_TEXTURE3D_DESC &desc3D)
{
    mData->resourceType = ResourceType::Texture3D;
    mExtents.width      = static_cast<int>(desc3D.Width);
    mExtents.height     = static_cast<int>(desc3D.Height);
    mExtents.depth      = static_cast<int>(desc3D.Depth);
    mSampleCount        = 1;
}

TextureHelper11 &TextureHelper11::operator=(TextureHelper11 &&other)
{
    std::swap(mData, other.mData);
    std::swap(mExtents, other.mExtents);
    std::swap(mFormatSet, other.mFormatSet);
    std::swap(mSampleCount, other.mSampleCount);
    return *this;
}

TextureHelper11 &TextureHelper11::operator=(const TextureHelper11 &other)
{
    mData        = other.mData;
    mExtents     = other.mExtents;
    mFormatSet   = other.mFormatSet;
    mSampleCount = other.mSampleCount;
    return *this;
}

bool TextureHelper11::operator==(const TextureHelper11 &other) const
{
    return mData->object == other.mData->object;
}

bool TextureHelper11::operator!=(const TextureHelper11 &other) const
{
    return mData->object != other.mData->object;
}

bool UsePresentPathFast(const Renderer11 *renderer,
                        const gl::FramebufferAttachment *framebufferAttachment)
{
    if (framebufferAttachment == nullptr)
    {
        return false;
    }

    return (framebufferAttachment->type() == GL_FRAMEBUFFER_DEFAULT &&
            renderer->presentPathFastEnabled());
}

bool UsePrimitiveRestartWorkaround(bool primitiveRestartFixedIndexEnabled,
                                   gl::DrawElementsType type)
{
    // We should never have to deal with primitive restart workaround issue with GL_UNSIGNED_INT
    // indices, since we restrict it via MAX_ELEMENT_INDEX.
    return (!primitiveRestartFixedIndexEnabled && type == gl::DrawElementsType::UnsignedShort);
}

IndexStorageType ClassifyIndexStorage(const gl::State &glState,
                                      const gl::Buffer *elementArrayBuffer,
                                      gl::DrawElementsType elementType,
                                      gl::DrawElementsType destElementType,
                                      unsigned int offset)
{
    // No buffer bound means we are streaming from a client pointer.
    if (!elementArrayBuffer || !IsOffsetAligned(elementType, offset))
    {
        return IndexStorageType::Dynamic;
    }

    // The buffer can be used directly if the storage supports it and no translation needed.
    BufferD3D *bufferD3D = GetImplAs<BufferD3D>(elementArrayBuffer);
    if (bufferD3D->supportsDirectBinding() && destElementType == elementType)
    {
        return IndexStorageType::Direct;
    }

    // Use a static copy when available.
    StaticIndexBufferInterface *staticBuffer = bufferD3D->getStaticIndexBuffer();
    if (staticBuffer != nullptr)
    {
        return IndexStorageType::Static;
    }

    // Static buffer not available, fall back to streaming.
    return IndexStorageType::Dynamic;
}
}  // namespace rx
