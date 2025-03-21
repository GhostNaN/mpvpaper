project('mpvpaper', ['c'])
cc = meson.get_compiler('c')

dl_dep = cc.find_library('dl', required : false)
wl_protocols=dependency('wayland-protocols')
wl_client=dependency('wayland-client')
wl_egl=dependency('wayland-egl')
egl=dependency('egl')
mpv=dependency('mpv')
threads=dependency('threads')

scanner=find_program('wayland-scanner')
scanner_private_code=generator(scanner,output: '@BASENAME@-protocol.c',arguments: ['private-code','@INPUT@','@OUTPUT@'])
scanner_client_header=generator(scanner,output: '@BASENAME@-client-protocol.h',arguments: ['client-header','@INPUT@','@OUTPUT@'])

protocols_src=[
  scanner_private_code.process('proto/wlr-layer-shell-unstable-v1.xml'),
  scanner_private_code.process(wl_protocols.get_variable(pkgconfig: 'pkgdatadir')+'/stable/xdg-shell/xdg-shell.xml')
]

protocols_headers=[
  scanner_client_header.process('proto/wlr-layer-shell-unstable-v1.xml')
]

lib_protocols=static_library('protocols',protocols_src+protocols_headers,dependencies: wl_client)
protocols_dep=declare_dependency(link_with: lib_protocols,sources: protocols_headers)

executable(meson.project_name(), ['src/main.c', 'src/glad.c', 'src/cflogprinter.c'],
include_directories : ['inc'],
dependencies: [dl_dep, wl_client, wl_egl, egl, mpv, threads, protocols_dep], install: true)

shm_dep = cc.find_library('rt', required : false)
executable(meson.project_name() + '-holder', ['src/holder.c'],
include_directories : ['inc'],
dependencies: [dl_dep, wl_client, shm_dep, protocols_dep], install: true)
