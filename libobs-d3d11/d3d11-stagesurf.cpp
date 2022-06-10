/******************************************************************************
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

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

#include "d3d11-subsystem.hpp"

gs_stage_surface::gs_stage_surface(gs_device_t *device, uint32_t width,
				   uint32_t height, gs_color_format colorFormat)
	: gs_obj(device, gs_type::gs_stage_surface),
	  width(width),
	  height(height),
	  format(colorFormat),
	  dxgiFormat(ConvertGSTextureFormatView(colorFormat))
{
	D3D12_RESOURCE_DESC cool;
	cool.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	cool.Alignment = 0;
	cool.Width = width;
	cool.Height = height;
	cool.DepthOrArraySize = 1;
	cool.MipLevels = 1;
	cool.Format = dxgiFormat;
	cool.SampleDesc.Count = 1;
	cool.SampleDesc.Quality = 0;
	cool.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	cool.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
		     D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	UINT64 totalBytes;
	device->d3d12Device->GetCopyableFootprints(
		&cool, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
	rowPitch = layout.Footprint.RowPitch;

	hp.Type = D3D12_HEAP_TYPE_READBACK;
	hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	hp.CreationNodeMask = 1;
	hp.VisibleNodeMask = 1;
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Alignment = 0;
	rd.Width = totalBytes;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.SampleDesc.Quality = 0;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags = D3D12_RESOURCE_FLAG_NONE;

	HRESULT hr = device->d3d12Device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(allocator.Assign()));
	if (FAILED(hr))
		throw HRError("Failed to CreateCommandAllocator", hr);

	hr = device->d3d12Device->CreateCommandList(
		1, D3D12_COMMAND_LIST_TYPE_COPY, allocator, nullptr,
		IID_PPV_ARGS(commandList.Assign()));
	if (FAILED(hr))
		throw HRError("Failed to CreateCommandList", hr);
	commandList->Close();

	hr = device->d3d12Device->CreateCommittedResource(
		&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr, IID_PPV_ARGS(resource.Assign()));
	if (FAILED(hr))
		throw HRError("Failed to create staging surface", hr);
}

gs_stage_surface::gs_stage_surface(gs_device_t *device, uint32_t width,
				   uint32_t height, bool p010)
	: gs_obj(device, gs_type::gs_stage_surface),
	  width(width),
	  height(height),
	  format(GS_UNKNOWN),
	  dxgiFormat(p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12)
{
	D3D12_RESOURCE_DESC cool;
	cool.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	cool.Alignment = 0;
	cool.Width = width;
	cool.Height = height;
	cool.DepthOrArraySize = 1;
	cool.MipLevels = 1;
	cool.Format = dxgiFormat;
	cool.SampleDesc.Count = 1;
	cool.SampleDesc.Quality = 0;
	cool.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	cool.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
		     D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	UINT64 totalBytes;
	device->d3d12Device->GetCopyableFootprints(
		&cool, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
	rowPitch = layout.Footprint.RowPitch;

	hp.Type = D3D12_HEAP_TYPE_READBACK;
	hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	hp.CreationNodeMask = 1;
	hp.VisibleNodeMask = 1;
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Alignment = 0;
	rd.Width = totalBytes;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format, DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.SampleDesc.Quality = 0;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags = D3D12_RESOURCE_FLAG_NONE;

	HRESULT hr = device->d3d12Device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(allocator.Assign()));
	if (FAILED(hr))
		throw HRError("Failed to CreateCommandAllocator", hr);

	hr = device->d3d12Device->CreateCommandList(
		1, D3D12_COMMAND_LIST_TYPE_COPY, allocator, nullptr,
		IID_PPV_ARGS(commandList.Assign()));
	if (FAILED(hr))
		throw HRError("Failed to CreateCommandList", hr);
	commandList->Close();

	hr = device->d3d12Device->CreateCommittedResource(
		&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr, IID_PPV_ARGS(resource.Assign()));
	if (FAILED(hr))
		throw HRError("Failed to create staging surface", hr);
}
