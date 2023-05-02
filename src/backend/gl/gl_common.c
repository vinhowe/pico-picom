// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "utils.h"

#include "backend/backend_common.h"
#include "backend/gl/gl_common.h"

GLuint gl_create_shader(GLenum shader_type, const char *shader_str) {
	log_trace("===\n%s\n===", shader_str);

	bool success = false;
	GLuint shader = glCreateShader(shader_type);
	if (!shader) {
		log_error("Failed to create shader with type %#x.", shader_type);
		goto end;
	}
	glShaderSource(shader, 1, &shader_str, NULL);
	glCompileShader(shader);

	// Get shader status
	{
		GLint status = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
		if (status == GL_FALSE) {
			GLint log_len = 0;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
			if (log_len) {
				char log[log_len + 1];
				glGetShaderInfoLog(shader, log_len, NULL, log);
				log_error("Failed to compile shader with type %d: %s",
				          shader_type, log);
			}
			goto end;
		}
	}

	success = true;

end:
	if (shader && !success) {
		glDeleteShader(shader);
		shader = 0;
	}
	gl_check_err();

	return shader;
}

GLuint gl_create_program(const GLuint *const shaders, int nshaders) {
	bool success = false;
	GLuint program = glCreateProgram();
	if (!program) {
		log_error("Failed to create program.");
		goto end;
	}

	for (int i = 0; i < nshaders; ++i) {
		glAttachShader(program, shaders[i]);
	}
	glLinkProgram(program);

	// Get program status
	{
		GLint status = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (status == GL_FALSE) {
			GLint log_len = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
			if (log_len) {
				char log[log_len + 1];
				glGetProgramInfoLog(program, log_len, NULL, log);
				log_error("Failed to link program: %s", log);
			}
			goto end;
		}
	}
	success = true;

end:
	if (program) {
		for (int i = 0; i < nshaders; ++i) {
			glDetachShader(program, shaders[i]);
		}
	}
	if (program && !success) {
		glDeleteProgram(program);
		program = 0;
	}
	gl_check_err();

	return program;
}

/**
 * @brief Create a program from NULL-terminated arrays of vertex and fragment shader
 * strings.
 */
GLuint gl_create_program_from_strv(const char **vert_shaders, const char **frag_shaders) {
	int vert_count, frag_count;
	for (vert_count = 0; vert_shaders && vert_shaders[vert_count]; ++vert_count) {
	}
	for (frag_count = 0; frag_shaders && frag_shaders[frag_count]; ++frag_count) {
	}

	GLuint prog = 0;
	auto shaders = (GLuint *)ccalloc(vert_count + frag_count, GLuint);
	for (int i = 0; i < vert_count; ++i) {
		shaders[i] = gl_create_shader(GL_VERTEX_SHADER, vert_shaders[i]);
		if (shaders[i] == 0) {
			goto out;
		}
	}
	for (int i = 0; i < frag_count; ++i) {
		shaders[vert_count + i] =
		    gl_create_shader(GL_FRAGMENT_SHADER, frag_shaders[i]);
		if (shaders[vert_count + i] == 0) {
			goto out;
		}
	}

	prog = gl_create_program(shaders, vert_count + frag_count);

out:
	for (int i = 0; i < vert_count + frag_count; ++i) {
		if (shaders[i] != 0) {
			glDeleteShader(shaders[i]);
		}
	}
	free(shaders);
	gl_check_err();

	return prog;
}

/**
 * @brief Create a program from vertex and fragment shader strings.
 */
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str) {
	const char *vert_shaders[2] = {vert_shader_str, NULL};
	const char *frag_shaders[2] = {frag_shader_str, NULL};

	return gl_create_program_from_strv(vert_shaders, frag_shaders);
}

void gl_destroy_window_shader(backend_t *backend_data attr_unused, void *shader) {
	if (!shader) {
		return;
	}

	auto pprogram = (gl_win_shader_t *)shader;
	if (pprogram->prog) {
		glDeleteProgram(pprogram->prog);
		pprogram->prog = 0;
	}
	gl_check_err();

	free(shader);
}

/**
 * Render a region with texture data.
 *
 * @param ptex the texture
 * @param target the framebuffer to render into
 * @param dst_x,dst_y the top left corner of region where this texture
 *                    should go. In OpenGL coordinate system (important!).
 * @param reg_tgt     the clip region, in Xorg coordinate system
 * @param reg_visible ignored
 */
static void _gl_compose(backend_t *base, struct backend_image *img, GLuint target,
                        GLint *coord, GLuint *indices, int nrects) {
	// FIXME(yshui) breaks when `mask` and `img` doesn't have the same y_inverted
	//              value. but we don't ever hit this problem because all of our
	//              images and masks are y_inverted.
	auto gd = (struct gl_data *)base;
	auto inner = (struct gl_texture *)img->inner;

	auto win_shader = inner->shader;
	if (!win_shader) {
		win_shader = gd->default_shader;
	}

	// assert(win_shader);
	// assert(win_shader->prog);
	glUseProgram(win_shader->prog);

	// log_trace("Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d\n",
	//          x, y, width, height, dx, dy, ptex->width, ptex->height, z);

	// Bind texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, inner->texture);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * nrects * 16, coord, GL_STATIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6,
	             indices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	// Cleanup
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);

	glUseProgram(0);

	gl_check_err();

	return;
}

/// Convert rectangles in X coordinates to OpenGL vertex and texture coordinates
/// @param[in] nrects, rects   rectangles
/// @param[in] image_dst       origin of the OpenGL texture, affect the calculated texture
///                            coordinates
/// @param[in] extend_height   height of the drawing extent
/// @param[in] texture_height  height of the OpenGL texture
/// @param[in] root_height     height of the back buffer
/// @param[in] y_inverted      whether the texture is y inverted
/// @param[out] coord, indices output
void x_rect_to_coords(int nrects, const rect_t *rects, coord_t image_dst,
                      int extent_height, int texture_height, int root_height,
                      bool y_inverted, GLint *coord, GLuint *indices) {
	image_dst.y = root_height - image_dst.y;
	image_dst.y -= extent_height;

	for (int i = 0; i < nrects; i++) {
		// Y-flip. Note after this, crect.y1 > crect.y2
		rect_t crect = rects[i];
		crect.y1 = root_height - crect.y1;
		crect.y2 = root_height - crect.y2;

		// Calculate texture coordinates
		// (texture_x1, texture_y1), texture coord for the _bottom left_ corner
		GLint texture_x1 = crect.x1 - image_dst.x,
		      texture_y1 = crect.y2 - image_dst.y,
		      texture_x2 = texture_x1 + (crect.x2 - crect.x1),
		      texture_y2 = texture_y1 + (crect.y1 - crect.y2);

		// X pixmaps might be Y inverted, invert the texture coordinates
		if (y_inverted) {
			texture_y1 = texture_height - texture_y1;
			texture_y2 = texture_height - texture_y2;
		}

		// Vertex coordinates
		auto vx1 = crect.x1;
		auto vy1 = crect.y2;
		auto vx2 = crect.x2;
		auto vy2 = crect.y1;

		// log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d",
		//          ri, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);

		memcpy(&coord[i * 16],
		       ((GLint[][2]){
		           {vx1, vy1},
		           {texture_x1, texture_y1},
		           {vx2, vy1},
		           {texture_x2, texture_y1},
		           {vx2, vy2},
		           {texture_x2, texture_y2},
		           {vx1, vy2},
		           {texture_x1, texture_y2},
		       }),
		       sizeof(GLint[2]) * 8);

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6],
		       ((GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0}),
		       sizeof(GLuint) * 6);
	}
}

// TODO(yshui) make use of reg_visible
void gl_compose(backend_t *base, void *image_data, coord_t image_dst,
                const region_t *reg_tgt, const region_t *reg_visible attr_unused) {
	auto gd = (struct gl_data *)base;
	struct backend_image *img = image_data;
	auto inner = (struct gl_texture *)img->inner;

	// Painting
	int nrects;
	const rect_t *rects;
	rects = pixman_region32_rectangles((region_t *)reg_tgt, &nrects);
	if (!nrects) {
		// Nothing to paint
		return;
	}

	// Until we start to use glClipControl, reg_tgt, dst_x and dst_y and
	// in a different coordinate system than the one OpenGL uses.
	// OpenGL window coordinate (or NDC) has the origin at the lower left of the
	// screen, with y axis pointing up; Xorg has the origin at the upper left of the
	// screen, with y axis pointing down. We have to do some coordinate conversion in
	// this function

	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	x_rect_to_coords(nrects, rects, image_dst, inner->height, inner->height,
	                 gd->height, inner->y_inverted, coord, indices);
	_gl_compose(base, img, gd->back_fbo, coord, indices, nrects);

	free(indices);
	free(coord);
}

/**
 * Load a GLSL main program from shader strings.
 */
static bool gl_win_shader_from_stringv(const char **vshader_strv,
                                       const char **fshader_strv, gl_win_shader_t *ret) {
	// Build program
	ret->prog = gl_create_program_from_strv(vshader_strv, fshader_strv);
	if (!ret->prog) {
		log_error("Failed to create GLSL program.");
		gl_check_err();
		return false;
	}

	// Get uniform addresses
	bind_uniform(ret, tex);

	gl_check_err();

	return true;
}

/**
 * Callback to run on root window size change.
 */
void gl_resize(struct gl_data *gd, int width, int height) {
	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);

	gd->height = height;
	gd->width = width;

	assert(viewport_dimensions[0] >= gd->width);
	assert(viewport_dimensions[1] >= gd->height);

	glBindTexture(GL_TEXTURE_2D, gd->back_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, gd->back_format, width, height, 0, GL_BGR,
	             GL_UNSIGNED_BYTE, NULL);

	gl_check_err();
}

/// Fill a given region in bound framebuffer.
/// @param[in] y_inverted whether the y coordinates in `clip` should be inverted
static void _gl_fill(backend_t *base, struct color c, const region_t *clip, GLuint target,
                     int height, bool y_inverted) {
	static const GLuint fill_vert_in_coord_loc = 0;
	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)clip, &nrects);
	auto gd = (struct gl_data *)base;

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glUseProgram(gd->fill_shader.prog);
	glUniform4f(gd->fill_shader.color_loc, (GLfloat)c.red, (GLfloat)c.green,
	            (GLfloat)c.blue, (GLfloat)c.alpha);
	glEnableVertexAttribArray(fill_vert_in_coord_loc);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);

	auto coord = ccalloc(nrects * 8, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	for (int i = 0; i < nrects; i++) {
		GLint y1 = y_inverted ? height - rect[i].y2 : rect[i].y1,
		      y2 = y_inverted ? height - rect[i].y1 : rect[i].y2;
		// clang-format off
		memcpy(&coord[i * 8],
		       ((GLint[][2]){
		           {rect[i].x1, y1}, {rect[i].x2, y1},
		           {rect[i].x2, y2}, {rect[i].x1, y2}}),
		       sizeof(GLint[2]) * 4);
		// clang-format on
		indices[i * 6 + 0] = (GLuint)i * 4 + 0;
		indices[i * 6 + 1] = (GLuint)i * 4 + 1;
		indices[i * 6 + 2] = (GLuint)i * 4 + 2;
		indices[i * 6 + 3] = (GLuint)i * 4 + 2;
		indices[i * 6 + 4] = (GLuint)i * 4 + 3;
		indices[i * 6 + 5] = (GLuint)i * 4 + 0;
	}
	glBufferData(GL_ARRAY_BUFFER, nrects * 8 * (long)sizeof(*coord), coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, nrects * 6 * (long)sizeof(*indices),
	             indices, GL_STREAM_DRAW);

	glVertexAttribPointer(fill_vert_in_coord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(*coord) * 2, (void *)0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(fill_vert_in_coord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	glDeleteBuffers(2, bo);
	free(indices);
	free(coord);

	gl_check_err();
}

void gl_fill(backend_t *base, struct color c, const region_t *clip) {
	auto gd = (struct gl_data *)base;
	return _gl_fill(base, c, clip, gd->back_fbo, gd->height, true);
}

static void gl_release_image_inner(backend_t *base, struct gl_texture *inner) {
	auto gd = (struct gl_data *)base;
	if (inner->user_data) {
		gd->release_user_data(base, inner);
	}
	assert(inner->user_data == NULL);

	glDeleteTextures(1, &inner->texture);
	glDeleteTextures(2, inner->auxiliary_texture);
	free(inner);
	gl_check_err();
}

void gl_release_image(backend_t *base, void *image_data) {
	struct backend_image *wd = image_data;
	auto inner = (struct gl_texture *)wd->inner;
	inner->refcount--;
	assert(inner->refcount >= 0);
	if (inner->refcount == 0) {
		gl_release_image_inner(base, inner);
	}
	free(wd);
}

void *gl_create_window_shader(backend_t *backend_data attr_unused, const char *source) {
	auto win_shader = (gl_win_shader_t *)ccalloc(1, gl_win_shader_t);

	const char *vert_shaders[2] = {vertex_shader, NULL};
	const char *frag_shaders[4] = {win_shader_glsl, source, NULL};

	if (!gl_win_shader_from_stringv(vert_shaders, frag_shaders, win_shader)) {
		free(win_shader);
		return NULL;
	}

	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);

	// Set projection matrix to gl viewport dimensions so we can use screen
	// coordinates for all vertices
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0F / (GLfloat)viewport_dimensions[0], 0, 0, 0},
	                                   {0, 2.0F / (GLfloat)viewport_dimensions[1], 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	int pml = glGetUniformLocationChecked(win_shader->prog, "projection");
	glUseProgram(win_shader->prog);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);

	return win_shader;
}

uint64_t gl_get_shader_attributes(backend_t *backend_data attr_unused, void *shader) {
	auto win_shader = (gl_win_shader_t *)shader;
	uint64_t ret = 0;
	if (glGetUniformLocation(win_shader->prog, "time") >= 0) {
		ret |= SHADER_ATTRIBUTE_ANIMATED;
	}
	return ret;
}

bool gl_init(struct gl_data *gd, session_t *ps) {
	// Initialize GLX data structure
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	glEnable(GL_BLEND);
	// X pixmap is in premultiplied alpha, so we might just as well use it too.
	// Thanks to derhass for help.
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// Initialize stencil buffer
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0x1);
	glStencilFunc(GL_EQUAL, 0x1, 0x1);

	// Set gl viewport to the maximum supported size so we won't have to worry about
	// it later on when the screen is resized. The corresponding projection matrix can
	// be set now and won't have to be updated. Since fragments outside the target
	// buffer are skipped anyways, this should have no impact on performance.
	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);
	glViewport(0, 0, viewport_dimensions[0], viewport_dimensions[1]);

	// Clear screen
	glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	glGenFramebuffers(1, &gd->back_fbo);
	glGenTextures(1, &gd->back_texture);
	if (!gd->back_fbo || !gd->back_texture) {
		log_error("Failed to generate a framebuffer object");
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, gd->back_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Initialize shaders
	gd->default_shader = gl_create_window_shader(NULL, NULL);
	if (!gd->default_shader) {
		log_error("Failed to create window shaders");
		return false;
	}

	// Set projection matrix to gl viewport dimensions so we can use screen
	// coordinates for all vertices
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0F / (GLfloat)viewport_dimensions[0], 0, 0, 0},
	                                   {0, 2.0F / (GLfloat)viewport_dimensions[1], 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	gd->fill_shader.prog = gl_create_program_from_str(fill_vert, fill_frag);
	gd->fill_shader.color_loc = glGetUniformLocation(gd->fill_shader.prog, "color");
	int pml = glGetUniformLocationChecked(gd->fill_shader.prog, "projection");
	glUseProgram(gd->fill_shader.prog);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);

	gd->present_prog =
	    gl_create_program_from_strv((const char *[]){present_vertex_shader, NULL},
	                                (const char *[]){dummy_frag, NULL});
	if (!gd->present_prog) {
		log_error("Failed to create the present shader");
		return false;
	}
	pml = glGetUniformLocationChecked(gd->present_prog, "projection");
	glUseProgram(gd->present_prog);
	glUniform1i(glGetUniformLocationChecked(gd->present_prog, "tex"), 0);
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);
	glUseProgram(0);

	// Set up the size and format of the back texture
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->back_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	const GLint *format = (const GLint[]){GL_RGB8, GL_RGBA8};
	for (int i = 0; i < 2; i++) {
		gd->back_format = format[i];
		gl_resize(gd, ps->root_width, ps->root_height);

		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, gd->back_texture, 0);
		if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
			log_info("Using back buffer format %#x", gd->back_format);
			break;
		}
	}
	if (!gl_check_fb_complete(GL_DRAW_FRAMEBUFFER)) {
		return false;
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	gd->logger = gl_string_marker_logger_new();
	if (gd->logger) {
		log_add_target_tls(gd->logger);
	}

	const char *vendor = (const char *)glGetString(GL_VENDOR);
	log_debug("GL_VENDOR = %s", vendor);
	if (strcmp(vendor, "NVIDIA Corporation") == 0) {
		log_info("GL vendor is NVIDIA, don't use glFinish");
		gd->is_nvidia = true;
	} else {
		gd->is_nvidia = false;
	}
	gd->has_robustness = gl_has_extension("GL_ARB_robustness");
	gl_check_err();

	return true;
}

void gl_deinit(struct gl_data *gd) {
	if (gd->logger) {
		log_remove_target_tls(gd->logger);
		gd->logger = NULL;
	}

	if (gd->default_shader) {
		gl_destroy_window_shader(&gd->base, gd->default_shader);
		gd->default_shader = NULL;
	}

	gl_check_err();
}

GLuint gl_new_texture(GLenum target) {
	GLuint texture;
	glGenTextures(1, &texture);
	if (!texture) {
		log_error("Failed to generate texture");
		return 0;
	}

	glBindTexture(target, texture);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(target, 0);

	return texture;
}

void gl_present(backend_t *base, const region_t *region) {
	auto gd = (struct gl_data *)base;

	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)region, &nrects);
	auto coord = ccalloc(nrects * 8, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	for (int i = 0; i < nrects; i++) {
		// clang-format off
		memcpy(&coord[i * 8],
		       ((GLint[]){rect[i].x1, gd->height - rect[i].y2,
		                 rect[i].x2, gd->height - rect[i].y2,
		                 rect[i].x2, gd->height - rect[i].y1,
		                 rect[i].x1, gd->height - rect[i].y1}),
		       sizeof(GLint) * 8);
		// clang-format on

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6],
		       ((GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0}),
		       sizeof(GLuint) * 6);
	}

	glUseProgram(gd->present_prog);
	glBindTexture(GL_TEXTURE_2D, gd->back_texture);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glEnableVertexAttribArray(vert_coord_loc);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(GLint) * nrects * 8, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(GLuint) * nrects * 6, indices,
	             GL_STREAM_DRAW);

	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 2, NULL);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glDeleteBuffers(2, bo);
	glDeleteVertexArrays(1, &vao);

	free(coord);
	free(indices);
}

bool gl_set_image_property(backend_t *base attr_unused, enum image_properties op,
                           void *image_data, void *arg) {
	struct backend_image *tex = image_data;
	int *iargs = arg;
	bool *bargs = arg;
	double *dargs = arg;
	switch (op) {
	case IMAGE_PROPERTY_EFFECTIVE_SIZE:
		// texture is already set to repeat, so nothing else we need to do
		tex->ewidth = iargs[0];
		tex->eheight = iargs[1];
		break;
	}

	return true;
}

enum device_status gl_device_status(backend_t *base) {
	auto gd = (struct gl_data *)base;
	if (!gd->has_robustness) {
		return DEVICE_STATUS_NORMAL;
	}
	if (glGetGraphicsResetStatusARB() == GL_NO_ERROR) {
		return DEVICE_STATUS_NORMAL;
	}
	return DEVICE_STATUS_RESETTING;
}
