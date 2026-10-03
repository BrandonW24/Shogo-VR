// ======================================================================= //
//
// MODULE  : VRMenu.h
//
// PURPOSE : Options > vr settings - the VR mod's settings inside the game,
//           usable with the motion controllers (stick up/down to choose,
//           left/right to change).  Shares ShogoVR\ShogoVR.ini with the
//           desktop "Shogo VR Settings" window and the headset bridge.
//
// ======================================================================= //

#ifndef __VRMENU_H
#define __VRMENU_H

#include "BaseMenu.h"

#define VRMENU_ITEMS	26

class CVRMenu : public CBaseMenu
{
public:

	CVRMenu();

	virtual DBOOL		Init (CClientDE* pClientDE, CRiotMenu* pRiotMenu, CBaseMenu* pParent, int nScreenWidth, int nScreenHeight);
	virtual void		ScreenDimsChanged (int nScreenWidth, int nScreenHeight);
	virtual void		Reset();

	virtual DBOOL		LoadAllSurfaces()		{ return LoadSurfaces(); }
	virtual void		UnloadAllSurfaces()		{ UnloadSurfaces(); }

	virtual void		Left();
	virtual void		Right();
	virtual void		Return();

	virtual void		Draw (HSURFACE hScreen, int nScreenWidth, int nScreenHeight, int nTextOffset = 0);

protected:

	virtual DBOOL		LoadSurfaces();
	virtual void		UnloadSurfaces();
	virtual void		PostCalculateMenuDims();

	void				ReadValues();
	void				Change (int nItem, int nDir);
	void				WriteValue (int nItem);
	void				FormatValue (int nItem, char* szOut, int nOutSize);
	DBOOL				MakeValueSurface (int nItem);
	const char*			IniPath();

protected:

	int					m_nSecondColumn;
	GENERIC_ITEM		m_Values[VRMENU_ITEMS];
	float				m_fValue[VRMENU_ITEMS];
	char				m_szIni[300];
};

#endif
