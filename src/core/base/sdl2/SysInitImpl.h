//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// System Initialization and Uninitialization
//---------------------------------------------------------------------------
#ifndef SysInitImplH
#define SysInitImplH

//---------------------------------------------------------------------------
extern void TVPDumpHWException();

extern void TVPInitializeBaseSystems();

extern tjs_string TVPNativeProjectDir;
extern tjs_string TVPNativeDataPath;

extern bool TVPProjectDirSelected;
extern void TVPEnsureDataPathDirectory();

// Re-run project discovery for an in-process restart (libretro Reset); see the
// definition in SysInitImpl.cpp.
extern "C" void krkrsdl2_reset_project_dir_selection(void);


extern bool TVPExecuteUserConfig();

extern bool TVPTerminated;
extern bool TVPTerminateOnWindowClose;
extern bool TVPTerminateOnNoWindowStartup;
extern int TVPTerminateCode;

//---------------------------------------------------------------------------


#include "SysInitIntf.h"

#endif
