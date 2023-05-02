# enable xrender
srcs += [ files('backend_common.c', 'dummy/dummy.c', 'backend.c', 'driver.c') ]

# enable opengl
if get_option('opengl')
  srcs += [ files('gl/gl_common.c', 'gl/glx.c', 'gl/shaders.c') ]
endif
