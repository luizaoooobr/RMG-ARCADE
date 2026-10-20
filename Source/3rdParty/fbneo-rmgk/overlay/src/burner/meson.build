fbneo_burner_sources = [
    'conc.cpp',
    'cong.cpp',
    'dat.cpp',
    'gamc.cpp',
    'gami.cpp',
    'image.cpp',
    'inputbuf.cpp',
    'ioapi.c',
    'misc.cpp',
    'sshot.cpp',
    'state.cpp',
    'statec.cpp',
    'unzip.c',
    'zipfn.cpp',
    '../dep/libs/libspng/spng.c',
    '../dep/libs/zip/zip.c',
    '../dep/libs/lzma/libchdr_lzma.c',
    '../dep/libs/zstd/zstddeclib.c',
    '../intf/audio/aud_dsp.cpp',
    '../intf/audio/aud_interface.cpp',
    '../intf/audio/lowpass2.cpp',
    '../intf/cd/cd_interface.cpp',
    '../intf/cd/cd_chd.cpp',
    '../intf/input/inp_interface.cpp',
    '../intf/interface.cpp',
    '../intf/video/vid_interface.cpp',
    '../intf/video/vid_support.cpp',
]

# The interactive files (conc/cong/dat/gamc/gami/image/inputbuf/misc/sshot/state)
# depend on the full application frontend surface — szAppBurnVer, TCHARToANSI,
# ANSIToTCHAR, bAlwaysProcessKeyboardInput, InpDIPSWResetDIPs, ... — which only
# a real UI provides. The embedded "lib" frontend has none of that, so it builds
# just the subset that the layers below actually link against:
#
#   archive     : zip.c, zipfn.cpp                         (ZipLoadOneFile)
#                 unz*/ioapi* come from the system minizip the host already
#                 links, so unzip.c/ioapi.c are left out (avoid duplicate syms)
#   CD          : cd_interface.cpp, cd_chd.cpp            (CDEmu*)
#   audio       : aud_dsp.cpp, aud_interface.cpp, lowpass2.cpp
#   input       : (none; nInputIntfMouseDivider is provided by rmgk/frontend.cpp,
#                 so intf/input/inp_interface.cpp is left out - it also needs the
#                 interactive globals nGameInpCount/nMacroCount/GameInp that the
#                 lib frontend does not define)
#   video       : vid_interface.cpp, vid_support.cpp       (BurnTransfer/pVidImage)
#   plumbing    : interface.cpp                            (pVidOut/... tables)
#   savestate   : statec.cpp                               (BurnState{Un,}Compress)
#   third-party : spng.c, libchdr_lzma.c, zstddeclib.c     (needed by cd_chd)
fbneo_burner_lib_sources = [
    'zipfn.cpp',
    '../dep/libs/libspng/spng.c',
    '../dep/libs/zip/zip.c',
    '../dep/libs/lzma/libchdr_lzma.c',
    '../dep/libs/zstd/zstddeclib.c',
    '../intf/audio/aud_dsp.cpp',
    '../intf/audio/aud_interface.cpp',
    '../intf/audio/lowpass2.cpp',
    '../intf/cd/cd_interface.cpp',
    '../intf/cd/cd_chd.cpp',
    '../intf/interface.cpp',
    '../intf/video/vid_interface.cpp',
    '../intf/video/vid_support.cpp',
    'statec.cpp',
]

# intf/* headers come from the global include set; only third-party dep
# include dirs need listing here.
burner_lib_inc = [
    '../dep/libs/dr_libs',
    '../dep/libs/libspng',
    '../dep/libs/lzma',
    '../dep/libs/lzma/include',
    '../dep/libs/zip',
    '../dep/libs/zstd',
]

burner_lib_deps = [
    burn_dep,
    burn_devices_dep,
    burn_snd_dep,
    burn_drv_dep,
    cpu_dep,
    zlib_dep,
    libchdr_dep,
]
if include_7z
    burner_lib_deps += lib7z_dep
endif

if frontend == 'win32'
    burner_lib_inc += '../dep/kaillera/client'
    if cpp.get_id() == 'msvc'
        burner_lib_inc += '../dep/vc/include'
    else
        burner_lib_inc += '../dep/mingw/include'
    endif
    burner_lib_deps += lua_dep
elif frontend == 'sdl2'
    burner_lib_deps += dependency('sdl2')
endif

burner_lib_defs = ['-DCHDR_SYSTEM_ZLIB']
if include_7z
    burner_lib_defs += ['-DINCLUDE_7Z_SUPPORT']
endif

burner_lib = static_library(
    'burner',
    frontend == 'lib' ? fbneo_burner_lib_sources : fbneo_burner_sources,
    include_directories: [include_directories(burner_lib_inc), global_inc],
    dependencies: burner_lib_deps,
    c_args: burner_lib_defs,
    cpp_args: burner_lib_defs,
)

burner_dep = declare_dependency(
    link_with: burner_lib,
    include_directories: include_directories('.'),
    dependencies: burner_lib_deps,
)
