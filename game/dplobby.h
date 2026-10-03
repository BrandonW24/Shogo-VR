// ======================================================================= //
//
// MODULE  : dplobby.h  (minimal replacement - Shogo VR mod)
//
// PURPOSE : Stand-in for the DirectX 6 SDK's DPLobby.h, which modern
//           Visual Studio / Windows SDK installs don't include.
//
//           Shogo only uses it in NetStart.cpp, to read the settings it
//           was given when launched from a DirectPlay lobby (a 1998
//           matchmaking feature that is never used today).  This header
//           declares just those pieces, with the same memory layout as
//           Microsoft's DirectPlay structures.
//
//           If you have the real DirectX SDK header you can use that
//           instead - delete this file.
//
// ======================================================================= //

#ifndef __SHOGO_MIN_DPLOBBY_H__
#define __SHOGO_MIN_DPLOBBY_H__

#if !defined(__DPLOBBY_INCLUDED__) && !defined(__WINE_DPLOBBY_H)

#include <windows.h>

// DPNAME - player name
typedef struct tagDPNAME
{
	DWORD	dwSize;
	DWORD	dwFlags;
	union
	{
		LPWSTR	lpszShortName;
		LPSTR	lpszShortNameA;
	};
	union
	{
		LPWSTR	lpszLongName;
		LPSTR	lpszLongNameA;
	};
} DPNAME, *LPDPNAME;

// DPSESSIONDESC2 - session description
typedef struct tagDPSESSIONDESC2
{
	DWORD	dwSize;
	DWORD	dwFlags;
	GUID	guidInstance;
	GUID	guidApplication;
	DWORD	dwMaxPlayers;
	DWORD	dwCurrentPlayers;
	union
	{
		LPWSTR	lpszSessionName;
		LPSTR	lpszSessionNameA;
	};
	union
	{
		LPWSTR	lpszPassword;
		LPSTR	lpszPasswordA;
	};
	DWORD	dwReserved1;
	DWORD	dwReserved2;
	DWORD	dwUser1;
	DWORD	dwUser2;
	DWORD	dwUser3;
	DWORD	dwUser4;
} DPSESSIONDESC2, *LPDPSESSIONDESC2;

// DPLCONNECTION - what the lobby hands the game at launch
typedef struct tagDPLCONNECTION
{
	DWORD				dwSize;
	DWORD				dwFlags;
	LPDPSESSIONDESC2	lpSessionDesc;
	LPDPNAME			lpPlayerName;
	GUID				guidSP;
	LPVOID				lpAddress;
	DWORD				dwAddressSize;
} DPLCONNECTION, *LPDPLCONNECTION;

// DPLCONNECTION flags
#define DPLCONNECTION_JOINSESSION		0x00000001
#define DPLCONNECTION_CREATESESSION		0x00000002

#endif

#endif // __SHOGO_MIN_DPLOBBY_H__
