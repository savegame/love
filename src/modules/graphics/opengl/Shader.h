/**
 * Copyright (c) 2006-2020 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#pragma once

// LOVE
#include "graphics/Shader.h"
#include "graphics/Graphics.h"
#include "graphics/Volatile.h"
#include "OpenGL.h"

// STL
#include <string>
#include <map>
#include <vector>

namespace love
{
namespace graphics
{
namespace opengl
{

// A GLSL shader
class Shader final : public love::graphics::Shader, public Volatile
{
public:

	/**
	 * Creates a new Shader using a list of source codes.
	 * Source must contain either vertex or pixel shader code, or both.
	 **/
	Shader(love::graphics::ShaderStage *vertex, love::graphics::ShaderStage *pixel);
	virtual ~Shader();

	// Implements Volatile
	bool loadVolatile() override;
	void unloadVolatile() override;

	// Implements Shader.
	void attach() override;
	std::string getWarnings() const override;
	int getVertexAttributeIndex(const std::string &name) override;
	const UniformInfo *getUniformInfo(const std::string &name) const override;
	const UniformInfo *getUniformInfo(BuiltinUniform builtin) const override;
	void updateUniform(const UniformInfo *info, int count) override;
	void sendTextures(const UniformInfo *info, love::graphics::Texture **textures, int count) override;
	void sendBuffers(const UniformInfo *info, love::graphics::Buffer **buffers, int count) override;
	bool hasUniform(const std::string &name) const override;
	ptrdiff_t getHandle() const override;
	void setVideoTextures(love::graphics::Texture *ytexture, love::graphics::Texture *cbtexture, love::graphics::Texture *crtexture) override;

	void updatePointSize(float size);
	void updateBuiltinUniforms(love::graphics::Graphics *gfx, int viewportW, int viewportH);

private:

	struct TextureUnit
	{
		GLuint texture = 0;
		TextureType type = TEXTURE_2D;
		bool isTexelBuffer = false;
		bool active = false;
	};

	// Map active uniform names to their locations.
	void mapActiveUniforms();

	void updateUniform(const UniformInfo *info, int count, bool internalupdate);
	void sendTextures(const UniformInfo *info, love::graphics::Texture **textures, int count, bool internalupdate);
	void sendBuffers(const UniformInfo *info, love::graphics::Buffer **buffers, int count, bool internalupdate);

	int getUniformTypeComponents(GLenum type) const;
	MatrixSize getMatrixSize(GLenum type) const;
	UniformType getUniformBaseType(GLenum type) const;
	TextureType getUniformTextureType(GLenum type) const;
	DataBaseType getUniformTexelBufferType(GLenum type) const;
	bool isDepthTextureType(GLenum type) const;

	void flushBatchedDraws() const;

	// Get any warnings or errors generated only by the shader program object.
	std::string getProgramWarnings() const;

	// volatile
	GLuint program;

	// Location values for any built-in uniform variables.
	GLint builtinUniforms[BUILTIN_MAX_ENUM];
	UniformInfo *builtinUniformInfo[BUILTIN_MAX_ENUM];

	std::map<std::string, GLint> attributes;

	// Uniform location buffer map
	std::map<std::string, UniformInfo> uniforms;

	// Texture unit pool for setting textures
	std::vector<TextureUnit> textureUnits;

	std::vector<std::pair<const UniformInfo *, int>> pendingUniformUpdates;

	std::vector<Buffer *> buffersToUnmap;

	float lastPointSize;

}; // Shader

} // opengl
} // graphics
} // love
