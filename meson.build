project('picom', 'c', version: '10',
        default_options: ['c_std=c11', 'warning_level=1'])

cc = meson.get_compiler('c')

# use project version by default
version = 'v'+meson.project_version()

# use git describe if that's available
git = find_program('git', required: false)
if git.found()
	gitv = run_command('git', 'rev-parse', '--short=5', 'HEAD', check: false)
	if gitv.returncode() == 0
		version = 'vgit-'+gitv.stdout().strip()
	endif
endif

add_global_arguments('-DPICOM_VERSION="'+version+'"', language: 'c')

if get_option('buildtype') == 'release'
	add_global_arguments('-DNDEBUG', language: 'c')
endif

if get_option('modularize')
	if not cc.has_argument('-fmodules')
		error('option \'modularize\' requires clang')
	endif
	add_global_arguments(['-fmodules',
	                      '-fmodule-map-file='+
	                      meson.current_source_dir()+
	                      '/src/picom.modulemap'],
	                     language: 'c')
endif

add_global_arguments('-D_GNU_SOURCE', language: 'c')

if cc.has_header('stdc-predef.h')
	add_global_arguments('-DHAS_STDC_PREDEF_H', language: 'c')
endif

if get_option('warning_level') != '0'
  warns = [ 'cast-function-type', 'ignored-qualifiers', 'missing-parameter-type',
            'nonnull', 'shadow', 'no-type-limits', 'old-style-declaration', 'override-init',
            'sign-compare', 'type-limits', 'uninitialized', 'shift-negative-value',
            'unused-but-set-parameter', 'unused-parameter', 'implicit-fallthrough=2',
            'no-unknown-warning-option', 'no-missing-braces', 'conversion', 'empty-body', 'unused-function' ]
  foreach w : warns
          if cc.has_argument('-W'+w)
                  add_global_arguments('-W'+w, language: 'c')
          endif
  endforeach
endif

profiling = get_option('profiling')
if profiling
  args = ['-pg']
  profiling = cc.has_multi_arguments(args)
  if profiling
    add_project_arguments(args, language: 'c')
    add_project_link_arguments(args,  language: 'c')
  endif
endif

subdir('src')
subdir('man')

install_data('picom.desktop', install_dir: 'share/applications')
install_data('picom.desktop', install_dir: get_option('sysconfdir') / 'xdg' / 'autostart')