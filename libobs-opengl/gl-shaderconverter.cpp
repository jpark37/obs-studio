/******************************************************************************
    Copyright (C) 2021 by Hugh Bailey <obs.jim@gmail.com>

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

#define NOMINMAX

#include <util/base.h>

#include "C:\\Users\\James\Downloads\\dxc_2021_04-20\\inc\\d3d12shader.h"
#include "C:\\Users\\James\Downloads\\dxc_2021_04-20\\inc\\dxcapi.h"
#include "C:\\SPIRV-Cross\\spirv_glsl.hpp"
#include <wrl/client.h>

#pragma comment( \
	lib,     \
	"C:\\Users\\James\\Downloads\\dxc_2021_04-20\\lib\\x64\\dxcompiler.lib")
#pragma comment(lib, "C:\\SPIRV-Cross\\build\\Debug\\spirv-cross-cored.lib")
#pragma comment(lib, "C:\\SPIRV-Cross\\build\\Debug\\spirv-cross-glsld.lib")

extern "C" {
#include "gl-shaderparser.h"
}

#include <sstream>
#include <string>
#include <vector>
#include <d3d11.h>

struct ShaderProcessor {
	static void BuildString(shader_parser &parser,
				std::string &outputString);
	static void Process(gl_shader_parser *glsp, const char *shader_string,
			    const char *file);
};

void ShaderProcessor::BuildString(shader_parser &parser,
				  std::string &outputString)
{
	std::stringstream output;
	output << "static const bool obs_glsl_compile = true;\n\n";

	cf_token *token = cf_preprocessor_get_tokens(&parser.cfp.pp);
	while (token->type != CFTOKEN_NONE) {
		/* cheaply just replace specific tokens */
		if (strref_cmp(&token->str, "POSITION") == 0)
			output << "SV_Position";
		else if (strref_cmp(&token->str, "TARGET") == 0)
			output << "SV_Target";
		else if (strref_cmp(&token->str, "texture2d") == 0)
			output << "Texture2D";
		else if (strref_cmp(&token->str, "texture3d") == 0)
			output << "Texture3D";
		else if (strref_cmp(&token->str, "texture_cube") == 0)
			output << "TextureCube";
		else if (strref_cmp(&token->str, "texture_rect") == 0)
			throw "texture_rect is not supported in D3D";
		else if (strref_cmp(&token->str, "sampler_state") == 0)
			output << "SamplerState";
		else if (strref_cmp(&token->str, "VERTEXID") == 0)
			output << "SV_VertexID";
		else
			output.write(token->str.array, token->str.len);

		token++;
	}

	outputString = std::move(output.str());
}

void ShaderProcessor::Process(gl_shader_parser *glsp, const char *shader_string,
			      const char *file)
{
	if (!gl_shader_parse(glsp, shader_string, file))
		throw "Failed to parse shader";
}

static const char *transpile_hlsl_to_glsl(gl_shader_parser *glsp, const char *s,
					  LPCWSTR profile, bool vertex)
{
	ShaderProcessor processor;
	std::string outputString;
	processor.Process(glsp, s, "PS.hlsl");
	processor.BuildString(glsp->parser, outputString);

	Microsoft::WRL::ComPtr<IDxcUtils> utils;
	HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
	//if(FAILED(hr)) Handle error...

	Microsoft::WRL::ComPtr<IDxcCompiler> compiler;
	hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
	//if(FAILED(hr)) Handle error...

	Microsoft::WRL::ComPtr<IDxcBlobEncoding> sourceBlob;
	hr = utils->CreateBlob(outputString.c_str(),
			       (UINT32)outputString.length() + 1, CP_UTF8,
			       &sourceBlob);
	//if(FAILED(hr)) Handle file loading error...

	LPCWSTR arguments[] = {L"-spirv", L"-Zi"};

	Microsoft::WRL::ComPtr<IDxcOperationResult> result;
	hr = compiler->Compile(sourceBlob.Get(), L"PS.hlsl", L"main", profile,
			       arguments, _countof(arguments), NULL, 0, NULL,
			       &result);
	if (SUCCEEDED(hr))
		result->GetStatus(&hr);
	if (FAILED(hr)) {
		if (result) {
			Microsoft::WRL::ComPtr<IDxcBlobEncoding> errorsBlob;
			if (SUCCEEDED(result->GetErrorBuffer(&errorsBlob)) &&
			    errorsBlob) {
				blog(LOG_DEBUG, "JP!: %s",
				     errorsBlob->GetBufferPointer());
			}
		}
	}

	Microsoft::WRL::ComPtr<IDxcBlob> code;
	result->GetResult(&code);

	uint32_t *buf = (uint32_t *)code->GetBufferPointer();
	size_t words = code->GetBufferSize() >> 2;

	spirv_cross::CompilerGLSL glsl(buf, words);

	// Set some options.
	spirv_cross::CompilerGLSL::Options options;
	options.version = 330;
	options.es = false;
	options.enable_420pack_extension = false;
	glsl.set_common_options(options);

	// Compile to GLSL, ready to give to GL driver.
	glsl.build_dummy_sampler_for_combined_images();
	glsl.build_combined_image_samplers();
	for (const spirv_cross::CombinedImageSampler &remap :
	     glsl.get_combined_image_samplers()) {
		glsl.set_name(remap.combined_id, glsl.get_name(remap.image_id));
	}

	char buffer[32];
	int index;
	if (vertex) {
		index = 0;
		for (const spirv_cross::Resource &resource :
		     glsl.get_shader_resources().stage_inputs) {
			snprintf(buffer, sizeof(buffer), "_input_attrib%d",
				 index);
			++index;
			glsl.set_name(resource.id, buffer);
		}

		index = 0;
		for (const spirv_cross::Resource &resource :
		     glsl.get_shader_resources().stage_outputs) {
			snprintf(buffer, sizeof(buffer), "_vertex_attrib%d",
				 index);
			++index;
			glsl.set_name(resource.id, buffer);
		}

		for (const spirv_cross::Resource &resource :
		     glsl.get_shader_resources().uniform_buffers) {
			if (strcmp(resource.name.c_str(), "type.$Globals") ==
			    0) {
				glsl.set_name(resource.base_type_id,
					      "type.$Globals_VS");
			}
		}
	} else {
		index = 0;
		for (const spirv_cross::Resource &resource :
		     glsl.get_shader_resources().stage_inputs) {
			snprintf(buffer, sizeof(buffer), "_vertex_attrib%d",
				 index);
			++index;
			glsl.set_name(resource.id, buffer);
		}

		for (const spirv_cross::Resource &resource :
		     glsl.get_shader_resources().uniform_buffers) {
			if (strcmp(resource.name.c_str(), "type.$Globals") ==
			    0) {
				glsl.set_name(resource.base_type_id,
					      "type.$Globals_PS");
			}
		}
	}

	char *shader = NULL;
	try {
		std::string source = glsl.compile();
		const size_t length = source.size() + 1;
		shader = (char *)bmalloc(length);
		memcpy(shader, source.c_str(), length);
	} catch (spirv_cross::CompilerError &e) {
		blog(LOG_DEBUG, "JP!: %s", e.what());
	}

	return shader;
}

extern "C" const char *
transpile_hlsl_to_glsl_vertex(struct gl_shader_parser *parser, const char *s)
{
	return transpile_hlsl_to_glsl(parser, s, L"vs_4_0", true);
}

extern "C" const char *
transpile_hlsl_to_glsl_pixel(struct gl_shader_parser *parser, const char *s)
{
	return transpile_hlsl_to_glsl(parser, s, L"ps_4_0", false);
}

extern "C" void free_glsl_string(const char *s)
{
	bfree((void *)s);
}
