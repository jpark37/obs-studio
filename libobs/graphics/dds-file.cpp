/******************************************************************************
    Copyright (C) 2022 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "dds-file.h"
#include "../util/platform.h"

#define blog(level, format, ...) \
	blog(level, "%s: " format, __FUNCTION__, __VA_ARGS__)

#include <d3d11.h>
#include <dxgi.h>
#include <Windows.h>
#include <algorithm>
#include <cassert>
#include <memory>

#define XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT DXGI_FORMAT(116)
#define XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT DXGI_FORMAT(117)
#define XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT DXGI_FORMAT(118)
#define XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS DXGI_FORMAT(119)
#define XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT DXGI_FORMAT(120)

#define WIN10_DXGI_FORMAT_P208 DXGI_FORMAT(130)
#define WIN10_DXGI_FORMAT_V208 DXGI_FORMAT(131)
#define WIN10_DXGI_FORMAT_V408 DXGI_FORMAT(132)

#define XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM DXGI_FORMAT(189)

#define XBOX_DXGI_FORMAT_R4G4_UNORM DXGI_FORMAT(190)

enum TEX_DIMENSION
// Subset here matches D3D10_RESOURCE_DIMENSION and D3D11_RESOURCE_DIMENSION
{
	TEX_DIMENSION_TEXTURE1D = 2,
	TEX_DIMENSION_TEXTURE2D = 3,
	TEX_DIMENSION_TEXTURE3D = 4,
};

enum TEX_ALPHA_MODE
// Matches DDS_ALPHA_MODE, encoded in MISC_FLAGS2
{
	TEX_ALPHA_MODE_UNKNOWN = 0,
	TEX_ALPHA_MODE_STRAIGHT = 1,
	TEX_ALPHA_MODE_PREMULTIPLIED = 2,
	TEX_ALPHA_MODE_OPAQUE = 3,
	TEX_ALPHA_MODE_CUSTOM = 4,
};

enum TEX_MISC_FLAG
// Subset here matches D3D10_RESOURCE_MISC_FLAG and D3D11_RESOURCE_MISC_FLAG
{
	TEX_MISC_TEXTURECUBE = 0x4L,
};

enum TEX_MISC_FLAG2 {
	TEX_MISC2_ALPHA_MODE_MASK = 0x7L,
};

struct TexMetadata {
	size_t width;
	size_t height;    // Should be 1 for 1D textures
	size_t depth;     // Should be 1 for 1D or 2D textures
	size_t arraySize; // For cubemap, this is a multiple of 6
	size_t mipLevels;
	uint32_t miscFlags;
	uint32_t miscFlags2;
	DXGI_FORMAT format;
	TEX_DIMENSION dimension;

	size_t __cdecl ComputeIndex(_In_ size_t mip, _In_ size_t item,
				    _In_ size_t slice) const noexcept
	{
		if (mip >= mipLevels)
			return size_t(-1);

		switch (dimension) {
		case TEX_DIMENSION_TEXTURE1D:
		case TEX_DIMENSION_TEXTURE2D:
			if (slice > 0)
				return size_t(-1);

			if (item >= arraySize)
				return size_t(-1);

			return (item * (mipLevels) + mip);

		case TEX_DIMENSION_TEXTURE3D:
			if (item > 0) {
				// No support for arrays of volumes
				return size_t(-1);
			} else {
				size_t index = 0;
				size_t d = depth;

				for (size_t level = 0; level < mip; ++level) {
					index += d;
					if (d > 1)
						d >>= 1;
				}

				if (slice >= d)
					return size_t(-1);

				index += slice;

				return index;
			}

		default:
			return size_t(-1);
		}
	}
	// Returns size_t(-1) to indicate an out-of-range error

	bool __cdecl IsCubemap() const noexcept
	{
		return (miscFlags & TEX_MISC_TEXTURECUBE) != 0;
	}
	// Helper for miscFlags

	bool __cdecl IsPMAlpha() const noexcept
	{
		return ((miscFlags2 & TEX_MISC2_ALPHA_MODE_MASK) ==
			TEX_ALPHA_MODE_PREMULTIPLIED) != 0;
	}
	void __cdecl SetAlphaMode(TEX_ALPHA_MODE mode) noexcept
	{
		miscFlags2 = (miscFlags2 & ~static_cast<uint32_t>(
						   TEX_MISC2_ALPHA_MODE_MASK)) |
			     static_cast<uint32_t>(mode);
	}
	TEX_ALPHA_MODE __cdecl GetAlphaMode() const noexcept
	{
		return static_cast<TEX_ALPHA_MODE>(miscFlags2 &
						   TEX_MISC2_ALPHA_MODE_MASK);
	}
	// Helpers for miscFlags2

	bool __cdecl IsVolumemap() const noexcept
	{
		return (dimension == TEX_DIMENSION_TEXTURE3D);
	}
	// Helper for dimension
};

enum CP_FLAGS : unsigned long {
	CP_FLAGS_NONE = 0x0, // Normal operation
	CP_FLAGS_LEGACY_DWORD =
		0x1, // Assume pitch is DWORD aligned instead of BYTE aligned
	CP_FLAGS_PARAGRAPH =
		0x2, // Assume pitch is 16-byte aligned instead of BYTE aligned
	CP_FLAGS_YMM =
		0x4, // Assume pitch is 32-byte aligned instead of BYTE aligned
	CP_FLAGS_ZMM =
		0x8, // Assume pitch is 64-byte aligned instead of BYTE aligned
	CP_FLAGS_PAGE4K =
		0x200, // Assume pitch is 4096-byte aligned instead of BYTE aligned
	CP_FLAGS_BAD_DXTN_TAILS =
		0x1000, // BC formats with malformed mipchain blocks smaller than 4x4
	CP_FLAGS_24BPP =
		0x10000, // Override with a legacy 24 bits-per-pixel format size
	CP_FLAGS_16BPP =
		0x20000, // Override with a legacy 16 bits-per-pixel format size
	CP_FLAGS_8BPP =
		0x40000, // Override with a legacy 8 bits-per-pixel format size
};

struct Image {
	size_t width;
	size_t height;
	DXGI_FORMAT format;
	size_t rowPitch;
	size_t slicePitch;
	uint8_t *pixels;
};

constexpr bool __cdecl IsValid(DXGI_FORMAT fmt) noexcept
{
	return (static_cast<size_t>(fmt) >= 1 &&
		static_cast<size_t>(fmt) <= 190);
}

inline bool __cdecl IsPalettized(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_AI44:
	case DXGI_FORMAT_IA44:
	case DXGI_FORMAT_P8:
	case DXGI_FORMAT_A8P8:
		return true;

	default:
		return false;
	}
}

size_t CountMips(_In_ size_t width, _In_ size_t height) noexcept
{
	size_t mipLevels = 1;

	while (height > 1 || width > 1) {
		if (height > 1)
			height >>= 1;

		if (width > 1)
			width >>= 1;

		++mipLevels;
	}

	return mipLevels;
}

size_t CountMips3D(_In_ size_t width, _In_ size_t height,
		   _In_ size_t depth) noexcept
{
	size_t mipLevels = 1;

	while (height > 1 || width > 1 || depth > 1) {
		if (height > 1)
			height >>= 1;

		if (width > 1)
			width >>= 1;

		if (depth > 1)
			depth >>= 1;

		++mipLevels;
	}

	return mipLevels;
}

bool CalculateMipLevels(size_t width, size_t height, size_t &mipLevels) noexcept
{
	if (mipLevels > 1) {
		const size_t maxMips = CountMips(width, height);
		if (mipLevels > maxMips)
			return false;
	} else if (mipLevels == 0) {
		mipLevels = CountMips(width, height);
	} else {
		mipLevels = 1;
	}
	return true;
}

bool CalculateMipLevels3D(size_t width, size_t height, size_t depth,
			  size_t &mipLevels) noexcept
{
	if (mipLevels > 1) {
		const size_t maxMips = CountMips3D(width, height, depth);
		if (mipLevels > maxMips)
			return false;
	} else if (mipLevels == 0) {
		mipLevels = CountMips3D(width, height, depth);
	} else {
		mipLevels = 1;
	}
	return true;
}

inline bool __cdecl IsCompressed(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return true;

	default:
		return false;
	}
}

bool IsPacked(DXGI_FORMAT fmt) noexcept
{
	switch (static_cast<int>(fmt)) {
	case DXGI_FORMAT_R8G8_B8G8_UNORM:
	case DXGI_FORMAT_G8R8_G8B8_UNORM:
	case DXGI_FORMAT_YUY2: // 4:2:2 8-bit
	case DXGI_FORMAT_Y210: // 4:2:2 10-bit
	case DXGI_FORMAT_Y216: // 4:2:2 16-bit
		return true;

	default:
		return false;
	}
}

bool IsPlanar(DXGI_FORMAT fmt) noexcept
{
	switch (static_cast<int>(fmt)) {
	case DXGI_FORMAT_NV12:       // 4:2:0 8-bit
	case DXGI_FORMAT_P010:       // 4:2:0 10-bit
	case DXGI_FORMAT_P016:       // 4:2:0 16-bit
	case DXGI_FORMAT_420_OPAQUE: // 4:2:0 8-bit
	case DXGI_FORMAT_NV11:       // 4:1:1 8-bit

	case WIN10_DXGI_FORMAT_P208: // 4:2:2 8-bit
	case WIN10_DXGI_FORMAT_V208: // 4:4:0 8-bit
	case WIN10_DXGI_FORMAT_V408: // 4:4:4 8-bit
		// These are JPEG Hardware decode formats (DXGI 1.4)

	case XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT:
	case XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
	case XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT:
		// These are Xbox One platform specific types
		return true;

	default:
		return false;
	}
}

size_t BitsPerPixel(DXGI_FORMAT fmt) noexcept
{
	switch (static_cast<int>(fmt)) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return 128;

	case DXGI_FORMAT_R32G32B32_TYPELESS:
	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
		return 96;

	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
	case DXGI_FORMAT_Y416:
	case DXGI_FORMAT_Y210:
	case DXGI_FORMAT_Y216:
		return 64;

	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
	case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
	case DXGI_FORMAT_R8G8_B8G8_UNORM:
	case DXGI_FORMAT_G8R8_G8B8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
	case DXGI_FORMAT_AYUV:
	case DXGI_FORMAT_Y410:
	case DXGI_FORMAT_YUY2:
	case XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT:
	case XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT:
	case XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM:
		return 32;

	case DXGI_FORMAT_P010:
	case DXGI_FORMAT_P016:
	case XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT:
	case XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
	case XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT:
	case WIN10_DXGI_FORMAT_V408:
		return 24;

	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_A8P8:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
	case WIN10_DXGI_FORMAT_P208:
	case WIN10_DXGI_FORMAT_V208:
		return 16;

	case DXGI_FORMAT_NV12:
	case DXGI_FORMAT_420_OPAQUE:
	case DXGI_FORMAT_NV11:
		return 12;

	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
	case DXGI_FORMAT_A8_UNORM:
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
	case DXGI_FORMAT_AI44:
	case DXGI_FORMAT_IA44:
	case DXGI_FORMAT_P8:
	case XBOX_DXGI_FORMAT_R4G4_UNORM:
		return 8;

	case DXGI_FORMAT_R1_UNORM:
		return 1;

	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		return 4;

	default:
		return 0;
	}
}

HRESULT ComputePitch(DXGI_FORMAT fmt, size_t width, size_t height,
		     size_t &rowPitch, size_t &slicePitch,
		     CP_FLAGS flags) noexcept
{
	uint64_t pitch = 0;
	uint64_t slice = 0;

	switch (static_cast<int>(fmt)) {
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		assert(IsCompressed(fmt));
		{
			if (flags & CP_FLAGS_BAD_DXTN_TAILS) {
				const size_t nbw = width >> 2;
				const size_t nbh = height >> 2;
				pitch = std::max<uint64_t>(1u,
							   uint64_t(nbw) * 8u);
				slice = std::max<uint64_t>(
					1u, pitch * uint64_t(nbh));
			} else {
				const uint64_t nbw = std::max<uint64_t>(
					1u, (uint64_t(width) + 3u) / 4u);
				const uint64_t nbh = std::max<uint64_t>(
					1u, (uint64_t(height) + 3u) / 4u);
				pitch = nbw * 8u;
				slice = pitch * nbh;
			}
		}
		break;

	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		assert(IsCompressed(fmt));
		{
			if (flags & CP_FLAGS_BAD_DXTN_TAILS) {
				const size_t nbw = width >> 2;
				const size_t nbh = height >> 2;
				pitch = std::max<uint64_t>(1u,
							   uint64_t(nbw) * 16u);
				slice = std::max<uint64_t>(
					1u, pitch * uint64_t(nbh));
			} else {
				const uint64_t nbw = std::max<uint64_t>(
					1u, (uint64_t(width) + 3u) / 4u);
				const uint64_t nbh = std::max<uint64_t>(
					1u, (uint64_t(height) + 3u) / 4u);
				pitch = nbw * 16u;
				slice = pitch * nbh;
			}
		}
		break;

	case DXGI_FORMAT_R8G8_B8G8_UNORM:
	case DXGI_FORMAT_G8R8_G8B8_UNORM:
	case DXGI_FORMAT_YUY2:
		assert(IsPacked(fmt));
		pitch = ((uint64_t(width) + 1u) >> 1) * 4u;
		slice = pitch * uint64_t(height);
		break;

	case DXGI_FORMAT_Y210:
	case DXGI_FORMAT_Y216:
		assert(IsPacked(fmt));
		pitch = ((uint64_t(width) + 1u) >> 1) * 8u;
		slice = pitch * uint64_t(height);
		break;

	case DXGI_FORMAT_NV12:
	case DXGI_FORMAT_420_OPAQUE:
		assert(IsPlanar(fmt));
		pitch = ((uint64_t(width) + 1u) >> 1) * 2u;
		slice = pitch *
			(uint64_t(height) + ((uint64_t(height) + 1u) >> 1));
		break;

	case DXGI_FORMAT_P010:
	case DXGI_FORMAT_P016:
	case XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT:
	case XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
	case XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT:
		assert(IsPlanar(fmt));
		pitch = ((uint64_t(width) + 1u) >> 1) * 4u;
		slice = pitch *
			(uint64_t(height) + ((uint64_t(height) + 1u) >> 1));
		break;

	case DXGI_FORMAT_NV11:
		assert(IsPlanar(fmt));
		pitch = ((uint64_t(width) + 3u) >> 2) * 4u;
		slice = pitch * uint64_t(height) * 2u;
		break;

	case WIN10_DXGI_FORMAT_P208:
		assert(IsPlanar(fmt));
		pitch = ((uint64_t(width) + 1u) >> 1) * 2u;
		slice = pitch * uint64_t(height) * 2u;
		break;

	case WIN10_DXGI_FORMAT_V208:
		assert(IsPlanar(fmt));
		pitch = uint64_t(width);
		slice = pitch * (uint64_t(height) +
				 (((uint64_t(height) + 1u) >> 1) * 2u));
		break;

	case WIN10_DXGI_FORMAT_V408:
		assert(IsPlanar(fmt));
		pitch = uint64_t(width);
		slice = pitch *
			(uint64_t(height) + (uint64_t(height >> 1) * 4u));
		break;

	default:
		assert(!IsCompressed(fmt) && !IsPacked(fmt) && !IsPlanar(fmt));
		{
			size_t bpp;

			if (flags & CP_FLAGS_24BPP)
				bpp = 24;
			else if (flags & CP_FLAGS_16BPP)
				bpp = 16;
			else if (flags & CP_FLAGS_8BPP)
				bpp = 8;
			else
				bpp = BitsPerPixel(fmt);

			if (!bpp)
				return E_INVALIDARG;

			if (flags &
			    (CP_FLAGS_LEGACY_DWORD | CP_FLAGS_PARAGRAPH |
			     CP_FLAGS_YMM | CP_FLAGS_ZMM | CP_FLAGS_PAGE4K)) {
				if (flags & CP_FLAGS_PAGE4K) {
					pitch = ((uint64_t(width) * bpp +
						  32767u) /
						 32768u) *
						4096u;
					slice = pitch * uint64_t(height);
				} else if (flags & CP_FLAGS_ZMM) {
					pitch = ((uint64_t(width) * bpp +
						  511u) /
						 512u) *
						64u;
					slice = pitch * uint64_t(height);
				} else if (flags & CP_FLAGS_YMM) {
					pitch = ((uint64_t(width) * bpp +
						  255u) /
						 256u) *
						32u;
					slice = pitch * uint64_t(height);
				} else if (flags & CP_FLAGS_PARAGRAPH) {
					pitch = ((uint64_t(width) * bpp +
						  127u) /
						 128u) *
						16u;
					slice = pitch * uint64_t(height);
				} else // DWORD alignment
				{
					// Special computation for some incorrectly created DDS files based on
					// legacy DirectDraw assumptions about pitch alignment
					pitch = ((uint64_t(width) * bpp + 31u) /
						 32u) *
						sizeof(uint32_t);
					slice = pitch * uint64_t(height);
				}
			} else {
				// Default byte alignment
				pitch = (uint64_t(width) * bpp + 7u) / 8u;
				slice = pitch * uint64_t(height);
			}
		}
		break;
	}

#if defined(_M_IX86) || defined(_M_ARM) || defined(_M_HYBRID_X86_ARM64)
	static_assert(sizeof(size_t) == 4, "Not a 32-bit platform!");
	if (pitch > UINT32_MAX || slice > UINT32_MAX) {
		rowPitch = slicePitch = 0;
		return HRESULT_E_ARITHMETIC_OVERFLOW;
	}
#else
	static_assert(sizeof(size_t) == 8, "Not a 64-bit platform!");
#endif

	rowPitch = static_cast<size_t>(pitch);
	slicePitch = static_cast<size_t>(slice);

	return S_OK;
}

bool DetermineImageArray(const TexMetadata &metadata, CP_FLAGS cpFlags,
			 size_t &nImages, size_t &pixelSize) noexcept
{
	assert(metadata.width > 0 && metadata.height > 0 && metadata.depth > 0);
	assert(metadata.arraySize > 0);
	assert(metadata.mipLevels > 0);

	uint64_t totalPixelSize = 0;
	size_t nimages = 0;

	switch (metadata.dimension) {
	case TEX_DIMENSION_TEXTURE1D:
	case TEX_DIMENSION_TEXTURE2D:
		for (size_t item = 0; item < metadata.arraySize; ++item) {
			size_t w = metadata.width;
			size_t h = metadata.height;

			for (size_t level = 0; level < metadata.mipLevels;
			     ++level) {
				size_t rowPitch, slicePitch;
				if (FAILED(ComputePitch(metadata.format, w, h,
							rowPitch, slicePitch,
							cpFlags))) {
					nImages = pixelSize = 0;
					return false;
				}

				totalPixelSize += uint64_t(slicePitch);
				++nimages;

				if (h > 1)
					h >>= 1;

				if (w > 1)
					w >>= 1;
			}
		}
		break;

	case TEX_DIMENSION_TEXTURE3D: {
		size_t w = metadata.width;
		size_t h = metadata.height;
		size_t d = metadata.depth;

		for (size_t level = 0; level < metadata.mipLevels; ++level) {
			size_t rowPitch, slicePitch;
			if (FAILED(ComputePitch(metadata.format, w, h, rowPitch,
						slicePitch, cpFlags))) {
				nImages = pixelSize = 0;
				return false;
			}

			for (size_t slice = 0; slice < d; ++slice) {
				totalPixelSize += uint64_t(slicePitch);
				++nimages;
			}

			if (h > 1)
				h >>= 1;

			if (w > 1)
				w >>= 1;

			if (d > 1)
				d >>= 1;
		}
	} break;

	default:
		nImages = pixelSize = 0;
		return false;
	}

#if defined(_M_IX86) || defined(_M_ARM) || defined(_M_HYBRID_X86_ARM64)
	static_assert(sizeof(size_t) == 4, "Not a 32-bit platform!");
	if (totalPixelSize > UINT32_MAX) {
		nImages = pixelSize = 0;
		return false;
	}
#else
	static_assert(sizeof(size_t) == 8, "Not a 64-bit platform!");
#endif

	nImages = nimages;
	pixelSize = static_cast<size_t>(totalPixelSize);

	return true;
}

bool SetupImageArray(uint8_t *pMemory, size_t pixelSize,
		     const TexMetadata &metadata, CP_FLAGS cpFlags,
		     Image *images, size_t nImages) noexcept
{
	assert(pMemory);
	assert(pixelSize > 0);
	assert(nImages > 0);

	if (!images)
		return false;

	size_t index = 0;
	uint8_t *pixels = pMemory;
	const uint8_t *pEndBits = pMemory + pixelSize;

	switch (metadata.dimension) {
	case TEX_DIMENSION_TEXTURE1D:
	case TEX_DIMENSION_TEXTURE2D:
		if (metadata.arraySize == 0 || metadata.mipLevels == 0) {
			return false;
		}

		for (size_t item = 0; item < metadata.arraySize; ++item) {
			size_t w = metadata.width;
			size_t h = metadata.height;

			for (size_t level = 0; level < metadata.mipLevels;
			     ++level) {
				if (index >= nImages) {
					return false;
				}

				size_t rowPitch, slicePitch;
				if (FAILED(ComputePitch(metadata.format, w, h,
							rowPitch, slicePitch,
							cpFlags)))
					return false;

				images[index].width = w;
				images[index].height = h;
				images[index].format = metadata.format;
				images[index].rowPitch = rowPitch;
				images[index].slicePitch = slicePitch;
				images[index].pixels = pixels;
				++index;

				pixels += slicePitch;
				if (pixels > pEndBits) {
					return false;
				}

				if (h > 1)
					h >>= 1;

				if (w > 1)
					w >>= 1;
			}
		}
		return true;

	case TEX_DIMENSION_TEXTURE3D: {
		if (metadata.mipLevels == 0 || metadata.depth == 0) {
			return false;
		}

		size_t w = metadata.width;
		size_t h = metadata.height;
		size_t d = metadata.depth;

		for (size_t level = 0; level < metadata.mipLevels; ++level) {
			size_t rowPitch, slicePitch;
			if (FAILED(ComputePitch(metadata.format, w, h, rowPitch,
						slicePitch, cpFlags)))
				return false;

			for (size_t slice = 0; slice < d; ++slice) {
				if (index >= nImages) {
					return false;
				}

				// We use the same memory organization that Direct3D 11 needs for D3D11_SUBRESOURCE_DATA
				// with all slices of a given miplevel being continuous in memory
				images[index].width = w;
				images[index].height = h;
				images[index].format = metadata.format;
				images[index].rowPitch = rowPitch;
				images[index].slicePitch = slicePitch;
				images[index].pixels = pixels;
				++index;

				pixels += slicePitch;
				if (pixels > pEndBits) {
					return false;
				}
			}

			if (h > 1)
				h >>= 1;

			if (w > 1)
				w >>= 1;

			if (d > 1)
				d >>= 1;
		}
	}
		return true;

	default:
		return false;
	}
}

class ScratchImage {
public:
	ScratchImage() noexcept
		: m_nimages(0),
		  m_size(0),
		  m_metadata{},
		  m_image(nullptr),
		  m_memory(nullptr)
	{
	}
	~ScratchImage() { Release(); }

	ScratchImage(const ScratchImage &) = delete;
	ScratchImage &operator=(const ScratchImage &) = delete;

	HRESULT __cdecl Initialize(_In_ const TexMetadata &mdata,
				   _In_ CP_FLAGS flags = CP_FLAGS_NONE) noexcept
	{
		if (!IsValid(mdata.format))
			return E_INVALIDARG;

		if (IsPalettized(mdata.format))
			return E_FAIL;

		size_t mipLevels = mdata.mipLevels;

		switch (mdata.dimension) {
		case TEX_DIMENSION_TEXTURE1D:
			if (!mdata.width || mdata.height != 1 ||
			    mdata.depth != 1 || !mdata.arraySize)
				return E_INVALIDARG;

			if (!CalculateMipLevels(mdata.width, 1, mipLevels))
				return E_INVALIDARG;
			break;

		case TEX_DIMENSION_TEXTURE2D:
			if (!mdata.width || !mdata.height || mdata.depth != 1 ||
			    !mdata.arraySize)
				return E_INVALIDARG;

			if (mdata.IsCubemap()) {
				if ((mdata.arraySize % 6) != 0)
					return E_INVALIDARG;
			}

			if (!CalculateMipLevels(mdata.width, mdata.height,
						mipLevels))
				return E_INVALIDARG;
			break;

		case TEX_DIMENSION_TEXTURE3D:
			if (!mdata.width || !mdata.height || !mdata.depth ||
			    mdata.arraySize != 1)
				return E_INVALIDARG;

			if (!CalculateMipLevels3D(mdata.width, mdata.height,
						  mdata.depth, mipLevels))
				return E_INVALIDARG;
			break;

		default:
			return E_FAIL;
		}

		Release();

		m_metadata.width = mdata.width;
		m_metadata.height = mdata.height;
		m_metadata.depth = mdata.depth;
		m_metadata.arraySize = mdata.arraySize;
		m_metadata.mipLevels = mipLevels;
		m_metadata.miscFlags = mdata.miscFlags;
		m_metadata.miscFlags2 = mdata.miscFlags2;
		m_metadata.format = mdata.format;
		m_metadata.dimension = mdata.dimension;

		size_t pixelSize, nimages;
		if (!DetermineImageArray(m_metadata, flags, nimages, pixelSize))
			return E_FAIL;

		m_image = new (std::nothrow) Image[nimages];
		if (!m_image)
			return E_OUTOFMEMORY;

		m_nimages = nimages;
		memset(m_image, 0, sizeof(Image) * nimages);

		m_memory =
			static_cast<uint8_t *>(_aligned_malloc(pixelSize, 16));
		if (!m_memory) {
			Release();
			return E_OUTOFMEMORY;
		}
		m_size = pixelSize;
		if (!SetupImageArray(m_memory, pixelSize, m_metadata, flags,
				     m_image, nimages)) {
			Release();
			return E_FAIL;
		}

		return S_OK;
	}

	void __cdecl Release() noexcept
	{
		m_nimages = 0;
		m_size = 0;

		if (m_image) {
			delete[] m_image;
			m_image = nullptr;
		}

		if (m_memory) {
			_aligned_free(m_memory);
			m_memory = nullptr;
		}

		memset(&m_metadata, 0, sizeof(m_metadata));
	}

	const TexMetadata &__cdecl GetMetadata() const noexcept
	{
		return m_metadata;
	}

	const Image *__cdecl GetImages() const noexcept { return m_image; }
	size_t __cdecl GetImageCount() const noexcept { return m_nimages; }

	uint8_t *__cdecl GetPixels() const noexcept { return m_memory; }
	size_t __cdecl GetPixelsSize() const noexcept { return m_size; }

	bool __cdecl IsAlphaAllOpaque() const noexcept;

private:
	size_t m_nimages;
	size_t m_size;
	TexMetadata m_metadata;
	Image *m_image;
	uint8_t *m_memory;
};

struct DDS_PIXELFORMAT {
	uint32_t size;
	uint32_t flags;
	uint32_t fourCC;
	uint32_t RGBBitCount;
	uint32_t RBitMask;
	uint32_t GBitMask;
	uint32_t BBitMask;
	uint32_t ABitMask;
};

struct DDS_HEADER {
	uint32_t size;
	uint32_t flags;
	uint32_t height;
	uint32_t width;
	uint32_t pitchOrLinearSize;
	uint32_t depth; // only if DDS_HEADER_FLAGS_VOLUME is set in flags
	uint32_t mipMapCount;
	uint32_t reserved1[11];
	DDS_PIXELFORMAT ddspf;
	uint32_t caps;
	uint32_t caps2;
	uint32_t caps3;
	uint32_t caps4;
	uint32_t reserved2;
};

struct DDS_HEADER_DXT10 {
	DXGI_FORMAT dxgiFormat;
	uint32_t resourceDimension;
	uint32_t miscFlag; // see D3D11_RESOURCE_MISC_FLAG
	uint32_t arraySize;
	uint32_t miscFlags2; // see DDS_MISC_FLAGS2
};

constexpr size_t MAX_HEADER_SIZE =
	sizeof(uint32_t) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10);

constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "

#define DDS_FOURCC 0x00000004      // DDPF_FOURCC
#define DDS_RGB 0x00000040         // DDPF_RGB
#define DDS_RGBA 0x00000041        // DDPF_RGB | DDPF_ALPHAPIXELS
#define DDS_LUMINANCE 0x00020000   // DDPF_LUMINANCE
#define DDS_LUMINANCEA 0x00020001  // DDPF_LUMINANCE | DDPF_ALPHAPIXELS
#define DDS_ALPHAPIXELS 0x00000001 // DDPF_ALPHAPIXELS
#define DDS_ALPHA 0x00000002       // DDPF_ALPHA
#define DDS_PAL8 0x00000020        // DDPF_PALETTEINDEXED8
#define DDS_PAL8A 0x00000021       // DDPF_PALETTEINDEXED8 | DDPF_ALPHAPIXELS
#define DDS_BUMPDUDV 0x00080000    // DDPF_BUMPDUDV

enum CONVERSION_FLAGS : uint32_t {
	CONV_FLAGS_NONE = 0x0,
	CONV_FLAGS_EXPAND = 0x1, // Conversion requires expanded pixel size
	CONV_FLAGS_NOALPHA =
		0x2, // Conversion requires setting alpha to known value
	CONV_FLAGS_SWIZZLE = 0x4,  // BGR/RGB order swizzling required
	CONV_FLAGS_PAL8 = 0x8,     // Has an 8-bit palette
	CONV_FLAGS_888 = 0x10,     // Source is an 8:8:8 (24bpp) format
	CONV_FLAGS_565 = 0x20,     // Source is a 5:6:5 (16bpp) format
	CONV_FLAGS_5551 = 0x40,    // Source is a 5:5:5:1 (16bpp) format
	CONV_FLAGS_4444 = 0x80,    // Source is a 4:4:4:4 (16bpp) format
	CONV_FLAGS_44 = 0x100,     // Source is a 4:4 (8bpp) format
	CONV_FLAGS_332 = 0x200,    // Source is a 3:3:2 (8bpp) format
	CONV_FLAGS_8332 = 0x400,   // Source is a 8:3:3:2 (16bpp) format
	CONV_FLAGS_A8P8 = 0x800,   // Has an 8-bit palette with an alpha channel
	CONV_FLAGS_DX10 = 0x10000, // Has the 'DX10' extension header
	CONV_FLAGS_PMALPHA = 0x20000, // Contains premultiplied alpha data
	CONV_FLAGS_L8 = 0x40000,      // Source is a 8 luminance format
	CONV_FLAGS_L16 = 0x80000,     // Source is a 16 luminance format
	CONV_FLAGS_A8L8 = 0x100000,   // Source is a 8:8 luminance format
};

DEFINE_ENUM_FLAG_OPERATORS(CP_FLAGS);

enum DDS_RESOURCE_MISC_FLAG : uint32_t {
	DDS_RESOURCE_MISC_TEXTURECUBE = 0x4L,
};

enum DDS_RESOURCE_DIMENSION : uint32_t {
	DDS_DIMENSION_TEXTURE1D = 2,
	DDS_DIMENSION_TEXTURE2D = 3,
	DDS_DIMENSION_TEXTURE3D = 4,
};

#define DDS_HEIGHT 0x00000002 // DDSD_HEIGHT
//#define DDS_WIDTH 0x00000004  // DDSD_WIDTH

//#define DDS_HEADER_FLAGS_TEXTURE 0x00001007 // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
//#define DDS_HEADER_FLAGS_MIPMAP 0x00020000     // DDSD_MIPMAPCOUNT
#define DDS_HEADER_FLAGS_VOLUME 0x00800000 // DDSD_DEPTH
//#define DDS_HEADER_FLAGS_PITCH 0x00000008      // DDSD_PITCH
//#define DDS_HEADER_FLAGS_LINEARSIZE 0x00080000 // DDSD_LINEARSIZE

#define DDS_CUBEMAP_POSITIVEX \
	0x00000600 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEX
#define DDS_CUBEMAP_NEGATIVEX \
	0x00000a00 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEX
#define DDS_CUBEMAP_POSITIVEY \
	0x00001200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEY
#define DDS_CUBEMAP_NEGATIVEY \
	0x00002200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEY
#define DDS_CUBEMAP_POSITIVEZ \
	0x00004200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEZ
#define DDS_CUBEMAP_NEGATIVEZ \
	0x00008200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEZ

#define DDS_CUBEMAP_ALLFACES                             \
	(DDS_CUBEMAP_POSITIVEX | DDS_CUBEMAP_NEGATIVEX | \
	 DDS_CUBEMAP_POSITIVEY | DDS_CUBEMAP_NEGATIVEY | \
	 DDS_CUBEMAP_POSITIVEZ | DDS_CUBEMAP_NEGATIVEZ)

#define DDS_CUBEMAP 0x00000200 // DDSCAPS2_CUBEMAP

enum DDS_MISC_FLAGS2 : uint32_t {
	DDS_MISC_FLAGS2_ALPHA_MODE_MASK = 0x7L,
};

enum DDS_ALPHA_MODE : uint32_t {
	DDS_ALPHA_MODE_UNKNOWN = 0,
	DDS_ALPHA_MODE_STRAIGHT = 1,
	DDS_ALPHA_MODE_PREMULTIPLIED = 2,
	DDS_ALPHA_MODE_OPAQUE = 3,
	DDS_ALPHA_MODE_CUSTOM = 4,
};

struct LegacyDDS {
	DXGI_FORMAT format;
	uint32_t convFlags;
	DDS_PIXELFORMAT ddpf;
};

#define DDSGLOBALCONST extern const __declspec(selectany)

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_DXT1 = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('D', 'X', 'T', '1'),
					     0,
					     0,
					     0,
					     0,
					     0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_DXT2 = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('D', 'X', 'T', '2'),
					     0,
					     0,
					     0,
					     0,
					     0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_DXT3 = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('D', 'X', 'T', '3'),
					     0,
					     0,
					     0,
					     0,
					     0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_DXT4 = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('D', 'X', 'T', '4'),
					     0,
					     0,
					     0,
					     0,
					     0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_DXT5 = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('D', 'X', 'T', '5'),
					     0,
					     0,
					     0,
					     0,
					     0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_BC4_UNORM = {sizeof(DDS_PIXELFORMAT),
						  DDS_FOURCC,
						  MAKEFOURCC('B', 'C', '4',
							     'U'),
						  0,
						  0,
						  0,
						  0,
						  0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_BC4_SNORM = {sizeof(DDS_PIXELFORMAT),
						  DDS_FOURCC,
						  MAKEFOURCC('B', 'C', '4',
							     'S'),
						  0,
						  0,
						  0,
						  0,
						  0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_BC5_UNORM = {sizeof(DDS_PIXELFORMAT),
						  DDS_FOURCC,
						  MAKEFOURCC('B', 'C', '5',
							     'U'),
						  0,
						  0,
						  0,
						  0,
						  0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_BC5_SNORM = {sizeof(DDS_PIXELFORMAT),
						  DDS_FOURCC,
						  MAKEFOURCC('B', 'C', '5',
							     'S'),
						  0,
						  0,
						  0,
						  0,
						  0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_R8G8_B8G8 = {sizeof(DDS_PIXELFORMAT),
						  DDS_FOURCC,
						  MAKEFOURCC('R', 'G', 'B',
							     'G'),
						  0,
						  0,
						  0,
						  0,
						  0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_G8R8_G8B8 = {sizeof(DDS_PIXELFORMAT),
						  DDS_FOURCC,
						  MAKEFOURCC('G', 'R', 'G',
							     'B'),
						  0,
						  0,
						  0,
						  0,
						  0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_YUY2 = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('Y', 'U', 'Y', '2'),
					     0,
					     0,
					     0,
					     0,
					     0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_UYVY = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('U', 'Y', 'V', 'Y'),
					     0,
					     0,
					     0,
					     0,
					     0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A8R8G8B8 = {sizeof(DDS_PIXELFORMAT),
						 DDS_RGBA,
						 0,
						 32,
						 0x00ff0000,
						 0x0000ff00,
						 0x000000ff,
						 0xff000000};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_X8R8G8B8 = {sizeof(DDS_PIXELFORMAT),
						 DDS_RGB,
						 0,
						 32,
						 0x00ff0000,
						 0x0000ff00,
						 0x000000ff,
						 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A8B8G8R8 = {sizeof(DDS_PIXELFORMAT),
						 DDS_RGBA,
						 0,
						 32,
						 0x000000ff,
						 0x0000ff00,
						 0x00ff0000,
						 0xff000000};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_X8B8G8R8 = {sizeof(DDS_PIXELFORMAT),
						 DDS_RGB,
						 0,
						 32,
						 0x000000ff,
						 0x0000ff00,
						 0x00ff0000,
						 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_G16R16 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0x0000ffff, 0xffff0000, 0, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_R5G6B5 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 16, 0xf800, 0x07e0, 0x001f, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A1R5G5B5 = {sizeof(DDS_PIXELFORMAT),
						 DDS_RGBA,
						 0,
						 16,
						 0x7c00,
						 0x03e0,
						 0x001f,
						 0x8000};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_X1R5G5B5 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 16, 0x7c00, 0x03e0, 0x001f, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A4R4G4B4 = {sizeof(DDS_PIXELFORMAT),
						 DDS_RGBA,
						 0,
						 16,
						 0x0f00,
						 0x00f0,
						 0x000f,
						 0xf000};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_X4R4G4B4 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 16, 0x0f00, 0x00f0, 0x000f, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_R8G8B8 = {sizeof(DDS_PIXELFORMAT),
					       DDS_RGB,
					       0,
					       24,
					       0xff0000,
					       0x00ff00,
					       0x0000ff,
					       0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A8R3G3B2 = {sizeof(DDS_PIXELFORMAT),
						 DDS_RGBA,
						 0,
						 16,
						 0x00e0,
						 0x001c,
						 0x0003,
						 0xff00};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_R3G3B2 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 8, 0xe0, 0x1c, 0x03, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A4L4 = {
	sizeof(DDS_PIXELFORMAT), DDS_LUMINANCEA, 0, 8, 0x0f, 0, 0, 0xf0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_L8 = {
	sizeof(DDS_PIXELFORMAT), DDS_LUMINANCE, 0, 8, 0xff, 0, 0, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_L16 = {
	sizeof(DDS_PIXELFORMAT), DDS_LUMINANCE, 0, 16, 0xffff, 0, 0, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A8L8 = {
	sizeof(DDS_PIXELFORMAT), DDS_LUMINANCEA, 0, 16, 0x00ff, 0, 0, 0xff00};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A8L8_ALT = {
	sizeof(DDS_PIXELFORMAT), DDS_LUMINANCEA, 0, 8, 0x00ff, 0, 0, 0xff00};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_L8_NVTT1 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 8, 0xff, 0, 0, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_L16_NVTT1 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 16, 0xffff, 0, 0, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A8L8_NVTT1 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGBA, 0, 16, 0x00ff, 0, 0, 0xff00};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A8 = {
	sizeof(DDS_PIXELFORMAT), DDS_ALPHA, 0, 8, 0, 0, 0, 0xff};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_V8U8 = {
	sizeof(DDS_PIXELFORMAT), DDS_BUMPDUDV, 0, 16, 0x00ff, 0xff00, 0, 0};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_Q8W8V8U8 = {sizeof(DDS_PIXELFORMAT),
						 DDS_BUMPDUDV,
						 0,
						 32,
						 0x000000ff,
						 0x0000ff00,
						 0x00ff0000,
						 0xff000000};

DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_V16U16 = {sizeof(DDS_PIXELFORMAT),
					       DDS_BUMPDUDV,
					       0,
					       32,
					       0x0000ffff,
					       0xffff0000,
					       0,
					       0};

// D3DFMT_A2R10G10B10/D3DFMT_A2B10G10R10 should be written using DX10 extension to avoid D3DX 10:10:10:2 reversal issue
DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A2R10G10B10 = {sizeof(DDS_PIXELFORMAT),
						    DDS_RGBA,
						    0,
						    32,
						    0x000003ff,
						    0x000ffc00,
						    0x3ff00000,
						    0xc0000000};
DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_A2B10G10R10 = {sizeof(DDS_PIXELFORMAT),
						    DDS_RGBA,
						    0,
						    32,
						    0x3ff00000,
						    0x000ffc00,
						    0x000003ff,
						    0xc0000000};

// We do not support the following legacy Direct3D 9 formats:
// DDSPF_A2W10V10U10 = { sizeof(DDS_PIXELFORMAT), DDS_BUMPDUDV, 0, 32, 0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000 };
// DDSPF_L6V5U5 = { sizeof(DDS_PIXELFORMAT), DDS_BUMPLUMINANCE, 0, 16, 0x001f, 0x03e0, 0xfc00, 0 };
// DDSPF_X8L8V8U8 = { sizeof(DDS_PIXELFORMAT), DDS_BUMPLUMINANCE, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0 };

// This indicates the DDS_HEADER_DXT10 extension is present (the format is in dxgiFormat)
DDSGLOBALCONST DDS_PIXELFORMAT DDSPF_DX10 = {sizeof(DDS_PIXELFORMAT),
					     DDS_FOURCC,
					     MAKEFOURCC('D', 'X', '1', '0'),
					     0,
					     0,
					     0,
					     0,
					     0};

const LegacyDDS g_LegacyDDSMap[] = {
	{DXGI_FORMAT_BC1_UNORM, CONV_FLAGS_NONE, DDSPF_DXT1}, // D3DFMT_DXT1
	{DXGI_FORMAT_BC2_UNORM, CONV_FLAGS_NONE, DDSPF_DXT3}, // D3DFMT_DXT3
	{DXGI_FORMAT_BC3_UNORM, CONV_FLAGS_NONE, DDSPF_DXT5}, // D3DFMT_DXT5

	{DXGI_FORMAT_BC2_UNORM, CONV_FLAGS_PMALPHA, DDSPF_DXT2}, // D3DFMT_DXT2
	{DXGI_FORMAT_BC3_UNORM, CONV_FLAGS_PMALPHA, DDSPF_DXT4}, // D3DFMT_DXT4

	{DXGI_FORMAT_BC4_UNORM, CONV_FLAGS_NONE, DDSPF_BC4_UNORM},
	{DXGI_FORMAT_BC4_SNORM, CONV_FLAGS_NONE, DDSPF_BC4_SNORM},
	{DXGI_FORMAT_BC5_UNORM, CONV_FLAGS_NONE, DDSPF_BC5_UNORM},
	{DXGI_FORMAT_BC5_SNORM, CONV_FLAGS_NONE, DDSPF_BC5_SNORM},

	{DXGI_FORMAT_BC4_UNORM,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, MAKEFOURCC('A', 'T', 'I', '1'),
	  0, 0, 0, 0, 0}},
	{DXGI_FORMAT_BC5_UNORM,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, MAKEFOURCC('A', 'T', 'I', '2'),
	  0, 0, 0, 0, 0}},

	{DXGI_FORMAT_BC6H_UF16,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, MAKEFOURCC('B', 'C', '6', 'H'),
	  0, 0, 0, 0, 0}},
	{DXGI_FORMAT_BC7_UNORM,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, MAKEFOURCC('B', 'C', '7', 'L'),
	  0, 0, 0, 0, 0}},
	{DXGI_FORMAT_BC7_UNORM,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, MAKEFOURCC('B', 'C', '7', '\0'),
	  0, 0, 0, 0, 0}},

	{DXGI_FORMAT_R8G8_B8G8_UNORM, CONV_FLAGS_NONE,
	 DDSPF_R8G8_B8G8}, // D3DFMT_R8G8_B8G8
	{DXGI_FORMAT_G8R8_G8B8_UNORM, CONV_FLAGS_NONE,
	 DDSPF_G8R8_G8B8}, // D3DFMT_G8R8_G8B8

	{DXGI_FORMAT_B8G8R8A8_UNORM, CONV_FLAGS_NONE,
	 DDSPF_A8R8G8B8}, // D3DFMT_A8R8G8B8 (uses DXGI 1.1 format)
	{DXGI_FORMAT_B8G8R8X8_UNORM, CONV_FLAGS_NONE,
	 DDSPF_X8R8G8B8}, // D3DFMT_X8R8G8B8 (uses DXGI 1.1 format)
	{DXGI_FORMAT_R8G8B8A8_UNORM, CONV_FLAGS_NONE,
	 DDSPF_A8B8G8R8}, // D3DFMT_A8B8G8R8
	{DXGI_FORMAT_R8G8B8A8_UNORM, CONV_FLAGS_NOALPHA,
	 DDSPF_X8B8G8R8}, // D3DFMT_X8B8G8R8
	{DXGI_FORMAT_R16G16_UNORM, CONV_FLAGS_NONE,
	 DDSPF_G16R16}, // D3DFMT_G16R16

	{DXGI_FORMAT_R10G10B10A2_UNORM, CONV_FLAGS_SWIZZLE,
	 DDSPF_A2R10G10B10}, // D3DFMT_A2R10G10B10 (D3DX reversal issue)
	{DXGI_FORMAT_R10G10B10A2_UNORM, CONV_FLAGS_NONE,
	 DDSPF_A2B10G10R10}, // D3DFMT_A2B10G10R10 (D3DX reversal issue)

	{DXGI_FORMAT_R8G8B8A8_UNORM,
	 CONV_FLAGS_EXPAND | CONV_FLAGS_NOALPHA | CONV_FLAGS_888,
	 DDSPF_R8G8B8}, // D3DFMT_R8G8B8

	{DXGI_FORMAT_B5G6R5_UNORM, CONV_FLAGS_565,
	 DDSPF_R5G6B5}, // D3DFMT_R5G6B5
	{DXGI_FORMAT_B5G5R5A1_UNORM, CONV_FLAGS_5551,
	 DDSPF_A1R5G5B5}, // D3DFMT_A1R5G5B5
	{DXGI_FORMAT_B5G5R5A1_UNORM, CONV_FLAGS_5551 | CONV_FLAGS_NOALPHA,
	 DDSPF_X1R5G5B5}, // D3DFMT_X1R5G5B5

	{DXGI_FORMAT_R8G8B8A8_UNORM, CONV_FLAGS_EXPAND | CONV_FLAGS_8332,
	 DDSPF_A8R3G3B2}, // D3DFMT_A8R3G3B2
	{DXGI_FORMAT_B5G6R5_UNORM, CONV_FLAGS_EXPAND | CONV_FLAGS_332,
	 DDSPF_R3G3B2}, // D3DFMT_R3G3B2

	{DXGI_FORMAT_R8_UNORM, CONV_FLAGS_NONE, DDSPF_L8},     // D3DFMT_L8
	{DXGI_FORMAT_R16_UNORM, CONV_FLAGS_NONE, DDSPF_L16},   // D3DFMT_L16
	{DXGI_FORMAT_R8G8_UNORM, CONV_FLAGS_NONE, DDSPF_A8L8}, // D3DFMT_A8L8
	{DXGI_FORMAT_R8G8_UNORM, CONV_FLAGS_NONE,
	 DDSPF_A8L8_ALT}, // D3DFMT_A8L8 (alternative bitcount)

	// NVTT v1 wrote these with RGB instead of LUMINANCE
	{DXGI_FORMAT_R8_UNORM, CONV_FLAGS_NONE, DDSPF_L8_NVTT1},   // D3DFMT_L8
	{DXGI_FORMAT_R16_UNORM, CONV_FLAGS_NONE, DDSPF_L16_NVTT1}, // D3DFMT_L16
	{DXGI_FORMAT_R8G8_UNORM, CONV_FLAGS_NONE,
	 DDSPF_A8L8_NVTT1}, // D3DFMT_A8L8

	{DXGI_FORMAT_A8_UNORM, CONV_FLAGS_NONE, DDSPF_A8}, // D3DFMT_A8

	{DXGI_FORMAT_R16G16B16A16_UNORM,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 36, 0, 0, 0, 0,
	  0}}, // D3DFMT_A16B16G16R16
	{DXGI_FORMAT_R16G16B16A16_SNORM,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 110, 0, 0, 0, 0,
	  0}}, // D3DFMT_Q16W16V16U16
	{DXGI_FORMAT_R16_FLOAT,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 111, 0, 0, 0, 0,
	  0}}, // D3DFMT_R16F
	{DXGI_FORMAT_R16G16_FLOAT,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 112, 0, 0, 0, 0,
	  0}}, // D3DFMT_G16R16F
	{DXGI_FORMAT_R16G16B16A16_FLOAT,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 113, 0, 0, 0, 0,
	  0}}, // D3DFMT_A16B16G16R16F
	{DXGI_FORMAT_R32_FLOAT,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 114, 0, 0, 0, 0,
	  0}}, // D3DFMT_R32F
	{DXGI_FORMAT_R32G32_FLOAT,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 115, 0, 0, 0, 0,
	  0}}, // D3DFMT_G32R32F
	{DXGI_FORMAT_R32G32B32A32_FLOAT,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_FOURCC, 116, 0, 0, 0, 0,
	  0}}, // D3DFMT_A32B32G32R32F

	{DXGI_FORMAT_R32_FLOAT,
	 CONV_FLAGS_NONE,
	 {sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0xffffffff, 0, 0,
	  0}}, // D3DFMT_R32F (D3DX uses FourCC 114 instead)

	{DXGI_FORMAT_R8G8B8A8_UNORM,
	 CONV_FLAGS_EXPAND | CONV_FLAGS_PAL8 | CONV_FLAGS_A8P8,
	 {sizeof(DDS_PIXELFORMAT), DDS_PAL8A, 0, 16, 0, 0, 0,
	  0}}, // D3DFMT_A8P8
	{DXGI_FORMAT_R8G8B8A8_UNORM,
	 CONV_FLAGS_EXPAND | CONV_FLAGS_PAL8,
	 {sizeof(DDS_PIXELFORMAT), DDS_PAL8, 0, 8, 0, 0, 0, 0}}, // D3DFMT_P8

	{DXGI_FORMAT_B4G4R4A4_UNORM, CONV_FLAGS_4444,
	 DDSPF_A4R4G4B4}, // D3DFMT_A4R4G4B4 (uses DXGI 1.2 format)
	{DXGI_FORMAT_B4G4R4A4_UNORM, CONV_FLAGS_NOALPHA | CONV_FLAGS_4444,
	 DDSPF_X4R4G4B4}, // D3DFMT_X4R4G4B4 (uses DXGI 1.2 format)
	{DXGI_FORMAT_B4G4R4A4_UNORM, CONV_FLAGS_EXPAND | CONV_FLAGS_44,
	 DDSPF_A4L4}, // D3DFMT_A4L4 (uses DXGI 1.2 format)

	{DXGI_FORMAT_YUY2, CONV_FLAGS_NONE,
	 DDSPF_YUY2}, // D3DFMT_YUY2 (uses DXGI 1.2 format)
	{DXGI_FORMAT_YUY2, CONV_FLAGS_SWIZZLE,
	 DDSPF_UYVY}, // D3DFMT_UYVY (uses DXGI 1.2 format)

	{DXGI_FORMAT_R8G8_SNORM, CONV_FLAGS_NONE, DDSPF_V8U8}, // D3DFMT_V8U8
	{DXGI_FORMAT_R8G8B8A8_SNORM, CONV_FLAGS_NONE,
	 DDSPF_Q8W8V8U8}, // D3DFMT_Q8W8V8U8
	{DXGI_FORMAT_R16G16_SNORM, CONV_FLAGS_NONE,
	 DDSPF_V16U16}, // D3DFMT_V16U16
};

DXGI_FORMAT MakeSRGB(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

	case DXGI_FORMAT_BC1_UNORM:
		return DXGI_FORMAT_BC1_UNORM_SRGB;

	case DXGI_FORMAT_BC2_UNORM:
		return DXGI_FORMAT_BC2_UNORM_SRGB;

	case DXGI_FORMAT_BC3_UNORM:
		return DXGI_FORMAT_BC3_UNORM_SRGB;

	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;

	case DXGI_FORMAT_BC7_UNORM:
		return DXGI_FORMAT_BC7_UNORM_SRGB;

	default:
		return fmt;
	}
}

DXGI_FORMAT GetDXGIFormat(const DDS_HEADER &hdr, const DDS_PIXELFORMAT &ddpf,
			  _Inout_ uint32_t &convFlags) noexcept
{
	uint32_t ddpfFlags = ddpf.flags;
	if (hdr.reserved1[9] == MAKEFOURCC('N', 'V', 'T', 'T')) {
		// Clear out non-standard nVidia DDS flags
		ddpfFlags &= ~0xC0000000 /* DDPF_SRGB | DDPF_NORMAL */;
	}

	constexpr size_t MAP_SIZE = sizeof(g_LegacyDDSMap) / sizeof(LegacyDDS);
	size_t index = 0;
	for (index = 0; index < MAP_SIZE; ++index) {
		const LegacyDDS *entry = &g_LegacyDDSMap[index];

		if ((ddpfFlags & DDS_FOURCC) &&
		    (entry->ddpf.flags & DDS_FOURCC)) {
			// In case of FourCC codes, ignore any other bits in ddpf.flags
			if (ddpf.fourCC == entry->ddpf.fourCC)
				break;
		} else if (ddpfFlags == entry->ddpf.flags) {
			if (entry->ddpf.flags & DDS_PAL8) {
				if (ddpf.RGBBitCount == entry->ddpf.RGBBitCount)
					break;
			} else if (entry->ddpf.flags & DDS_ALPHA) {
				if (ddpf.RGBBitCount ==
					    entry->ddpf.RGBBitCount &&
				    ddpf.ABitMask == entry->ddpf.ABitMask)
					break;
			} else if (entry->ddpf.flags & DDS_LUMINANCE) {
				if (entry->ddpf.flags & DDS_ALPHAPIXELS) {
					// LUMINANCEA
					if (ddpf.RGBBitCount ==
						    entry->ddpf.RGBBitCount &&
					    ddpf.RBitMask ==
						    entry->ddpf.RBitMask &&
					    ddpf.ABitMask ==
						    entry->ddpf.ABitMask)
						break;
				} else {
					// LUMINANCE
					if (ddpf.RGBBitCount ==
						    entry->ddpf.RGBBitCount &&
					    ddpf.RBitMask ==
						    entry->ddpf.RBitMask)
						break;
				}
			} else if (entry->ddpf.flags & DDS_BUMPDUDV) {
				if (ddpf.RGBBitCount ==
					    entry->ddpf.RGBBitCount &&
				    ddpf.RBitMask == entry->ddpf.RBitMask &&
				    ddpf.GBitMask == entry->ddpf.GBitMask &&
				    ddpf.BBitMask == entry->ddpf.BBitMask &&
				    ddpf.ABitMask == entry->ddpf.ABitMask)
					break;
			} else if (ddpf.RGBBitCount ==
				   entry->ddpf.RGBBitCount) {
				if (entry->ddpf.flags & DDS_ALPHAPIXELS) {
					// RGBA
					if (ddpf.RBitMask ==
						    entry->ddpf.RBitMask &&
					    ddpf.GBitMask ==
						    entry->ddpf.GBitMask &&
					    ddpf.BBitMask ==
						    entry->ddpf.BBitMask &&
					    ddpf.ABitMask ==
						    entry->ddpf.ABitMask)
						break;
				} else {
					// RGB
					if (ddpf.RBitMask ==
						    entry->ddpf.RBitMask &&
					    ddpf.GBitMask ==
						    entry->ddpf.GBitMask &&
					    ddpf.BBitMask ==
						    entry->ddpf.BBitMask)
						break;
				}
			}
		}
	}

	if (index >= MAP_SIZE)
		return DXGI_FORMAT_UNKNOWN;

	uint32_t cflags = g_LegacyDDSMap[index].convFlags;
	DXGI_FORMAT format = g_LegacyDDSMap[index].format;

	if ((hdr.reserved1[9] == MAKEFOURCC('N', 'V', 'T', 'T')) &&
	    (ddpf.flags & 0x40000000 /* DDPF_SRGB */)) {
		format = MakeSRGB(format);
	}

	convFlags = cflags;

	return format;
}

bool DecodeDDSHeader(_In_reads_bytes_(size) const void *pSource, size_t size,
		     _Out_ TexMetadata &metadata,
		     _Inout_ uint32_t &convFlags) noexcept
{
	if (!pSource)
		return false;

	memset(&metadata, 0, sizeof(TexMetadata));

	if (size < (sizeof(DDS_HEADER) + sizeof(uint32_t))) {
		return false;
	}

	// DDS files always start with the same magic number ("DDS ")
	auto const dwMagicNumber = *static_cast<const uint32_t *>(pSource);
	if (dwMagicNumber != DDS_MAGIC) {
		return false;
	}

	auto pHeader = reinterpret_cast<const DDS_HEADER *>(
		static_cast<const uint8_t *>(pSource) + sizeof(uint32_t));

	// Verify header to validate DDS file
	if (pHeader->size != sizeof(DDS_HEADER) ||
	    pHeader->ddspf.size != sizeof(DDS_PIXELFORMAT)) {
		return false;
	}

	metadata.mipLevels = pHeader->mipMapCount;
	if (metadata.mipLevels == 0)
		metadata.mipLevels = 1;

	// Check for DX10 extension
	if ((pHeader->ddspf.flags & DDS_FOURCC) &&
	    (MAKEFOURCC('D', 'X', '1', '0') == pHeader->ddspf.fourCC)) {
		// Buffer must be big enough for both headers and magic value
		if (size < (sizeof(DDS_HEADER) + sizeof(uint32_t) +
			    sizeof(DDS_HEADER_DXT10))) {
			return false;
		}

		auto d3d10ext = reinterpret_cast<const DDS_HEADER_DXT10 *>(
			static_cast<const uint8_t *>(pSource) +
			sizeof(uint32_t) + sizeof(DDS_HEADER));
		convFlags |= CONV_FLAGS_DX10;

		metadata.arraySize = d3d10ext->arraySize;
		if (metadata.arraySize == 0) {
			return false;
		}

		metadata.format = d3d10ext->dxgiFormat;
		if (!IsValid(metadata.format) ||
		    IsPalettized(metadata.format)) {
			return false;
		}

		static_assert(
			static_cast<int>(TEX_MISC_TEXTURECUBE) ==
				static_cast<int>(DDS_RESOURCE_MISC_TEXTURECUBE),
			"DDS header mismatch");

		metadata.miscFlags =
			d3d10ext->miscFlag &
			~static_cast<uint32_t>(TEX_MISC_TEXTURECUBE);

		switch (d3d10ext->resourceDimension) {
		case DDS_DIMENSION_TEXTURE1D:

			// D3DX writes 1D textures with a fixed Height of 1
			if ((pHeader->flags & DDS_HEIGHT) &&
			    pHeader->height != 1) {
				return false;
			}

			metadata.width = pHeader->width;
			metadata.height = 1;
			metadata.depth = 1;
			metadata.dimension = TEX_DIMENSION_TEXTURE1D;
			break;

		case DDS_DIMENSION_TEXTURE2D:
			if (d3d10ext->miscFlag &
			    DDS_RESOURCE_MISC_TEXTURECUBE) {
				metadata.miscFlags |= TEX_MISC_TEXTURECUBE;
				metadata.arraySize *= 6;
			}

			metadata.width = pHeader->width;
			metadata.height = pHeader->height;
			metadata.depth = 1;
			metadata.dimension = TEX_DIMENSION_TEXTURE2D;
			break;

		case DDS_DIMENSION_TEXTURE3D:
			if (!(pHeader->flags & DDS_HEADER_FLAGS_VOLUME)) {
				return false;
			}

			if (metadata.arraySize > 1)
				return false;

			metadata.width = pHeader->width;
			metadata.height = pHeader->height;
			metadata.depth = pHeader->depth;
			metadata.dimension = TEX_DIMENSION_TEXTURE3D;
			break;

		default:
			return false;
		}

		static_assert(static_cast<int>(TEX_MISC2_ALPHA_MODE_MASK) ==
				      static_cast<int>(
					      DDS_MISC_FLAGS2_ALPHA_MODE_MASK),
			      "DDS header mismatch");

		static_assert(static_cast<int>(TEX_ALPHA_MODE_UNKNOWN) ==
				      static_cast<int>(DDS_ALPHA_MODE_UNKNOWN),
			      "DDS header mismatch");
		static_assert(static_cast<int>(TEX_ALPHA_MODE_STRAIGHT) ==
				      static_cast<int>(DDS_ALPHA_MODE_STRAIGHT),
			      "DDS header mismatch");
		static_assert(
			static_cast<int>(TEX_ALPHA_MODE_PREMULTIPLIED) ==
				static_cast<int>(DDS_ALPHA_MODE_PREMULTIPLIED),
			"DDS header mismatch");
		static_assert(static_cast<int>(TEX_ALPHA_MODE_OPAQUE) ==
				      static_cast<int>(DDS_ALPHA_MODE_OPAQUE),
			      "DDS header mismatch");
		static_assert(static_cast<int>(TEX_ALPHA_MODE_CUSTOM) ==
				      static_cast<int>(DDS_ALPHA_MODE_CUSTOM),
			      "DDS header mismatch");

		metadata.miscFlags2 = d3d10ext->miscFlags2;
	} else {
		metadata.arraySize = 1;

		if (pHeader->flags & DDS_HEADER_FLAGS_VOLUME) {
			metadata.width = pHeader->width;
			metadata.height = pHeader->height;
			metadata.depth = pHeader->depth;
			metadata.dimension = TEX_DIMENSION_TEXTURE3D;
		} else {
			if (pHeader->caps2 & DDS_CUBEMAP) {
				// We require all six faces to be defined
				if ((pHeader->caps2 & DDS_CUBEMAP_ALLFACES) !=
				    DDS_CUBEMAP_ALLFACES)
					return false;

				metadata.arraySize = 6;
				metadata.miscFlags |= TEX_MISC_TEXTURECUBE;
			}

			metadata.width = pHeader->width;
			metadata.height = pHeader->height;
			metadata.depth = 1;
			metadata.dimension = TEX_DIMENSION_TEXTURE2D;

			// Note there's no way for a legacy Direct3D 9 DDS to express a '1D' texture
		}

		metadata.format =
			GetDXGIFormat(*pHeader, pHeader->ddspf, convFlags);

		if (metadata.format == DXGI_FORMAT_UNKNOWN)
			return false;
	}

	// Implicit alpha mode
	if (convFlags & CONV_FLAGS_NOALPHA) {
		metadata.SetAlphaMode(TEX_ALPHA_MODE_OPAQUE);
	} else if (convFlags & CONV_FLAGS_PMALPHA) {
		metadata.SetAlphaMode(TEX_ALPHA_MODE_PREMULTIPLIED);
	}

	// 16k is the maximum required resource size supported by Direct3D
	if (metadata.width >
		    16384u /* D3D12_REQ_TEXTURE1D_U_DIMENSION, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION */
	    ||
	    metadata.height > 16384u /* D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION */
	    || metadata.mipLevels > 15u /* D3D12_REQ_MIP_LEVELS */) {
		return false;
	}

	// 2048 is the maximum required depth/array size supported by Direct3D
	if (metadata.arraySize >
		    2048u /* D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION, D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION */
	    || metadata.depth >
		       2048u /* D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION */) {
		return false;
	}

	return true;
}

enum TEXP_SCANLINE_FLAGS : uint32_t {
	TEXP_SCANLINE_NONE = 0,
	TEXP_SCANLINE_SETALPHA = 0x1, // Set alpha channel to known opaque value
	TEXP_SCANLINE_LEGACY =
		0x2, // Enables specific legacy format conversion cases
};

enum TEXP_LEGACY_FORMAT {
	TEXP_LEGACY_UNKNOWN = 0,
	TEXP_LEGACY_R8G8B8,
	TEXP_LEGACY_R3G3B2,
	TEXP_LEGACY_A8R3G3B2,
	TEXP_LEGACY_P8,
	TEXP_LEGACY_A8P8,
	TEXP_LEGACY_A4L4,
	TEXP_LEGACY_B4G4R4A4,
	TEXP_LEGACY_L8,
	TEXP_LEGACY_L16,
	TEXP_LEGACY_A8L8
};

size_t ComputeScanlines(DXGI_FORMAT fmt, size_t height) noexcept
{
	switch (static_cast<int>(fmt)) {
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		assert(IsCompressed(fmt));
		return std::max<size_t>(1, (height + 3) / 4);

	case DXGI_FORMAT_NV11:
	case WIN10_DXGI_FORMAT_P208:
		assert(IsPlanar(fmt));
		return height * 2;

	case WIN10_DXGI_FORMAT_V208:
		assert(IsPlanar(fmt));
		return height + (((height + 1) >> 1) * 2);

	case WIN10_DXGI_FORMAT_V408:
		assert(IsPlanar(fmt));
		return height + ((height >> 1) * 4);

	case DXGI_FORMAT_NV12:
	case DXGI_FORMAT_P010:
	case DXGI_FORMAT_P016:
	case DXGI_FORMAT_420_OPAQUE:
	case XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT:
	case XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
	case XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT:
		assert(IsPlanar(fmt));
		return height + ((height + 1) >> 1);

	default:
		assert(IsValid(fmt));
		assert(!IsCompressed(fmt) && !IsPlanar(fmt));
		return height;
	}
}

bool ExpandScanline(void *pDestination, size_t outSize, DXGI_FORMAT outFormat,
		    const void *pSource, size_t inSize, DXGI_FORMAT inFormat,
		    uint32_t tflags) noexcept
{
	assert(pDestination && outSize > 0);
	assert(pSource && inSize > 0);
	assert(IsValid(outFormat) && !IsPlanar(outFormat) &&
	       !IsPalettized(outFormat));
	assert(IsValid(inFormat) && !IsPlanar(inFormat) &&
	       !IsPalettized(inFormat));

	switch (inFormat) {
	case DXGI_FORMAT_B5G6R5_UNORM:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// DXGI_FORMAT_B5G6R5_UNORM -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 2 && outSize >= 4) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 3)));
			     icount += 2, ocount += 4) {
				const uint16_t t = *(sPtr++);

				uint32_t t1 = uint32_t(((t & 0xf800) >> 8) |
						       ((t & 0xe000) >> 13));
				uint32_t t2 = uint32_t(((t & 0x07e0) << 5) |
						       ((t & 0x0600) >> 5));
				uint32_t t3 = uint32_t(((t & 0x001f) << 19) |
						       ((t & 0x001c) << 14));

				*(dPtr++) = t1 | t2 | t3 | 0xff000000;
			}
			return true;
		}
		return false;

	case DXGI_FORMAT_B5G5R5A1_UNORM:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// DXGI_FORMAT_B5G5R5A1_UNORM -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 2 && outSize >= 4) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 3)));
			     icount += 2, ocount += 4) {
				const uint16_t t = *(sPtr++);

				uint32_t t1 = uint32_t(((t & 0x7c00) >> 7) |
						       ((t & 0x7000) >> 12));
				uint32_t t2 = uint32_t(((t & 0x03e0) << 6) |
						       ((t & 0x0380) << 1));
				uint32_t t3 = uint32_t(((t & 0x001f) << 19) |
						       ((t & 0x001c) << 14));
				uint32_t ta = (tflags & TEXP_SCANLINE_SETALPHA)
						      ? 0xff000000
						      : ((t & 0x8000)
								 ? 0xff000000
								 : 0);

				*(dPtr++) = t1 | t2 | t3 | ta;
			}
			return true;
		}
		return false;

	case DXGI_FORMAT_B4G4R4A4_UNORM:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// DXGI_FORMAT_B4G4R4A4_UNORM -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 2 && outSize >= 4) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 3)));
			     icount += 2, ocount += 4) {
				const uint16_t t = *(sPtr++);

				uint32_t t1 = uint32_t(((t & 0x0f00) >> 4) |
						       ((t & 0x0f00) >> 8));
				uint32_t t2 = uint32_t(((t & 0x00f0) << 8) |
						       ((t & 0x00f0) << 4));
				uint32_t t3 = uint32_t(((t & 0x000f) << 20) |
						       ((t & 0x000f) << 16));
				uint32_t ta =
					(tflags & TEXP_SCANLINE_SETALPHA)
						? 0xff000000
						: uint32_t(
							  ((t & 0xf000) << 16) |
							  ((t & 0xf000) << 12));

				*(dPtr++) = t1 | t2 | t3 | ta;
			}
			return true;
		}
		return false;

	default:
		return false;
	}
}

bool LegacyExpandScanline(_Out_writes_bytes_(outSize) void *pDestination,
			  size_t outSize, _In_ DXGI_FORMAT outFormat,
			  _In_reads_bytes_(inSize) const void *pSource,
			  size_t inSize, _In_ TEXP_LEGACY_FORMAT inFormat,
			  _In_reads_opt_(256) const uint32_t *pal8,
			  _In_ uint32_t tflags) noexcept
{
	assert(pDestination && outSize > 0);
	assert(pSource && inSize > 0);
	assert(IsValid(outFormat) && !IsPlanar(outFormat) &&
	       !IsPalettized(outFormat));

	switch (inFormat) {
	case TEXP_LEGACY_R8G8B8:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// D3DFMT_R8G8B8 -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 3 && outSize >= 4) {
			const uint8_t *__restrict sPtr =
				static_cast<const uint8_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 2)) &&
			      (ocount < (outSize - 3)));
			     icount += 3, ocount += 4) {
				// 24bpp Direct3D 9 files are actually BGR, so need to swizzle as well
				uint32_t t1 = uint32_t(*(sPtr) << 16);
				uint32_t t2 = uint32_t(*(sPtr + 1) << 8);
				uint32_t t3 = uint32_t(*(sPtr + 2));

				*(dPtr++) = t1 | t2 | t3 | 0xff000000;
				sPtr += 3;
			}
			return true;
		}
		return false;

	case TEXP_LEGACY_R3G3B2:
		switch (outFormat) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			// D3DFMT_R3G3B2 -> DXGI_FORMAT_R8G8B8A8_UNORM
			if (inSize >= 1 && outSize >= 4) {
				const uint8_t *__restrict sPtr =
					static_cast<const uint8_t *>(pSource);
				uint32_t *__restrict dPtr =
					static_cast<uint32_t *>(pDestination);

				for (size_t ocount = 0, icount = 0;
				     ((icount < inSize) &&
				      (ocount < (outSize - 3)));
				     ++icount, ocount += 4) {
					const uint8_t t = *(sPtr++);

					uint32_t t1 = uint32_t(
						(t & 0xe0) | ((t & 0xe0) >> 3) |
						((t & 0xc0) >> 6));
					uint32_t t2 =
						uint32_t(((t & 0x1c) << 11) |
							 ((t & 0x1c) << 8) |
							 ((t & 0x18) << 5));
					uint32_t t3 =
						uint32_t(((t & 0x03) << 22) |
							 ((t & 0x03) << 20) |
							 ((t & 0x03) << 18) |
							 ((t & 0x03) << 16));

					*(dPtr++) = t1 | t2 | t3 | 0xff000000;
				}
				return true;
			}
			return false;

		case DXGI_FORMAT_B5G6R5_UNORM:
			// D3DFMT_R3G3B2 -> DXGI_FORMAT_B5G6R5_UNORM
			if (inSize >= 1 && outSize >= 2) {
				const uint8_t *__restrict sPtr =
					static_cast<const uint8_t *>(pSource);
				uint16_t *__restrict dPtr =
					static_cast<uint16_t *>(pDestination);

				for (size_t ocount = 0, icount = 0;
				     ((icount < inSize) &&
				      (ocount < (outSize - 1)));
				     ++icount, ocount += 2) {
					const unsigned t = *(sPtr++);

					unsigned t1 = ((t & 0xe0u) << 8) |
						      ((t & 0xc0u) << 5);
					unsigned t2 = ((t & 0x1cu) << 6) |
						      ((t & 0x1cu) << 3);
					unsigned t3 = ((t & 0x03u) << 3) |
						      ((t & 0x03u) << 1) |
						      ((t & 0x02) >> 1);

					*(dPtr++) = static_cast<uint16_t>(
						t1 | t2 | t3);
				}
				return true;
			}
			return false;

		default:
			return false;
		}

	case TEXP_LEGACY_A8R3G3B2:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// D3DFMT_A8R3G3B2 -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 2 && outSize >= 4) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 3)));
			     icount += 2, ocount += 4) {
				const uint16_t t = *(sPtr++);

				uint32_t t1 = uint32_t((t & 0x00e0) |
						       ((t & 0x00e0) >> 3) |
						       ((t & 0x00c0) >> 6));
				uint32_t t2 = uint32_t(((t & 0x001c) << 11) |
						       ((t & 0x001c) << 8) |
						       ((t & 0x0018) << 5));
				uint32_t t3 = uint32_t(((t & 0x0003) << 22) |
						       ((t & 0x0003) << 20) |
						       ((t & 0x0003) << 18) |
						       ((t & 0x0003) << 16));
				uint32_t ta =
					(tflags & TEXP_SCANLINE_SETALPHA)
						? 0xff000000
						: uint32_t((t & 0xff00) << 16);

				*(dPtr++) = t1 | t2 | t3 | ta;
			}
			return true;
		}
		return false;

	case TEXP_LEGACY_P8:
		if ((outFormat != DXGI_FORMAT_R8G8B8A8_UNORM) || !pal8)
			return false;

		// D3DFMT_P8 -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 1 && outSize >= 4) {
			const uint8_t *__restrict sPtr =
				static_cast<const uint8_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < inSize) && (ocount < (outSize - 3)));
			     ++icount, ocount += 4) {
				uint8_t t = *(sPtr++);

				*(dPtr++) = pal8[t];
			}
			return true;
		}
		return false;

	case TEXP_LEGACY_A8P8:
		if ((outFormat != DXGI_FORMAT_R8G8B8A8_UNORM) || !pal8)
			return false;

		// D3DFMT_A8P8 -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 2 && outSize >= 4) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 3)));
			     icount += 2, ocount += 4) {
				const uint16_t t = *(sPtr++);

				uint32_t t1 = pal8[t & 0xff];
				uint32_t ta =
					(tflags & TEXP_SCANLINE_SETALPHA)
						? 0xff000000
						: uint32_t((t & 0xff00) << 16);

				*(dPtr++) = t1 | ta;
			}
			return true;
		}
		return false;

	case TEXP_LEGACY_A4L4:
		switch (outFormat) {
		case DXGI_FORMAT_B4G4R4A4_UNORM:
			// D3DFMT_A4L4 -> DXGI_FORMAT_B4G4R4A4_UNORM
			if (inSize >= 1 && outSize >= 2) {
				const uint8_t *__restrict sPtr =
					static_cast<const uint8_t *>(pSource);
				uint16_t *__restrict dPtr =
					static_cast<uint16_t *>(pDestination);

				for (size_t ocount = 0, icount = 0;
				     ((icount < inSize) &&
				      (ocount < (outSize - 1)));
				     ++icount, ocount += 2) {
					const unsigned t = *(sPtr++);

					unsigned t1 = (t & 0x0fu);
					unsigned ta =
						(tflags &
						 TEXP_SCANLINE_SETALPHA)
							? 0xf000u
							: ((t & 0xf0u) << 8);

					*(dPtr++) = static_cast<uint16_t>(
						t1 | (t1 << 4) | (t1 << 8) |
						ta);
				}
				return true;
			}
			return false;

		case DXGI_FORMAT_R8G8B8A8_UNORM:
			// D3DFMT_A4L4 -> DXGI_FORMAT_R8G8B8A8_UNORM
			if (inSize >= 1 && outSize >= 4) {
				const uint8_t *__restrict sPtr =
					static_cast<const uint8_t *>(pSource);
				uint32_t *__restrict dPtr =
					static_cast<uint32_t *>(pDestination);

				for (size_t ocount = 0, icount = 0;
				     ((icount < inSize) &&
				      (ocount < (outSize - 3)));
				     ++icount, ocount += 4) {
					const uint8_t t = *(sPtr++);

					uint32_t t1 = uint32_t(
						((t & 0x0f) << 4) | (t & 0x0f));
					uint32_t ta =
						(tflags &
						 TEXP_SCANLINE_SETALPHA)
							? 0xff000000
							: uint32_t(((t & 0xf0)
								    << 24) |
								   ((t & 0xf0)
								    << 20));

					*(dPtr++) = t1 | (t1 << 8) |
						    (t1 << 16) | ta;
				}
				return true;
			}
			return false;

		default:
			return false;
		}

	case TEXP_LEGACY_B4G4R4A4:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// D3DFMT_A4R4G4B4 -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 2 && outSize >= 4) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 3)));
			     icount += 2, ocount += 4) {
				const uint32_t t = *(sPtr++);

				uint32_t t1 = uint32_t((t & 0x0f00) >> 4) |
					      ((t & 0x0f00) >> 8);
				uint32_t t2 = uint32_t((t & 0x00f0) << 8) |
					      ((t & 0x00f0) << 4);
				uint32_t t3 = uint32_t((t & 0x000f) << 20) |
					      ((t & 0x000f) << 16);
				uint32_t ta = uint32_t(
					(tflags & TEXP_SCANLINE_SETALPHA)
						? 0xff000000
						: (((t & 0xf000) << 16) |
						   ((t & 0xf000) << 12)));

				*(dPtr++) = t1 | t2 | t3 | ta;
			}
			return true;
		}
		return false;

	case TEXP_LEGACY_L8:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// D3DFMT_L8 -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 1 && outSize >= 4) {
			const uint8_t *__restrict sPtr =
				static_cast<const uint8_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < inSize) && (ocount < (outSize - 3)));
			     ++icount, ocount += 4) {
				uint32_t t1 = *(sPtr++);
				uint32_t t2 = (t1 << 8);
				uint32_t t3 = (t1 << 16);

				*(dPtr++) = t1 | t2 | t3 | 0xff000000;
			}
			return true;
		}
		return false;

	case TEXP_LEGACY_L16:
		if (outFormat != DXGI_FORMAT_R16G16B16A16_UNORM)
			return false;

		// D3DFMT_L16 -> DXGI_FORMAT_R16G16B16A16_UNORM
		if (inSize >= 2 && outSize >= 8) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint64_t *__restrict dPtr =
				static_cast<uint64_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 7)));
			     icount += 2, ocount += 8) {
				const uint16_t t = *(sPtr++);

				uint64_t t1 = t;
				uint64_t t2 = (t1 << 16);
				uint64_t t3 = (t1 << 32);

				*(dPtr++) = t1 | t2 | t3 | 0xffff000000000000;
			}
			return true;
		}
		return false;

	case TEXP_LEGACY_A8L8:
		if (outFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;

		// D3DFMT_A8L8 -> DXGI_FORMAT_R8G8B8A8_UNORM
		if (inSize >= 2 && outSize >= 4) {
			const uint16_t *__restrict sPtr =
				static_cast<const uint16_t *>(pSource);
			uint32_t *__restrict dPtr =
				static_cast<uint32_t *>(pDestination);

			for (size_t ocount = 0, icount = 0;
			     ((icount < (inSize - 1)) &&
			      (ocount < (outSize - 3)));
			     icount += 2, ocount += 4) {
				const uint16_t t = *(sPtr++);

				uint32_t t1 = uint32_t(t & 0xff);
				uint32_t t2 = uint32_t(t1 << 8);
				uint32_t t3 = uint32_t(t1 << 16);
				uint32_t ta =
					(tflags & TEXP_SCANLINE_SETALPHA)
						? 0xff000000
						: uint32_t((t & 0xff00) << 16);

				*(dPtr++) = t1 | t2 | t3 | ta;
			}
			return true;
		}
		return false;

	default:
		return false;
	}
}

constexpr TEXP_LEGACY_FORMAT FindLegacyFormat(uint32_t flags) noexcept
{
	TEXP_LEGACY_FORMAT lformat = TEXP_LEGACY_UNKNOWN;

	if (flags & CONV_FLAGS_PAL8) {
		lformat = (flags & CONV_FLAGS_A8P8) ? TEXP_LEGACY_A8P8
						    : TEXP_LEGACY_P8;
	} else if (flags & CONV_FLAGS_888)
		lformat = TEXP_LEGACY_R8G8B8;
	else if (flags & CONV_FLAGS_332)
		lformat = TEXP_LEGACY_R3G3B2;
	else if (flags & CONV_FLAGS_8332)
		lformat = TEXP_LEGACY_A8R3G3B2;
	else if (flags & CONV_FLAGS_44)
		lformat = TEXP_LEGACY_A4L4;
	else if (flags & CONV_FLAGS_4444)
		lformat = TEXP_LEGACY_B4G4R4A4;
	else if (flags & CONV_FLAGS_L8)
		lformat = TEXP_LEGACY_L8;
	else if (flags & CONV_FLAGS_L16)
		lformat = TEXP_LEGACY_L16;
	else if (flags & CONV_FLAGS_A8L8)
		lformat = TEXP_LEGACY_A8L8;

	return lformat;
}

void SwizzleScanline(void *pDestination, size_t outSize, const void *pSource,
		     size_t inSize, DXGI_FORMAT format,
		     uint32_t tflags) noexcept
{
	assert(pDestination && outSize > 0);
	assert(pSource && inSize > 0);
	assert(IsValid(format) && !IsPlanar(format) && !IsPalettized(format));

	switch (static_cast<int>(format)) {
		//---------------------------------------------------------------------------------
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
	case XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM:
		if (inSize >= 4 && outSize >= 4) {
			if (tflags & TEXP_SCANLINE_LEGACY) {
				// Swap Red (R) and Blue (B) channel (used for D3DFMT_A2R10G10B10 legacy sources)
				if (pDestination == pSource) {
					auto dPtr = static_cast<uint32_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 3);
					     count += 4) {
						const uint32_t t = *dPtr;

						uint32_t t1 =
							(t & 0x3ff00000) >> 20;
						uint32_t t2 = (t & 0x000003ff)
							      << 20;
						uint32_t t3 = (t & 0x000ffc00);
						uint32_t ta =
							(tflags &
							 TEXP_SCANLINE_SETALPHA)
								? 0xC0000000
								: (t &
								   0xC0000000);

						*(dPtr++) = t1 | t2 | t3 | ta;
					}
				} else {
					const uint32_t *__restrict sPtr =
						static_cast<const uint32_t *>(
							pSource);
					uint32_t *__restrict dPtr =
						static_cast<uint32_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 3); count += 4) {
						const uint32_t t = *(sPtr++);

						uint32_t t1 =
							(t & 0x3ff00000) >> 20;
						uint32_t t2 = (t & 0x000003ff)
							      << 20;
						uint32_t t3 = (t & 0x000ffc00);
						uint32_t ta =
							(tflags &
							 TEXP_SCANLINE_SETALPHA)
								? 0xC0000000
								: (t &
								   0xC0000000);

						*(dPtr++) = t1 | t2 | t3 | ta;
					}
				}
				return;
			}
		}
		break;

		//---------------------------------------------------------------------------------
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		if (inSize >= 4 && outSize >= 4) {
			// Swap Red (R) and Blue (B) channels (used to convert from DXGI 1.1 BGR formats to DXGI 1.0 RGB)
			if (pDestination == pSource) {
				auto dPtr =
					static_cast<uint32_t *>(pDestination);
				for (size_t count = 0; count < (outSize - 3);
				     count += 4) {
					const uint32_t t = *dPtr;

					uint32_t t1 = (t & 0x00ff0000) >> 16;
					uint32_t t2 = (t & 0x000000ff) << 16;
					uint32_t t3 = (t & 0x0000ff00);
					uint32_t ta =
						(tflags &
						 TEXP_SCANLINE_SETALPHA)
							? 0xff000000
							: (t & 0xFF000000);

					*(dPtr++) = t1 | t2 | t3 | ta;
				}
			} else {
				const uint32_t *__restrict sPtr =
					static_cast<const uint32_t *>(pSource);
				uint32_t *__restrict dPtr =
					static_cast<uint32_t *>(pDestination);
				const size_t size =
					std::min<size_t>(outSize, inSize);
				for (size_t count = 0; count < (size - 3);
				     count += 4) {
					const uint32_t t = *(sPtr++);

					uint32_t t1 = (t & 0x00ff0000) >> 16;
					uint32_t t2 = (t & 0x000000ff) << 16;
					uint32_t t3 = (t & 0x0000ff00);
					uint32_t ta =
						(tflags &
						 TEXP_SCANLINE_SETALPHA)
							? 0xff000000
							: (t & 0xFF000000);

					*(dPtr++) = t1 | t2 | t3 | ta;
				}
			}
			return;
		}
		break;

		//---------------------------------------------------------------------------------
	case DXGI_FORMAT_YUY2:
		if (inSize >= 4 && outSize >= 4) {
			if (tflags & TEXP_SCANLINE_LEGACY) {
				// Reorder YUV components (used to convert legacy UYVY -> YUY2)
				if (pDestination == pSource) {
					auto dPtr = static_cast<uint32_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 3);
					     count += 4) {
						const uint32_t t = *dPtr;

						uint32_t t1 = (t & 0x000000ff)
							      << 8;
						uint32_t t2 =
							(t & 0x0000ff00) >> 8;
						uint32_t t3 = (t & 0x00ff0000)
							      << 8;
						uint32_t t4 =
							(t & 0xff000000) >> 8;

						*(dPtr++) = t1 | t2 | t3 | t4;
					}
				} else {
					const uint32_t *__restrict sPtr =
						static_cast<const uint32_t *>(
							pSource);
					uint32_t *__restrict dPtr =
						static_cast<uint32_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 3); count += 4) {
						const uint32_t t = *(sPtr++);

						uint32_t t1 = (t & 0x000000ff)
							      << 8;
						uint32_t t2 =
							(t & 0x0000ff00) >> 8;
						uint32_t t3 = (t & 0x00ff0000)
							      << 8;
						uint32_t t4 =
							(t & 0xff000000) >> 8;

						*(dPtr++) = t1 | t2 | t3 | t4;
					}
				}
				return;
			}
		}
		break;
	}

	// Fall-through case is to just use memcpy (assuming this is not an in-place operation)
	if (pDestination == pSource)
		return;

	const size_t size = std::min<size_t>(outSize, inSize);
	memcpy(pDestination, pSource, size);
}

void CopyScanline(void *pDestination, size_t outSize, const void *pSource,
		  size_t inSize, DXGI_FORMAT format, uint32_t tflags) noexcept
{
	assert(pDestination && outSize > 0);
	assert(pSource && inSize > 0);
	assert(IsValid(format) && !IsPalettized(format));

	if (tflags & TEXP_SCANLINE_SETALPHA) {
		switch (static_cast<int>(format)) {
			//-----------------------------------------------------------------------------
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			if (inSize >= 16 && outSize >= 16) {
				uint32_t alpha;
				if (format == DXGI_FORMAT_R32G32B32A32_FLOAT)
					alpha = 0x3f800000;
				else if (format ==
					 DXGI_FORMAT_R32G32B32A32_SINT)
					alpha = 0x7fffffff;
				else
					alpha = 0xffffffff;

				if (pDestination == pSource) {
					auto dPtr = static_cast<uint32_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 15);
					     count += 16) {
						dPtr += 3;
						*(dPtr++) = alpha;
					}
				} else {
					const uint32_t *__restrict sPtr =
						static_cast<const uint32_t *>(
							pSource);
					uint32_t *__restrict dPtr =
						static_cast<uint32_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 15); count += 16) {
						*(dPtr++) = *(sPtr++);
						*(dPtr++) = *(sPtr++);
						*(dPtr++) = *(sPtr++);
						*(dPtr++) = alpha;
						++sPtr;
					}
				}
			}
			return;

			//-----------------------------------------------------------------------------
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_Y416:
			if (inSize >= 8 && outSize >= 8) {
				uint16_t alpha;
				if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
					alpha = 0x3c00;
				else if (format ==
						 DXGI_FORMAT_R16G16B16A16_SNORM ||
					 format ==
						 DXGI_FORMAT_R16G16B16A16_SINT)
					alpha = 0x7fff;
				else
					alpha = 0xffff;

				if (pDestination == pSource) {
					auto dPtr = static_cast<uint16_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 7);
					     count += 8) {
						dPtr += 3;
						*(dPtr++) = alpha;
					}
				} else {
					const uint16_t *__restrict sPtr =
						static_cast<const uint16_t *>(
							pSource);
					uint16_t *__restrict dPtr =
						static_cast<uint16_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 7); count += 8) {
						*(dPtr++) = *(sPtr++);
						*(dPtr++) = *(sPtr++);
						*(dPtr++) = *(sPtr++);
						*(dPtr++) = alpha;
						++sPtr;
					}
				}
			}
			return;

			//-----------------------------------------------------------------------------
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UINT:
		case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
		case DXGI_FORMAT_Y410:
		case XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT:
		case XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT:
		case XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM:
			if (inSize >= 4 && outSize >= 4) {
				if (pDestination == pSource) {
					auto dPtr = static_cast<uint32_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 3);
					     count += 4) {
						*dPtr |= 0xC0000000;
						++dPtr;
					}
				} else {
					const uint32_t *__restrict sPtr =
						static_cast<const uint32_t *>(
							pSource);
					uint32_t *__restrict dPtr =
						static_cast<uint32_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 3); count += 4) {
						*(dPtr++) = *(sPtr++) |
							    0xC0000000;
					}
				}
			}
			return;

			//-----------------------------------------------------------------------------
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_AYUV:
			if (inSize >= 4 && outSize >= 4) {
				const uint32_t alpha =
					(format == DXGI_FORMAT_R8G8B8A8_SNORM ||
					 format == DXGI_FORMAT_R8G8B8A8_SINT)
						? 0x7f000000
						: 0xff000000;

				if (pDestination == pSource) {
					auto dPtr = static_cast<uint32_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 3);
					     count += 4) {
						uint32_t t = *dPtr & 0xFFFFFF;
						t |= alpha;
						*(dPtr++) = t;
					}
				} else {
					const uint32_t *__restrict sPtr =
						static_cast<const uint32_t *>(
							pSource);
					uint32_t *__restrict dPtr =
						static_cast<uint32_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 3); count += 4) {
						uint32_t t = *(sPtr++) &
							     0xFFFFFF;
						t |= alpha;
						*(dPtr++) = t;
					}
				}
			}
			return;

			//-----------------------------------------------------------------------------
		case DXGI_FORMAT_B5G5R5A1_UNORM:
			if (inSize >= 2 && outSize >= 2) {
				if (pDestination == pSource) {
					auto dPtr = static_cast<uint16_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 1);
					     count += 2) {
						*(dPtr++) |= 0x8000;
					}
				} else {
					const uint16_t *__restrict sPtr =
						static_cast<const uint16_t *>(
							pSource);
					uint16_t *__restrict dPtr =
						static_cast<uint16_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 1); count += 2) {
						*(dPtr++) = uint16_t(*(sPtr++) |
								     0x8000);
					}
				}
			}
			return;

			//-----------------------------------------------------------------------------
		case DXGI_FORMAT_A8_UNORM:
			memset(pDestination, 0xff, outSize);
			return;

			//-----------------------------------------------------------------------------
		case DXGI_FORMAT_B4G4R4A4_UNORM:
			if (inSize >= 2 && outSize >= 2) {
				if (pDestination == pSource) {
					auto dPtr = static_cast<uint16_t *>(
						pDestination);
					for (size_t count = 0;
					     count < (outSize - 1);
					     count += 2) {
						*(dPtr++) |= 0xF000;
					}
				} else {
					const uint16_t *__restrict sPtr =
						static_cast<const uint16_t *>(
							pSource);
					uint16_t *__restrict dPtr =
						static_cast<uint16_t *>(
							pDestination);
					const size_t size = std::min<size_t>(
						outSize, inSize);
					for (size_t count = 0;
					     count < (size - 1); count += 2) {
						*(dPtr++) = uint16_t(*(sPtr++) |
								     0xF000);
					}
				}
			}
			return;
		}
	}

	// Fall-through case is to just use memcpy (assuming this is not an in-place operation)
	if (pDestination == pSource)
		return;

	const size_t size = std::min<size_t>(outSize, inSize);
	memcpy(pDestination, pSource, size);
}

bool CopyImage(_In_reads_bytes_(size) const void *pPixels, _In_ size_t size,
	       _In_ const TexMetadata &metadata, _In_ CP_FLAGS cpFlags,
	       _In_ uint32_t convFlags,
	       _In_reads_opt_(256) const uint32_t *pal8,
	       _In_ const ScratchImage &image) noexcept
{
	assert(pPixels);
	assert(image.GetPixels());

	if (!size)
		return false;

	if (convFlags & CONV_FLAGS_EXPAND) {
		if (convFlags & CONV_FLAGS_888)
			cpFlags |= CP_FLAGS_24BPP;
		else if (convFlags &
			 (CONV_FLAGS_565 | CONV_FLAGS_5551 | CONV_FLAGS_4444 |
			  CONV_FLAGS_8332 | CONV_FLAGS_A8P8 | CONV_FLAGS_L16 |
			  CONV_FLAGS_A8L8))
			cpFlags |= CP_FLAGS_16BPP;
		else if (convFlags & (CONV_FLAGS_44 | CONV_FLAGS_332 |
				      CONV_FLAGS_PAL8 | CONV_FLAGS_L8))
			cpFlags |= CP_FLAGS_8BPP;
	}

	size_t pixelSize, nimages;
	if (!DetermineImageArray(metadata, cpFlags, nimages, pixelSize))
		return false;

	if ((nimages == 0) || (nimages != image.GetImageCount())) {
		return false;
	}

	if (pixelSize > size) {
		return false;
	}

	std::unique_ptr<Image[]> timages(new (std::nothrow) Image[nimages]);
	if (!timages) {
		return false;
	}

	if (!SetupImageArray(
		    const_cast<uint8_t *>(
			    reinterpret_cast<const uint8_t *>(pPixels)),
		    pixelSize, metadata, cpFlags, timages.get(), nimages)) {
		return false;
	}

	if (nimages != image.GetImageCount()) {
		return false;
	}

	const Image *images = image.GetImages();
	if (!images) {
		return false;
	}

	uint32_t tflags =
		(convFlags & CONV_FLAGS_NOALPHA) ? TEXP_SCANLINE_SETALPHA : 0u;
	if (convFlags & CONV_FLAGS_SWIZZLE)
		tflags |= TEXP_SCANLINE_LEGACY;

	switch (metadata.dimension) {
	case TEX_DIMENSION_TEXTURE1D:
	case TEX_DIMENSION_TEXTURE2D: {
		size_t index = 0;
		for (size_t item = 0; item < metadata.arraySize; ++item) {
			size_t lastgood = 0;
			for (size_t level = 0; level < metadata.mipLevels;
			     ++level, ++index) {
				if (index >= nimages)
					return false;

				if (images[index].height !=
				    timages[index].height)
					return false;

				size_t dpitch = images[index].rowPitch;
				const size_t spitch = timages[index].rowPitch;

				const uint8_t *pSrc = timages[index].pixels;
				if (!pSrc)
					return false;

				uint8_t *pDest = images[index].pixels;
				if (!pDest)
					return false;

				if (IsCompressed(metadata.format)) {
					size_t csize = std::min<size_t>(
						images[index].slicePitch,
						timages[index].slicePitch);
					memcpy(pDest, pSrc, csize);

					if (cpFlags & CP_FLAGS_BAD_DXTN_TAILS) {
						if (images[index].width < 4 ||
						    images[index].height < 4) {
							csize = std::min<size_t>(
								images[index]
									.slicePitch,
								timages[lastgood]
									.slicePitch);
							memcpy(pDest,
							       timages[lastgood]
								       .pixels,
							       csize);
						} else {
							lastgood = index;
						}
					}
				} else if (IsPlanar(metadata.format)) {
					const size_t count = ComputeScanlines(
						metadata.format,
						images[index].height);
					if (!count)
						return false;

					const size_t csize = std::min<size_t>(
						dpitch, spitch);
					for (size_t h = 0; h < count; ++h) {
						memcpy(pDest, pSrc, csize);
						pSrc += spitch;
						pDest += dpitch;
					}
				} else {
					for (size_t h = 0;
					     h < images[index].height; ++h) {
						if (convFlags &
						    CONV_FLAGS_EXPAND) {
							if (convFlags &
							    (CONV_FLAGS_565 |
							     CONV_FLAGS_5551 |
							     CONV_FLAGS_4444)) {
								if (!ExpandScanline(
									    pDest,
									    dpitch,
									    DXGI_FORMAT_R8G8B8A8_UNORM,
									    pSrc,
									    spitch,
									    (convFlags &
									     CONV_FLAGS_565)
										    ? DXGI_FORMAT_B5G6R5_UNORM
										    : DXGI_FORMAT_B5G5R5A1_UNORM,
									    tflags))
									return false;
							} else {
								const TEXP_LEGACY_FORMAT
									lformat = FindLegacyFormat(
										convFlags);
								if (!LegacyExpandScanline(
									    pDest,
									    dpitch,
									    metadata.format,
									    pSrc,
									    spitch,
									    lformat,
									    pal8,
									    tflags))
									return false;
							}
						} else if (convFlags &
							   CONV_FLAGS_SWIZZLE) {
							SwizzleScanline(
								pDest, dpitch,
								pSrc, spitch,
								metadata.format,
								tflags);
						} else {
							CopyScanline(
								pDest, dpitch,
								pSrc, spitch,
								metadata.format,
								tflags);
						}

						pSrc += spitch;
						pDest += dpitch;
					}
				}
			}
		}
	} break;

	case TEX_DIMENSION_TEXTURE3D: {
		size_t index = 0;
		size_t d = metadata.depth;

		size_t lastgood = 0;
		for (size_t level = 0; level < metadata.mipLevels; ++level) {
			for (size_t slice = 0; slice < d; ++slice, ++index) {
				if (index >= nimages)
					return false;

				if (images[index].height !=
				    timages[index].height)
					return false;

				size_t dpitch = images[index].rowPitch;
				const size_t spitch = timages[index].rowPitch;

				const uint8_t *pSrc = timages[index].pixels;
				if (!pSrc)
					return false;

				uint8_t *pDest = images[index].pixels;
				if (!pDest)
					return false;

				if (IsCompressed(metadata.format)) {
					size_t csize = std::min<size_t>(
						images[index].slicePitch,
						timages[index].slicePitch);
					memcpy(pDest, pSrc, csize);

					if (cpFlags & CP_FLAGS_BAD_DXTN_TAILS) {
						if (images[index].width < 4 ||
						    images[index].height < 4) {
							csize = std::min<size_t>(
								images[index]
									.slicePitch,
								timages[lastgood +
									slice]
									.slicePitch);
							memcpy(pDest,
							       timages[lastgood +
								       slice]
								       .pixels,
							       csize);
						} else if (!slice) {
							lastgood = index;
						}
					}
				} else if (IsPlanar(metadata.format)) {
					// Direct3D does not support any planar formats for Texture3D
					return false;
				} else {
					for (size_t h = 0;
					     h < images[index].height; ++h) {
						if (convFlags &
						    CONV_FLAGS_EXPAND) {
							if (convFlags &
							    (CONV_FLAGS_565 |
							     CONV_FLAGS_5551 |
							     CONV_FLAGS_4444)) {
								if (!ExpandScanline(
									    pDest,
									    dpitch,
									    DXGI_FORMAT_R8G8B8A8_UNORM,
									    pSrc,
									    spitch,
									    (convFlags &
									     CONV_FLAGS_565)
										    ? DXGI_FORMAT_B5G6R5_UNORM
										    : DXGI_FORMAT_B5G5R5A1_UNORM,
									    tflags))
									return false;
							} else {
								const TEXP_LEGACY_FORMAT
									lformat = FindLegacyFormat(
										convFlags);
								if (!LegacyExpandScanline(
									    pDest,
									    dpitch,
									    metadata.format,
									    pSrc,
									    spitch,
									    lformat,
									    pal8,
									    tflags))
									return false;
							}
						} else if (convFlags &
							   CONV_FLAGS_SWIZZLE) {
							SwizzleScanline(
								pDest, dpitch,
								pSrc, spitch,
								metadata.format,
								tflags);
						} else {
							CopyScanline(
								pDest, dpitch,
								pSrc, spitch,
								metadata.format,
								tflags);
						}

						pSrc += spitch;
						pDest += dpitch;
					}
				}
			}

			if (d > 1)
				d >>= 1;
		}
	} break;

	default:
		return false;
	}

	return true;
}

bool CopyImageInPlace(uint32_t convFlags,
		      _In_ const ScratchImage &image) noexcept
{
	if (!image.GetPixels())
		return false;

	const Image *images = image.GetImages();
	if (!images)
		return false;

	const TexMetadata &metadata = image.GetMetadata();

	if (IsPlanar(metadata.format))
		return false;

	uint32_t tflags =
		(convFlags & CONV_FLAGS_NOALPHA) ? TEXP_SCANLINE_SETALPHA : 0u;
	if (convFlags & CONV_FLAGS_SWIZZLE)
		tflags |= TEXP_SCANLINE_LEGACY;

	for (size_t i = 0; i < image.GetImageCount(); ++i) {
		const Image *img = &images[i];
		uint8_t *pPixels = img->pixels;
		if (!pPixels)
			return false;

		size_t rowPitch = img->rowPitch;

		for (size_t h = 0; h < img->height; ++h) {
			if (convFlags & CONV_FLAGS_SWIZZLE) {
				SwizzleScanline(pPixels, rowPitch, pPixels,
						rowPitch, metadata.format,
						tflags);
			} else {
				CopyScanline(pPixels, rowPitch, pPixels,
					     rowPitch, metadata.format, tflags);
			}

			pPixels += rowPitch;
		}
	}

	return true;
}

bool LoadFromDDSFile(const char *path, TexMetadata *metadata,
		     ScratchImage &image) noexcept
{
	if (!path)
		return false;

	image.Release();

	FILE *file = os_fopen(path, "rb");
	if (!file)
		return false;

	// Get the file size
	fseek(file, 0, SEEK_END);
	const int64_t len = os_ftelli64(file);
	fseek(file, 0, SEEK_SET);

	// Need at least enough data to fill the standard header and magic number to be a valid DDS
	if (len < (sizeof(DDS_HEADER) + sizeof(uint32_t))) {
		return false;
	}

	// Read the header in (including extended header if present)
	uint8_t header[MAX_HEADER_SIZE] = {};

	const size_t headerLen = std::min<size_t>(len, MAX_HEADER_SIZE);

	if (fread(header, 1, headerLen, file) != headerLen)
		return false;

	uint32_t convFlags = 0;
	TexMetadata mdata;
	if (!DecodeDDSHeader(header, headerLen, mdata, convFlags))
		return false;

	size_t offset = MAX_HEADER_SIZE;

	if (!(convFlags & CONV_FLAGS_DX10)) {
		// Must reset file position since we read more than the standard header above
		offset = sizeof(uint32_t) + sizeof(DDS_HEADER);
		fseek(file, (long)offset, SEEK_SET);
	}

	std::unique_ptr<uint32_t[]> pal8;
	if (convFlags & CONV_FLAGS_PAL8) {
		pal8.reset(new (std::nothrow) uint32_t[256]);
		if (!pal8) {
			return false;
		}

		if (fread(pal8.get(), 1, 256 * sizeof(uint32_t), file) !=
		    (256 * sizeof(uint32_t))) {
			return false;
		}

		offset += (256 * sizeof(uint32_t));
	}

	const size_t remaining = len - offset;
	if (remaining == 0)
		return false;

	HRESULT hr = image.Initialize(mdata);
	if (FAILED(hr))
		return hr;

	if (convFlags & CONV_FLAGS_EXPAND) {
		std::unique_ptr<uint8_t[]> temp(new (std::nothrow)
							uint8_t[remaining]);
		if (!temp) {
			image.Release();
			return false;
		}

		if (fread(temp.get(), 1, remaining, file) != remaining) {
			image.Release();
			return false;
		}

		CP_FLAGS cflags = CP_FLAGS_NONE;

		if (!CopyImage(temp.get(), remaining, mdata, cflags, convFlags,
			       pal8.get(), image)) {
			image.Release();
			return false;
		}
	} else {
		if (remaining < image.GetPixelsSize()) {
			image.Release();
			return false;
		}

		if (image.GetPixelsSize() > UINT32_MAX) {
			image.Release();
			return false;
		}

		if (fread(image.GetPixels(), 1, image.GetPixelsSize(), file) !=
		    image.GetPixelsSize()) {
			image.Release();
			return false;
		}

		if (convFlags & (CONV_FLAGS_SWIZZLE | CONV_FLAGS_NOALPHA)) {
			// Swizzle/copy image in place
			if (!CopyImageInPlace(convFlags, image)) {
				image.Release();
				return false;
			}
		}
	}

	if (metadata)
		memcpy(metadata, &mdata, sizeof(TexMetadata));

	return true;
}

static gs_color_format ConvertDXGITextureFormat(DXGI_FORMAT format)
{
	switch ((unsigned long)format) {
	case DXGI_FORMAT_A8_UNORM:
		return GS_A8;
	case DXGI_FORMAT_R8_UNORM:
		return GS_R8;
	case DXGI_FORMAT_R8G8_UNORM:
		return GS_R8G8;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		return GS_RGBA;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		return GS_BGRX;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return GS_BGRA;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		return GS_RGBA16;
	case DXGI_FORMAT_R16_UNORM:
		return GS_R16;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return GS_RGBA16F;
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return GS_RGBA32F;
	case DXGI_FORMAT_R16G16_FLOAT:
		return GS_RG16F;
	case DXGI_FORMAT_R32G32_FLOAT:
		return GS_RG32F;
	case DXGI_FORMAT_R16_FLOAT:
		return GS_R16F;
	case DXGI_FORMAT_R32_FLOAT:
		return GS_R32F;
	case DXGI_FORMAT_BC1_UNORM:
		return GS_DXT1;
	case DXGI_FORMAT_BC2_UNORM:
		return GS_DXT3;
	case DXGI_FORMAT_BC3_UNORM:
		return GS_DXT5;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return GS_RGBA_UNORM;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return GS_BGRX_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return GS_BGRA_UNORM;
	case DXGI_FORMAT_R16G16_UNORM:
		return GS_RG16;
	}

	return GS_UNKNOWN;
}

gs_texture_t *CreateTextureEx(const Image *srcImages, size_t nimages,
			      const TexMetadata &metadata) noexcept
{
	if (!srcImages || !nimages)
		return NULL;

	if (!metadata.mipLevels || !metadata.arraySize)
		return NULL;

	if ((metadata.width > UINT32_MAX) || (metadata.height > UINT32_MAX) ||
	    (metadata.mipLevels > UINT16_MAX) ||
	    (metadata.arraySize > UINT16_MAX))
		return NULL;

	std::unique_ptr<const uint8_t *[]> initData(
		new (std::nothrow) const uint8_t
			*[metadata.mipLevels * metadata.arraySize]);
	if (!initData)
		return NULL;

	// Fill out subresource array
	if (metadata.IsVolumemap()) {
		//--- Volume case -------------------------------------------------------------
		if (!metadata.depth)
			return NULL;

		if (metadata.depth > UINT16_MAX)
			return NULL;

		if (metadata.arraySize > 1)
			// Direct3D 11 doesn't support arrays of 3D textures
			return NULL;

		size_t depth = metadata.depth;

		size_t idx = 0;
		for (size_t level = 0; level < metadata.mipLevels; ++level) {
			const size_t index = metadata.ComputeIndex(level, 0, 0);
			if (index >= nimages)
				return NULL;

			const Image &img = srcImages[index];

			if (img.format != metadata.format)
				return NULL;

			if (!img.pixels)
				return NULL;

			// Verify pixels in image 1 .. (depth-1) are exactly image->slicePitch apart
			// For 3D textures, this relies on all slices of the same miplevel being continous in memory
			// (this is how ScratchImage lays them out), which is why we just give the 0th slice to Direct3D 11
			const uint8_t *pSlice = img.pixels + img.slicePitch;
			for (size_t slice = 1; slice < depth; ++slice) {
				const size_t tindex =
					metadata.ComputeIndex(level, 0, slice);
				if (tindex >= nimages)
					return NULL;

				const Image &timg = srcImages[tindex];

				if (!timg.pixels)
					return NULL;

				if (timg.pixels != pSlice ||
				    timg.format != metadata.format ||
				    timg.rowPitch != img.rowPitch ||
				    timg.slicePitch != img.slicePitch)
					return NULL;

				pSlice = timg.pixels + img.slicePitch;
			}

			assert(idx < (metadata.mipLevels * metadata.arraySize));

			initData[idx] = img.pixels;
			++idx;

			if (depth > 1)
				depth >>= 1;
		}
	} else {
		//--- 1D or 2D texture case ---------------------------------------------------
		size_t idx = 0;
		for (size_t item = 0; item < metadata.arraySize; ++item) {
			for (size_t level = 0; level < metadata.mipLevels;
			     ++level) {
				const size_t index =
					metadata.ComputeIndex(level, item, 0);
				if (index >= nimages)
					return NULL;

				const Image &img = srcImages[index];

				if (img.format != metadata.format)
					return NULL;

				if (!img.pixels)
					return NULL;

				assert(idx < (metadata.mipLevels *
					      metadata.arraySize));

				initData[idx] = img.pixels;
				++idx;
			}
		}
	}

	const enum gs_color_format tformat =
		ConvertDXGITextureFormat(metadata.format);
	if (tformat == GS_UNKNOWN)
		return NULL;

	switch (metadata.dimension) {
	case TEX_DIMENSION_TEXTURE1D:
		/* not implkemented */
		return NULL;

	case TEX_DIMENSION_TEXTURE2D:
		if (metadata.IsCubemap()) {
			return gs_cubetexture_create(
				(uint32_t)metadata.width, tformat,
				(uint32_t)metadata.mipLevels, initData.get(),
				0);
		} else {
			return gs_texture_create((uint32_t)metadata.width,
						 (uint32_t)metadata.height,
						 tformat,
						 (uint32_t)metadata.mipLevels,
						 initData.get(), 0);
		}

	case TEX_DIMENSION_TEXTURE3D:
		return gs_voltexture_create((uint32_t)metadata.width,
					    (uint32_t)metadata.height,
					    (uint32_t)metadata.depth, tformat,
					    (uint32_t)metadata.mipLevels,
					    initData.get(), 0);
	}

	return NULL;
}

gs_texture_t *gs_create_texture_from_dds_file(const char *file)
{
	ScratchImage image;
	TexMetadata mdata;
	if (!LoadFromDDSFile(file, &mdata, image))
		return NULL;

	// Special case to make sure Texture cubes remain arrays
	mdata.miscFlags &= ~TEX_MISC_TEXTURECUBE;

	return CreateTextureEx(image.GetImages(), image.GetImageCount(), mdata);
}
