project(
    'fbneo',
    ['c', 'cpp'],
    version: '1.0.0.3',
    default_options: ['c_std=c99', 'cpp_std=c++11', 'b_staticpic=true'],
)

perl = find_program('perl', required: true)
cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

if cc.get_id() in ['gcc', 'clang']
    add_project_arguments('-D_GNU_SOURCE', language: 'c')
endif
if cpp.get_id() in ['gcc', 'clang']
    add_project_arguments('-D_GNU_SOURCE', language: 'cpp')
endif

if cpp.get_id() in ['gcc', 'clang']
    add_project_arguments('-Wno-write-strings', language: 'cpp')
elif cpp.get_id() == 'msvc'
    add_project_arguments('/Zc:strictStrings-', language: 'cpp')
endif

include_7z = get_option('include_7z')
frontend = get_option('frontend')
if frontend == 'auto'
    if host_machine.system() == 'windows'
        frontend = 'win32'
    elif host_machine.system() == 'darwin'
        frontend = 'macos'
    else
        frontend = 'sdl2'
    endif
endif

message('FBNeo frontend: ' + frontend)

# frontend -> source subdir (used for the subdir dispatch below)
frontend_dirs = {
    'win32': 'win32',
    'sdl2': 'sdl',
    'qt': 'qt',
    'macos': 'macos',
    'pi': 'pi',
    'psp': 'psp',
    'lib': 'rmgk',
}

# Build-selection macro for the active frontend, defined once here and threaded
# through the burn static library (and its dependents) via burn_defs.
frontend_defs = ['-DBUILD_' + frontend.to_upper()]
if frontend == 'win32'
    frontend_defs += ['-DUNICODE', '-D_UNICODE']
endif

# Warning flags shared by the cpu and burn_drv static libraries (identical set).
common_cpp_warning_args = []
if cpp.get_id() in ['gcc', 'clang']
    common_cpp_warning_args = [
        '-Wno-sequence-point',
        '-Wno-sign-compare',
        # FBNeo writes `INT32 a[N] = { (double)expr, ... }` (e.g. d_namcos86.cpp's
        # nCyclesTotal). GCC/MSVC accept the implicit narrowing; clang reports it
        # as -Wc++11-narrowing, a separate diagnostic group from -Wnarrowing, so
        # it must be disabled explicitly to build the drivers/cores with clang.
        '-Wno-c++11-narrowing',
        '-Wno-unused-but-set-variable',
        '-Wno-unused-parameter',
        '-Wno-unused-value',
        '-Wno-unused-variable',
    ]
endif

# Platform math macro reused by cpu / snd / burn_drv on Windows.
platform_math_defs = host_machine.system() == 'windows' ? ['-D_USE_MATH_DEFINES'] : []

# Cross-cutting header directories made globally visible to every internal
# library and the final executable. Code includes these bare (e.g. "z80_intf.h",
# "msm6295.h", "eeprom.h", "burnint.h"), so exposing the directories globally
# removes the per-library include juggling and the reverse-include hacks.
# (Verified: no duplicate header basenames across this set.) cpu core and driver
# group subdirectories stay local to their own libraries on purpose.
global_inc = include_directories(
    'src/burn',
    'src/burn/devices',
    'src/burn/snd',
    'src/cpu',
    'src/burner',
    'src/burner' / frontend_dirs[frontend],
    'src/intf',
    'src/intf/audio',
    'src/intf/video',
    'src/intf/input',
    'src/intf/cd',
    'src/intf/perfcount',
)
force_pulse_audio = get_option('force_pulse_audio')
rom_verify = get_option('rom_verify')
fastcall_support = get_option('fastcall_support')

if fastcall_support
    fastcall_test = []
    warn_as_error_args = []

    if cc.get_id() in ['gcc', 'clang']
        fastcall_test = '''
        int __attribute__((fastcall)) test_fastcall(int a) { return a; }
        int main(void) { return test_fastcall(0); }
        '''

        warn_as_error_args = '-Werror'
    elif cc.get_id() == 'msvc'
        fastcall_test = '''
        int __fastcall test_fastcall(int a) { return a; }
        int main(void) { return test_fastcall(0); }
        '''

        warn_as_error_args = ['/WX']
    endif

    if not cc.compiles(
        fastcall_test,
        name: '__fastcall keyword (C)',
        args: warn_as_error_args,
    )
        add_project_arguments('-D__fastcall=', language: 'c')
    else
        add_project_arguments('-DFASTCALL', language: 'c')
    endif

    if not cpp.compiles(
        fastcall_test,
        name: '__fastcall keyword (C++)',
        args: warn_as_error_args,
    )
        add_project_arguments('-D__fastcall=', language: 'cpp')
    else
        add_project_arguments('-DFASTCALL', language: 'cpp')
    endif
endif

burn_driver_list = get_option('driver_list')

fs = import('fs')

# driver_list selects what to build at driver granularity. Each
# entry is one of:
#   capcom                 -> the whole capcom group
#   capcom/d_cps1.cpp      -> just that driver
#   -capcom                -> exclude the whole capcom group
#   -capcom/d_cps1.cpp     -> exclude that driver
# First pass: positives -> full_groups (whole group) or partial (specific
# drivers); negatives -> exclusion sets. (Kept as separate lists rather than a
# mixed-type value because meson forbids comparing an array with a string.)
full_groups = []                  # groups selected in full
partial = {}                      # group -> [specific driver files]
group_exclude = []
driver_exclude = []
foreach entry : burn_driver_list
    if entry.startswith('-')
        item = entry.substring(1)
        if item.contains('/')
            driver_exclude += item
        else
            group_exclude += item
        endif
    elif entry.contains('/')
        parts = entry.split('/')
        g = parts[0]
        if not full_groups.contains(g)
            partial += {g: partial.get(g, []) + [parts[1]]}
        endif
    elif not full_groups.contains(entry)
        full_groups += entry      # whole group wins over specific drivers
    endif
endforeach

# Unique list of selected groups (full first, then partial-only).
selected_groups = full_groups
foreach g, _drv : partial
    if not selected_groups.contains(g)
        selected_groups += g
    endif
endforeach

# Collect each selected group's self-describing fragment. Each
# src/burn/drv/<group>/meson.build sets (data only, no targets):
#   group_desc / group_srcs (shared support) / drivers ({file: [components]})
enabled_components = []
burn_drivers = {}                 # group -> { 'desc', 'drivers', 'srcs' }
# d_parent.cpp lives outside the group dirs and is always compiled (see
# src/burn/drv/meson.build); feed it to gamelist.pl too, otherwise its parent
# drivers are missing from driverlist.h and their clones show up orphaned.
driver_files_abs = [meson.current_source_dir() / 'src/burn/drv/d_parent.cpp']
seen_drivers = []                 # for exclusion validation
foreach g : selected_groups
    assert(
        fs.is_dir('src/burn/drv' / g),
        'Unknown driver group in driver_list: ' + g,
    )
    if group_exclude.contains(g)
        continue
    endif
    subdir('src/burn/drv' / g)
    chosen = full_groups.contains(g) ? drivers.keys() : partial[g]
    enabled_drivers = []
    foreach d : chosen
        assert(
            drivers.has_key(d),
            'Unknown driver "' + d + '" in group ' + g,
        )
        seen_drivers += g / d
        if not driver_exclude.contains(g / d)
            enabled_drivers += d
            driver_files_abs += meson.current_source_dir() / 'src/burn/drv' / g / d
            foreach c : drivers[d]
                if not enabled_components.contains(c)
                    enabled_components += c
                endif
            endforeach
        endif
    endforeach
    # group_srcs only compiles when at least one driver in the group is built.
    if enabled_drivers.length() > 0
        burn_drivers += {
            g: {'desc': group_desc, 'drivers': enabled_drivers, 'srcs': group_srcs},
        }
    endif
endforeach

foreach ge : group_exclude
    assert(fs.is_dir('src/burn/drv' / ge), 'Unknown group in exclusion: -' + ge)
endforeach
foreach de : driver_exclude
    assert(seen_drivers.contains(de), 'Unknown driver in exclusion: -' + de)
endforeach

# win32 always needs the 68000 core (matches original build)
if frontend == 'win32' and not enabled_components.contains('m68000')
    enabled_components += 'm68000'
endif

subdir('src/burn')
subdir('src/cpu')
subdir('src/burn/devices')
subdir('src/burn/snd')

# Every component named in a group_use must resolve to a known cpu/device/sound.
known_components = all_cpu.keys() + all_burn_devices.keys() + all_burn_sounds.keys()
foreach c : enabled_components
    assert(known_components.contains(c), 'Unknown component in a group_use: ' + c)
endforeach

subdir('src/burn/drv')

subdir('src/dep/libs/zlib')
subdir('src/dep/libs/lua')
subdir('src/dep/libs/libchdr')
if include_7z
    subdir('src/dep/libs/lib7z')
endif

# The interactive burner/interface layer is only needed by the standalone
# frontends. The embedded "lib" frontend deliberately does not link it: the
# host app (RMG-K) provides archive loading, video/audio/input and config, and
# only ../statec.cpp (savestate buffer compression) is pulled in for rollback.
#
# NOTE: this is no longer literally true — see the "frontend == 'lib'" block at
# the bottom, which also links burner_lib, because burn/snd/drv really do call
# into it (ZipLoadOneFile from samples.cpp, CDEmu* from neo_run.cpp/pce,
# LowPass2 from several audio drivers, Reinitialise from cps/megadrive/...).
# What is intentionally NOT compiled is nothing extra: burner_lib is the whole
# layer, and only the *interactive* sections of it are unused at runtime.
if frontend != 'lib'
    subdir('src/burner')
endif

fbneo_sources = []
build_details_exe = executable(
    'build_details',
    'src/dep/scripts/build_details.cpp',
    cpp_args: ['-D_UNICODE']
    + (host_machine.cpu_family() == 'x86_64' ? ['-DBUILD_X64_EXE'] : []),
    native: true,
)
build_details_h = custom_target(
    'build_details_h',
    output: 'build_details.h',
    command: [build_details_exe],
    capture: true,
)
fbneo_sources += build_details_h

subdir('src/burner' / frontend_dirs[frontend])

# Each frontend fragment must export these (implicit contract).
assert(is_variable('frontend_sources'), 'frontend must define frontend_sources')
assert(is_variable('frontend_dep'), 'frontend must define frontend_dep')
assert(is_variable('frontend_link_args'), 'frontend must define frontend_link_args')

fbneo_sources += frontend_sources

defs = []
extra_deps = []
if include_7z
    fbneo_sources += ['src/burner/un7z.cpp']
    defs += ['-DINCLUDE_7Z_SUPPORT']
    extra_deps += lib7z_dep
endif
if force_pulse_audio
    defs += ['-DFORCE_PULSE_AUDIO']
endif
if rom_verify
    defs += ['-DROM_VERIFY']
endif
fbneo_includes = include_directories(
    [
        'src/intf',
        'src/intf/video',
        'src/intf/video/scalers',
        'src/intf/audio',
        'src/intf/input',
        'src/intf/cd',
        'src/intf/perfcount',
    ],
)

deps = [
    dependency('threads'),
]
if force_pulse_audio
    deps += [dependency('pulse-simple')]
endif

c_warning_args = []
cpp_warning_args = []
if cc.get_id() in ['gcc', 'clang']
    c_warning_args = [
        '-Wno-long-long',
        '-Wno-sign-compare',
        '-Wno-uninitialized',
        '-Wno-unused',
        '-Wno-conversion',
        '-Wno-attributes',
        '-Wno-unused-parameter',
        '-Wno-unused-value',
    ]
endif
if cpp.get_id() in ['gcc', 'clang']
    cpp_warning_args = [
        '-Wconversion',
        '-Winline',
        '-Wno-attributes',
        '-Wno-conversion',
        '-Wno-long-long',
        '-Wno-missing-braces',
        '-Wno-multichar',
        '-Wno-narrowing',
        # FBNeo writes `INT32 a[N] = { (double)expr, ... }` (e.g. d_namcos86.cpp's
        # nCyclesTotal). GCC/MSVC accept the implicit narrowing; clang reports it
        # as -Wc++11-narrowing, a different diagnostic group from -Wnarrowing, so
        # it has to be disabled separately to build with clang.
        '-Wno-c++11-narrowing',
        '-Wno-sign-compare',
        '-Wno-unused-parameter',
        '-Wno-unused-value',
        '-Wno-write-strings',
        '-Wpointer-arith',
        '-Wundef',
        '-Wuninitialized',
        '-Wunknown-pragmas',
    ]
endif

link_args = []
if host_machine.system() != 'windows'
    link_args += ['-lm']
endif
link_args += frontend_link_args

core_deps = [
    cpu_dep,
    burn_dep,
    burn_devices_dep,
    burn_snd_dep,
    burn_drv_dep,
    frontend_dep,
]

if frontend == 'lib'
    # The embedded frontend cannot skip the burner/interface layer after all:
    # the layer below it calls into it directly. Concretely:
    #   - samples.cpp          -> ZipLoadOneFile()   (src/burner/zipfn.cpp)
    #   - neo_run.cpp / pce    -> CDEmu*             (src/intf/cd/cd_interface.cpp)
    #   - d_mcr3/d_snk6502/... -> LowPass2           (src/intf/audio/lowpass2.cpp)
    #   - cps_run/megadrive/.. -> Reinitialise()     (src/burner/scrn.cpp family)
    # burner_lib carries those, so it has to be built and linked here too.
    # Only its interactive parts (gami/conc/dat/state/...) end up unused at
    # runtime; they are still compiled, which also keeps the file list in sync
    # with the standalone frontends.
    subdir('src/burner')
    core_deps += burner_dep
endif

if frontend == 'lib'
    # Embedded/headless frontend: emit a self-contained STATIC library that
    # exposes the burn API (BurnLibInit/BurnDrvInit/BurnDrvFrame/...) so a host
    # application (e.g. RMG-K) can link against it. Every enabled driver is
    # retained because burn.cpp's pDriver[] table references each driver symbol.
    # The host frontend supplies the application callbacks
    # (bprintf/BurnHighCol/BurnExtLoadRom/...) plus the burner-layer pieces it
    # needs (savestate compression, archive loading) itself, so the heavy
    # interactive burner/interface layer is deliberately not linked in.
    # A static archive is used because the host links it PRIVATEly into its own
    # library (RMG-Core.dll/so), which also removes any symbol-export concerns.
    # NOTE 1: extra_deps MUST be included — it carries lib7z_dep when 7z support
    # is on, which supplies both the 7z.h include dir and the lib7z archive that
    # src/burner/un7z.cpp needs. Without it the lib frontend fails to find 7z.h.
    # NOTE 2: link_whole is required. A static_library only archives its OWN
    # sources, so the internal cpu/burn/drv/snd/devices static libs that
    # core_deps merely points at would never land in libfbneo_core.a — the
    # archive would end up holding nothing but frontend.cpp/statec.cpp/un7z.cpp.
    # link_whole forces every object of each internal lib into the archive, so
    # the host can link this single file.
    core_whole_libs = [burn_lib, burn_drv_lib, burn_snd_lib, burn_devices_lib, cpu_lib]
    core_whole_libs += burner_lib
    foreach opt_lib : ['zlib_lib', 'lua_lib', 'libchdr_lib', 'lib7z_lib']
        if is_variable(opt_lib)
            core_whole_libs += get_variable(opt_lib)
        endif
    endforeach

    static_library(
        'fbneo_core',
        fbneo_sources,
        include_directories: [fbneo_includes, global_inc],
        dependencies: deps + core_deps + extra_deps,
        link_whole: core_whole_libs,
        c_args: defs + c_warning_args,
        cpp_args: defs + cpp_warning_args,
        install: false,
    )
else
    executable(
        'fbneo',
        fbneo_sources,
        include_directories: [fbneo_includes, global_inc],
        dependencies: deps + core_deps + [burner_dep] + extra_deps,
        c_args: defs + c_warning_args,
        cpp_args: defs + cpp_warning_args,
        link_args: link_args,
        win_subsystem: frontend == 'win32' ? 'windows' : 'console',
    )
endif
