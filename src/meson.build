libev = dependency('libev', required: false)
if not libev.found()
	libev = cc.find_library('ev')
endif
base_deps = [
	cc.find_library('m'),
	libev
]

srcs = [ files('picom.c', 'win.c', 'x.c', 'config.c', 'vsync.c',
               'render.c', 'log.c', 'options.c', 'event.c', 'cache.c',
			   'atom.c') ]
picom_inc = include_directories('.')

cflags = []

required_xcb_packages = [
	'xcb-render', 'xcb-damage', 'xcb-randr', 'xcb-sync', 'xcb-composite',
	'xcb-shape', 'xcb-xfixes', 'xcb-present', 'xcb-glx', 'xcb-dpms', 'xcb'
]

required_packages = [
	'x11', 'x11-xcb', 'xcb-renderutil', 'xcb-image', 'xext', 'pixman-1'
]

foreach i : required_packages
	base_deps += [dependency(i, required: true)]
endforeach

foreach i : required_xcb_packages
	base_deps += [dependency(i, version: '>=1.12.0', required: true)]
endforeach

if not cc.has_header('uthash.h')
  error('Dependency uthash not found')
endif

deps = []

cflags += ['-DCONFIG_OPENGL', '-DGL_GLEXT_PROTOTYPES']
deps += [dependency('gl', required: true)]
srcs += [ 'opengl.c' ]

host_system = host_machine.system()
if host_system == 'inux'
	cflags += ['-DHAS_INOTIFY']
elif (host_system == 'freebsd' or host_system == 'netbsd' or
      host_system == 'dragonfly' or host_system == 'openbsd')
	cflags += ['-DHAS_KQUEUE']
endif

subdir('backend')

picom = executable('picom', srcs, c_args: cflags,
  dependencies: [ base_deps, deps ],
  install: true, include_directories: picom_inc)