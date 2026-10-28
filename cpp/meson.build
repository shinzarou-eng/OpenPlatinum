project('openplatinum', 'cpp',
    version: '0.1',
    default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=1'])

imgui_dir = 'thirdparty/imgui-1.91.9b'
sdl_root = 'tools/SDL3-3.4.18/x86_64-w64-mingw32'

sdl_inc = include_directories(
    sdl_root / 'include',
    imgui_dir,
    imgui_dir / 'backends')

sdl_libdir = meson.current_source_dir() / sdl_root / 'lib'
sdl_dep = declare_dependency(
    link_args: ['-L' + sdl_libdir, '-lSDL3'],
    include_directories: sdl_inc)

imgui_src = files(
    imgui_dir / 'imgui.cpp',
    imgui_dir / 'imgui_draw.cpp',
    imgui_dir / 'imgui_tables.cpp',
    imgui_dir / 'imgui_widgets.cpp',
    imgui_dir / 'backends/imgui_impl_sdl3.cpp',
    imgui_dir / 'backends/imgui_impl_opengl3.cpp')

src = files(
    'src/battle.cpp',
    'src/main.cpp',
    'src/nds.cpp',
    'src/bmd.cpp',
    'src/land.cpp',
    'src/msg.cpp',
    'src/render.cpp',
    'src/script.cpp',
    'src/spr.cpp',
    'src/world.cpp',
    'src/world_collision.cpp',
    'src/world_intro.cpp',
    'src/world_move.cpp',
    'src/world_render.cpp',
    'src/world_script.cpp',
    'src/world_tick.cpp',
    'src/world_warp.cpp')

win_deps = declare_dependency(
    link_args: ['-lopengl32', '-lwinmm', '-limm32', '-lversion', '-lsetupapi'])

executable('OpenPlatinum', src + imgui_src,
    dependencies: [sdl_dep, win_deps],
    win_subsystem: 'console',
    install: false)
