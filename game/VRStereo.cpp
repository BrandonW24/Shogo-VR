// ======================================================================= //
//
// MODULE  : VRStereo.cpp
//
// PURPOSE : Side-by-side stereo rendering for Shogo (VR mod, stage 1)
//
// ======================================================================= //

#include "VRStereo.h"
#include "ClientWeaponUtils.h"
#include "RiotCommandIDs.h"
#include "ModelFuncs.h"
#include "CharacterAlignment.h"
#include "WeaponDefs.h"
#include "VRCapture.h"
#include "BitmapFont.h"
#include "TextHelper.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

CVRStereo g_VRStereo;

#define VR_PI				3.141592653589793f
#define VR_DEG2RAD(x)		((x) * VR_PI / 180.0f)
#define VR_RAD2DEG(x)		((x) * 180.0f / VR_PI)
#define VR_MIN_FOV			0.0315f		// engine clamps to PI/100
#define VR_MAX_FOV			3.1100f		// engine clamps to 99*PI/100
#define VR_AIM_RANGE		10000.0f	// how far we look for something under the crosshair
#define VR_AIM_SMOOTHING	12.0f		// higher = crosshair depth follows faster
#define VR_FLOOR_RANGE		4000.0f		// how far VRCalibrate looks for the floor

static int VRRound(DFLOAT f)
{
	return (int)floor(f + 0.5f);
}

static DFLOAT VRMinF(DFLOAT a, DFLOAT b) { return (a < b) ? a : b; }

// Clips one axis of a blit to [c0, c1), moving the source edges by the same
// proportion so a clipped scaled blit still lines up.
static DBOOL VRClip1D(int &d0, int &d1, int &s0, int &s1, int c0, int c1)
{
	if (d1 <= d0 || s1 <= s0) return DFALSE;

	int nDstLen = d1 - d0;
	int nSrcLen = s1 - s0;

	if (d0 < c0)
	{
		s0 += (int)(((DFLOAT)(c0 - d0) * (DFLOAT)nSrcLen) / (DFLOAT)nDstLen);
		d0 = c0;
	}
	if (d1 > c1)
	{
		s1 -= (int)(((DFLOAT)(d1 - c1) * (DFLOAT)nSrcLen) / (DFLOAT)nDstLen);
		d1 = c1;
	}

	return (d1 > d0 && s1 > s0);
}


// ======================================================================= //
//	Construction / setup
// ======================================================================= //

CVRStereo::CVRStereo()
{
	m_pClientDE			= DNULL;
	m_bHooked			= DFALSE;
	m_bCrosshairPass	= DFALSE;
	m_bLastImmersive	= DFALSE;
	m_nCurEye			= 0;
	m_hScreen			= DNULL;
	m_nMode				= VR_STEREO_OFF;
	m_nScreenW			= 640;
	m_nScreenH			= 480;
	m_fLastBaseFovX		= VR_DEG2RAD(90.0f);
	m_fFocalBase		= 1.0f;
	m_fFocalRender		= 1.0f;
	m_fIpdWorld			= 0.0f;
	m_fEyeFovX			= 0.0f;
	m_fEyeFovY			= 0.0f;
	m_fAimInvDepth		= 0.0f;
	m_fBaseTanX			= 1.0f;
	m_fBaseTanY			= 1.0f;

	m_hMapping				= DNULL;
	m_pShared				= DNULL;
	m_bBridgeAlive			= DFALSE;
	m_nLastBridgeBeat		= 0;
	m_fLastBridgeBeatTime	= -100.0f;
	m_nBridgeFlags			= 0;
	m_nPoseId				= 0;
	VEC_SET(m_vHeadF, 0.0f, 0.0f, 1.0f);
	VEC_SET(m_vHeadU, 0.0f, 1.0f, 0.0f);
	VEC_SET(m_vHeadP, 0.0f, 0.0f, 0.0f);
	m_fHeadYaw				= 0.0f;
	m_fHeadPitch			= 0.0f;
	m_fNeedTanX				= 0.0f;
	m_fNeedTanY				= 0.0f;
	m_bHaveLastHeadYaw		= DFALSE;
	m_fLastHeadYaw			= 0.0f;
	m_fBodyYaw				= 0.0f;
	m_bHeadApplied			= DFALSE;
	m_nGameHwnd				= 0;
	m_nGameBeat				= 0;
	m_nReportPoseId			= 0;
	m_nReportViewMode		= SHOGOVR_VIEW_OFF;
	m_fReportTanX			= 1.0f;
	m_fReportTanY			= 1.0f;
	m_fReportSep			= 0.0f;
	m_nRenderW = m_nRenderH	= 0;
	m_nClientW = m_nClientH	= 0;
	m_nSurfW = m_nSurfH		= 0;
	m_fRendererBadSince		= -1.0f;
	m_fLastRendererReset	= -1000.0f;
	m_nRendererResets		= 0;
	m_fGunScaleUsed			= 1.0f;
	m_szIniPath[0]			= 0;
	m_nIniTimeLo = m_nIniTimeHi = 0;
	m_fLastIniCheck			= -100.0f;

	m_pfnIsCommandOn		= DNULL;
	m_nCtrlFlags			= 0;
	m_nButtons				= 0;
	m_nPrevButtons			= 0;
	m_nPressed				= 0;
	m_nReleased				= 0;
	m_fMoveX = m_fMoveY		= 0.0f;
	m_fTurnX = m_fTurnY		= 0.0f;
	VEC_SET(m_vRightF, 0.0f, 0.0f, 1.0f);	VEC_SET(m_vRightU, 0.0f, 1.0f, 0.0f);	VEC_INIT(m_vRightP);
	VEC_SET(m_vLeftF, 0.0f, 0.0f, 1.0f);	VEC_SET(m_vLeftU, 0.0f, 1.0f, 0.0f);	VEC_INIT(m_vLeftP);
	m_bGameplayInput		= DFALSE;
	m_nSuppressed			= 0;
	m_bAimByHand			= DFALSE;
	m_bHaveBody				= DFALSE;
	m_fLastOutYaw			= 0.0f;
	m_bSnapLatched			= DFALSE;
	m_nWeaponLatch			= 0;
	m_nMenuDir				= 0;
	m_fMenuRepeatTime		= 0.0f;
	m_fWorldScaleUsed		= 40.0f;
	m_hAimMarker			= DNULL;
	m_bTwoHanded			= DFALSE;
	VEC_SET(m_vAimF, 0.0f, 0.0f, 1.0f);
	VEC_SET(m_vAimU, 0.0f, 1.0f, 0.0f);
	m_hBody					= DNULL;
	m_nBodyModelId			= -1;
	VEC_INIT(m_vBodyLastPos);
	VEC_INIT(m_vBodyVel);
	m_fBodyMaxSpeed			= 1.0f;
	m_fBodyVisYaw			= 0.0f;
	m_bBodyVisYawSet		= DFALSE;
	m_szBodyAnim[0]			= 0;
	m_nBodyHidden			= 0;
	m_bBodyArmsShown		= DFALSE;
	m_hHeldGun				= DNULL;
	m_nHeldGunId			= -1;
	m_hHiddenPV				= DNULL;
	m_hArmlessPV			= DNULL;
	m_nDumpedWeapons[0] = m_nDumpedWeapons[1] = 0;
	m_bCaptureHook			= DFALSE;
	m_bMech					= DFALSE;
	m_bVehicle				= DFALSE;
	m_fLoggedZoom			= 1.0f;
	m_fLoggedCamFovX = m_fLoggedCamFovY = 0.0f;
	m_nLoggedRenderW = m_nLoggedRenderH = m_nLoggedSurfW = m_nLoggedSurfH = m_nLoggedClientW = m_nLoggedClientH = 0;
	m_bLoggedBridge			= DFALSE;
	m_nLoggedHudStrip		= -1;
	m_nLoggedCapture		= 0;
	m_nCrouchDownTick		= 0;
	m_bCrouchLong			= DFALSE;
	m_bTransformKeyDown		= DFALSE;
	m_nTransformUpTick		= 0;
	m_nTransformScan		= 0;
	m_bTransformExt			= DFALSE;
	m_bWarnedNoTransformKey	= DFALSE;
	m_bStartedBridge		= DFALSE;
	m_szAuthors[0]			= 0;
	m_fNoticeStart			= -1.0f;
	m_hNotice[0] = m_hNotice[1] = DNULL;
	m_bNoticeDone			= DFALSE;

	m_pfnDrawSurfaceToSurface				= DNULL;
	m_pfnDrawSurfaceToSurfaceTransparent	= DNULL;
	m_pfnDrawSurfaceSolidColor				= DNULL;
	m_pfnScaleSurfaceToSurface				= DNULL;
	m_pfnScaleSurfaceToSurfaceTransparent	= DNULL;
	m_pfnScaleSurfaceToSurfaceSolidColor	= DNULL;
	m_pfnFillRect							= DNULL;
	m_pfnDrawStringToSurface				= DNULL;
	m_pfnGetFrameTime						= DNULL;
	m_pfnGetSurfaceDims						= DNULL;
	m_pfnGetCameraRect						= DNULL;
	m_bVirtualHud							= DFALSE;
	m_bSkipNonCross							= DFALSE;
	m_bFlatFrame							= DFALSE;
	m_bHudStripAllowed						= DFALSE;
	m_bHudStrip								= DFALSE;
	m_nEyeH									= 480;
	m_nHudX = m_nHudY = m_nHudW = m_nHudH	= 0;
	m_nHudScale								= 1;
	m_fHudVirtW								= 512.0f;
	m_fHudVirtH								= 384.0f;
	m_fVirtW								= 640.0f;
	m_fVirtH								= 480.0f;

	memset(&m_xfHud, 0, sizeof(m_xfHud));
	memset(&m_xfCross, 0, sizeof(m_xfCross));
}

void CVRStereo::Init(CClientDE* pClientDE)
{
	if (!pClientDE) return;

	// Existing values (from autoexec.cfg or the command line) are kept;
	// these defaults are only used if the variable doesn't exist yet.
	m_vtStereo.Init			(pClientDE, "VRStereo",			NULL, 0.0f);
	m_vtIPD.Init			(pClientDE, "VRIPD",			NULL, 64.0f);
	m_vtFov.Init			(pClientDE, "VRFov",			NULL, 0.0f);
	m_vtScaleFoot.Init		(pClientDE, "VRScaleFoot",		NULL, 40.0f);
	m_vtScaleMCA.Init		(pClientDE, "VRScaleMCA",		NULL, 8.0f);
	m_vtEyeHeightFoot.Init	(pClientDE, "VREyeHeightFoot",	NULL, 1.7f);
	m_vtEyeHeightMCA.Init	(pClientDE, "VREyeHeightMCA",	NULL, 10.0f);
	m_vtHudDepth.Init		(pClientDE, "VRHudDepth",		NULL, 3.5f);
	m_vtHudScale.Init		(pClientDE, "VRHudScale",		NULL, 1.0f);
	m_vtHudWidth.Init		(pClientDE, "VRHudWidth",		NULL, 60.0f);
	m_vtHudLayout.Init		(pClientDE, "VRHudLayout",		NULL, 512.0f);
	m_vtMenuWidth.Init		(pClientDE, "VRMenuWidth",		NULL, 80.0f);
	m_vtHudStrip.Init		(pClientDE, "VRHudStrip",		NULL, -1.0f);
	m_vtHudPixels.Init		(pClientDE, "VRHudPixels",		NULL, 1.0f);
	m_vtHudSnap.Init		(pClientDE, "VRHudSnap",		NULL, 1.0f);
	m_vtFlatMenus.Init		(pClientDE, "VRFlatMenus",		NULL, 1.0f);
	m_vtCrossScale.Init		(pClientDE, "VRCrosshairScale",	NULL, 1.0f);
	m_vtCrossDepth.Init		(pClientDE, "VRCrosshairDepth",	NULL, 1.0f);
	m_vtCutscenePanel.Init	(pClientDE, "VRCutscenePanel",	NULL, 1.0f);
	m_vtCutsceneStereo.Init	(pClientDE, "VRCutsceneStereo",	NULL, 0.5f);
	m_vtSwapEyes.Init		(pClientDE, "VRSwapEyes",		NULL, 0.0f);
	m_vtAutoStereo.Init		(pClientDE, "VRAutoStereo",		NULL, 1.0f);
	m_vtHeadTracking.Init	(pClientDE, "VRHeadTracking",	NULL, 1.0f);
	m_vtHeadPosition.Init	(pClientDE, "VRHeadPosition",	NULL, 1.0f);
	m_vtControllers.Init	(pClientDE, "VRControllers",	NULL, 1.0f);
	m_vtAimMode.Init		(pClientDE, "VRAimMode",		NULL, 1.0f);
	m_vtSnapTurn.Init		(pClientDE, "VRSnapTurn",		NULL, 45.0f);
	m_vtTurnSpeed.Init		(pClientDE, "VRTurnSpeed",		NULL, 150.0f);
	m_vtGunX.Init			(pClientDE, "VRGunX",			NULL, 0.0f);
	m_vtGunY.Init			(pClientDE, "VRGunY",			NULL, 0.0f);
	m_vtGunZ.Init			(pClientDE, "VRGunZ",			NULL, 0.0f);
	m_vtAimMarker.Init		(pClientDE, "VRAimMarker",		NULL, 1.0f);
	m_vtGunScale.Init		(pClientDE, "VRGunScale",		NULL, 1.5f);
	m_vtGunScaleMCA.Init	(pClientDE, "VRGunScaleMCA",	NULL, 1.0f);
	m_vtFixRenderer.Init	(pClientDE, "VRFixRenderer",	NULL, 1.0f);
	m_vtMoveDir.Init		(pClientDE, "VRMoveDir",		NULL, 0.0f);
	m_vtTwoHand.Init		(pClientDE, "VRTwoHand",		NULL, 1.0f);
	m_vtBody.Init			(pClientDE, "VRBody",			NULL, 1.0f);
	m_vtBodyArms.Init		(pClientDE, "VRBodyArms",		NULL, 0.0f);
	m_vtBodyOffset.Init		(pClientDE, "VRBodyOffset",		NULL, 0.15f);
	m_vtGunModel.Init		(pClientDE, "VRGunModel",		NULL, 0.0f);
	m_vtGunArms.Init		(pClientDE, "VRGunArms",		NULL, 1.0f);
	m_vtEasyLadders.Init	(pClientDE, "VREasyLadders",	NULL, 1.0f);
	m_vtAutoBridge.Init		(pClientDE, "VRAutoBridge",		NULL, 1.0f);
	m_vtDirectCapture.Init	(pClientDE, "VRDirectCapture",	NULL, 1.0f);
	m_vtHeldPitch.Init		(pClientDE, "VRHeldPitch",		NULL, 0.0f);
	m_vtHeldYaw.Init		(pClientDE, "VRHeldYaw",		NULL, 0.0f);
	m_vtHeldRoll.Init		(pClientDE, "VRHeldRoll",		NULL, 0.0f);
	m_vtHeldX.Init			(pClientDE, "VRHeldX",			NULL, 0.0f);
	m_vtHeldY.Init			(pClientDE, "VRHeldY",			NULL, 0.0f);
	m_vtHeldZ.Init			(pClientDE, "VRHeldZ",			NULL, 0.0f);
	m_vtHeldScale.Init		(pClientDE, "VRHeldScale",		NULL, 1.0f);

	m_pClientDE = pClientDE;

	// Window handle for the bridge, so it knows what to capture.
	void* pHwnd = DNULL;
	pClientDE->GetEngineHook("HWND", &pHwnd);
	m_nGameHwnd = (DDWORD)pHwnd;

	OpenShared();

	// Controller buttons become game commands (fire, jump, crouch) by
	// answering "yes" when the game asks the engine whether they're on.
	m_pfnIsCommandOn = pClientDE->IsCommandOn;
	pClientDE->IsCommandOn = Hook_IsCommandOn;

	LoadSettingsFile(DTRUE);
	Log("Shogo VR started (CShell.dll built " __DATE__ " " __TIME__ ")");
	RememberCommandLine();
	StartBridgeIfNeeded();

	// The creators (ShogoVR\AUTHORS.txt, one per line) for the opening notice.
	if (m_szIniPath[0])
	{
		char szPath[320];
		strncpy(szPath, m_szIniPath, sizeof(szPath) - 1);
		szPath[sizeof(szPath) - 1] = 0;
		char* pSlash = strrchr(szPath, '\\');
		if (pSlash)
		{
			strcpy(pSlash + 1, "AUTHORS.txt");
			FILE* f = fopen(szPath, "r");
			if (f)
			{
				char szLine[200];
				while (fgets(szLine, sizeof(szLine), f))
				{
					char* p = szLine;
					if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
					int n = (int)strlen(p);
					while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) p[--n] = 0;
					if (!n) continue;
					if (m_szAuthors[0] && strlen(m_szAuthors) + 2 < sizeof(m_szAuthors)) strcat(m_szAuthors, ", ");
					strncat(m_szAuthors, p, sizeof(m_szAuthors) - strlen(m_szAuthors) - 1);
				}
				fclose(f);
			}
		}
	}

	// Direct capture: frames straight from the renderer to the headset bridge.
	if (m_vtDirectCapture.GetFloat(1.0f) != 0.0f)
	{
		m_bCaptureHook = VRCapture_Install() ? DTRUE : DFALSE;
	}

	pClientDE->CPrint("Shogo VR stereo module loaded (VRStereo = %d).", (int)m_vtStereo.GetFloat());
}

void CVRStereo::Term()
{
	ReleaseTransformKey();

	// Close the headset bridge if the game started it.
	if (m_bStartedBridge)
	{
		HANDLE hQuit = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Local\\ShogoVRBridge_Quit");
		if (hQuit) { SetEvent(hQuit); CloseHandle(hQuit); }
		m_bStartedBridge = DFALSE;
	}
	VRCapture_Uninstall();		// before this DLL goes away
	m_bCaptureHook = DFALSE;
	RemoveHooks();
	if (m_pClientDE && m_pfnIsCommandOn)
	{
		m_pClientDE->IsCommandOn = m_pfnIsCommandOn;
		m_pfnIsCommandOn = DNULL;
	}
	m_hAimMarker = DNULL;
	m_hBody = DNULL;
	m_hHeldGun = DNULL;
	CloseShared();
	m_bCrosshairPass = DFALSE;
	m_pClientDE = DNULL;
}

DBOOL CVRStereo::IsActive()
{
	if (!m_pClientDE) return DFALSE;
	return (GetMode() != VR_STEREO_OFF);
}

DBOOL CVRStereo::IsStereoFrame()
{
	return IsActive() && !m_bFlatFrame;
}

void CVRStereo::BeginFrame(DBOOL bFlat, DBOOL bHudStripAllowed)
{
	// Flat frames only with a bridge that can show them on a floating screen.
	DBOOL bLayers = m_bBridgeAlive && (m_nBridgeFlags & SHOGOVR_BRIDGE_LAYERS);
	m_bFlatFrame		= bFlat && bLayers && IsActive() && m_vtFlatMenus.GetFloat(1.0f) != 0.0f;
	m_bHudStripAllowed	= bHudStripAllowed && bLayers;

	if (m_bFlatFrame)
	{
		if (m_hAimMarker) m_pClientDE->SetObjectFlags(m_hAimMarker, 0);	// world objects - keep them out of cutscenes
		HideBody();
		if (m_hHeldGun) m_pClientDE->SetObjectFlags(m_hHeldGun, 0);
		m_bHudStrip			= DFALSE;
		m_nReportViewMode	= SHOGOVR_VIEW_FLAT;
		m_nReportPoseId		= 0;
		m_fReportSep		= 0.0f;
		WriteGameBlock();
	}
}

int CVRStereo::GetMode()
{
	int nMode = (int)m_vtStereo.GetFloat(0.0f);
	if (nMode != VR_STEREO_FULLSBS && nMode != VR_STEREO_HALFSBS) nMode = VR_STEREO_OFF;

	// The headset bridge is running: switch stereo on by itself.
	if (nMode == VR_STEREO_OFF && m_bBridgeAlive && m_vtAutoStereo.GetFloat(1.0f) != 0.0f)
	{
		nMode = VR_STEREO_FULLSBS;
	}
	return nMode;
}


// ======================================================================= //
//	Geometry helpers
// ======================================================================= //

void CVRStereo::RefreshFrame(DFLOAT fBaseFovX)
{
	if (!m_pClientDE) return;

	DDWORD nWidth = 640, nHeight = 480;
	m_pClientDE->GetSurfaceDims(m_pClientDE->GetScreenSurface(), &nWidth, &nHeight);
	if (nWidth < 2)  nWidth  = 2;
	if (nHeight < 1) nHeight = 1;

	m_nScreenW	= nWidth;
	m_nScreenH	= nHeight;
	m_nMode		= GetMode();

	if (fBaseFovX > 0.0f) m_fLastBaseFovX = fBaseFovX;

	ComputeLayout();

	DFLOAT fFovY = BaseEyeFovY(m_fLastBaseFovX);
	DFLOAT fFovX = ClampFov(FovXFromFovY(fFovY, EyeAspect()));
	m_fFocalBase = FocalFromFovX(fFovX);
	m_fBaseTanX  = (DFLOAT)tan(fFovX * 0.5f);
	m_fBaseTanY  = (DFLOAT)tan(fFovY * 0.5f);
}

void CVRStereo::GetHalfRect(int nEye, int &nLeft, int &nRight)
{
	// VRSwapEyes puts the right eye's picture on the left half (cross-eyed viewing)
	int nHalf = nEye;
	if (m_vtSwapEyes.GetFloat(0.0f) != 0.0f) nHalf = 1 - nEye;

	int nMid = (int)m_nScreenW / 2;
	if (nHalf == 0)
	{
		nLeft	= 0;
		nRight	= nMid;
	}
	else
	{
		nLeft	= nMid;
		nRight	= (int)m_nScreenW;
	}
}

void CVRStereo::ComputeLayout()
{
	// The HUD in the headset is laid out for a small virtual screen...
	DFLOAT fLayout = m_vtHudLayout.GetFloat(512.0f);
	if (fLayout < 512.0f) fLayout = 512.0f;		// below this Shogo shrinks its own HUD
	if (fLayout > (DFLOAT)m_nScreenW) fLayout = (DFLOAT)m_nScreenW;
	m_fHudVirtW = (DFLOAT)VRRound(fLayout);
	m_fHudVirtH = (DFLOAT)VRRound(fLayout * 0.75f);

	// ...and, when possible, drawn once at its original pixel size into a
	// strip along the bottom of the window, which the bridge shows as its own
	// panel - pixel-exact instead of being squeezed into each eye's picture.
	m_nEyeH		= (long)m_nScreenH;
	m_bHudStrip	= DFALSE;
	m_nHudX = m_nHudY = m_nHudW = m_nHudH = 0;

	DFLOAT fSetting = m_vtHudStrip.GetFloat(-1.0f);
	if (!m_bHudStripAllowed || m_bFlatFrame || fSetting == 0.0f || m_nMode != VR_STEREO_FULLSBS) return;

	int nScale = (int)m_vtHudPixels.GetFloat(1.0f);
	if (nScale < 1) nScale = 1;
	if (nScale > 4) nScale = 4;

	long nHudW = (long)m_fHudVirtW * nScale;
	long nHudH = (long)m_fHudVirtH * nScale;
	long nEyeH = (long)m_nScreenH - nHudH;
	if (nHudW > (long)m_nScreenW || nEyeH < (long)m_nScreenH / 2) return;

	// Automatic: only when the eyes lose (almost) nothing.  The eye pictures
	// are usually taller than the headset can show (the extra rows are cut
	// off anyway), so the strip often comes for free; accept up to a 10%
	// cost, since a sharp HUD is worth it.
	if (fSetting < 0.0f)
	{
		if (m_fNeedTanX < 0.01f || m_fNeedTanY < 0.01f) return;
		DFLOAT fNeededEyeH = ((DFLOAT)m_nScreenW * 0.5f) * m_fNeedTanY / m_fNeedTanX;
		if ((DFLOAT)nEyeH < fNeededEyeH * 0.90f) return;
	}

	m_bHudStrip	= DTRUE;
	if (m_nLoggedHudStrip != 1) { Log("HUD on its own panel (%ldx%ld)", (long)m_fHudVirtW * nScale, (long)m_fHudVirtH * nScale); m_nLoggedHudStrip = 1; }
	m_nHudScale	= nScale;
	m_nEyeH		= nEyeH;
	m_nHudX		= 0;
	m_nHudY		= nEyeH;
	m_nHudW		= nHudW;
	m_nHudH		= nHudH;
}

DBOOL CVRStereo::SkipThisDraw()
{
	if (m_bCrosshairPass && m_bAimByHand) return DTRUE;			// the 3D aim dot replaces it
	if (m_bSkipNonCross && !(m_bCrosshairPass && m_bLastImmersive)) return DTRUE;	// HUD already in its strip
	return DFALSE;
}

DFLOAT CVRStereo::EyeAspect()
{
	// Aspect of one eye's picture as the viewer finally sees it.  In half SBS
	// the picture gets stretched back to full width by the TV/player.
	DFLOAT fW = (DFLOAT)m_nScreenW;
	DFLOAT fH = (DFLOAT)m_nEyeH;

	if (m_nMode == VR_STEREO_HALFSBS) return fW / fH;
	return (fW * 0.5f) / fH;
}

DFLOAT CVRStereo::BaseEyeFovY(DFLOAT fBaseFovX)
{
	if (m_bBridgeAlive && m_fNeedTanX > 0.01f && m_fNeedTanY > 0.01f)
	{
		// In the headset: the eye picture's shape is fixed by the screen
		// mode, so pick the vertical FOV that makes the picture at least as
		// wide AND as tall as the headset's view (any extra is cropped off).
		DFLOAT fTanY	= m_fNeedTanY;
		DFLOAT fAspect	= EyeAspect();
		if (fAspect > 0.01f && (m_fNeedTanX / fAspect) > fTanY) fTanY = m_fNeedTanX / fAspect;
		return ClampFov(2.0f * (DFLOAT)atan(fTanY));
	}

	DFLOAT fFov = m_vtFov.GetFloat(0.0f);
	if (fFov >= 1.0f) return ClampFov(VR_DEG2RAD(fFov));

	// Match the game: Shogo uses fovY = fovX * height / width.
	return ClampFov((fBaseFovX * (DFLOAT)m_nScreenH) / (DFLOAT)m_nScreenW);
}

DFLOAT CVRStereo::FovXFromFovY(DFLOAT fFovY, DFLOAT fAspect)
{
	return 2.0f * (DFLOAT)atan(tan(fFovY * 0.5f) * fAspect);
}

DFLOAT CVRStereo::FocalFromFovX(DFLOAT fFovX)
{
	// Pixels (in the eye's half of the screen) per unit of tangent.
	DFLOAT fTan = (DFLOAT)tan(fFovX * 0.5f);
	if (fTan < 0.001f) fTan = 0.001f;
	return (EyeWidth() * 0.5f) / fTan;
}

DFLOAT CVRStereo::ClampFov(DFLOAT fFov)
{
	if (fFov < VR_MIN_FOV) return VR_MIN_FOV;
	if (fFov > VR_MAX_FOV) return VR_MAX_FOV;
	return fFov;
}

DFLOAT CVRStereo::GetWorldScale(DBOOL bFootScale)
{
	DFLOAT fScale = bFootScale ? m_vtScaleFoot.GetFloat(40.0f) : m_vtScaleMCA.GetFloat(8.0f);
	if (fScale < 0.01f) fScale = 0.01f;
	return fScale;
}

DFLOAT CVRStereo::GetIPDMeters()
{
	DFLOAT fIPD = m_vtIPD.GetFloat(64.0f);
	if (fIPD < 0.0f)   fIPD = 0.0f;
	if (fIPD > 200.0f) fIPD = 200.0f;
	return fIPD / 1000.0f;
}

DFLOAT CVRStereo::GetHudShift()
{
	// Horizontal shift (pixels) that puts a flat element VRHudDepth metres away.
	DFLOAT fDepth = m_vtHudDepth.GetFloat(3.5f);
	if (fDepth < 0.1f) fDepth = 0.1f;
	return (GetIPDMeters() * 0.5f) * m_fFocalBase / fDepth;
}

void CVRStereo::ComputeXform(int nEye, DFLOAT fScaleX, DFLOAT fScaleY, DFLOAT fShift,
							 DFLOAT fVirtW, DFLOAT fVirtH, VRXform &x)
{
	// Maps the centre of the virtual screen (the one the 2D code thinks it's
	// drawing to) onto the centre of this eye's half, scaled by
	// (fScaleX, fScaleY) and shifted sideways.
	int nLeft = 0, nRight = 0;
	GetHalfRect(nEye, nLeft, nRight);

	DFLOAT fH		= (DFLOAT)m_nEyeH;
	DFLOAT fEyeW	= (DFLOAT)(nRight - nLeft);

	x.sx	= fScaleX;
	x.sy	= fScaleY;
	x.ox	= (DFLOAT)nLeft + fEyeW * 0.5f - fVirtW * 0.5f * fScaleX + fShift;
	x.oy	= fH * 0.5f - fVirtH * 0.5f * fScaleY;
	x.clipL	= nLeft;
	x.clipR	= nRight;
	x.clipT	= 0;
	x.clipB	= (int)m_nEyeH;
}

void CVRStereo::ComputeHudXform(int nEye, DBOOL bVirtualHud, VRXform &x)
{
	DFLOAT fVirtW = bVirtualHud ? m_fVirtW : (DFLOAT)m_nScreenW;
	DFLOAT fVirtH = bVirtualHud ? m_fVirtH : (DFLOAT)m_nScreenH;
	DFLOAT sx, sy;

	if (m_bBridgeAlive)
	{
		// In the headset: give the panel a fixed angular width, so it sits
		// comfortably inside your view whatever the resolution or headset.
		DFLOAT fDeg = bVirtualHud ? m_vtHudWidth.GetFloat(60.0f) : m_vtMenuWidth.GetFloat(80.0f);
		if (fDeg < 10.0f)  fDeg = 10.0f;
		if (fDeg > 170.0f) fDeg = 170.0f;

		DFLOAT fTanW	= 2.0f * (DFLOAT)tan(VR_DEG2RAD(fDeg) * 0.5f);		// panel width in tangent units
		DFLOAT fFocalX	= m_fFocalBase;										// eye pixels per tangent unit
		DFLOAT fFocalY	= ((DFLOAT)m_nEyeH * 0.5f) / (m_fBaseTanY > 0.01f ? m_fBaseTanY : 1.0f);
		sx = fTanW * fFocalX / fVirtW;
		sy = fTanW * fFocalY / fVirtW;		// same tangent scale vertically keeps the shape

		// Shogo's HUD is pixel art: uneven scaling (0.76x, 1.5x...) drops or
		// doubles pixel columns and makes text ragged.  Round to whole pixels.
		if (bVirtualHud && m_vtHudSnap.GetFloat(1.0f) != 0.0f && m_nMode == VR_STEREO_FULLSBS)
		{
			DFLOAT fWhole = (DFLOAT)floor(sx + 0.5f);
			if (fWhole < 1.0f) fWhole = 1.0f;
			sx = sy = fWhole;
		}
	}
	else
	{
		DFLOAT fHudScale = m_vtHudScale.GetFloat(1.0f);
		if (fHudScale < 0.1f) fHudScale = 0.1f;

		int nLeft = 0, nRight = 0;
		GetHalfRect(nEye, nLeft, nRight);
		DFLOAT fEyeW = (DFLOAT)(nRight - nLeft);

		if (m_nMode == VR_STEREO_HALFSBS)
		{
			sx = 0.5f * fHudScale;
			sy = fHudScale;
		}
		else
		{
			DFLOAT fFit = VRMinF(fEyeW / fVirtW, (DFLOAT)m_nScreenH / fVirtH);
			if (fFit > 1.0f) fFit = 1.0f;
			sx = sy = fFit * fHudScale;
		}
	}

	// Left eye moves right, right eye moves left => the panel floats in front.
	DFLOAT fShift = -EyeSign(nEye) * GetHudShift();

	ComputeXform(nEye, sx, sy, fShift, fVirtW, fVirtH, x);
}


// ======================================================================= //
//	3D
// ======================================================================= //

void CVRStereo::RenderStereo(HLOCALOBJ hCamera, HLOCALOBJ* pObjects, int nObjects,
							 DFLOAT fBaseFovX, int nMode3D, DBOOL bFootScale)
{
	if (!m_pClientDE || !hCamera) return;

	CClientDE* pClientDE = m_pClientDE;

	RefreshFrame(fBaseFovX);

	// Save the camera state so the rest of the game never sees our changes...

	DBOOL bOrigFull = DFALSE;
	int nOrigL = 0, nOrigT = 0, nOrigR = 0, nOrigB = 0;
	pClientDE->GetCameraRect(hCamera, &bOrigFull, &nOrigL, &nOrigT, &nOrigR, &nOrigB);

	DFLOAT fOrigFovX = 0.0f, fOrigFovY = 0.0f;
	pClientDE->GetCameraFOV(hCamera, &fOrigFovX, &fOrigFovY);

	DVector vOrigPos;
	pClientDE->GetObjectPos(hCamera, &vOrigPos);

	DRotation rRot;
	DVector vU, vR, vF;
	pClientDE->GetObjectRotation(hCamera, &rRot);
	pClientDE->GetRotationVectors(&rRot, &vU, &vR, &vF);
	VEC_NORM(vR);

	// The mono rectangle the game wants (used by the panel modes)...

	DRect rcMono;
	if (bOrigFull)
	{
		rcMono.left = 0; rcMono.top = 0;
		rcMono.right = (int)m_nScreenW; rcMono.bottom = (int)m_nScreenH;
	}
	else
	{
		rcMono.left = nOrigL; rcMono.top = nOrigT;
		rcMono.right = nOrigR; rcMono.bottom = nOrigB;
	}

	// Zoom: only a real zoom IN (the sniper scope) narrows the view.  The
	// camera's FOV changes for other reasons too - the server sends one made
	// for a 4:3 screen on every player-mode change (each level start, each
	// transform) and a wider one in a mech's vehicle mode - and in a headset
	// none of that may change what you see.  So: horizontal angle only (the
	// server's vertical one assumes 4:3), and never wider than normal.

	DFLOAT fZoom = 1.0f;
	if (fOrigFovX > 0.001f && m_fLastBaseFovX > 0.001f) fZoom = fOrigFovX / m_fLastBaseFovX;
	if (fZoom > 0.97f) fZoom = 1.0f;
	if (fZoom < 0.01f) fZoom = 0.01f;

	if (fZoom >= 1.0f && ((DFLOAT)fabs(fOrigFovX - m_fLoggedCamFovX) > 0.01f || (DFLOAT)fabs(fOrigFovY - m_fLoggedCamFovY) > 0.01f))
	{
		Log("Game camera FOV is now %.1f x %.1f degrees (headset view %s)", VR_RAD2DEG(fOrigFovX), VR_RAD2DEG(fOrigFovY),
			fZoom < 1.0f ? "zoomed in to match" : "unchanged");
		m_fLoggedCamFovX = fOrigFovX;
		m_fLoggedCamFovY = fOrigFovY;
	}
	if ((DFLOAT)fabs(fZoom - m_fLoggedZoom) > 0.05f)
	{
		Log("Zoom %.2fx", fZoom);
		m_fLoggedZoom = fZoom;
	}

	DFLOAT fWorldScale = GetWorldScale(bFootScale);
	m_fWorldScaleUsed = fWorldScale;
	DFLOAT fIpdWorld = GetIPDMeters() * fWorldScale;

	m_bLastImmersive = (nMode3D == VR3D_IMMERSIVE);

	// Head tracking: view = body yaw (mouse) followed by the full head
	// orientation, plus the head's position.  The aim already includes the
	// head's yaw/pitch (ApplyHeadTracking), so the crosshair stays centred.

	DBOOL bTracked = (nMode3D == VR3D_IMMERSIVE) && m_bHeadApplied && IsHeadTracking();
	DRotation rRender;
	ROT_COPY(rRender, rRot);
	DVector vBasePos;
	VEC_COPY(vBasePos, vOrigPos);

	if (bTracked)
	{
		DRotation rBody;
		DVector bU, bR, bF, vT, vHF, vHU;
		pClientDE->SetupEuler(&rBody, 0.0f, m_fBodyYaw, 0.0f);
		pClientDE->GetRotationVectors(&rBody, &bU, &bR, &bF);

		VEC_MULSCALAR(vHF, bR, m_vHeadF.x);
		VEC_MULSCALAR(vT,  bU, m_vHeadF.y);	VEC_ADD(vHF, vHF, vT);
		VEC_MULSCALAR(vT,  bF, m_vHeadF.z);	VEC_ADD(vHF, vHF, vT);

		VEC_MULSCALAR(vHU, bR, m_vHeadU.x);
		VEC_MULSCALAR(vT,  bU, m_vHeadU.y);	VEC_ADD(vHU, vHU, vT);
		VEC_MULSCALAR(vT,  bF, m_vHeadU.z);	VEC_ADD(vHU, vHU, vT);

		pClientDE->AlignRotation(&rRender, &vHF, &vHU);
		pClientDE->GetRotationVectors(&rRender, &vU, &vR, &vF);
		VEC_NORM(vR);

		if (m_vtHeadPosition.GetFloat(1.0f) != 0.0f)
		{
			DVector vOff;
			VEC_MULSCALAR(vOff, bR, m_vHeadP.x * fWorldScale);
			VEC_MULSCALAR(vT,   bU, m_vHeadP.y * fWorldScale);	VEC_ADD(vOff, vOff, vT);
			VEC_MULSCALAR(vT,   bF, m_vHeadP.z * fWorldScale);	VEC_ADD(vOff, vOff, vT);
			VEC_ADD(vBasePos, vBasePos, vOff);
		}

		pClientDE->SetObjectRotation(hCamera, &rRender);
	}

	DFLOAT fReportSep = 0.0f;

	for (int nEye = 0; nEye < 2; nEye++)
	{
		DFLOAT fEyeFovX	= fOrigFovX;
		DFLOAT fEyeFovY	= fOrigFovY;
		DFLOAT fSep		= 0.0f;

		if (nMode3D == VR3D_IMMERSIVE)
		{
			int nLeft = 0, nRight = 0;
			GetHalfRect(nEye, nLeft, nRight);
			pClientDE->SetCameraRect(hCamera, DFALSE, nLeft, 0, nRight, (int)m_nEyeH);

			fEyeFovY = ClampFov(BaseEyeFovY(m_fLastBaseFovX) * fZoom);
			fEyeFovX = ClampFov(FovXFromFovY(fEyeFovY, EyeAspect()));

			// Shrink the eye distance while zoomed so the scope doesn't
			// turn into binoculars with extreme depth.
			fSep = fIpdWorld * (fZoom < 1.0f ? fZoom : 1.0f);
			fReportSep = GetIPDMeters() * (fZoom < 1.0f ? fZoom : 1.0f);

			m_fFocalRender	= FocalFromFovX(fEyeFovX);
			m_fIpdWorld		= fSep;
			m_fEyeFovX		= fEyeFovX;
			m_fEyeFovY		= fEyeFovY;
		}
		else
		{
			VRXform x;
			ComputeHudXform(nEye, DFALSE, x);

			DRect rcEye;
			rcEye.left		= VRRound(x.ox + (DFLOAT)rcMono.left   * x.sx);
			rcEye.right		= VRRound(x.ox + (DFLOAT)rcMono.right  * x.sx);
			rcEye.top		= VRRound(x.oy + (DFLOAT)rcMono.top    * x.sy);
			rcEye.bottom	= VRRound(x.oy + (DFLOAT)rcMono.bottom * x.sy);

			if (rcEye.left < x.clipL)				rcEye.left = x.clipL;
			if (rcEye.right > x.clipR)				rcEye.right = x.clipR;
			if (rcEye.top < 0)						rcEye.top = 0;
			if (rcEye.bottom > (int)m_nScreenH)		rcEye.bottom = (int)m_nScreenH;
			if (rcEye.right <= rcEye.left || rcEye.bottom <= rcEye.top) continue;

			pClientDE->SetCameraRect(hCamera, DFALSE, rcEye.left, rcEye.top, rcEye.right, rcEye.bottom);

			if (nMode3D == VR3D_PANEL_STEREO)
			{
				DFLOAT fStrength = m_vtCutsceneStereo.GetFloat(0.5f);
				if (fStrength < 0.0f) fStrength = 0.0f;
				if (fStrength > 1.0f) fStrength = 1.0f;
				fSep = fIpdWorld * fStrength;
				fReportSep = GetIPDMeters() * fStrength;
			}
		}

		pClientDE->SetCameraFOV(hCamera, fEyeFovX, fEyeFovY);

		DVector vEyePos, vOffset;
		VEC_MULSCALAR(vOffset, vR, EyeSign(nEye) * fSep * 0.5f);
		VEC_ADD(vEyePos, vBasePos, vOffset);
		pClientDE->SetObjectPos(hCamera, &vEyePos);

		if (pObjects)
		{
			pClientDE->RenderObjects(hCamera, pObjects, nObjects);
		}
		else
		{
			pClientDE->RenderCamera(hCamera);
		}
	}

	// Put everything back...

	pClientDE->SetCameraRect(hCamera, bOrigFull, nOrigL, nOrigT, nOrigR, nOrigB);
	pClientDE->SetCameraFOV(hCamera, fOrigFovX, fOrigFovY);
	pClientDE->SetObjectPos(hCamera, &vOrigPos);
	if (bTracked) pClientDE->SetObjectRotation(hCamera, &rRot);

	// Tell the bridge what this frame was rendered with...

	if (nMode3D == VR3D_IMMERSIVE)
	{
		m_fReportTanX = (DFLOAT)tan(m_fEyeFovX * 0.5f);
		m_fReportTanY = (DFLOAT)tan(m_fEyeFovY * 0.5f);
	}
	else
	{
		m_fReportTanX = m_fBaseTanX;
		m_fReportTanY = m_fBaseTanY;
	}
	m_fReportSep		= fReportSep;
	m_nReportViewMode	= bTracked ? SHOGOVR_VIEW_TRACKED : SHOGOVR_VIEW_SCREEN;
	m_nReportPoseId		= bTracked ? m_nPoseId : 0;
	m_bHeadApplied		= DFALSE;

	WriteGameBlock();
}

void CVRStereo::UpdateAimDepth(HLOCALOBJ hCamera, HLOCALOBJ* pFilterList)
{
	if (!m_pClientDE || !hCamera || !IsActive()) return;

	CClientDE* pClientDE = m_pClientDE;

	DVector vPos, vU, vR, vF;
	DRotation rRot;
	pClientDE->GetObjectPos(hCamera, &vPos);
	pClientDE->GetObjectRotation(hCamera, &rRot);
	pClientDE->GetRotationVectors(&rRot, &vU, &vR, &vF);
	VEC_NORM(vF);

	ClientIntersectQuery query;
	ClientIntersectInfo info;

	VEC_COPY(query.m_From, vPos);
	VEC_MULSCALAR(query.m_To, vF, VR_AIM_RANGE);
	VEC_ADD(query.m_To, query.m_To, vPos);
	VEC_COPY(query.m_Direction, vF);
	query.m_Flags		= INTERSECT_OBJECTS | IGNORE_NONSOLID;
	query.m_FilterFn	= pFilterList ? ObjListFilterFn : DNULL;
	query.m_pUserData	= pFilterList;

	DFLOAT fTarget = 0.0f;		// 0 = nothing hit, crosshair at infinity

	if (pClientDE->IntersectSegment(&query, &info))
	{
		DVector vDiff;
		VEC_SUB(vDiff, info.m_Point, vPos);
		DFLOAT fDist = VEC_MAG(vDiff);
		if (fDist < 1.0f) fDist = 1.0f;
		fTarget = 1.0f / fDist;
	}

	// Smooth it (in inverse depth, which is what the eyes respond to)...

	DFLOAT fFrameTime = pClientDE->GetFrameTime();
	DFLOAT k = fFrameTime * VR_AIM_SMOOTHING;
	if (k < 0.0f) k = 0.0f;
	if (k > 1.0f) k = 1.0f;

	m_fAimInvDepth += (fTarget - m_fAimInvDepth) * k;
}


// ======================================================================= //
//	2D
// ======================================================================= //

void CVRStereo::Begin2D(int nEye, DBOOL bHud)
{
	if (!m_pClientDE || !IsStereoFrame()) return;

	if (nEye == 0)
	{
		RefreshFrame(0.0f);
	}

	m_nCurEye = nEye;
	m_hScreen = m_pClientDE->GetScreenSurface();

	// In the headset, the in-game HUD is laid out for a smaller (4:3)
	// virtual screen: Shogo's HUD art has a fixed pixel size, so a smaller
	// layout makes it bigger, and it pulls the corner gauges in from the
	// blurry edges of the lenses.  Menus keep their own layout.

	m_bVirtualHud = bHud && m_bBridgeAlive;
	if (m_bVirtualHud)
	{
		m_fVirtW = m_fHudVirtW;
		m_fVirtH = m_fHudVirtH;
	}
	else
	{
		m_fVirtW = (DFLOAT)m_nScreenW;
		m_fVirtH = (DFLOAT)m_nScreenH;
	}

	// HUD / menu panel...

	m_bSkipNonCross = DFALSE;
	if (m_bVirtualHud && m_bHudStrip)
	{
		// The HUD goes once, at whole-pixel size, into its own strip; only
		// the crosshair (if any) is drawn into each eye.
		if (nEye == 0)
		{
			m_xfHud.sx		= (DFLOAT)m_nHudScale;
			m_xfHud.sy		= (DFLOAT)m_nHudScale;
			m_xfHud.ox		= (DFLOAT)m_nHudX;
			m_xfHud.oy		= (DFLOAT)m_nHudY;
			m_xfHud.clipL	= (int)m_nHudX;
			m_xfHud.clipR	= (int)(m_nHudX + m_nHudW);
			m_xfHud.clipT	= (int)m_nHudY;
			m_xfHud.clipB	= (int)(m_nHudY + m_nHudH);
		}
		else
		{
			m_bSkipNonCross = DTRUE;
		}
	}
	else
	{
		ComputeHudXform(nEye, m_bVirtualHud, m_xfHud);
	}

	// Crosshair: original pixel size, centred on the eye, at aim depth...

	DFLOAT fCrossScale = m_vtCrossScale.GetFloat(1.0f);
	if (fCrossScale < 0.1f) fCrossScale = 0.1f;

	DFLOAT fCrossSX = (m_nMode == VR_STEREO_HALFSBS) ? fCrossScale * 0.5f : fCrossScale;
	DFLOAT fCrossSY = fCrossScale;

	DFLOAT fDisparity = GetHudShift();
	if (m_vtCrossDepth.GetFloat(1.0f) != 0.0f)
	{
		fDisparity = (m_fIpdWorld * 0.5f) * m_fFocalRender * m_fAimInvDepth;
	}

	ComputeXform(nEye, fCrossSX, fCrossSY, -EyeSign(nEye) * fDisparity, m_fVirtW, m_fVirtH, m_xfCross);

	InstallHooks();
}

void CVRStereo::End2D()
{
	if (!m_bHooked) return;

	RemoveHooks();
	m_nCurEye = 0;
	m_bVirtualHud = DFALSE;
	m_bSkipNonCross = DFALSE;
	m_fVirtW = (DFLOAT)m_nScreenW;
	m_fVirtH = (DFLOAT)m_nScreenH;
}

void CVRStereo::InstallHooks()
{
	if (m_bHooked || !m_pClientDE) return;

	CClientDE* p = m_pClientDE;

	m_pfnDrawSurfaceToSurface				= p->DrawSurfaceToSurface;
	m_pfnDrawSurfaceToSurfaceTransparent	= p->DrawSurfaceToSurfaceTransparent;
	m_pfnDrawSurfaceSolidColor				= p->DrawSurfaceSolidColor;
	m_pfnScaleSurfaceToSurface				= p->ScaleSurfaceToSurface;
	m_pfnScaleSurfaceToSurfaceTransparent	= p->ScaleSurfaceToSurfaceTransparent;
	m_pfnScaleSurfaceToSurfaceSolidColor	= p->ScaleSurfaceToSurfaceSolidColor;
	m_pfnFillRect							= p->FillRect;
	m_pfnDrawStringToSurface				= p->DrawStringToSurface;
	m_pfnGetFrameTime						= p->GetFrameTime;
	m_pfnGetSurfaceDims						= p->GetSurfaceDims;
	m_pfnGetCameraRect						= p->GetCameraRect;

	p->DrawSurfaceToSurface					= Hook_DrawSurfaceToSurface;
	p->DrawSurfaceToSurfaceTransparent		= Hook_DrawSurfaceToSurfaceTransparent;
	p->DrawSurfaceSolidColor				= Hook_DrawSurfaceSolidColor;
	p->ScaleSurfaceToSurface				= Hook_ScaleSurfaceToSurface;
	p->ScaleSurfaceToSurfaceTransparent		= Hook_ScaleSurfaceToSurfaceTransparent;
	p->ScaleSurfaceToSurfaceSolidColor		= Hook_ScaleSurfaceToSurfaceSolidColor;
	p->FillRect								= Hook_FillRect;
	p->DrawStringToSurface					= Hook_DrawStringToSurface;
	p->GetFrameTime							= Hook_GetFrameTime;
	p->GetSurfaceDims						= Hook_GetSurfaceDims;
	p->GetCameraRect						= Hook_GetCameraRect;

	m_bHooked = DTRUE;
}

void CVRStereo::RemoveHooks()
{
	if (!m_bHooked || !m_pClientDE) return;

	CClientDE* p = m_pClientDE;

	p->DrawSurfaceToSurface					= m_pfnDrawSurfaceToSurface;
	p->DrawSurfaceToSurfaceTransparent		= m_pfnDrawSurfaceToSurfaceTransparent;
	p->DrawSurfaceSolidColor				= m_pfnDrawSurfaceSolidColor;
	p->ScaleSurfaceToSurface				= m_pfnScaleSurfaceToSurface;
	p->ScaleSurfaceToSurfaceTransparent		= m_pfnScaleSurfaceToSurfaceTransparent;
	p->ScaleSurfaceToSurfaceSolidColor		= m_pfnScaleSurfaceToSurfaceSolidColor;
	p->FillRect								= m_pfnFillRect;
	p->DrawStringToSurface					= m_pfnDrawStringToSurface;
	p->GetFrameTime							= m_pfnGetFrameTime;
	p->GetSurfaceDims						= m_pfnGetSurfaceDims;
	p->GetCameraRect						= m_pfnGetCameraRect;

	m_bHooked = DFALSE;
}

void CVRStereo::MapRect(const DRect &rcIn, DRect &rcOut)
{
	const VRXform &x = CurXform();

	rcOut.left		= VRRound(x.ox + (DFLOAT)rcIn.left   * x.sx);
	rcOut.right		= VRRound(x.ox + (DFLOAT)rcIn.right  * x.sx);
	rcOut.top		= VRRound(x.oy + (DFLOAT)rcIn.top    * x.sy);
	rcOut.bottom	= VRRound(x.oy + (DFLOAT)rcIn.bottom * x.sy);

	// Don't let thin lines vanish when scaled down.
	if (rcOut.right <= rcOut.left && rcIn.right > rcIn.left)	rcOut.right = rcOut.left + 1;
	if (rcOut.bottom <= rcOut.top && rcIn.bottom > rcIn.top)	rcOut.bottom = rcOut.top + 1;
}

// Works out where a blit to the virtual screen lands on the real screen.
// pVirtDest == DNULL means "unscaled blit at (rcDest.left, rcDest.top)".
// Returns DFALSE if nothing is left to draw after clipping.
DBOOL CVRStereo::MapBlit(HSURFACE hSrc, DRect *pSrcRect, DRect *pVirtDest, DRect &rcDest, DRect &rcSrc)
{
	if (SkipThisDraw()) return DFALSE;

	if (pSrcRect)
	{
		rcSrc = *pSrcRect;
	}
	else
	{
		DDWORD nW = 0, nH = 0;
		m_pClientDE->GetSurfaceDims(hSrc, &nW, &nH);
		rcSrc.left = 0; rcSrc.top = 0;
		rcSrc.right = (int)nW; rcSrc.bottom = (int)nH;
	}

	DRect rcVirt;
	if (pVirtDest)
	{
		rcVirt = *pVirtDest;
	}
	else
	{
		rcVirt.left		= rcDest.left;
		rcVirt.top		= rcDest.top;
		rcVirt.right	= rcDest.left + (rcSrc.right - rcSrc.left);
		rcVirt.bottom	= rcDest.top + (rcSrc.bottom - rcSrc.top);
	}

	MapRect(rcVirt, rcDest);

	const VRXform &x = CurXform();

	if (!VRClip1D(rcDest.left, rcDest.right, rcSrc.left, rcSrc.right, x.clipL, x.clipR)) return DFALSE;
	if (!VRClip1D(rcDest.top, rcDest.bottom, rcSrc.top, rcSrc.bottom, x.clipT, x.clipB)) return DFALSE;

	return DTRUE;
}


// ======================================================================= //
//	Hooked engine functions
// ======================================================================= //

DRESULT CVRStereo::Hook_DrawSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc, DRect *pSrcRect, int destX, int destY)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen) return s.m_pfnDrawSurfaceToSurface(hDest, hSrc, pSrcRect, destX, destY);

	DRect rcDest, rcSrc;
	rcDest.left = destX; rcDest.top = destY;
	if (!s.MapBlit(hSrc, pSrcRect, DNULL, rcDest, rcSrc)) return LT_OK;

	return s.m_pfnScaleSurfaceToSurface(hDest, hSrc, &rcDest, &rcSrc);
}

DRESULT CVRStereo::Hook_DrawSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc, DRect *pSrcRect, int destX, int destY, HDECOLOR hColor)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen) return s.m_pfnDrawSurfaceToSurfaceTransparent(hDest, hSrc, pSrcRect, destX, destY, hColor);

	DRect rcDest, rcSrc;
	rcDest.left = destX; rcDest.top = destY;
	if (!s.MapBlit(hSrc, pSrcRect, DNULL, rcDest, rcSrc)) return LT_OK;

	return s.m_pfnScaleSurfaceToSurfaceTransparent(hDest, hSrc, &rcDest, &rcSrc, hColor);
}

DRESULT CVRStereo::Hook_DrawSurfaceSolidColor(HSURFACE hDest, HSURFACE hSrc, DRect *pSrcRect, int destX, int destY, HDECOLOR hTransColor, HDECOLOR hFillColor)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen) return s.m_pfnDrawSurfaceSolidColor(hDest, hSrc, pSrcRect, destX, destY, hTransColor, hFillColor);

	DRect rcDest, rcSrc;
	rcDest.left = destX; rcDest.top = destY;
	if (!s.MapBlit(hSrc, pSrcRect, DNULL, rcDest, rcSrc)) return LT_OK;

	return s.m_pfnScaleSurfaceToSurfaceSolidColor(hDest, hSrc, &rcDest, &rcSrc, hTransColor, hFillColor);
}

DRESULT CVRStereo::Hook_ScaleSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc, DRect *pDestRect, DRect *pSrcRect)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen) return s.m_pfnScaleSurfaceToSurface(hDest, hSrc, pDestRect, pSrcRect);

	DRect rcFull;
	if (!pDestRect)
	{
		rcFull.left = 0; rcFull.top = 0;
		rcFull.right = VRRound(s.m_fVirtW); rcFull.bottom = VRRound(s.m_fVirtH);
		pDestRect = &rcFull;
	}

	DRect rcDest, rcSrc;
	if (!s.MapBlit(hSrc, pSrcRect, pDestRect, rcDest, rcSrc)) return LT_OK;

	return s.m_pfnScaleSurfaceToSurface(hDest, hSrc, &rcDest, &rcSrc);
}

DRESULT CVRStereo::Hook_ScaleSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc, DRect *pDestRect, DRect *pSrcRect, HDECOLOR hColor)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen) return s.m_pfnScaleSurfaceToSurfaceTransparent(hDest, hSrc, pDestRect, pSrcRect, hColor);

	DRect rcFull;
	if (!pDestRect)
	{
		rcFull.left = 0; rcFull.top = 0;
		rcFull.right = VRRound(s.m_fVirtW); rcFull.bottom = VRRound(s.m_fVirtH);
		pDestRect = &rcFull;
	}

	DRect rcDest, rcSrc;
	if (!s.MapBlit(hSrc, pSrcRect, pDestRect, rcDest, rcSrc)) return LT_OK;

	return s.m_pfnScaleSurfaceToSurfaceTransparent(hDest, hSrc, &rcDest, &rcSrc, hColor);
}

DRESULT CVRStereo::Hook_ScaleSurfaceToSurfaceSolidColor(HSURFACE hDest, HSURFACE hSrc, DRect *pDestRect, DRect *pSrcRect, HDECOLOR hTransColor, HDECOLOR hFillColor)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen) return s.m_pfnScaleSurfaceToSurfaceSolidColor(hDest, hSrc, pDestRect, pSrcRect, hTransColor, hFillColor);

	DRect rcFull;
	if (!pDestRect)
	{
		rcFull.left = 0; rcFull.top = 0;
		rcFull.right = VRRound(s.m_fVirtW); rcFull.bottom = VRRound(s.m_fVirtH);
		pDestRect = &rcFull;
	}

	DRect rcDest, rcSrc;
	if (!s.MapBlit(hSrc, pSrcRect, pDestRect, rcDest, rcSrc)) return LT_OK;

	return s.m_pfnScaleSurfaceToSurfaceSolidColor(hDest, hSrc, &rcDest, &rcSrc, hTransColor, hFillColor);
}

DRESULT CVRStereo::Hook_FillRect(HSURFACE hDest, DRect *pRect, HDECOLOR hColor)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen) return s.m_pfnFillRect(hDest, pRect, hColor);
	if (s.SkipThisDraw()) return LT_OK;

	DRect rcVirt;
	if (pRect)
	{
		rcVirt = *pRect;
	}
	else
	{
		rcVirt.left = 0; rcVirt.top = 0;
		rcVirt.right = VRRound(s.m_fVirtW); rcVirt.bottom = VRRound(s.m_fVirtH);
	}

	DRect rcDest;
	s.MapRect(rcVirt, rcDest);

	// Clip (the "source" edges are dummies here)...
	const VRXform &x = s.CurXform();
	int s0 = rcDest.left, s1 = rcDest.right;
	if (!VRClip1D(rcDest.left, rcDest.right, s0, s1, x.clipL, x.clipR)) return LT_OK;
	s0 = rcDest.top; s1 = rcDest.bottom;
	if (!VRClip1D(rcDest.top, rcDest.bottom, s0, s1, x.clipT, x.clipB)) return LT_OK;

	return s.m_pfnFillRect(hDest, &rcDest, hColor);
}

void CVRStereo::Hook_DrawStringToSurface(HSURFACE hDest, HDEFONT hFont, HSTRING hString, DRect *pRect, HDECOLOR hForeColor, HDECOLOR hBackColor)
{
	CVRStereo &s = g_VRStereo;
	if (hDest != s.m_hScreen || !pRect)
	{
		s.m_pfnDrawStringToSurface(hDest, hFont, hString, pRect, hForeColor, hBackColor);
		return;
	}
	if (s.SkipThisDraw()) return;

	// Engine text can't be scaled, so keep it at its native size and move
	// its centre to where the scaled layout says it should be.

	int nTextW = 0, nTextH = 0;
	s.m_pClientDE->GetStringDimensions(hFont, hString, &nTextW, &nTextH);

	const VRXform &x = s.CurXform();
	DFLOAT fCX = x.ox + ((DFLOAT)pRect->left + (DFLOAT)nTextW * 0.5f) * x.sx;
	DFLOAT fCY = x.oy + ((DFLOAT)pRect->top + (DFLOAT)nTextH * 0.5f) * x.sy;

	DRect rc;
	rc.left		= VRRound(fCX - (DFLOAT)nTextW * 0.5f);
	rc.top		= VRRound(fCY - (DFLOAT)nTextH * 0.5f);
	rc.right	= x.clipR;
	rc.bottom	= x.clipB;
	if (rc.left < x.clipL) rc.left = x.clipL;
	if (rc.top < x.clipT) rc.top = x.clipT;
	if (rc.left >= rc.right || rc.top >= rc.bottom) return;

	s.m_pfnDrawStringToSurface(hDest, hFont, hString, &rc, hForeColor, hBackColor);
}

void CVRStereo::Hook_GetSurfaceDims(HSURFACE hSurf, DDWORD *pWidth, DDWORD *pHeight)
{
	CVRStereo &s = g_VRStereo;
	if (s.m_bVirtualHud && hSurf == s.m_hScreen)
	{
		if (pWidth)  *pWidth  = (DDWORD)VRRound(s.m_fVirtW);
		if (pHeight) *pHeight = (DDWORD)VRRound(s.m_fVirtH);
		return;
	}
	s.m_pfnGetSurfaceDims(hSurf, pWidth, pHeight);
}

void CVRStereo::Hook_GetCameraRect(HLOCALOBJ hObj, DBOOL *pbFullscreen, int *pLeft, int *pTop, int *pRight, int *pBottom)
{
	CVRStereo &s = g_VRStereo;
	s.m_pfnGetCameraRect(hObj, pbFullscreen, pLeft, pTop, pRight, pBottom);
	if (!s.m_bVirtualHud) return;

	// Express the view rectangle on the virtual HUD screen too.
	DFLOAT fX = s.m_fVirtW / (DFLOAT)s.m_nScreenW;
	DFLOAT fY = s.m_fVirtH / (DFLOAT)s.m_nScreenH;
	if (pbFullscreen && *pbFullscreen)
	{
		if (pLeft)   *pLeft   = 0;
		if (pTop)    *pTop    = 0;
		if (pRight)  *pRight  = VRRound(s.m_fVirtW);
		if (pBottom) *pBottom = VRRound(s.m_fVirtH);
		return;
	}
	if (pLeft)   *pLeft   = VRRound((DFLOAT)*pLeft   * fX);
	if (pTop)    *pTop    = VRRound((DFLOAT)*pTop    * fY);
	if (pRight)  *pRight  = VRRound((DFLOAT)*pRight  * fX);
	if (pBottom) *pBottom = VRRound((DFLOAT)*pBottom * fY);
}

float CVRStereo::Hook_GetFrameTime()
{
	CVRStereo &s = g_VRStereo;

	// The second eye redraws the same frame - don't advance animations again.
	if (s.m_nCurEye > 0) return 0.0f;

	return s.m_pfnGetFrameTime();
}


// ======================================================================= //
//	Console tools
// ======================================================================= //

void CVRStereo::Calibrate(HLOCALOBJ hCamera, HLOCALOBJ* pFilterList, DBOOL bFootScale)
{
	if (!m_pClientDE) return;
	CClientDE* pClientDE = m_pClientDE;

	if (!hCamera)
	{
		pClientDE->CPrint("VRCalibrate: no camera yet - start a level first.");
		return;
	}

	DVector vPos;
	pClientDE->GetObjectPos(hCamera, &vPos);

	ClientIntersectQuery query;
	ClientIntersectInfo info;

	VEC_COPY(query.m_From, vPos);
	VEC_COPY(query.m_To, vPos);
	query.m_To.y -= VR_FLOOR_RANGE;
	VEC_SET(query.m_Direction, 0.0f, -1.0f, 0.0f);
	query.m_Flags		= INTERSECT_OBJECTS | IGNORE_NONSOLID;
	query.m_FilterFn	= pFilterList ? ObjListFilterFn : DNULL;
	query.m_pUserData	= pFilterList;

	if (!pClientDE->IntersectSegment(&query, &info))
	{
		pClientDE->CPrint("VRCalibrate: couldn't find the floor below the camera.");
		return;
	}

	DFLOAT fHeightUnits = vPos.y - info.m_Point.y;
	if (fHeightUnits < 1.0f)
	{
		pClientDE->CPrint("VRCalibrate: the camera is too close to the floor (%.1f units).", fHeightUnits);
		return;
	}

	DFLOAT fEyeHeight = bFootScale ? m_vtEyeHeightFoot.GetFloat(1.7f) : m_vtEyeHeightMCA.GetFloat(10.0f);
	if (fEyeHeight < 0.1f) fEyeHeight = 0.1f;

	DFLOAT fScale = fHeightUnits / fEyeHeight;
	const char* pVar = bFootScale ? "VRScaleFoot" : "VRScaleMCA";

	char str[128];
	sprintf(str, "+%s %f", pVar, fScale);		// '+' = saved in the config file
	pClientDE->RunConsoleString(str);

	pClientDE->CPrint("VRCalibrate: eye is %.1f units above the floor, target %.2f m -> %s = %.2f units per metre.",
					  fHeightUnits, fEyeHeight, pVar, fScale);
}

void CVRStereo::PrintInfo()
{
	if (!m_pClientDE) return;
	CClientDE* pClientDE = m_pClientDE;

	RefreshFrame(0.0f);

	static const char* s_pModeNames[] = { "off", "full side-by-side", "half side-by-side" };

	int nEyeW = (int)m_nScreenW / 2;
	pClientDE->CPrint("VR: VRStereo %d (%s), screen %dx%d, each eye %dx%d",
					  m_nMode, s_pModeNames[m_nMode], (int)m_nScreenW, (int)m_nScreenH, nEyeW, (int)m_nScreenH);

	pClientDE->CPrint("VR: VRIPD %.1f mm, VRFov %.1f (last eye FOV %.1f x %.1f deg)",
					  m_vtIPD.GetFloat(64.0f), m_vtFov.GetFloat(0.0f),
					  VR_RAD2DEG(m_fEyeFovX), VR_RAD2DEG(m_fEyeFovY));

	pClientDE->CPrint("VR: VRScaleFoot %.2f, VRScaleMCA %.2f units per metre (last eye separation %.2f units)",
					  m_vtScaleFoot.GetFloat(40.0f), m_vtScaleMCA.GetFloat(8.0f), m_fIpdWorld);

	pClientDE->CPrint("VR: headset bridge %s, head tracking %s, controllers %s (%s aims)",
					  m_bBridgeAlive ? "connected" : "not running",
					  IsHeadTracking() ? "on" : "off",
					  IsControllerActive() ? "on" : "off",
					  UsesControllerAim() ? "right hand" : "head");
	pClientDE->CPrint("VR: game resolution %dx%d, drawing at %dx%d, window %dx%d",
					  (int)m_nRenderW, (int)m_nRenderH, (int)m_nSurfW, (int)m_nSurfH, (int)m_nClientW, (int)m_nClientH);

	DFLOAT fAimDist = (m_fAimInvDepth > 0.00001f) ? (1.0f / m_fAimInvDepth) : -1.0f;
	pClientDE->CPrint("VR: HUD %s (strip %dx%d at %dx), eye pictures %dx%d",
					  m_bHudStrip ? "on its own sharp panel" : "drawn into each eye",
					  (int)m_nHudW, (int)m_nHudH, m_nHudScale, (int)m_nScreenW / 2, (int)m_nEyeH);
	pClientDE->CPrint("VR: direct capture %s - renderer frames seen %d, handed to the bridge %d",
					  m_bCaptureHook ? "ready" : "not available", VRCapture_Seen(), VRCapture_Frames());
	pClientDE->CPrint("VR: VRHudWidth %.0f deg, VRHudLayout %.0f, VRMenuWidth %.0f deg",
					  m_vtHudWidth.GetFloat(60.0f), m_vtHudLayout.GetFloat(512.0f), m_vtMenuWidth.GetFloat(80.0f));
	pClientDE->CPrint("VR: VRHudDepth %.2f m (%.1f px shift), VRHudScale %.2f, aim distance %s%.0f units",
					  m_vtHudDepth.GetFloat(3.5f), GetHudShift(), m_vtHudScale.GetFloat(1.0f),
					  (fAimDist < 0.0f) ? "infinite " : "", (fAimDist < 0.0f) ? 0.0f : fAimDist);
}


// ======================================================================= //
//	Headset link (ShogoVRBridge.exe)
// ======================================================================= //

void CVRStereo::OpenShared()
{
	if (m_pShared) return;

	HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
									 sizeof(ShogoVRShared), SHOGOVR_SHARED_NAME);
	if (!hMap) return;
	DBOOL bExisted = (GetLastError() == ERROR_ALREADY_EXISTS);

	ShogoVRShared* p = (ShogoVRShared*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShogoVRShared));
	if (!p)
	{
		CloseHandle(hMap);
		return;
	}

	if (!bExisted || p->magic != SHOGOVR_MAGIC)
	{
		p->magic		= SHOGOVR_MAGIC;
		p->version		= SHOGOVR_VERSION;
		p->structSize	= sizeof(ShogoVRShared);
	}

	m_hMapping	= (void*)hMap;
	m_pShared	= p;
}

void CVRStereo::CloseShared()
{
	if (m_pShared)
	{
		m_nReportViewMode = SHOGOVR_VIEW_OFF;
		WriteGameBlock();
		UnmapViewOfFile(m_pShared);
		m_pShared = DNULL;
	}
	if (m_hMapping)
	{
		CloseHandle((HANDLE)m_hMapping);
		m_hMapping = DNULL;
	}
	m_bBridgeAlive = DFALSE;
}

void CVRStereo::UpdateBridge()
{
	if (!m_pClientDE) return;
	CheckSettingsFile();
	if (!m_pShared) return;

	if (!m_nGameHwnd)
	{
		void* pHwnd = DNULL;
		m_pClientDE->GetEngineHook("HWND", &pHwnd);
		m_nGameHwnd = (DDWORD)pHwnd;
	}

	VRCapture_SetGame((void*)m_nGameHwnd, m_pShared, m_vtDirectCapture.GetFloat(1.0f) != 0.0f);

	ShogoVRShared* s = m_pShared;
	if (s->magic != SHOGOVR_MAGIC || s->version != SHOGOVR_VERSION || s->structSize != sizeof(ShogoVRShared))
	{
		m_bBridgeAlive = DFALSE;
		return;
	}

	// Read the bridge's block.  The sequence number is odd while the bridge
	// is writing, and changes if it wrote while we were reading - retry then.

	long nBeat = 0, nFlags = 0, nPoseId = 0, nCtrl = 0, nButtons = 0;
	float f[3], u[3], p[3], fYaw = 0.0f, fPitch = 0.0f, fTanX = 0.0f, fTanY = 0.0f;
	float rf[3], ru[3], rp[3], lf[3], lu[3], lp[3], mx = 0.0f, my = 0.0f, tx = 0.0f, ty = 0.0f;
	DBOOL bGot = DFALSE;
	int nTry, i;

	for (nTry = 0; nTry < 1000 && !bGot; nTry++)
	{
		long nSeq1 = InterlockedExchangeAdd((long*)&s->bridgeSeq, 0);
		if (nSeq1 & 1) continue;

		nBeat	= s->bridgeHeartbeat;
		nFlags	= s->bridgeFlags;
		nPoseId	= s->poseId;
		for (i = 0; i < 3; i++)
		{
			f[i] = s->headForward[i];
			u[i] = s->headUp[i];
			p[i] = s->headPos[i];
			rf[i] = s->rightForward[i];	ru[i] = s->rightUp[i];	rp[i] = s->rightPos[i];
			lf[i] = s->leftForward[i];	lu[i] = s->leftUp[i];	lp[i] = s->leftPos[i];
		}
		nCtrl		= s->controllerFlags;
		nButtons	= s->buttons;
		mx = s->moveX;	my = s->moveY;
		tx = s->turnX;	ty = s->turnY;
		fYaw	= s->headYaw;
		fPitch	= s->headPitch;
		fTanX	= s->needTanX;
		fTanY	= s->needTanY;

		long nSeq2 = InterlockedExchangeAdd((long*)&s->bridgeSeq, 0);
		if (nSeq1 == nSeq2) bGot = DTRUE;
	}

	// Alive = the heartbeat moved within the last second.

	DFLOAT fNow = m_pClientDE->GetTime();
	if (bGot && nBeat != m_nLastBridgeBeat)
	{
		m_nLastBridgeBeat = nBeat;
		m_fLastBridgeBeatTime = fNow;
	}
	m_bBridgeAlive = (m_nLastBridgeBeat != 0) && ((fNow - m_fLastBridgeBeatTime) < 1.0f);
	if (m_bBridgeAlive != m_bLoggedBridge)
	{
		Log(m_bBridgeAlive ? "Headset bridge connected" : "Headset bridge gone");
		m_bLoggedBridge = m_bBridgeAlive;
	}
	if (m_nRenderW != m_nLoggedRenderW || m_nRenderH != m_nLoggedRenderH || m_nSurfW != m_nLoggedSurfW ||
		m_nSurfH != m_nLoggedSurfH || m_nClientW != m_nLoggedClientW || m_nClientH != m_nLoggedClientH)
	{
		Log("Resolution %ldx%ld, drawing at %ldx%ld, window %ldx%ld", m_nRenderW, m_nRenderH, m_nSurfW, m_nSurfH, m_nClientW, m_nClientH);
		m_nLoggedRenderW = m_nRenderW; m_nLoggedRenderH = m_nRenderH;
		m_nLoggedSurfW = m_nSurfW; m_nLoggedSurfH = m_nSurfH;
		m_nLoggedClientW = m_nClientW; m_nLoggedClientH = m_nClientH;
	}
	if (m_nLoggedCapture < 1 && VRCapture_Seen() > 0) { Log("Direct capture: renderer frames are coming through"); m_nLoggedCapture = 1; }
	if (m_nLoggedCapture < 2 && VRCapture_Frames() > 0) { Log("Direct capture: frames handed to the bridge"); m_nLoggedCapture = 2; }

	if (bGot && m_bBridgeAlive)
	{
		m_nBridgeFlags	= nFlags;
		m_nPoseId		= nPoseId;
		VEC_SET(m_vHeadF, f[0], f[1], f[2]);
		VEC_SET(m_vHeadU, u[0], u[1], u[2]);
		VEC_SET(m_vHeadP, p[0], p[1], p[2]);
		m_fHeadYaw		= fYaw;
		m_fHeadPitch	= fPitch;
		m_fNeedTanX		= fTanX;
		m_fNeedTanY		= fTanY;

		m_nCtrlFlags	= nCtrl;
		m_nButtons		= (DDWORD)nButtons;
		m_fMoveX = mx;	m_fMoveY = my;
		m_fTurnX = tx;	m_fTurnY = ty;
		VEC_SET(m_vRightF, rf[0], rf[1], rf[2]);	VEC_SET(m_vRightU, ru[0], ru[1], ru[2]);	VEC_SET(m_vRightP, rp[0], rp[1], rp[2]);
		VEC_SET(m_vLeftF, lf[0], lf[1], lf[2]);		VEC_SET(m_vLeftU, lu[0], lu[1], lu[2]);		VEC_SET(m_vLeftP, lp[0], lp[1], lp[2]);
	}
	else if (!m_bBridgeAlive)
	{
		m_nCtrlFlags = 0;
		m_nButtons = 0;
	}

	UpdateTwoHanded();

	// Button edges for this frame
	if (!IsControllerActive()) m_nButtons = 0;
	m_nPressed		= m_nButtons & ~m_nPrevButtons;
	m_nReleased		= m_nPrevButtons & ~m_nButtons;
	m_nPrevButtons	= m_nButtons;
	m_nSuppressed	&= m_nButtons;
	m_bAimByHand	= DFALSE;		// ApplyHeadTracking sets it again during gameplay

	UpdateMechCrouch();

	if (!IsHeadTracking()) m_bHaveBody = DFALSE;
	if (!IsActive()) m_nReportViewMode = SHOGOVR_VIEW_OFF;

	// Resolution and window size, so the bridge can fix a wrongly sized window.
	RMode mode;
	memset(&mode, 0, sizeof(mode));
	if (m_pClientDE->GetRenderMode(&mode) == LT_OK)
	{
		m_nRenderW = (long)mode.m_Width;
		m_nRenderH = (long)mode.m_Height;
	}
	DDWORD nSurfW = 0, nSurfH = 0;
	m_pClientDE->GetSurfaceDims(m_pClientDE->GetScreenSurface(), &nSurfW, &nSurfH);
	m_nSurfW = (long)nSurfW;
	m_nSurfH = (long)nSurfH;

	RECT rcClient;
	if (m_nGameHwnd && GetClientRect((HWND)m_nGameHwnd, &rcClient))
	{
		m_nClientW = rcClient.right - rcClient.left;
		m_nClientH = rcClient.bottom - rcClient.top;
	}

	WriteGameBlock();
}

DBOOL CVRStereo::IsHeadTracking()
{
	if (!m_pClientDE || !m_bBridgeAlive || m_nPoseId == 0) return DFALSE;
	if (!(m_nBridgeFlags & SHOGOVR_BRIDGE_HEADSET_ACTIVE)) return DFALSE;
	if (m_vtHeadTracking.GetFloat(1.0f) == 0.0f) return DFALSE;
	return IsActive();
}

void CVRStereo::ApplyHeadTracking(DFLOAT &fYaw, DFLOAT &fPitch)
{
	if (!IsHeadTracking())
	{
		m_bHaveBody = DFALSE;
		return;
	}

	// Body yaw: where your feet point.  The mouse (and anything else in the
	// game that turns you, e.g. respawning) changes the game's yaw between
	// our calls - fold that into the body, then add stick turning.

	if (!m_bHaveBody)
	{
		m_fBodyYaw		= fYaw - m_fHeadYaw;
		m_fLastOutYaw	= fYaw;
		m_bHaveBody		= DTRUE;
	}
	m_fBodyYaw += (fYaw - m_fLastOutYaw);
	m_fBodyYaw += StickTurn();

	// Aim: the right hand if you have one (VRAimMode 1), otherwise the head.
	// Pitch never comes from the mouse - tilting the world is a quick way to feel sick.

	DFLOAT fAimYaw = m_fHeadYaw, fAimPitch = m_fHeadPitch;
	if (UsesControllerAim())
	{
		DFLOAT fx = m_vAimF.x, fy = m_vAimF.y, fz = m_vAimF.z;
		fAimYaw		= (DFLOAT)atan2(fx, fz);
		fAimPitch	= (DFLOAT)-atan2(fy, (DFLOAT)sqrt(fx*fx + fz*fz));
		m_bAimByHand = DTRUE;
	}

	DFLOAT fMaxPitch = VR_PI * 0.5f - 0.1f;
	if (fAimPitch >  fMaxPitch) fAimPitch =  fMaxPitch;
	if (fAimPitch < -fMaxPitch) fAimPitch = -fMaxPitch;

	fYaw	= m_fBodyYaw + fAimYaw;
	fPitch	= fAimPitch;

	m_fLastOutYaw	= fYaw;
	m_bHeadApplied	= DTRUE;
}

void CVRStereo::RequestRecenter()
{
	if (!m_pShared || !m_pClientDE) return;

	InterlockedIncrement((long*)&m_pShared->recenterRequest);
	if (m_bBridgeAlive)	m_pClientDE->CPrint("VR: recentred.");
	else				m_pClientDE->CPrint("VR: the headset bridge isn't running.");
}

void CVRStereo::WriteGameBlock()
{
	if (!m_pShared) return;
	ShogoVRShared* s = m_pShared;

	InterlockedIncrement((long*)&s->gameSeq);		// odd: writing

	m_nGameBeat++;
	if (m_nGameBeat <= 0) m_nGameBeat = 1;
	s->gameHeartbeat	= m_nGameBeat;
	s->gameHwnd			= (unsigned int)m_nGameHwnd;
	s->usedPoseId		= m_nReportPoseId;
	s->viewMode			= m_nReportViewMode;
	s->swapEyes			= (m_vtSwapEyes.GetFloat(0.0f) != 0.0f) ? 1 : 0;
	s->renderTanX		= m_fReportTanX;
	s->renderTanY		= m_fReportTanY;
	s->eyeSeparation	= m_fReportSep;
	s->renderWidth		= m_nRenderW;
	s->renderHeight		= m_nRenderH;
	s->clientWidth		= m_nClientW;
	s->clientHeight		= m_nClientH;
	s->surfaceWidth		= m_nSurfW;
	s->surfaceHeight	= m_nSurfH;

	DBOOL bStripShown	= m_bHudStrip && (m_nReportViewMode == SHOGOVR_VIEW_TRACKED || m_nReportViewMode == SHOGOVR_VIEW_SCREEN);
	s->layoutFlags		= bStripShown ? SHOGOVR_LAYOUT_HUD : 0;
	s->eyeHeight		= (m_nReportViewMode == SHOGOVR_VIEW_FLAT) ? 0 : m_nEyeH;
	s->hudX				= m_nHudX;
	s->hudY				= m_nHudY;
	s->hudW				= m_nHudW;
	s->hudH				= m_nHudH;
	s->hudAngle			= VR_DEG2RAD(m_vtHudWidth.GetFloat(60.0f));
	s->hudDistance		= m_vtHudDepth.GetFloat(3.5f);
	s->screenAngle		= VR_DEG2RAD(m_vtMenuWidth.GetFloat(80.0f));

	InterlockedIncrement((long*)&s->gameSeq);		// even: done
}


// ======================================================================= //
//	Motion controllers
// ======================================================================= //

DBOOL CVRStereo::IsControllerActive()
{
	if (!m_pClientDE || !m_bBridgeAlive) return DFALSE;
	if (!(m_nCtrlFlags & SHOGOVR_CTRL_ACTIVE)) return DFALSE;
	if (m_vtControllers.GetFloat(1.0f) == 0.0f) return DFALSE;
	return IsActive();
}

DBOOL CVRStereo::UsesControllerAim()
{
	return IsControllerActive() && (m_nCtrlFlags & SHOGOVR_CTRL_RIGHT_POSE) && m_vtAimMode.GetFloat(1.0f) != 0.0f;
}

DBOOL CVRStereo::Hook_IsCommandOn(int nCommand)
{
	CVRStereo &s = g_VRStereo;
	if (s.m_pfnIsCommandOn && s.m_pfnIsCommandOn(nCommand)) return DTRUE;
	return s.VRCommandOn(nCommand);
}

DBOOL CVRStereo::VRCommandOn(int nCommand)
{
	if (!m_bGameplayInput || !IsControllerActive()) return DFALSE;

	DDWORD nHeld = m_nButtons & ~m_nSuppressed;
	switch (nCommand)
	{
		case COMMAND_ID_FIRING:	return (nHeld & SHOGOVR_BTN_FIRE) ? DTRUE : DFALSE;
		case COMMAND_ID_JUMP:	return (nHeld & SHOGOVR_BTN_JUMP) ? DTRUE : DFALSE;
		case COMMAND_ID_DUCK:	return m_bMech ? m_bCrouchLong : ((nHeld & SHOGOVR_BTN_CROUCH) ? DTRUE : DFALSE);
	}
	return DFALSE;
}

void CVRStereo::SetGameplayInput(DBOOL b)
{
	// Coming back from a menu with the trigger still down shouldn't fire.
	if (b && !m_bGameplayInput) m_nSuppressed = m_nButtons;
	m_bGameplayInput = b;
}

DBOOL CVRStereo::GetMoveAxes(DFLOAT &fForward, DFLOAT &fRight)
{
	fForward = fRight = 0.0f;
	if (!m_bGameplayInput || !IsControllerActive()) return DFALSE;

	// Round dead zone, then rescale so movement starts gently at its edge.
	const DFLOAT fDead = 0.15f;
	DFLOAT fLen = (DFLOAT)sqrt(m_fMoveX * m_fMoveX + m_fMoveY * m_fMoveY);
	if (fLen <= fDead) return DFALSE;

	DFLOAT fScale = ((fLen > 1.0f ? 1.0f : fLen) - fDead) / (1.0f - fDead) / fLen;
	fForward	= m_fMoveY * fScale;
	fRight		= m_fMoveX * fScale;

	// The game walks you relative to where you aim.  To walk where you look
	// (or where the other hand points) instead, turn the stick by the angle
	// between the two.
	int nDir = (int)m_vtMoveDir.GetFloat(0.0f);
	if (nDir != 0 && IsHeadTracking())
	{
		DFLOAT fAimYaw = UsesControllerAim() ? (DFLOAT)atan2(m_vAimF.x, m_vAimF.z) : m_fHeadYaw;
		DFLOAT fRefYaw = fAimYaw;
		if (nDir == 1)
			fRefYaw = m_fHeadYaw;
		else if (nDir == 2 && (m_nCtrlFlags & SHOGOVR_CTRL_LEFT_POSE))
			fRefYaw = (DFLOAT)atan2(m_vLeftF.x, m_vLeftF.z);

		DFLOAT fPhi = fRefYaw - fAimYaw;
		DFLOAT c = (DFLOAT)cos(fPhi), sn = (DFLOAT)sin(fPhi);
		DFLOAT x = fRight, y = fForward;
		fRight		= x * c + y * sn;
		fForward	= -x * sn + y * c;
	}
	return DTRUE;
}

// ======================================================================= //
//	Settings file
// ======================================================================= //

void CVRStereo::LoadSettingsFile(DBOOL bFirst)
{
	if (!m_pClientDE) return;

	// The game runs in its own folder; the mod and its settings are in ShogoVR\.
	if (!m_szIniPath[0])
	{
		char szDir[MAX_PATH];
		szDir[0] = 0;
		GetCurrentDirectoryA(MAX_PATH, szDir);
		_snprintf(m_szIniPath, sizeof(m_szIniPath) - 1, "%s\\ShogoVR\\ShogoVR.ini", szDir);
		m_szIniPath[sizeof(m_szIniPath) - 1] = 0;
	}

	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExA(m_szIniPath, GetFileExInfoStandard, &fa)) return;
	m_nIniTimeLo = fa.ftLastWriteTime.dwLowDateTime;
	m_nIniTimeHi = fa.ftLastWriteTime.dwHighDateTime;

	// [Game] holds console variables: "VRHudWidth=60" -> "VRHudWidth 60".
	// Only VR... names with plain numbers are accepted.
	static char s_buf[8192];
	DWORD nLen = GetPrivateProfileSectionA("Game", s_buf, sizeof(s_buf), m_szIniPath);
	if (nLen == 0) return;

	int nApplied = 0;
	char* p = s_buf;
	while (*p)
	{
		char* pNext = p + strlen(p) + 1;
		char* pEq = strchr(p, '=');
		if (pEq && (p[0] == 'V' || p[0] == 'v') && (p[1] == 'R' || p[1] == 'r'))
		{
			*pEq = 0;
			char* pVal = pEq + 1;
			while (*pVal == ' ' || *pVal == '\t') pVal++;

			DBOOL bOk = (strlen(p) < 40 && *pVal != 0 && strlen(pVal) < 20);
			char* q;
			for (q = p; bOk && *q; q++)		if (!isalnum((unsigned char)*q)) bOk = DFALSE;
			for (q = pVal; bOk && *q; q++)	if (!strchr("0123456789.-+eE", *q)) bOk = DFALSE;

			if (bOk)
			{
				char szCmd[96];
				_snprintf(szCmd, sizeof(szCmd) - 1, "%s %s", p, pVal);
				szCmd[sizeof(szCmd) - 1] = 0;
				m_pClientDE->RunConsoleString(szCmd);
				nApplied++;
			}
		}
		p = pNext;
	}

	if (!bFirst && nApplied) m_pClientDE->CPrint("VR: settings updated from ShogoVR.ini.");
}

void CVRStereo::CheckSettingsFile()
{
	if (!m_pClientDE || !m_szIniPath[0]) return;
	DFLOAT fNow = m_pClientDE->GetTime();
	if (fNow - m_fLastIniCheck < 1.0f && fNow >= m_fLastIniCheck) return;
	m_fLastIniCheck = fNow;

	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExA(m_szIniPath, GetFileExInfoStandard, &fa)) return;
	if (fa.ftLastWriteTime.dwLowDateTime != m_nIniTimeLo || fa.ftLastWriteTime.dwHighDateTime != m_nIniTimeHi)
	{
		LoadSettingsFile(DFALSE);
	}
}

DFLOAT CVRStereo::StickTurn()
{
	if (!m_bGameplayInput || !IsControllerActive()) return 0.0f;

	DFLOAT x = m_fTurnX, y = m_fTurnY;
	DFLOAT ax = (DFLOAT)fabs(x), ay = (DFLOAT)fabs(y);
	if (ay > 0.6f && ay > ax) return 0.0f;		// that's weapon switching, not turning

	DFLOAT fSnap = m_vtSnapTurn.GetFloat(45.0f);
	if (fSnap > 0.0f)
	{
		if (!m_bSnapLatched && ax > 0.7f)
		{
			m_bSnapLatched = DTRUE;
			return (x > 0.0f ? 1.0f : -1.0f) * VR_DEG2RAD(fSnap);
		}
		if (ax < 0.3f) m_bSnapLatched = DFALSE;
		return 0.0f;
	}

	const DFLOAT fDead = 0.2f;
	if (ax <= fDead) return 0.0f;
	DFLOAT fAmount = (ax - fDead) / (1.0f - fDead);
	if (fAmount > 1.0f) fAmount = 1.0f;
	return (x > 0.0f ? 1.0f : -1.0f) * fAmount * VR_DEG2RAD(m_vtTurnSpeed.GetFloat(150.0f)) * m_pClientDE->GetFrameTime();
}

int CVRStereo::PollWeaponCycle()
{
	if (!m_bGameplayInput || !IsControllerActive())
	{
		m_nWeaponLatch = 0;
		return 0;
	}

	DFLOAT y = m_fTurnY, ax = (DFLOAT)fabs(m_fTurnX), ay = (DFLOAT)fabs(m_fTurnY);
	if (m_nWeaponLatch == 0 && ay > 0.7f && ay > ax)
	{
		m_nWeaponLatch = (y > 0.0f) ? 1 : -1;
		return m_nWeaponLatch;
	}
	if (ay < 0.3f) m_nWeaponLatch = 0;
	return 0;
}

int CVRStereo::PollMenuDirection()
{
	if (!IsControllerActive())
	{
		m_nMenuDir = 0;
		return 0;
	}

	// Either stick, whichever is pushed further, acts as the arrow keys.
	DFLOAT x = m_fMoveX, y = m_fMoveY;
	if ((m_fTurnX * m_fTurnX + m_fTurnY * m_fTurnY) > (x * x + y * y)) { x = m_fTurnX; y = m_fTurnY; }

	int nDir = 0;
	if ((DFLOAT)fabs(y) > 0.6f && fabs(y) >= fabs(x))	nDir = (y > 0.0f) ? VK_UP : VK_DOWN;
	else if ((DFLOAT)fabs(x) > 0.6f)					nDir = (x > 0.0f) ? VK_RIGHT : VK_LEFT;

	DFLOAT fNow = m_pClientDE->GetTime();
	if (nDir == 0)
	{
		if (fabs(x) < 0.3f && fabs(y) < 0.3f) m_nMenuDir = 0;
		return 0;
	}
	if (nDir != m_nMenuDir)
	{
		m_nMenuDir = nDir;
		m_fMenuRepeatTime = fNow + 0.45f;
		return nDir;
	}
	if (fNow >= m_fMenuRepeatTime)
	{
		m_fMenuRepeatTime = fNow + 0.15f;
		return nDir;
	}
	return 0;
}

void CVRStereo::GetHandWorld(const DVector &vCamPos, DVector &vPos, DVector &vF, DVector &vU)
{
	CClientDE* pClientDE = m_pClientDE;

	DRotation rBody;
	DVector bU, bR, bF, vT;
	pClientDE->SetupEuler(&rBody, 0.0f, m_fBodyYaw, 0.0f);
	pClientDE->GetRotationVectors(&rBody, &bU, &bR, &bF);

	DFLOAT fScale = m_fWorldScaleUsed;
	VEC_MULSCALAR(vPos, bR, m_vRightP.x * fScale);
	VEC_MULSCALAR(vT,   bU, m_vRightP.y * fScale);	VEC_ADD(vPos, vPos, vT);
	VEC_MULSCALAR(vT,   bF, m_vRightP.z * fScale);	VEC_ADD(vPos, vPos, vT);
	VEC_ADD(vPos, vPos, vCamPos);

	VEC_MULSCALAR(vF, bR, m_vAimF.x);
	VEC_MULSCALAR(vT, bU, m_vAimF.y);	VEC_ADD(vF, vF, vT);
	VEC_MULSCALAR(vT, bF, m_vAimF.z);	VEC_ADD(vF, vF, vT);

	VEC_MULSCALAR(vU, bR, m_vAimU.x);
	VEC_MULSCALAR(vT, bU, m_vAimU.y);	VEC_ADD(vU, vU, vT);
	VEC_MULSCALAR(vT, bF, m_vAimU.z);	VEC_ADD(vU, vU, vT);
}

void CVRStereo::AdjustWeaponPose(DVector &vPos, DRotation &rRot, DBOOL bFootScale)
{
	if (!m_pClientDE || !m_bAimByHand) return;

	// The gun model is scaled about its own origin, so scale the nudge too -
	// that keeps the grip where it was in your hand when you change the size.
	DFLOAT fGunScale = bFootScale ? m_vtGunScale.GetFloat(1.5f) : m_vtGunScaleMCA.GetFloat(1.0f);
	if (fGunScale < 0.2f) fGunScale = 0.2f;
	if (fGunScale > 5.0f) fGunScale = 5.0f;

	// vPos arrives as the camera position; the gun goes to the right hand,
	// pointing where the hand points.
	DVector vHandPos, vHandF, vHandU, hU, hR, hF, vT;
	GetHandWorld(vPos, vHandPos, vHandF, vHandU);
	m_pClientDE->AlignRotation(&rRot, &vHandF, &vHandU);
	m_pClientDE->GetRotationVectors(&rRot, &hU, &hR, &hF);

	VEC_COPY(vPos, vHandPos);
	VEC_MULSCALAR(vT, hR, m_vtGunX.GetFloat(0.0f) * fGunScale);	VEC_ADD(vPos, vPos, vT);
	VEC_MULSCALAR(vT, hU, m_vtGunY.GetFloat(0.0f) * fGunScale);	VEC_ADD(vPos, vPos, vT);
	VEC_MULSCALAR(vT, hF, m_vtGunZ.GetFloat(0.0f) * fGunScale);	VEC_ADD(vPos, vPos, vT);
}

void CVRStereo::ScaleWeapon(HLOCALOBJ hWeapon, DBOOL bFootScale)
{
	if (!m_pClientDE || !hWeapon) return;

	// Shogo's arms and guns were sized to sit in the corner of a flat screen;
	// held in your hand in VR they look small, so they can be enlarged.
	DFLOAT fScale = 1.0f;
	if (m_bAimByHand)
	{
		fScale = bFootScale ? m_vtGunScale.GetFloat(1.5f) : m_vtGunScaleMCA.GetFloat(1.0f);
		if (fScale < 0.2f) fScale = 0.2f;
		if (fScale > 5.0f) fScale = 5.0f;
	}

	DVector vScale;
	VEC_SET(vScale, fScale, fScale, fScale);
	m_pClientDE->SetObjectScale(hWeapon, &vScale);
	m_fGunScaleUsed = fScale;
}

void CVRStereo::CheckRenderer(DBOOL bStable)
{
	// If the renderer rebuilt itself while the window was tiny or minimized,
	// it can carry on drawing a smaller picture inside the full-size window -
	// which in the headset looks like the world shrinking and the field of
	// view ballooning.  Re-applying the screen mode makes it draw at full size.

	if (!m_pClientDE || !IsActive() || !m_bBridgeAlive || m_vtFixRenderer.GetFloat(1.0f) == 0.0f) return;
	if (m_nRenderW <= 0 || m_nRenderH <= 0 || m_nSurfW <= 0 || m_nSurfH <= 0) return;

	DFLOAT fNow = m_pClientDE->GetTime();
	DBOOL bWrong = (m_nSurfW != m_nRenderW || m_nSurfH != m_nRenderH);
	DBOOL bWindowReady = (m_nClientW == m_nRenderW && m_nClientH == m_nRenderH) &&
						 !(m_nGameHwnd && IsIconic((HWND)m_nGameHwnd));

	if (!bWrong || !bStable || !bWindowReady)
	{
		m_fRendererBadSince = -1.0f;		// fine, or wait until the window is right again
		return;
	}

	if (m_fRendererBadSince < 0.0f)
	{
		m_fRendererBadSince = fNow;
		return;
	}
	if (fNow - m_fRendererBadSince < 1.5f) return;

	// Don't loop if it doesn't help: at most 3 resets a minute.
	if (fNow - m_fLastRendererReset > 60.0f) m_nRendererResets = 0;
	if (m_nRendererResets >= 3) return;

	RMode mode;
	memset(&mode, 0, sizeof(mode));
	if (m_pClientDE->GetRenderMode(&mode) != LT_OK) return;

	Log("Renderer was drawing at %ldx%ld instead of %ldx%ld - resetting it", m_nSurfW, m_nSurfH, m_nRenderW, m_nRenderH);
	m_pClientDE->CPrint("VR: Shogo was drawing at %dx%d instead of %dx%d - resetting the renderer.",
						(int)m_nSurfW, (int)m_nSurfH, (int)m_nRenderW, (int)m_nRenderH);
	m_pClientDE->SetRenderMode(&mode);

	m_nRendererResets++;
	m_fLastRendererReset = fNow;
	m_fRendererBadSince = -1.0f;
}

void CVRStereo::UpdateAimMarker(HLOCALOBJ hCamera, HLOCALOBJ* pFilterList)
{
	if (!m_pClientDE || !hCamera) return;
	CClientDE* pClientDE = m_pClientDE;

	DBOOL bShow = m_bAimByHand && m_vtAimMarker.GetFloat(1.0f) != 0.0f;
	if (!bShow)
	{
		if (m_hAimMarker) pClientDE->SetObjectFlags(m_hAimMarker, 0);
		return;
	}

	if (!m_hAimMarker)
	{
		// Same sprite and settings the game uses for its third-person crosshair.
		ObjectCreateStruct theStruct;
		INIT_OBJECTCREATESTRUCT(theStruct);
		theStruct.m_ObjectType = OT_SPRITE;
		strncpy(theStruct.m_Filename, "Sprites\\Crosshair.spr", sizeof(theStruct.m_Filename) - 1);
		theStruct.m_Flags = FLAG_VISIBLE | FLAG_GLOWSPRITE | FLAG_NOLIGHT;
		m_hAimMarker = pClientDE->CreateObject(&theStruct);
		if (!m_hAimMarker) return;

		DVector vScale;
		VEC_SET(vScale, 0.5f, 0.5f, 1.0f);
		pClientDE->SetObjectScale(m_hAimMarker, &vScale);
	}

	// Cast from the hand along the gun.
	DVector vCamPos, vHandPos, vHandF, vHandU;
	pClientDE->GetObjectPos(hCamera, &vCamPos);
	GetHandWorld(vCamPos, vHandPos, vHandF, vHandU);
	VEC_NORM(vHandF);

	ClientIntersectQuery query;
	ClientIntersectInfo info;
	VEC_COPY(query.m_From, vHandPos);
	VEC_MULSCALAR(query.m_To, vHandF, VR_AIM_RANGE);
	VEC_ADD(query.m_To, query.m_To, vHandPos);
	VEC_COPY(query.m_Direction, vHandF);
	query.m_Flags		= INTERSECT_OBJECTS | IGNORE_NONSOLID;
	query.m_FilterFn	= pFilterList ? ObjListFilterFn : DNULL;
	query.m_pUserData	= pFilterList;

	DVector vHit;
	DFLOAT fDist = VR_AIM_RANGE;
	if (pClientDE->IntersectSegment(&query, &info))
	{
		DVector vDiff;
		VEC_SUB(vDiff, info.m_Point, vHandPos);
		fDist = VEC_MAG(vDiff);
	}

	// Pull it slightly towards you so it doesn't sink into walls.
	DFLOAT fPlace = (fDist > 20.0f) ? fDist - 8.0f : fDist * 0.6f;
	VEC_MULSCALAR(vHit, vHandF, fPlace);
	VEC_ADD(vHit, vHit, vHandPos);

	pClientDE->SetObjectPos(m_hAimMarker, &vHit);
	pClientDE->SetObjectFlags(m_hAimMarker, FLAG_VISIBLE | FLAG_GLOWSPRITE | FLAG_NOLIGHT);
}

void CVRStereo::OnExitWorld()
{
	if (m_pClientDE && m_hAimMarker)
	{
		m_pClientDE->DeleteObject(m_hAimMarker);
	}
	m_hAimMarker = DNULL;
	if (m_pClientDE && m_hBody)
	{
		m_pClientDE->DeleteObject(m_hBody);
	}
	m_hBody = DNULL;
	m_nBodyModelId = -1;
	if (m_pClientDE && m_hHeldGun)
	{
		m_pClientDE->DeleteObject(m_hHeldGun);
	}
	m_hHeldGun = DNULL;
	m_nHeldGunId = -1;
	m_hHiddenPV = DNULL;
	m_bHaveBody = DFALSE;
}


// ======================================================================= //
//	Two-handed aiming
// ======================================================================= //

// Hold your other hand out in front of the gun hand, roughly along the
// barrel, and the gun points from your gun hand towards it - steadier, and
// how you'd hold a rifle.  Lets go when the hands come apart or off-line.
void CVRStereo::UpdateTwoHanded()
{
	DBOOL bWas = m_bTwoHanded;
	m_bTwoHanded = DFALSE;
	VEC_COPY(m_vAimF, m_vRightF);
	VEC_COPY(m_vAimU, m_vRightU);

	if (m_vtTwoHand.GetFloat(1.0f) == 0.0f) return;
	if (!(m_nCtrlFlags & SHOGOVR_CTRL_RIGHT_POSE) || !(m_nCtrlFlags & SHOGOVR_CTRL_LEFT_POSE)) return;

	DVector vD;
	VEC_SUB(vD, m_vLeftP, m_vRightP);
	DFLOAT fDist = VEC_MAG(vD);
	if (fDist < 0.12f || fDist > 0.9f) return;		// metres

	DVector vDir;
	VEC_MULSCALAR(vDir, vD, 1.0f / fDist);
	DFLOAT fCos = VEC_DOT(vDir, m_vRightF);
	DFLOAT fNeed = bWas ? 0.766f : 0.906f;			// enter within 25 degrees, stay within 40
	if (fCos < fNeed) return;

	m_bTwoHanded = DTRUE;
	VEC_COPY(m_vAimF, vDir);

	// Keep the gun's roll from the gun hand.
	DVector vT;
	DFLOAT fUp = VEC_DOT(vDir, m_vRightU);
	VEC_MULSCALAR(vT, vDir, fUp);
	VEC_SUB(m_vAimU, m_vRightU, vT);
	DFLOAT fLen = VEC_MAG(m_vAimU);
	if (fLen > 0.001f)
	{
		VEC_MULSCALAR(m_vAimU, m_vAimU, 1.0f / fLen);
	}
	else
	{
		VEC_COPY(m_vAimU, m_vRightU);
	}
}

// ======================================================================= //
//	Body
// ======================================================================= //

// Hides the parts of a model whose names say head (so you don't see the
// inside of your own head) and, optionally, arms (the engine can't bend
// them to follow your controllers).  Returns how many nodes are hidden.
int CVRStereo::HideModelNodes(HLOCALOBJ hObj, DBOOL bHead, DBOOL bArms)
{
	if (!m_pClientDE || !hObj) return 0;

	static const char* s_head[] = { "head", "neck", "helm", "face", "hair", "eye", "jaw", "mouth", "skull", "visor", 0 };
	static const char* s_arms[] = { "arm", "hand", "finger", "thumb", "elbow", "wrist", "shoulder", "bicep", "clav",
									"palm", "gun", "weapon", "rifle", 0 };
	int nHidden = 0;
	HMODELNODE hNode = INVALID_MODEL_NODE;
	while (m_pClientDE->GetNextModelNode(hObj, hNode, &hNode) == LT_OK)
	{
		char szName[64], szLower[64];
		szName[0] = 0;
		m_pClientDE->GetModelNodeName(hObj, hNode, szName, sizeof(szName));
		int i;
		for (i = 0; szName[i] && i < 63; i++) szLower[i] = (char)tolower((unsigned char)szName[i]);
		szLower[i] = 0;

		DBOOL bIsHead = DFALSE, bIsArm = DFALSE;
		for (i = 0; s_head[i]; i++) if (strstr(szLower, s_head[i])) bIsHead = DTRUE;
		for (i = 0; s_arms[i]; i++) if (strstr(szLower, s_arms[i])) bIsArm = DTRUE;

		DBOOL bHide = (bHead && bIsHead) || (bArms && bIsArm);
		m_pClientDE->SetModelNodeHideStatus(hObj, szName, bHide);
		if (bHide) nHidden++;
	}
	return nHidden;
}

void CVRStereo::SetAllNodesHidden(HLOCALOBJ hObj, DBOOL bHide)
{
	if (!m_pClientDE || !hObj) return;
	HMODELNODE hNode = INVALID_MODEL_NODE;
	while (m_pClientDE->GetNextModelNode(hObj, hNode, &hNode) == LT_OK)
	{
		char szName[64];
		szName[0] = 0;
		m_pClientDE->GetModelNodeName(hObj, hNode, szName, sizeof(szName));
		m_pClientDE->SetModelNodeHideStatus(hObj, szName, bHide);
	}
}

// Hides (or shows) the arm and hand parts of a first-person weapon model -
// by part name, so it works for every weapon whose arms are separate parts.
int CVRStereo::SetArmNodesHidden(HLOCALOBJ hObj, DBOOL bHide)
{
	if (!m_pClientDE || !hObj) return 0;
	static const char* s_arms[] = { "arm", "hand", "finger", "thumb", "elbow", "wrist", "shoulder", "bicep", "clav",
									"palm", "glove", "sleeve", "knuckle", 0 };
	int nCount = 0;
	HMODELNODE hNode = INVALID_MODEL_NODE;
	while (m_pClientDE->GetNextModelNode(hObj, hNode, &hNode) == LT_OK)
	{
		char szName[64], szLower[64];
		szName[0] = 0;
		m_pClientDE->GetModelNodeName(hObj, hNode, szName, sizeof(szName));
		int i;
		for (i = 0; szName[i] && i < 63; i++) szLower[i] = (char)tolower((unsigned char)szName[i]);
		szLower[i] = 0;
		for (i = 0; s_arms[i]; i++)
		{
			if (strstr(szLower, s_arms[i]))
			{
				m_pClientDE->SetModelNodeHideStatus(hObj, szName, bHide);
				nCount++;
				break;
			}
		}
	}
	return nCount;
}

// Appends a weapon model's part names to ShogoVR\ShogoVR_models.txt - which
// parts exist decides what can be hidden; send this file in if the arms on
// some gun don't disappear.
void CVRStereo::DumpModelNodes(HLOCALOBJ hObj, int nWeaponId)
{
	if (!m_pClientDE || !hObj || !m_szIniPath[0]) return;
	char szPath[320];
	strncpy(szPath, m_szIniPath, sizeof(szPath) - 1);
	szPath[sizeof(szPath) - 1] = 0;
	char* pSlash = strrchr(szPath, '\\');
	if (!pSlash) return;
	strcpy(pSlash + 1, "ShogoVR_models.txt");

	FILE* f = fopen(szPath, "a");
	if (!f) return;
	fprintf(f, "Weapon %d (first-person model %s):", nWeaponId, GetPVModelName(nWeaponId) ? GetPVModelName(nWeaponId) : "?");
	HMODELNODE hNode = INVALID_MODEL_NODE;
	int n = 0;
	while (m_pClientDE->GetNextModelNode(hObj, hNode, &hNode) == LT_OK)
	{
		char szName[64];
		szName[0] = 0;
		m_pClientDE->GetModelNodeName(hObj, hNode, szName, sizeof(szName));
		fprintf(f, "%s %s", n++ ? "," : "", szName);
	}
	fprintf(f, "  (%d parts)\n", n);
	fclose(f);
}

// ======================================================================= //
//	Transforming (mech vehicle mode)
// ======================================================================= //

void CVRStereo::Log(const char* szFormat, ...)
{
	if (!m_szIniPath[0]) return;
	static int s_bStarted = 0;
	char szPath[320];
	strncpy(szPath, m_szIniPath, sizeof(szPath) - 1);
	szPath[sizeof(szPath) - 1] = 0;
	char* pSlash = strrchr(szPath, '\\');
	if (!pSlash) return;
	strcpy(pSlash + 1, "ShogoVR_game.log");

	FILE* f = fopen(szPath, s_bStarted ? "a" : "w");		// a fresh log each time the game starts
	if (!f) return;
	s_bStarted = 1;
	time_t t = time(NULL);
	struct tm* pTm = localtime(&t);
	if (pTm) fprintf(f, "[%02d:%02d:%02d] ", pTm->tm_hour, pTm->tm_min, pTm->tm_sec);
	va_list args;
	va_start(args, szFormat);
	vfprintf(f, szFormat, args);
	va_end(args);
	fprintf(f, "\n");
	fclose(f);
}

// However Shogo was started (Shogo.exe, Steam, the Shogo VR shortcut), make
// sure the headset bridge runs: start it - without a window, so it can't
// take focus from the game - unless it's already running.
void CVRStereo::StartBridgeIfNeeded()
{
	if (m_vtAutoBridge.GetFloat(1.0f) == 0.0f || !m_szIniPath[0]) return;

	HANDLE hRunning = OpenMutexA(SYNCHRONIZE, FALSE, "Local\\ShogoVRBridge_Running");
	if (hRunning)
	{
		CloseHandle(hRunning);
		Log("Headset bridge already running");
		return;
	}

	char szDir[320];
	strncpy(szDir, m_szIniPath, sizeof(szDir) - 1);
	szDir[sizeof(szDir) - 1] = 0;
	char* pSlash = strrchr(szDir, '\\');
	if (!pSlash) return;
	*pSlash = 0;

	char szExe[400];
	_snprintf(szExe, sizeof(szExe) - 1, "%s\\ShogoVRBridge.exe", szDir);
	szExe[sizeof(szExe) - 1] = 0;
	if (GetFileAttributesA(szExe) == INVALID_FILE_ATTRIBUTES)
	{
		Log("Headset bridge not found at %s", szExe);
		return;
	}

	char szCmd[440];
	_snprintf(szCmd, sizeof(szCmd) - 1, "\"%s\" --game-pid %lu", szExe, (unsigned long)GetCurrentProcessId());
	szCmd[sizeof(szCmd) - 1] = 0;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	si.cb = sizeof(si);
	if (CreateProcessA(szExe, szCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, szDir, &si, &pi))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		m_bStartedBridge = DTRUE;
		Log("Started the headset bridge");
	}
	else
	{
		Log("Couldn't start the headset bridge (error %lu)", GetLastError());
	}
}

// Remember exactly how Shogo was started, so the Shogo VR shortcut can start
// it the same way (whatever Shogo's own launcher passes on this PC).
void CVRStereo::RememberCommandLine()
{
	const char* szCmd = GetCommandLineA();
	if (!szCmd || !m_szIniPath[0]) return;
	Log("Command line: %s", szCmd);
	if (strstr(szCmd, "+VRLauncher")) return;			// the shortcut started us - nothing new to learn

	// Skip the program's own path (quoted or not).
	const char* p = szCmd;
	while (*p == ' ') p++;
	if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
	else { while (*p && *p != ' ') p++; }
	while (*p == ' ') p++;
	if (!*p) return;

	// A leading '!' keeps the ini reader from stripping quotes off the ends.
	char szValue[2048];
	_snprintf(szValue, sizeof(szValue) - 1, "!%s", p);
	szValue[sizeof(szValue) - 1] = 0;
	WritePrivateProfileStringA("Launch", "GameArgs", szValue, m_szIniPath);
}

void CVRStereo::DrawOpeningNotice(void* pFont)
{
	if (!m_pClientDE || !pFont || m_bNoticeDone) return;
	CClientDE* pClientDE = m_pClientDE;

	DFLOAT fNow = pClientDE->GetTime();
	if (m_fNoticeStart < 0.0f) m_fNoticeStart = fNow;
	if (fNow - m_fNoticeStart > 12.0f)
	{
		int i;
		for (i = 0; i < 2; i++) { if (m_hNotice[i]) pClientDE->DeleteSurface(m_hNotice[i]); m_hNotice[i] = DNULL; }
		m_bNoticeDone = DTRUE;
		return;
	}

	if (!m_hNotice[0])
	{
		char szLine1[300];
		_snprintf(szLine1, sizeof(szLine1) - 1, "SHOGO VR - unofficial VR mod by %s", m_szAuthors[0] ? m_szAuthors : "its creators");
		szLine1[sizeof(szLine1) - 1] = 0;
		m_hNotice[0] = CTextHelper::CreateSurfaceFromString(pClientDE, (CBitmapFont*)pFont, szLine1);
		m_hNotice[1] = CTextHelper::CreateSurfaceFromString(pClientDE, (CBitmapFont*)pFont,
			(char*)"THIS LEVEL IS NOT MADE BY OR SUPPORTED BY Monolith Productions, or any of its affiliates and subsidiaries.");
		if (!m_hNotice[0] || !m_hNotice[1]) return;
	}

	HSURFACE hScreen = pClientDE->GetScreenSurface();
	DDWORD nScreenW = 0, nScreenH = 0, w[2] = { 0, 0 }, h[2] = { 0, 0 };
	pClientDE->GetSurfaceDims(hScreen, &nScreenW, &nScreenH);
	pClientDE->GetSurfaceDims(m_hNotice[0], &w[0], &h[0]);
	pClientDE->GetSurfaceDims(m_hNotice[1], &w[1], &h[1]);

	// A dark band along the bottom so it reads over any menu background.
	int nBandH = (int)(h[0] + h[1]) + 16;
	DRect rcBand;
	rcBand.left = 0;
	rcBand.top = (int)nScreenH - nBandH;
	rcBand.right = (int)nScreenW;
	rcBand.bottom = (int)nScreenH;
	pClientDE->FillRect(hScreen, &rcBand, pClientDE->SetupColor1(0.0f, 0.0f, 0.0f, DFALSE));

	int y = rcBand.top + 6, i;
	for (i = 0; i < 2; i++)
	{
		pClientDE->DrawSurfaceToSurfaceTransparent(hScreen, m_hNotice[i], DNULL, ((int)nScreenW - (int)w[i]) / 2, y, DNULL);
		y += (int)h[i] + 4;
	}
}

void CVRStereo::SetMechMode(DBOOL bMech, DBOOL bVehicle)
{
	if (bMech != m_bMech || bVehicle != m_bVehicle)
	{
		Log("Player mode: %s", !bMech ? "on foot" : (bVehicle ? "mech, vehicle mode" : "mech, walking"));
	}
	m_bMech = bMech;
	m_bVehicle = bVehicle;
}

DBOOL CVRStereo::EasyLadders()
{
	return m_vtEasyLadders.GetFloat(1.0f) != 0.0f;
}

static void VRSendScanCode(DDWORD nScan, DBOOL bExtended, DBOOL bUp)
{
	INPUT in;
	ZeroMemory(&in, sizeof(in));
	in.type = INPUT_KEYBOARD;
	in.ki.wScan = (WORD)nScan;
	in.ki.dwFlags = KEYEVENTF_SCANCODE | (bExtended ? KEYEVENTF_EXTENDEDKEY : 0) | (bUp ? KEYEVENTF_KEYUP : 0);
	SendInput(1, &in, sizeof(INPUT));
}

// Key name as the game's input system reports it -> keyboard scan code.
static DBOOL VRKeyNameToScan(const char* szName, DDWORD &nScan, DBOOL &bExtended)
{
	if (szName[0] == '#' && szName[1] == '#')			// "##30" style: the code itself
	{
		int n = atoi(szName + 2);
		if (n <= 0 || n > 255) return DFALSE;
		nScan = (DDWORD)(n & 0x7F);
		bExtended = (n & 0x80) ? DTRUE : DFALSE;
		return DTRUE;
	}

	static const struct { const char* szKey; DDWORD nScan; int bExt; } s_named[] =
	{
		{ "Left Shift", 0x2A, 0 }, { "Right Shift", 0x36, 0 }, { "Left Ctrl", 0x1D, 0 }, { "Right Ctrl", 0x1D, 1 },
		{ "Left Alt", 0x38, 0 }, { "Right Alt", 0x38, 1 }, { "Space", 0x39, 0 }, { "Enter", 0x1C, 0 },
		{ "Return", 0x1C, 0 }, { "Tab", 0x0F, 0 }, { "Caps Lock", 0x3A, 0 }, { "Backspace", 0x0E, 0 }, { 0, 0, 0 }
	};
	int i;
	for (i = 0; s_named[i].szKey; i++)
	{
		if (_stricmp(szName, s_named[i].szKey) == 0)
		{
			nScan = s_named[i].nScan;
			bExtended = s_named[i].bExt ? DTRUE : DFALSE;
			return DTRUE;
		}
	}

	// Everything else: DirectInput names keys the way Windows does.
	int nExt, nCode;
	char szKey[64];
	for (nExt = 0; nExt < 2; nExt++)
	{
		for (nCode = 1; nCode < 128; nCode++)
		{
			if (GetKeyNameTextA((nCode << 16) | (nExt << 24), szKey, sizeof(szKey)) > 0 && _stricmp(szKey, szName) == 0)
			{
				nScan = (DDWORD)nCode;
				bExtended = nExt ? DTRUE : DFALSE;
				return DTRUE;
			}
		}
	}
	return DFALSE;
}

DBOOL CVRStereo::FindTransformKey(DDWORD &nScan, DBOOL &bExtended)
{
	if (!m_pClientDE) return DFALSE;
	char szName[INPUTNAME_LEN];
	szName[0] = 0;

	DeviceBinding* pBindings = m_pClientDE->GetDeviceBindings(DEVICETYPE_KEYBOARD);
	DeviceBinding* p;
	for (p = pBindings; p && !szName[0]; p = p->pNext)
	{
		GameAction* pAction;
		for (pAction = p->pActionHead; pAction; pAction = pAction->pNext)
		{
			if (pAction->nActionCode == COMMAND_ID_VEHICLETOGGLE)
			{
				strncpy(szName, p->strTriggerName, sizeof(szName) - 1);
				szName[sizeof(szName) - 1] = 0;
				break;
			}
		}
	}
	if (pBindings) m_pClientDE->FreeDeviceBindings(pBindings);

	return szName[0] ? VRKeyNameToScan(szName, nScan, bExtended) : DFALSE;
}

void CVRStereo::RequestTransform()
{
	if (!m_pClientDE || m_bTransformKeyDown) return;

	DDWORD nScan = 0;
	DBOOL bExt = DFALSE;
	if (!FindTransformKey(nScan, bExt))
	{
		if (!m_bWarnedNoTransformKey)
		{
			m_pClientDE->CPrint("VR: to transform with the controller, bind a key to 'Vehicle mode toggle' in Options > Keyboard.");
			m_bWarnedNoTransformKey = DTRUE;
		}
		return;
	}

	// Only into Shogo itself, never into another program.
	if (!m_nGameHwnd || GetForegroundWindow() != (HWND)m_nGameHwnd) return;

	VRSendScanCode(nScan, bExt, DFALSE);
	m_bTransformKeyDown	= DTRUE;
	m_nTransformScan	= nScan;
	m_bTransformExt		= bExt;
	m_nTransformUpTick	= GetTickCount() + 150;		// long enough for the game to see it
}

void CVRStereo::ReleaseTransformKey()
{
	if (!m_bTransformKeyDown) return;
	VRSendScanCode(m_nTransformScan, m_bTransformExt, DTRUE);
	m_bTransformKeyDown = DFALSE;
}

// In a mech: tap the crouch button to transform, hold it (0.35 s) to crouch.
void CVRStereo::UpdateMechCrouch()
{
	if (m_bTransformKeyDown && (long)(GetTickCount() - m_nTransformUpTick) >= 0) ReleaseTransformKey();

	DDWORD nHeld = m_nButtons & ~m_nSuppressed;
	if (!m_bMech || !m_bGameplayInput || !IsControllerActive())
	{
		m_nCrouchDownTick = 0;
		m_bCrouchLong = DFALSE;
		return;
	}

	if ((m_nPressed & ~m_nSuppressed) & SHOGOVR_BTN_TRANSFORM) RequestTransform();		// a button bound in SteamVR

	if (nHeld & SHOGOVR_BTN_CROUCH)
	{
		if (!m_nCrouchDownTick) m_nCrouchDownTick = GetTickCount() | 1;
		if (GetTickCount() - m_nCrouchDownTick >= 350) m_bCrouchLong = DTRUE;
	}
	else
	{
		if (m_nCrouchDownTick && !m_bCrouchLong) RequestTransform();		// a tap
		m_nCrouchDownTick = 0;
		m_bCrouchLong = DFALSE;
	}
}

void CVRStereo::SetBodyAnim(const char* szAnim, DBOOL bLoop)
{
	if (!m_hBody || strcmp(szAnim, m_szBodyAnim) == 0) return;

	HMODELANIM hAnim = m_pClientDE->GetAnimIndex(m_hBody, (char*)szAnim);
	if (hAnim == (HMODELANIM)-1)
	{
		// Not on this model - fall back to standing.
		hAnim = m_pClientDE->GetAnimIndex(m_hBody, (char*)"IR1");
		if (hAnim == (HMODELANIM)-1) return;
	}
	m_pClientDE->SetModelAnimation(m_hBody, hAnim);
	m_pClientDE->SetModelLooping(m_hBody, bLoop);
	strncpy(m_szBodyAnim, szAnim, sizeof(m_szBodyAnim) - 1);
	m_szBodyAnim[sizeof(m_szBodyAnim) - 1] = 0;
}

void CVRStereo::HideBody()
{
	if (m_pClientDE && m_hBody) m_pClientDE->SetObjectFlags(m_hBody, 0);
}

void CVRStereo::UpdateBody(HLOCALOBJ hPlayerObj, int nModelId, DBOOL bShow, DBOOL bCrouching)
{
	if (!m_pClientDE) return;
	CClientDE* pClientDE = m_pClientDE;

	bShow = bShow && hPlayerObj && m_vtBody.GetFloat(1.0f) != 0.0f && IsHeadTracking() && nModelId >= 0 &&
			!m_bVehicle;		// in vehicle mode the game shows its own mech model
	if (!bShow)
	{
		HideBody();
		m_bBodyVisYawSet = DFALSE;
		return;
	}

	// (Re)create when the player model changes (on foot, kid, each MCA).
	if (!m_hBody || nModelId != m_nBodyModelId)
	{
		if (m_hBody) pClientDE->DeleteObject(m_hBody);
		m_hBody = DNULL;

		ObjectCreateStruct theStruct;
		INIT_OBJECTCREATESTRUCT(theStruct);
		theStruct.m_ObjectType = OT_MODEL;
		strncpy(theStruct.m_Filename, GetModel((DBYTE)nModelId), sizeof(theStruct.m_Filename) - 1);
		strncpy(theStruct.m_SkinName, GetSkin((DBYTE)nModelId, UCA, MS_NORMAL, DFALSE), sizeof(theStruct.m_SkinName) - 1);
		theStruct.m_Flags = FLAG_VISIBLE | FLAG_MODELGOURAUDSHADE;
		pClientDE->GetObjectPos(hPlayerObj, &theStruct.m_Pos);
		m_hBody = pClientDE->CreateObject(&theStruct);
		if (!m_hBody) return;

		m_nBodyModelId = nModelId;
		m_szBodyAnim[0] = 0;
		VEC_COPY(m_vBodyLastPos, theStruct.m_Pos);
		VEC_INIT(m_vBodyVel);
		m_fBodyMaxSpeed = 1.0f;
		m_bBodyArmsShown = (m_vtBodyArms.GetFloat(0.0f) != 0.0f);
		m_nBodyHidden = HideModelNodes(m_hBody, DTRUE, !m_bBodyArmsShown);
	}

	DBOOL bArms = (m_vtBodyArms.GetFloat(0.0f) != 0.0f);
	if (bArms != m_bBodyArmsShown)
	{
		m_bBodyArmsShown = bArms;
		m_nBodyHidden = HideModelNodes(m_hBody, DTRUE, !bArms);
	}

	// Same size as the real player model.
	DVector vScale;
	VEC_SET(vScale, 1.0f, 1.0f, 1.0f);
	pClientDE->GetObjectScale(hPlayerObj, &vScale);
	pClientDE->SetObjectScale(m_hBody, &vScale);

	// Which way the body faces: follows your head, but only once you've
	// turned it more than ~35 degrees (like turning your shoulders).
	DFLOAT fHeadWorldYaw = m_fBodyYaw + m_fHeadYaw;
	if (!m_bBodyVisYawSet) { m_fBodyVisYaw = fHeadWorldYaw; m_bBodyVisYawSet = DTRUE; }
	DFLOAT fDiff = fHeadWorldYaw - m_fBodyVisYaw;
	while (fDiff >  VR_PI) fDiff -= 2.0f * VR_PI;
	while (fDiff < -VR_PI) fDiff += 2.0f * VR_PI;
	const DFLOAT fFree = 0.6f;
	if (fDiff >  fFree) m_fBodyVisYaw += fDiff - fFree;
	if (fDiff < -fFree) m_fBodyVisYaw += fDiff + fFree;

	// Movement: velocity of the real player object (smoothed).
	DVector vPos, vStep;
	pClientDE->GetObjectPos(hPlayerObj, &vPos);
	DFLOAT fDt = pClientDE->GetFrameTime();
	if (fDt > 0.0001f && fDt < 0.5f)
	{
		VEC_SUB(vStep, vPos, m_vBodyLastPos);
		VEC_MULSCALAR(vStep, vStep, 1.0f / fDt);
		DFLOAT k = fDt * 10.0f;
		if (k > 1.0f) k = 1.0f;
		m_vBodyVel.x += (vStep.x - m_vBodyVel.x) * k;
		m_vBodyVel.y += (vStep.y - m_vBodyVel.y) * k;
		m_vBodyVel.z += (vStep.z - m_vBodyVel.z) * k;
	}
	VEC_COPY(m_vBodyLastPos, vPos);

	DFLOAT fSin = (DFLOAT)sin(m_fBodyVisYaw), fCos = (DFLOAT)cos(m_fBodyVisYaw);
	DFLOAT fFwd = m_vBodyVel.x * fSin + m_vBodyVel.z * fCos;		// along the body's facing
	DFLOAT fSide = m_vBodyVel.x * fCos - m_vBodyVel.z * fSin;		// to its right
	DFLOAT fSpeed = (DFLOAT)sqrt(fFwd * fFwd + fSide * fSide);
	m_fBodyMaxSpeed *= 0.9995f;
	if (fSpeed > m_fBodyMaxSpeed) m_fBodyMaxSpeed = fSpeed;

	// Pick an animation - the same ones the game uses for characters.
	DBOOL bInAir = ((DFLOAT)fabs(m_vBodyVel.y) > 0.6f * m_fBodyMaxSpeed && m_fBodyMaxSpeed > 1.0f);
	DBOOL bMoving = fSpeed > 0.08f * m_fBodyMaxSpeed && fSpeed > 1.0f;
	DBOOL bRunning = fSpeed > 0.65f * m_fBodyMaxSpeed;
	int nDir = 0;									// 0 forward, 1 back, 2 left, 3 right
	if ((DFLOAT)fabs(fSide) > (DFLOAT)fabs(fFwd)) nDir = (fSide > 0.0f) ? 3 : 2;
	else nDir = (fFwd >= 0.0f) ? 0 : 1;

	static const char* s_walk[4]	= { "WR", "WRB", "WSLR", "WSRR" };
	static const char* s_run[4]		= { "RR", "RRB", "RSLR", "RSRR" };
	static const char* s_crouch[4]	= { "CWR", "CWRB", "CSLR", "CSRR" };

	if (bInAir)						SetBodyAnim(m_vBodyVel.y > 0.0f ? "JUMP_UP" : "JUMP_DOWN", DFALSE);
	else if (bCrouching && bMoving)	SetBodyAnim(s_crouch[nDir], DTRUE);
	else if (bCrouching)			SetBodyAnim("CR", DTRUE);
	else if (bMoving && bRunning)	SetBodyAnim(s_run[nDir], DTRUE);
	else if (bMoving)				SetBodyAnim(s_walk[nDir], DTRUE);
	else							SetBodyAnim("IR1", DTRUE);

	// Place it where the real player model is, nudged back so you look
	// down at your chest and legs rather than out of your own neck.
	DFLOAT fBack = m_vtBodyOffset.GetFloat(0.15f) * m_fWorldScaleUsed;
	DVector vBody;
	VEC_COPY(vBody, vPos);
	vBody.x -= fSin * fBack;
	vBody.z -= fCos * fBack;

	DRotation rRot;
	pClientDE->SetupEuler(&rRot, 0.0f, m_fBodyVisYaw, 0.0f);
	pClientDE->SetObjectPos(m_hBody, &vBody);
	pClientDE->SetObjectRotation(m_hBody, &rRot);
	pClientDE->SetObjectFlags(m_hBody, FLAG_VISIBLE | FLAG_MODELGOURAUDSHADE);
}

// "VRBodyInfo": lists the parts (nodes) of your body model and of the gun
// model, so hiding can be tuned for models whose parts are named differently.
static void PrintNodes(CClientDE* pClientDE, HLOCALOBJ hObj, const char* szWhat)
{
	if (!hObj) { pClientDE->CPrint("VR: %s: none", szWhat); return; }
	char szLine[512];
	szLine[0] = 0;
	int nCount = 0;
	HMODELNODE hNode = INVALID_MODEL_NODE;
	while (pClientDE->GetNextModelNode(hObj, hNode, &hNode) == LT_OK)
	{
		char szName[64];
		szName[0] = 0;
		pClientDE->GetModelNodeName(hObj, hNode, szName, sizeof(szName));
		DBOOL bHidden = DFALSE;
		pClientDE->GetModelNodeHideStatus(hObj, szName, &bHidden);
		char szItem[80];
		_snprintf(szItem, sizeof(szItem) - 1, "%s%s%s", nCount ? ", " : "", szName, bHidden ? "(hidden)" : "");
		szItem[sizeof(szItem) - 1] = 0;
		if (strlen(szLine) + strlen(szItem) > 120)
		{
			pClientDE->CPrint("VR: %s: %s", szWhat, szLine);
			szLine[0] = 0;
		}
		strcat(szLine, szItem);
		nCount++;
	}
	pClientDE->CPrint("VR: %s: %s (%d nodes)", szWhat, szLine, nCount);
}

void CVRStereo::PrintBodyInfo(HLOCALOBJ hWeaponModel)
{
	if (!m_pClientDE) return;
	m_pClientDE->CPrint("VR: body %s, animation %s, %d parts hidden, two-handed %s",
						m_hBody ? "on" : "off", m_szBodyAnim[0] ? m_szBodyAnim : "-", m_nBodyHidden,
						m_bTwoHanded ? "yes" : "no");
	PrintNodes(m_pClientDE, m_hBody, "body parts");
	PrintNodes(m_pClientDE, hWeaponModel, "gun parts");
}


// ======================================================================= //
//	Held gun: the third-person weapon model (no arms) in your hand
// ======================================================================= //

// The models other players see you holding (from the game's weapon
// definitions on the server side).  Their origin is the grip.
struct HeldGunModel { int nId; const char* szModel; const char* szSkin; };
static const HeldGunModel s_HeldGuns[] =
{
	{ GUN_PULSERIFLE_ID,	"Models\\Powerups\\PulseRifle.abc",		"Skins\\Powerups\\PulseRifle_a.dtx" },
	{ GUN_SHREDDER_ID,		"Models\\Powerups\\Shredder.abc",		"Skins\\Powerups\\Shredder_a.dtx" },
	{ GUN_BULLGUT_ID,		"Models\\Powerups\\Bullgut.abc",		"Skins\\Powerups\\Bullgut_a.dtx" },
	{ GUN_JUGGERNAUT_ID,	"Models\\Powerups\\Juggernaut.abc",		"Skins\\Powerups\\Juggernaut_a.dtx" },
	{ GUN_SPIDER_ID,		"Models\\Powerups\\Spider.abc",			"Skins\\Powerups\\Spider_a.dtx" },
	{ GUN_REDRIOT_ID,		"Models\\Powerups\\RedRiot.abc",		"Skins\\Powerups\\RedRiot_a.dtx" },
	{ GUN_SNIPERRIFLE_ID,	"Models\\Powerups\\SniperRifle.abc",	"Skins\\Powerups\\SniperRifle_a.dtx" },
	{ GUN_ENERGYBATON_ID,	"Models\\Powerups\\EnergyBaton.abc",	"Skins\\Powerups\\EnergyBaton.dtx" },
	{ GUN_ENERGYBLADE_ID,	"Models\\Powerups\\EnergyBlade.abc",	"Skins\\Powerups\\EnergyBlade.dtx" },
	{ GUN_KATANA_ID,		"Models\\Powerups\\Katana.abc",			"Skins\\Powerups\\Katana.dtx" },
	{ GUN_MONOKNIFE_ID,		"Models\\Powerups\\MonoKnife.abc",		"Skins\\Powerups\\MonoKnife.dtx" },
	{ GUN_COLT45_ID,		"Models\\Powerups\\Colt45.abc",			"Skins\\Powerups\\Colt45_a.dtx" },
	{ GUN_SHOTGUN_ID,		"Models\\Powerups\\Shotgun.abc",		"Skins\\Powerups\\Shotgun_a.dtx" },
	{ GUN_MAC10_ID,			"Models\\Powerups\\Machinegun.abc",		"Skins\\Powerups\\Machinegun_a.dtx" },
	{ GUN_ASSAULTRIFLE_ID,	"Models\\Powerups\\AssaultRifle.abc",	"Skins\\Powerups\\AssaultRifle_a.dtx" },
	{ GUN_ENERGYGRENADE_ID,	"Models\\Powerups\\EnergyGrenade.abc",	"Skins\\Powerups\\EnergyGrenade_a.dtx" },
	{ GUN_TOW_ID,			"Models\\Powerups\\TOW.abc",			"Skins\\Powerups\\TOW_a.dtx" },
	{ GUN_LASERCANNON_ID,	"Models\\Powerups\\LaserCannon.abc",	"Skins\\Powerups\\LaserCannon_a.dtx" },
	{ GUN_KATOGRENADE_ID,	"Models\\Powerups\\KatoGrenade.abc",	"Skins\\Powerups\\KatoGrenade_a.dtx" },
	{ GUN_TANTO_ID,			"Models\\Powerups\\Tanto.abc",			"Skins\\Powerups\\Tanto_a.dtx" },
	{ -1, 0, 0 }
};

void CVRStereo::UpdateHeldGun(HLOCALOBJ hPVModel, int nWeaponId, const DVector &vCamPos)
{
	if (!m_pClientDE) return;
	CClientDE* pClientDE = m_pClientDE;

	// Only when the hand aims, the option is on, and the game is actually
	// showing a gun right now (not mid-switch, not with view weapons off).
	DDWORD dwPVFlags = hPVModel ? pClientDE->GetObjectFlags(hPVModel) : 0;
	const HeldGunModel* pDef = DNULL;
	int i;
	for (i = 0; s_HeldGuns[i].nId >= 0; i++) if (s_HeldGuns[i].nId == nWeaponId) pDef = &s_HeldGuns[i];

	// The first-person model (with its arms) stays in place and "visible" as
	// far as the game is concerned - it still drives the muzzle flash and
	// where shots come from - but every part of it is hidden.  (Clearing
	// its visible flag instead would also hide the muzzle flash.)
	DBOOL bUseHeld = m_bAimByHand && m_vtGunModel.GetFloat(1.0f) != 0.0f && pDef;
	// Re-applied every frame: the game makes a new model on each weapon
	// switch, and we never touch an old (possibly deleted) one.
	if (bUseHeld)
	{
		if (hPVModel) SetAllNodesHidden(hPVModel, DTRUE);
		m_hHiddenPV = hPVModel;
	}
	else if (m_hHiddenPV)
	{
		if (m_hHiddenPV == hPVModel) SetAllNodesHidden(hPVModel, DFALSE);
		m_hHiddenPV = DNULL;
	}

	// First-person gun: list its parts once (ShogoVR_models.txt), and hide
	// Sanjuro's arms on it unless they're wanted.
	if (hPVModel && nWeaponId >= 0 && nWeaponId < 64 && !(m_nDumpedWeapons[nWeaponId / 32] & (1u << (nWeaponId % 32))))
	{
		m_nDumpedWeapons[nWeaponId / 32] |= (1u << (nWeaponId % 32));
		DumpModelNodes(hPVModel, nWeaponId);
	}
	DBOOL bHideArms = !bUseHeld && m_bAimByHand && m_vtGunArms.GetFloat(1.0f) == 0.0f;
	if (bHideArms && hPVModel)
	{
		SetArmNodesHidden(hPVModel, DTRUE);				// every frame: new model on each weapon switch
		m_hArmlessPV = hPVModel;
	}
	else if (m_hArmlessPV)
	{
		if (m_hArmlessPV == hPVModel && !bUseHeld) SetArmNodesHidden(hPVModel, DFALSE);
		m_hArmlessPV = DNULL;
	}

	DBOOL bShow = bUseHeld && (dwPVFlags & FLAG_VISIBLE);
	if (!bShow)
	{
		if (m_hHeldGun) pClientDE->SetObjectFlags(m_hHeldGun, 0);
		return;
	}

	if (!m_hHeldGun || nWeaponId != m_nHeldGunId)
	{
		if (m_hHeldGun) pClientDE->DeleteObject(m_hHeldGun);
		m_hHeldGun = DNULL;

		ObjectCreateStruct theStruct;
		INIT_OBJECTCREATESTRUCT(theStruct);
		theStruct.m_ObjectType = OT_MODEL;
		strncpy(theStruct.m_Filename, pDef->szModel, sizeof(theStruct.m_Filename) - 1);
		strncpy(theStruct.m_SkinName, pDef->szSkin, sizeof(theStruct.m_SkinName) - 1);
		theStruct.m_Flags = FLAG_VISIBLE | FLAG_MODELGOURAUDSHADE;
		VEC_COPY(theStruct.m_Pos, vCamPos);
		m_hHeldGun = pClientDE->CreateObject(&theStruct);
		if (!m_hHeldGun) return;
		m_nHeldGunId = nWeaponId;
	}

	// In your hand, pointing where you aim (one or two hands).
	DVector vHandPos, vHandF, vHandU, hU, hR, hF, vT;
	GetHandWorld(vCamPos, vHandPos, vHandF, vHandU);

	DRotation rRot;
	pClientDE->AlignRotation(&rRot, &vHandF, &vHandU);
	DFLOAT fPitch = m_vtHeldPitch.GetFloat(0.0f), fYaw = m_vtHeldYaw.GetFloat(0.0f), fRoll = m_vtHeldRoll.GetFloat(0.0f);
	if (fYaw != 0.0f)	pClientDE->EulerRotateY(&rRot, VR_DEG2RAD(fYaw));
	if (fPitch != 0.0f)	pClientDE->EulerRotateX(&rRot, VR_DEG2RAD(fPitch));
	if (fRoll != 0.0f)	pClientDE->EulerRotateZ(&rRot, VR_DEG2RAD(fRoll));

	pClientDE->GetRotationVectors(&rRot, &hU, &hR, &hF);
	VEC_MULSCALAR(vT, hR, m_vtHeldX.GetFloat(0.0f));	VEC_ADD(vHandPos, vHandPos, vT);
	VEC_MULSCALAR(vT, hU, m_vtHeldY.GetFloat(0.0f));	VEC_ADD(vHandPos, vHandPos, vT);
	VEC_MULSCALAR(vT, hF, m_vtHeldZ.GetFloat(0.0f));	VEC_ADD(vHandPos, vHandPos, vT);

	DFLOAT fScale = m_vtHeldScale.GetFloat(1.0f);
	if (fScale < 0.1f) fScale = 0.1f;
	DVector vScale;
	VEC_SET(vScale, fScale, fScale, fScale);

	pClientDE->SetObjectPos(m_hHeldGun, &vHandPos);
	pClientDE->SetObjectRotation(m_hHeldGun, &rRot);
	pClientDE->SetObjectScale(m_hHeldGun, &vScale);
	pClientDE->SetObjectFlags(m_hHeldGun, FLAG_VISIBLE | FLAG_MODELGOURAUDSHADE);
}
