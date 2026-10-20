# Headless "lib" frontend used when FBNeo is embedded as a core library
# (frontend=lib), e.g. by RMG-K's FbneoRuntime.
#
# The interactive burner/interface layer is intentionally not linked in; this
# frontend only supplies:
#   - the application callbacks the burn layer expects
#     (bprintf / BurnHighCol / BurnExtLoadRom / progress / resize / ...),
#   - the savestate-from-buffer implementation (../statec.cpp), which the host
#     drives through BurnStateCompress()/BurnStateDecompress() for rollback,
#   - plus the two pieces of the interface layer that drivers link against
#     directly: the LowPass2 audio filter (src/intf/audio/lowpass2.cpp, used by
#     d_mcr3.cpp/d_snk6502.cpp/d_suprnova.cpp) and the screen-reinit hooks
#     (Reinitialise/ReinitialiseVideo, provided as stubs in frontend.cpp).
frontend_sources = files(
    'frontend.cpp',
    '../statec.cpp',
    # relative to src/burner/rmgk/, this resolves to src/intf/audio/lowpass2.cpp
    '../../intf/audio/lowpass2.cpp',
)

frontend_dep = declare_dependency()
frontend_link_args = []
