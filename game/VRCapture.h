// ======================================================================= //
//
// MODULE  : VRCapture.h
//
// PURPOSE : Direct capture: hands each finished frame from the renderer
//           (dgVoodoo's Direct3D 11 output) to the headset bridge through a
//           shared GPU texture, so the picture isn't limited by the monitor.
//
// ======================================================================= //

#ifndef __VRCAPTURE_H
#define __VRCAPTURE_H

int		VRCapture_Install();		// 1 if the frame hook is in place
void	VRCapture_SetGame(void* hwnd, void* pShared, int bEnabled);		// pShared: the ShogoVRShared block
void	VRCapture_Uninstall();		// must run before the DLL unloads
int		VRCapture_Frames();			// frames handed over so far
int		VRCapture_Seen();			// frames seen from the game's window

#endif
