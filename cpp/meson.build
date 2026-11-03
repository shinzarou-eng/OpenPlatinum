project('openplatinum', 'cpp',
    version: '0.1',
    default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=1'])

imgui_dir = 'thirdparty/imgui-1.91.9b'

imgui_inc = include_directories(
    imgui_dir,
    imgui_dir / 'backends')

deps = []
link_args = []
cpp_args = []

sys = host_machine.system()

if sys == 'windows'
    # SDL3 MinGW pré-téléchargé dans cpp/tools/ (non versionné, voir README)
    sdl_root = 'tools/SDL3-3.4.18/x86_64-w64-mingw32'
    sdl_libdir = meson.current_source_dir() / sdl_root / 'lib'
    deps += declare_dependency(
        link_args: ['-L' + sdl_libdir, '-lSDL3'],
        include_directories: include_directories(sdl_root / 'include'))
    link_args += ['-lopengl32', '-lwinmm', '-limm32', '-lversion', '-lsetupapi']
elif sys == 'darwin'
    # brew install sdl3 — OpenGL.framework est déprécié mais encore dispo
    deps += dependency('sdl3')
    deps += dependency('appleframeworks', modules: ['OpenGL'])
    cpp_args += ['-D_stricmp=strcasecmp']   # shim CRT Windows -> POSIX
else
    # Linux/BSD : sdl3 + libGL via pkg-config
    deps += dependency('sdl3')
    deps += dependency('gl')
endif

imgui_src = files(
    imgui_dir / 'imgui.cpp',
    imgui_dir / 'imgui_draw.cpp',
    imgui_dir / 'imgui_tables.cpp',
    imgui_dir / 'imgui_widgets.cpp',
    imgui_dir / 'backends/imgui_impl_sdl3.cpp',
    imgui_dir / 'backends/imgui_impl_opengl3.cpp')

src = files(
    'src/banim.cpp',
    'src/battle.cpp',
    'src/bw_sprites.cpp',
    'src/main.cpp',
    'src/nds.cpp',
    'src/bmd.cpp',
    'src/land.cpp',
    'src/msg.cpp',
    'src/render.cpp',
    'src/script.cpp',
    'src/sdat.cpp',
    'src/spr.cpp',
    'src/trainer_kit.cpp',
    'src/world.cpp',
    'src/world_collision.cpp',
    'src/world_dw.cpp',
    'src/world_intro.cpp',
    'src/world_move.cpp',
    'src/world_render.cpp',
    'src/world_script.cpp',
    'src/world_tick.cpp',
    'src/world_warp.cpp')

executable('OpenPlatinum', src + imgui_src,
    dependencies: deps,
    include_directories: imgui_inc,
    link_args: link_args,
    cpp_args: cpp_args,
    win_subsystem: 'console',
    install: false)
