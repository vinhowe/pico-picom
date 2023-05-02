#include "gl_common.h"

// clang-format off
const char dummy_frag[] = GLSL(330,
	uniform sampler2D tex;
	in vec2 texcoord;
	void main() {
		gl_FragColor = texelFetch(tex, ivec2(texcoord.xy), 0);
	}
);

const char fill_frag[] = GLSL(330,
	uniform vec4 color;
	void main() {
		gl_FragColor = color;
	}
);

const char fill_vert[] = GLSL(330,
	layout(location = 0) in vec2 in_coord;
	uniform mat4 projection;
	void main() {
		gl_Position = projection * vec4(in_coord, 0, 1);
	}
);

const char win_shader_glsl[] = GLSL(330,
	in vec2 texcoord;
	uniform sampler2D tex;

	void main() {
		gl_FragColor = texelFetch(tex, ivec2(texcoord), 0);
	}
);

const char present_vertex_shader[] = GLSL(330,
	uniform mat4 projection;
	layout(location = 0) in vec2 coord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(coord, 0, 1);
		texcoord = coord;
	}
);

const char vertex_shader[] = GLSL(330,
	uniform mat4 projection;
	uniform float scale = 1.0;
	uniform vec2 texorig;
	layout(location = 0) in vec2 coord;
	layout(location = 1) in vec2 in_texcoord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(coord, 0, scale);
		texcoord = in_texcoord + texorig;
	}
);
// clang-format on
