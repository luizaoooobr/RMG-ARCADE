// Headless "lib" frontend for embedding FBNeo as a core library.
//
// The burner/burn layer expects a handful of application-level globals and
// callbacks that a normal frontend (win32/sdl/qt) provides from its window /
// menu / config code. When FBNeo is embedded (frontend=lib) there is no such
// application, so this translation unit supplies safe defaults. The host
// (RMG-K's FbneoRuntime) overrides the ones that matter at runtime.
//
// It also provides no-op video/audio/input/CD plugins so the shared
// intf/*_interface.cpp plugin tables have a valid (non-empty) entry when
// built with -DBUILD_LIB.

#include "burner.h"
#include "burnint.h"

// ---------------------------------------------------------------------------
// Application search paths (normally set up by support_paths.cpp)
// ---------------------------------------------------------------------------
TCHAR szAppHiscorePath[MAX_PATH] = _T("");
TCHAR szAppSamplesPath[MAX_PATH] = _T("");
TCHAR szAppHDDPath[MAX_PATH] = _T("");
TCHAR szAppBlendPath[MAX_PATH] = _T("");
TCHAR szAppEEPROMPath[MAX_PATH] = _T("");
TCHAR szAppSnesMsu1Path[MAX_PATH] = _T("");

TCHAR szAppListsPath[MAX_PATH] = _T("");
TCHAR szAppDatListsPath[MAX_PATH] = _T("");
TCHAR szAppArchivesPath[MAX_PATH] = _T("");

char szAppRomPaths[DIRS_MAX][MAX_PATH] = { { 0 } };

// ---------------------------------------------------------------------------
// ROM-data / driver-name tables (normally set up by romdata.cpp)
// ---------------------------------------------------------------------------
RomDataInfo* pRDI = NULL;
BurnRomInfo* pDataRomDesc = NULL;

// NOTE: pszCustomNameA / szBackupNameA / szBackupNameW / szShortNamesExArray /
// szLongNamesExArray / nNamesExArray are deliberately NOT defined here. burn.h
// declares them, and the actual definitions already live in src/burn/burn.cpp,
// which this frontend links against. Defining them here as well makes the link
// fail with "duplicate symbol" (verified by linking libfbneo_core.a).

// ---------------------------------------------------------------------------
// Driver state flags (normally owned by the frontend's drv.cpp / run.cpp)
// ---------------------------------------------------------------------------
int bDrvOkay = 0;
INT32 bRunPause = 0;

bool AppProcessKeyboardInput()
{
	return true;
}

// ---------------------------------------------------------------------------
// Replay/movie globals referenced by the burner savestate code
// (normally defined in the frontend's replay.cpp)
// ---------------------------------------------------------------------------
INT32 nReplayStatus = 0;
INT32 nReplayUndoCount = 0;
UINT32 nStartFrame = 0;

// ---------------------------------------------------------------------------
// IPS patch state (normally defined in ips_manager.cpp). The host does not use
// IPS patches, so IpsApplyPatches() is a no-op.
// ---------------------------------------------------------------------------
UINT32 nIpsDrvDefine = 0;
UINT32 nIpsMemExpLen[SND2_ROM + 1] = { 0 };
bool bDoIpsPatch = false;

void IpsApplyPatches(UINT8* /*base*/, char* /*rom_name*/, UINT32 /*rom_crc*/, bool /*readonly*/)
{
}

// ---------------------------------------------------------------------------
// String helpers normally provided by the frontend (char builds only here, so
// TCHAR == char and these are plain copies).
// ---------------------------------------------------------------------------
TCHAR* ANSIToTCHAR(const char* pszInString, TCHAR* pszOutString, int nOutSize)
{
	if (pszOutString) {
		strncpy(pszOutString, pszInString, nOutSize);
		pszOutString[nOutSize - 1] = '\0';
		return pszOutString;
	}
	return (TCHAR*)pszInString;
}

char* TCHARToANSI(const TCHAR* pszInString, char* pszOutString, int nOutSize)
{
	if (pszOutString) {
		strncpy(pszOutString, pszInString, nOutSize);
		pszOutString[nOutSize - 1] = '\0';
		return pszOutString;
	}
	return (char*)pszInString;
}

// ---------------------------------------------------------------------------
// NVRAM/EEPROM persistence toggle referenced by statec.cpp.
// ---------------------------------------------------------------------------
bool bWithEEPROM = false;

// ---------------------------------------------------------------------------
// Declared in burnint.h ("from intf/input/inp_interface.cpp") and referenced by
// burn.cpp's BurnSetMouseDivider(). That file is part of the interface layer,
// which the lib frontend does not compile, so the storage lives here. The value
// (1) matches inp_interface.cpp's initialiser; the host does not use the mouse.
// ---------------------------------------------------------------------------
INT32 nInputIntfMouseDivider = 1;

// ---------------------------------------------------------------------------
// Screen/reinit hooks. Several drivers (cps_run.cpp, megadrive.cpp, cps3run.cpp,
// d_segas32.cpp, d_backfire.cpp) call these when the game changes resolution
// mid-run. The other frontends re-read BurnDrvGetFullSize() and re-point
// pBurnDraw; here the host owns the framebuffer via FbneoRuntime, so all we do
// is drop pBurnDraw, which makes the driver re-emit the full frame into whatever
// buffer the host installs next.
// ---------------------------------------------------------------------------
void Reinitialise(void)
{
	pBurnDraw = NULL;
}

void ReinitialiseVideo(void)
{
	pBurnDraw = NULL;
}

// ---------------------------------------------------------------------------
// Recording / netgame helper (normally in scrn.cpp)
// ---------------------------------------------------------------------------
// Latched by the host before driver init (declared in burner_rmgk.h). Two things
// are seeded from is_netgame_or_recording(): the NeoGeo uPD4990A RTC and
// BurnRandom. With 0 the RTC reads the machine's local wall clock
// (BurnGetLocalTime -> time(NULL)), so two rollback peers boot with different
// state and the GekkoNet desync check fires from frame 0. When nonzero the time
// is pinned (2018-06-01) and BurnRandom is reseeded, exactly like FinalBurn
// Neo's own libretro build. 0 keeps real local time for single-player.
int nRMGKNetgameMode = 0;

INT32 is_netgame_or_recording()
{
	return nRMGKNetgameMode;
}

// ---------------------------------------------------------------------------
// Video plugin: does nothing. The host drives BurnDrvFrame() directly and
// manages its own framebuffer, so none of these entry points are ever called.
// ---------------------------------------------------------------------------
static INT32 VidNull_Init() { return 0; }
static INT32 VidNull_Exit() { return 0; }
static INT32 VidNull_Frame(bool /*bRedraw*/) { return 0; }
static INT32 VidNull_Paint(INT32 /*bValidate*/) { return 0; }
static INT32 VidNull_ImageSize(RECT* pRect, INT32 /*nGameWidth*/, INT32 /*nGameHeight*/)
{
	if (pRect) {
		pRect->left = 0;
		pRect->top = 0;
		pRect->right = 0;
		pRect->bottom = 0;
	}
	return 0;
}
static INT32 VidNull_GetPluginSettings(InterfaceInfo* pInfo)
{
	if (pInfo) {
		pInfo->pszModuleName = _T("lib (null video)");
	}
	return 1;
}

struct VidOut VidOutNull = {
	VidNull_Init,
	VidNull_Exit,
	VidNull_Frame,
	VidNull_Paint,
	VidNull_ImageSize,
	VidNull_GetPluginSettings,
	_T("lib (null video)"),
};

// ---------------------------------------------------------------------------
// Audio plugin: no-op. The host pulls samples from pBurnSoundOut directly.
// ---------------------------------------------------------------------------
static INT32 AudNull_BlankSound() { return 0; }
static INT32 AudNull_SoundCheck() { return 0; }
static INT32 AudNull_SoundInit() { return 0; }
static INT32 AudNull_SetCallback(INT32 (* /*pCallback*/)(INT32)) { return 0; }
static INT32 AudNull_SoundPlay() { return 0; }
static INT32 AudNull_SoundStop() { return 0; }
static INT32 AudNull_SoundExit() { return 0; }
static INT32 AudNull_SoundSetVolume() { return 0; }
static INT32 AudNull_GetPluginSettings(InterfaceInfo* pInfo)
{
	if (pInfo) {
		pInfo->pszModuleName = _T("lib (null audio)");
	}
	return 1;
}

struct AudOut AudOutNull = {
	AudNull_BlankSound,
	AudNull_SoundCheck,
	AudNull_SoundInit,
	AudNull_SetCallback,
	AudNull_SoundPlay,
	AudNull_SoundStop,
	AudNull_SoundExit,
	AudNull_SoundSetVolume,
	AudNull_GetPluginSettings,
	_T("lib (null audio)"),
};

// ---------------------------------------------------------------------------
// Input plugin: no-op. The host writes directly into the driver's input
// pointers, so no input device is required.
// ---------------------------------------------------------------------------
static INT32 InpNull_Init() { return 0; }
static INT32 InpNull_Exit() { return 0; }
static INT32 InpNull_SetCooperativeLevel(bool /*bExclusive*/, bool /*bForeground*/) { return 0; }
static INT32 InpNull_NewFrame() { return 0; }
static INT32 InpNull_ReadSwitch(INT32 /*nCode*/) { return 0; }
static INT32 InpNull_ReadJoyAxis(INT32 /*i*/, INT32 /*nAxis*/) { return 0; }
static INT32 InpNull_ReadMouseAxis(INT32 /*i*/, INT32 /*nAxis*/) { return 0; }
static INT32 InpNull_Find(bool /*CreateBaseline*/) { return 0; }
static INT32 InpNull_GetControlName(INT32 /*nCode*/, TCHAR* pszDeviceName, TCHAR* pszControlName)
{
	if (pszDeviceName) pszDeviceName[0] = _T('\0');
	if (pszControlName) pszControlName[0] = _T('\0');
	return 0;
}
static INT32 InpNull_GetPluginSettings(InterfaceInfo* pInfo)
{
	if (pInfo) {
		pInfo->pszModuleName = _T("lib (null input)");
	}
	return 1;
}

struct InputInOut InputInOutNull = {
	InpNull_Init,
	InpNull_Exit,
	InpNull_SetCooperativeLevel,
	InpNull_NewFrame,
	InpNull_ReadSwitch,
	InpNull_ReadJoyAxis,
	InpNull_ReadMouseAxis,
	InpNull_Find,
	InpNull_GetControlName,
	InpNull_GetPluginSettings,
	_T("lib (null input)"),
};

// ---------------------------------------------------------------------------
// CD emulation plugin: no-op. CD-based drivers are not used by the host.
// ---------------------------------------------------------------------------
static INT32 CDNull_Exit() { return 0; }
static INT32 CDNull_Init() { return 0; }
static INT32 CDNull_Stop() { return 0; }
static INT32 CDNull_Play(UINT8 /*M*/, UINT8 /*S*/, UINT8 /*F*/) { return 0; }
static INT32 CDNull_LoadSector(INT32 /*LBA*/, char* /*pBuffer*/) { return 0; }
static INT32 CDNull_ReadDataSector(INT32 /*nLba*/, UINT8* /*pBuffer*/) { return 0; }
static UINT8* CDNull_ReadTOC(INT32 /*track*/) { return NULL; }
static UINT8* CDNull_ReadQChannel() { return NULL; }
static INT32 CDNull_SetVolume(double /*dVolume*/) { return 0; }
static INT32 CDNull_GetCurrentLBA() { return 0; }
static INT32 CDNull_GetSoundBuffer(INT16* /*buffer*/, INT32 /*samples*/) { return 0; }
static INT32 CDNull_Scan(INT32 /*nAction*/, INT32* /*pnMin*/) { return 0; }
static INT32 CDNull_GetPluginSettings(InterfaceInfo* pInfo)
{
	if (pInfo) {
		pInfo->pszModuleName = _T("lib (null cd)");
	}
	return 1;
}

struct CDEmuDo cdlibDo = {
	CDNull_Exit,
	CDNull_Init,
	CDNull_Stop,
	CDNull_Play,
	CDNull_LoadSector,
	CDNull_ReadDataSector,
	CDNull_ReadTOC,
	CDNull_ReadQChannel,
	CDNull_SetVolume,
	CDNull_GetCurrentLBA,
	CDNull_GetSoundBuffer,
	CDNull_Scan,
	CDNull_GetPluginSettings,
	_T("lib (null cd)"),
};
