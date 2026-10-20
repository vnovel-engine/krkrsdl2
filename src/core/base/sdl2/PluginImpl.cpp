//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "Plugins" class implementation / Service for plug-ins
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include <algorithm>
#include <dirent.h>
#include <functional>
#include "ScriptMgnIntf.h"
#include "PluginImpl.h"
#include "StorageImpl.h"
#include "GraphicsLoaderImpl.h"

#include "MsgImpl.h"
#include "SysInitIntf.h"

#include "tjsHashSearch.h"
#include "EventIntf.h"
#include "TransIntf.h"
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "DebugIntf.h"
#ifdef KRKRSDL2_ENABLE_PLUGINS
#include "FuncStubs.h"
#endif
#include "tjs.h"

// Engine-provided replacements for game plugins (see BuiltinPlugins.cpp): a
// plugin the engine handles itself is never loaded from disk.
bool TVPIsBuiltinPlugin(const ttstr &name);
void TVPInstallBuiltinPlugin(const ttstr &name);
void TVPInstallBuiltinPlugins(void);

#ifdef _WIN32
#if 0
#ifdef TVP_SUPPORT_OLD_WAVEUNPACKER
	#include "oldwaveunpacker.h"
#endif
#endif

#pragma pack(push, 8)
	///  tvpsnd.h needs packing size of 8
	#include "tvpsnd.h"
#pragma pack(pop)

#if 0
#ifdef TVP_SUPPORT_KPI
	#include "kmp_pi.h"
#endif
#endif
#endif

#include "FilePathUtil.h"
#include "Application.h"
#include "SysInitImpl.h"
#include <set>


//---------------------------------------------------------------------------
// export table
//---------------------------------------------------------------------------
#ifdef KRKRSDL2_ENABLE_PLUGINS
static tTJSHashTable<ttstr, void *> TVPExportFuncs;
static bool TVPExportFuncsInit = false;
#endif
void TVPAddExportFunction(const char *name, void *ptr)
{
#ifdef KRKRSDL2_ENABLE_PLUGINS
	TVPExportFuncs.Add(name, ptr);
#endif
}
void TVPAddExportFunction(const tjs_char *name, void *ptr)
{
#ifdef KRKRSDL2_ENABLE_PLUGINS
	TVPExportFuncs.Add(name, ptr);
#endif
}
#ifdef KRKRSDL2_ENABLE_PLUGINS
static void TVPInitExportFuncs()
{
	if(TVPExportFuncsInit) return;
	TVPExportFuncsInit = true;


	// Export functions
	TVPExportFunctions();
}
//---------------------------------------------------------------------------
struct tTVPFunctionExporter : iTVPFunctionExporter
{
	bool TJS_INTF_METHOD QueryFunctions(const tjs_char **name, void **function,
		tjs_uint count);
	bool TJS_INTF_METHOD QueryFunctionsByNarrowString(const char **name,
		void **function, tjs_uint count);
} static TVPFunctionExporter;
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPFunctionExporter::QueryFunctions(const tjs_char **name, void **function,
		tjs_uint count)
{
	// retrieve function table by given name table.
	// return false if any function is missing.
	bool ret = true;
	ttstr tname;
	for(tjs_uint i = 0; i<count; i++)
	{
		tname = name[i];
		void ** ptr = TVPExportFuncs.Find(tname);
		if(ptr)
			function[i] = *ptr;
		else
			function[i] = NULL, ret= false;
	}
	return ret;
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPFunctionExporter::QueryFunctionsByNarrowString(
	const char **name, void **function, tjs_uint count)
{
	// retrieve function table by given name table.
	// return false if any function is missing.
	bool ret = true;
	ttstr tname;
	for(tjs_uint i = 0; i<count; i++)
	{
		tname = name[i];
		void ** ptr = TVPExportFuncs.Find(tname);
		if(ptr)
			function[i] = *ptr;
		else
			function[i] = NULL, ret= false;
	}
	return ret;
}
//---------------------------------------------------------------------------
extern "C" iTVPFunctionExporter * TVPGetFunctionExporter()
{
	// for external applications
	TVPInitExportFuncs();
    return &TVPFunctionExporter;
}
//---------------------------------------------------------------------------
#endif


//---------------------------------------------------------------------------
void TVPThrowPluginUnboundFunctionError(const char *funcname)
{
	TVPThrowExceptionMessage(TVPPluginUnboundFunctionError, funcname);
}
//---------------------------------------------------------------------------
void TVPThrowPluginUnboundFunctionError(const tjs_char *funcname)
{
	TVPThrowExceptionMessage(TVPPluginUnboundFunctionError, funcname);
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// implementation of IStorageProvider
//---------------------------------------------------------------------------
class tTVPStorageProvider : public ITSSStorageProvider
{
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppvObjOut)
	{
		if(!ppvObjOut) return E_INVALIDARG;

		*ppvObjOut = NULL;
		if(!memcmp(&iid, &IID_IUnknown, 16))
			*ppvObjOut = (IUnknown*)this;
		else if(!memcmp(&iid, &IID_ITSSStorageProvider, 16))
			*ppvObjOut = (ITSSStorageProvider*)this;

		if(*ppvObjOut)
		{
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef(void) { return 1; }
	ULONG STDMETHODCALLTYPE Release(void) { return 1; }

	HRESULT STDMETHODCALLTYPE GetStreamForRead(
		TSS_LPWSTR url,
		IUnknown * *stream);

	HRESULT STDMETHODCALLTYPE GetStreamForWrite(
		TSS_LPWSTR url,
		IUnknown * *stream) { return E_NOTIMPL; }

	HRESULT STDMETHODCALLTYPE GetStreamForUpdate(
		TSS_LPWSTR url,
		IUnknown * *stream) { return E_NOTIMPL; }
};
//---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE tTVPStorageProvider::GetStreamForRead(
		TSS_LPWSTR url,
		IUnknown * *stream)
{
	tTJSBinaryStream *stream0;
	try
	{
		stream0 = TVPCreateStream(url);
	}
	catch(...)
	{
		return E_FAIL;
	}

	IUnknown *istream = (IUnknown*)(IStream*)new tTVPIStreamAdapter(stream0);
	*stream = istream;

	return S_OK;
}
//---------------------------------------------------------------------------



#ifdef KRKRSDL2_ENABLE_PLUGINS
//---------------------------------------------------------------------------
// Plug-ins management
//---------------------------------------------------------------------------
struct tTVPPlugin
{
	ttstr Name;
	void *Instance = nullptr;

	tTVPPluginHolder *Holder = nullptr;

#ifdef _WIN32
	bool IsSusiePicturePlugin; // Susie picture plugins are managed in GraphicsLoaderImpl.cpp
	bool IsSusieArchivePlugin; // Susie archive plugins are managed in SusieArchive.cpp
#endif

	ITSSModule *TSSModule = nullptr;

#if 0
#ifdef TVP_SUPPORT_KPI
	KMPMODULE *KMPModule;
#endif
#endif

	tTVPV2LinkProc V2Link = nullptr;
	tTVPV2UnlinkProc V2Unlink = nullptr;


	tTVPGetModuleInstanceProc GetModuleInstance = nullptr;
#if 0
	tTVPGetModuleThreadModelProc GetModuleThreadModel = nullptr;
	tTVPShowConfigWindowProc ShowConfigWindow = nullptr;
	tTVPCanUnloadNowProc CanUnloadNow = nullptr;
#ifdef TVP_SUPPORT_OLD_WAVEUNPACKER
	tTVPCreateWaveUnpackerProc CreateWaveUnpacker = nullptr;
#endif

#ifdef TVP_SUPPORT_KPI
	pfnGetKMPModule GetKMPModule = nullptr
#endif
#endif

	std::vector<ttstr> SupportedExts;

	tTVPPlugin(const ttstr & name, ITSSStorageProvider *storageprovider);
	~tTVPPlugin();

	bool Uninit();
};

/* Plugins the engine simulates itself. Many games call Plugins.link()
 * unguarded for these Windows-only plugins; if the load is made to fail they
 * abort at startup. Treating them as successfully-linked no-ops keeps the
 * scripts running, while the engine supplies the actual behaviour (fonts,
 * frame limiting, file stats, LayerEx rendering, Ogg Vorbis decoding).
 *
 * Deliberately NOT listed: krkrsteam.dll. Fata Morgana links it inside a
 * try/catch and relies on the failure to set `global.Steam = void`. */
static bool TVPIsEngineStubPlugin(const ttstr &name)
{
	// The list of engine-simulated plugins is the overrides table
	// (data/overrides.txt): a record naming the plugin means the engine answers
	// the link itself instead of loading the Windows PE. Behaviour for those
	// that need setup is supplied in TVPInstallBuiltinPlugin(); the rest need
	// nothing beyond a successful link.
	return TVPIsBuiltinPlugin(name);
}
//---------------------------------------------------------------------------
tTVPPlugin::tTVPPlugin(const ttstr & name, ITSSStorageProvider *storageprovider)
{
	Name = name;

	Instance = NULL;
	Holder = new tTVPPluginHolder(name);
	std::string filename;
	if (TVPUtf16ToUtf8(filename, Holder->GetLocalName().AsStdString()))
	{
		if (TVPCheckExistentLocalFile(Holder->GetLocalName()))
		{
			Instance = SDL_LoadObject(filename.c_str());
		}
	}

	if(!Instance)
	{
		if (Holder != NULL)
		{
			delete Holder;
			Holder = NULL;
		}
		if (TVPIsEngineStubPlugin(name))
		{
			/* Silence: a stub is a normal, successful outcome (the plugin is
			 * linked as a no-op) and games link several, so a line per stub is
			 * boot noise, not information. */
			return; // successfully "linked" as a no-op
		}
		/* Unknown plugin: preserve KiriKiri semantics and throw so that games
		 * which guard the link with try/catch take their fallback path. */
		TVPThrowExceptionMessage(TVPCannotLoadPlugin, name);
	}

	try
	{
		// retrieve each functions
		V2Link = (tTVPV2LinkProc)
			SDL_LoadFunction(Instance, "V2Link");
		V2Unlink = (tTVPV2UnlinkProc)
			SDL_LoadFunction(Instance, "V2Unlink");

		GetModuleInstance = (tTVPGetModuleInstanceProc)
			SDL_LoadFunction(Instance, "GetModuleInstance");
#if 0
		GetModuleThreadModel = (tTVPGetModuleThreadModelProc)
			SDL_LoadFunction(Instance, "GetModuleThreadModel");
		ShowConfigWindow = (tTVPShowConfigWindowProc)
			SDL_LoadFunction(Instance, "ShowConfigWindow");
		CanUnloadNow = (tTVPCanUnloadNowProc)
			SDL_LoadFunction(Instance, "CanUnloadNow");
#ifdef TVP_SUPPORT_OLD_WAVEUNPACKER
		CreateWaveUnpacker = (tTVPCreateWaveUnpackerProc)
			SDL_LoadFunction(Instance, "CreateWaveUnpacker");
#endif

#ifdef TVP_SUPPORT_KPI
		GetKMPModule = (pfnGetKMPModule)
			SDL_LoadFunction(Instance, SZ_KMP_GETMODULE);
#endif
#endif

		// link
		if(V2Link)
		{
			V2Link(TVPGetFunctionExporter());
		}

#ifdef _WIN32
		// retrieve ModuleInstance
		// Susie Plug-in check
		if(SDL_LoadFunction(Instance, "GetPicture"))
		{
			IsSusiePicturePlugin = true;
			TVPLoadPictureSPI(Instance);
			return;
		}
		if(SDL_LoadFunction(Instance, "GetFile"))
		{
			IsSusieArchivePlugin = true;
			TVPLoadArchiveSPI(Instance);
			return;
		}
#endif

		if(GetModuleInstance)
		{
			TSS_HWND mainwin = NULL;
#ifdef _WIN32
			mainwin = Application->GetHandle();
#endif
			HRESULT hr = GetModuleInstance(&TSSModule, storageprovider,
				 NULL, mainwin);
			if(FAILED(hr) || TSSModule == NULL)
				TVPThrowExceptionMessage(TVPCannotLoadPlugin, name);

			// get supported extensions
			TSS_ULONG index = 0;
			while(true)
			{
				tjs_char mediashortname[33];
				tjs_char buf[256];
				HRESULT hr = TSSModule->GetSupportExts(index,
					mediashortname, buf, 255);
				if(hr == S_OK)
					SupportedExts.push_back(ttstr(buf).AsLowerCase());
				else
					break;
				index ++;
			}
		}


#if 0
#ifdef TVP_SUPPORT_KPI
		// retrieve KbMediaPlayer Plug-in module instance
		if(GetKMPModule)
		{
			KMPModule = GetKMPModule();
			if(KMPModule->dwVersion != 100)
				TVPThrowExceptionMessage(TVPCannotLoadPlugin, name +
					TJS_W(" (invalid version)"));
			if(!KMPModule->dwReentrant)
				TVPThrowExceptionMessage(TVPCannotLoadPlugin, name +
					TJS_W(" (is not re-entrant)"));

			if(KMPModule->Init) KMPModule->Init();
		}
#endif
#endif
	}
	catch(...)
	{
		if (Instance != NULL)
		{
			SDL_UnloadObject(Instance);
			Instance = NULL;
		}
		if (Holder != NULL)
		{
			delete Holder;
			Holder = NULL;
		}
		throw;
	}
}
//---------------------------------------------------------------------------
tTVPPlugin::~tTVPPlugin()
{
}
//---------------------------------------------------------------------------
bool tTVPPlugin::Uninit()
{
	tTJS *tjs = TVPGetScriptEngine();
	if(tjs) tjs->DoGarbageCollection(); // to release unused objects

	if(V2Unlink)
	{
 		if(TJS_FAILED(V2Unlink())) return false;
	}
#ifdef _WIN32
#if 0
#ifdef TVP_SUPPORT_KPI
	if(KMPModule) if(KMPModule->Deinit) KMPModule->Deinit();
#endif
#endif
	if(TSSModule) TSSModule->Release();
#ifdef _WIN32
	if(IsSusiePicturePlugin) TVPUnloadPictureSPI(Instance);
	if(IsSusieArchivePlugin) TVPUnloadArchiveSPI(Instance);
#endif
#endif

	if (Instance != NULL)
	{
		SDL_UnloadObject(Instance);
		Instance = NULL;
	}
	if (Holder != NULL)
	{
		delete Holder;
		Holder = NULL;
	}
	return true;
}
#endif
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
#ifdef KRKRSDL2_ENABLE_PLUGINS
bool TVPPluginUnloadedAtSystemExit = false;
typedef std::vector<tTVPPlugin*> tTVPPluginVectorType;
#endif
struct tTVPPluginVectorStruc
{
#ifdef KRKRSDL2_ENABLE_PLUGINS
	tTVPPluginVectorType Vector;
#endif
	tTVPStorageProvider StorageProvider;
} static TVPPluginVector;
#ifdef KRKRSDL2_ENABLE_PLUGINS
static void TVPDestroyPluginVector(void)
{
	// state all plugins are to be released
	TVPPluginUnloadedAtSystemExit = true;

	// delete all objects
	tTVPPluginVectorType::iterator i;
	while(TVPPluginVector.Vector.size())
	{
		i = TVPPluginVector.Vector.end() - 1;
		try
		{
			(*i)->Uninit();
			delete *i;
		}
		catch(...)
		{
		}
		TVPPluginVector.Vector.pop_back();
	}
}
tTVPAtExit TVPDestroyPluginVectorAtExit
	(TVP_ATEXIT_PRI_RELEASE, TVPDestroyPluginVector);
#endif
//---------------------------------------------------------------------------
static bool TVPPluginLoading = false;
void TVPLoadPlugin(const ttstr & name)
{
#ifdef KRKRSDL2_ENABLE_PLUGINS
	// load plugin
	if(TVPPluginLoading)
		TVPThrowExceptionMessage(TVPCannnotLinkPluginWhilePluginLinking);
			// linking plugin while other plugin is linking, is prohibited
			// by data security reason.

	// check whether the same plugin was already loaded
	tTVPPluginVectorType::iterator i;
	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->Name == name) return;
	}

	// Engine-provided plugin: never touch the (Windows PE) file on disk.
	// The loader's search path, the game directory and the system directory
	// are all irrelevant here — the behaviour is compiled in.
	if (TVPIsBuiltinPlugin(name))
	{
		/* No log line: this fires once per plugin a game links, is a normal
		 * outcome (the behaviour is compiled in), and a game linking a dozen
		 * plugins would put a dozen lines in every boot log. A plugin that is
		 * NOT built in is reported by the loader's failure path instead. */
		TVPInstallBuiltinPlugin(name);
		return;
	}

	tTVPPlugin * p;

	try
	{
		TVPPluginLoading = true;
		p = new tTVPPlugin(name, &TVPPluginVector.StorageProvider);
		TVPPluginLoading = false;
	}
	catch(...)
	{
		TVPPluginLoading = false;
		throw;
	}

	TVPPluginVector.Vector.push_back(p);
#endif
}
//---------------------------------------------------------------------------
bool TVPUnloadPlugin(const ttstr & name)
{
	// unload plugin

#ifdef KRKRSDL2_ENABLE_PLUGINS
	tTVPPluginVectorType::iterator i;
	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->Name == name)
		{
			if(!(*i)->Uninit()) return false;
			delete *i;
			TVPPluginVector.Vector.erase(i);
			return true;
		}
	}
	TVPThrowExceptionMessage(TVPNotLoadedPlugin, name);
	return false;
#else
	return true;
#endif
}
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// plug-in autoload support (cross-platform using dirent)
//---------------------------------------------------------------------------
struct tTVPFoundPlugin
{
	tjs_string Path;
	tjs_string Name;
	bool operator < (const tTVPFoundPlugin &rhs) const { return Name < rhs.Name; }
};
static tjs_int TVPAutoLoadPluginCount = 0;
static void TVPSearchPluginsAt(std::vector<tTVPFoundPlugin> &list, const tjs_string &folder)
{
	/* Convert tjs_string (UTF-16) to char* for opendir, fixing backslashes */
	std::string folder_str;
	for (tjs_char c : folder) {
		if (c == '\\') c = '/';
		folder_str += (char)(unsigned char)c;
	}
	DIR *dir = opendir(folder_str.c_str());
	if (!dir) return;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL)
	{
		tjs_string name;
		for (const char *p = ent->d_name; *p; p++)
			name += (tjs_char)(unsigned char)*p;
		if (name.length() >= 4)
		{
			tjs_string ext = name.substr(name.length() - 4);
			if (ext == TJS_W(".tpm") || ext == TJS_W(".TPM") || ext == TJS_W(".Tpm"))
			{
				// A plugin the engine supplies itself is not loaded from
				// disk: its file is a Windows PE we cannot dlopen, and the
				// behaviour is compiled in (see TVPLoadPlugin).
				if (TVPIsBuiltinPlugin(name)) continue;
				tTVPFoundPlugin fp;
				fp.Path = folder;
				fp.Name = name;
				list.push_back(fp);
			}
		}
	}
	closedir(dir);
}
void TVPLoadPluigins(void)
{
	std::vector<tTVPFoundPlugin> list;
	tjs_string exepath = IncludeTrailingBackslash(ExtractFileDir(ExePath()));
	/* Fix Windows-style backslashes to forward slashes for Linux */
	std::replace(exepath.begin(), exepath.end(), '\\', '/');
	TVPSearchPluginsAt(list, exepath);
	TVPSearchPluginsAt(list, exepath + TJS_W("system/"));
	TVPSearchPluginsAt(list, exepath + TJS_W("plugin/"));
	std::sort(list.begin(), list.end());
	TVPAutoLoadPluginCount = (tjs_int)list.size();
	for (auto i = list.begin(); i != list.end(); i++)
	{
		TVPAddImportantLog(ttstr(TJS_W("(info) Loading ")) + ttstr(i->Name));
		TVPLoadPlugin(i->Path + i->Name);
	}
}
//---------------------------------------------------------------------------
tjs_int TVPGetAutoLoadPluginCount() { return TVPAutoLoadPluginCount; }
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// interface for built-in Wave decode plugins
//---------------------------------------------------------------------------
struct tTVPTSSModuleWrapper
{
	ITSSModule *TSSModule = nullptr;

	tTVPGetModuleInstanceProc GetModuleInstance = nullptr;

	std::vector<ttstr> SupportedExts;

	tTVPTSSModuleWrapper(tTVPGetModuleInstanceProc GetModuleInstanceProc, ITSSStorageProvider *storageprovider);
	~tTVPTSSModuleWrapper();
};
//---------------------------------------------------------------------------
tTVPTSSModuleWrapper::tTVPTSSModuleWrapper(tTVPGetModuleInstanceProc GetModuleInstanceProc, ITSSStorageProvider *storageprovider)
{
	GetModuleInstance = GetModuleInstanceProc;
	if(GetModuleInstance)
	{
		TSS_HWND mainwin = NULL;
#ifdef _WIN32
		mainwin = Application->GetHandle();
#endif
		HRESULT hr = GetModuleInstance(&TSSModule, storageprovider,
			 NULL, mainwin);
		if(FAILED(hr) || TSSModule == NULL)
			TVPThrowExceptionMessage(TJS_W("TSSModule retrieval failure"));

		// get supported extensions
		TSS_ULONG index = 0;
		while(true)
		{
			tjs_char mediashortname[33];
			tjs_char buf[256];
			HRESULT hr = TSSModule->GetSupportExts(index,
				mediashortname, buf, 255);
			if(hr == S_OK)
				SupportedExts.push_back(ttstr(buf).AsLowerCase());
			else
				break;
			index ++;
		}
	}
}
//---------------------------------------------------------------------------
tTVPTSSModuleWrapper::~tTVPTSSModuleWrapper()
{
	if(TSSModule) TSSModule->Release();
}
typedef std::vector<tTVPTSSModuleWrapper*> tTVPTSSModuleWrapperType;
struct tTVPTSSModuleWrapperVectorStruc
{
	tTVPTSSModuleWrapperType Vector;
} static TVPTSSModuleWrapperVector;

void TVPRegisterTSSWaveDecoder(tTVPGetModuleInstanceProc GetModuleInstance)
{
	tTVPTSSModuleWrapper * p;

	try
	{
		p = new tTVPTSSModuleWrapper(GetModuleInstance, &TVPPluginVector.StorageProvider);
	}
	catch(...)
	{
		throw;
	}

	TVPTSSModuleWrapperVector.Vector.push_back(p);
}
//---------------------------------------------------------------------------
// interface to Wave decode plugins
//---------------------------------------------------------------------------
ITSSWaveDecoder * TVPSearchAvailTSSWaveDecoder(const ttstr & storage, const ttstr & extension)
{
	{
		tTVPTSSModuleWrapperType::iterator i;
		for(i = TVPTSSModuleWrapperVector.Vector.begin();
			i != TVPTSSModuleWrapperVector.Vector.end(); i++)
		{
			if((*i)->TSSModule)
			{
				// check whether the plugin supports extension
				bool supported = false;
				std::vector<ttstr>::iterator ei;
				for(ei = (*i)->SupportedExts.begin(); ei != (*i)->SupportedExts.end(); ei++)
				{
					if(ei->GetLen() == 0) { supported = true; break; }
					if(extension == *ei) { supported = true; break; }
				}

				if(!supported) continue;

				// retrieve instance from (*i)->TSSModule
				IUnknown *intf = NULL;
				HRESULT hr = (*i)->TSSModule->GetMediaInstance(
					(tjs_char*)storage.c_str(), &intf);
				if(SUCCEEDED(hr))
				{
					try
					{
						// check  whether the instance has IID_ITSSWaveDecoder
						// interface.
						ITSSWaveDecoder * decoder;
						if(SUCCEEDED(intf->QueryInterface(IID_ITSSWaveDecoder,
							(void**) &decoder)))
						{
							intf->Release();
							return decoder; // OK
						}
					}
					catch(...)
					{
						intf->Release();
						throw;
					}
					intf->Release();
				}

			}
		}
	}
#ifdef KRKRSDL2_ENABLE_PLUGINS
	tTVPPluginVectorType::iterator i;
	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->TSSModule)
		{
			// check whether the plugin supports extension
			bool supported = false;
			std::vector<ttstr>::iterator ei;
			for(ei = (*i)->SupportedExts.begin(); ei != (*i)->SupportedExts.end(); ei++)
			{
				if(ei->GetLen() == 0) { supported = true; break; }
				if(extension == *ei) { supported = true; break; }
			}

			if(!supported) continue;

			// retrieve instance from (*i)->TSSModule
			IUnknown *intf = NULL;
			HRESULT hr = (*i)->TSSModule->GetMediaInstance(
				(tjs_char*)storage.c_str(), &intf);
			if(SUCCEEDED(hr))
			{
				try
				{
					// check  whether the instance has IID_ITSSWaveDecoder
					// interface.
					ITSSWaveDecoder * decoder;
					if(SUCCEEDED(intf->QueryInterface(IID_ITSSWaveDecoder,
						(void**) &decoder)))
					{
						intf->Release();
						return decoder; // OK
					}
				}
				catch(...)
				{
					intf->Release();
					throw;
				}
				intf->Release();
			}

		}
	}
#endif
	return NULL; // not found
}
//---------------------------------------------------------------------------
#if 0
//---------------------------------------------------------------------------
#ifdef TVP_SUPPORT_OLD_WAVEUNPACKER
IWaveUnpacker * TVPSearchAvailWaveUnpacker(const ttstr & storage, IStream **stream)
{
	tTVPPluginVectorType::iterator i;
	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->CreateWaveUnpacker) break;
	}
	if(i == TVPPluginVector.Vector.end()) return NULL; // KPI not found

	// retrieve IStream interface
	AnsiString ansiname = storage.AsAnsiString();

	tTJSBinaryStream *stream0 = NULL;
	long size;
	try
	{
		stream0 = TVPCreateStream(storage);
		size = (long)stream0->GetSize();
	}
	catch(...)
	{
		if(stream0) delete stream0;
		return NULL;
	}

	IStream *istream = new tTVPIStreamAdapter(stream0);

	try
	{

		for(i = TVPPluginVector.Vector.begin();
			i != TVPPluginVector.Vector.end(); i++)
		{
			if((*i)->CreateWaveUnpacker)
			{
				// call CreateWaveUnpacker to retrieve decoder instance
				IWaveUnpacker *out;
				HRESULT hr = (*i)->CreateWaveUnpacker(istream, size,
					ansiname.c_str(), &out);
				if(SUCCEEDED(hr))
				{
					*stream = istream;
					return out;
				}
			}
		}
	}
	catch(...)
	{
		istream->Release();
		return NULL;
	}
	istream->Release();
	return NULL; // not found
}
#endif
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
#ifdef TVP_SUPPORT_KPI
void * TVPSearchAvailKMPWaveDecoder(const ttstr & storage, KMPMODULE ** module,
	SOUNDINFO * info)
{
	tTVPPluginVectorType::iterator i;
	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->KMPModule) break;
	}
	if(i == TVPPluginVector.Vector.end()) return NULL; // KPI not found

	AnsiString localname;

	if(TJS_strchr(storage.c_str(), TVPArchiveDelimiter)) return NULL;
		// in-archive storage is not supported

	try
	{
		ttstr ln(TVPSearchPlacedPath(storage));
		TVPGetLocalName(ln);
		localname  = ln.AsAnsiString();
	}
	catch(...)
	{
		return NULL;
	}

	AnsiString ext = TVPExtractStorageExt(storage).AsAnsiString();

	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->KMPModule)
		{
			// search over available extensions
			const char **module_ext = (*i)->KMPModule->ppszSupportExts;
			while(*module_ext)
			{
				if(!strcmpi(ext.c_str(), *module_ext)) break;
				module_ext ++;
			}
			if(!*module_ext) continue; // not found in this plug-in

			*module = (*i)->KMPModule;
			HKMP hkmp = (*i)->KMPModule->Open(localname.c_str(), info);
			if(hkmp)
				(*i)->KMPModule->SetPosition(hkmp, 0);
					// rewind; some plug-ins crash when the initial rewind is
					// not processed...
			return hkmp;
		}
	}
	return NULL; // not found
}
#endif
//---------------------------------------------------------------------------
#endif





//---------------------------------------------------------------------------
// some service functions for plugin
//---------------------------------------------------------------------------
#include "zlib/zlib.h"
int ZLIB_uncompress(unsigned char *dest, unsigned long *destlen,
	const unsigned char *source, unsigned long sourcelen)
{
	return uncompress(dest, destlen, source, sourcelen);
}
//---------------------------------------------------------------------------
int ZLIB_compress(unsigned char *dest, unsigned long *destlen,
	const unsigned char *source, unsigned long sourcelen)
{
	return compress(dest, destlen, source, sourcelen);
}
//---------------------------------------------------------------------------
int ZLIB_compress2(unsigned char *dest, unsigned long *destlen,
	const unsigned char *source, unsigned long sourcelen, int level)
{
	return compress2(dest, destlen, source, sourcelen, level);
}
//---------------------------------------------------------------------------
#include "md5.h"
static char TVP_assert_md5_state_t_size[
	 (sizeof(TVP_md5_state_t) >= sizeof(md5_state_t))];
	// if this errors, sizeof(TVP_md5_state_t) is not equal to sizeof(md5_state_t).
	// sizeof(TVP_md5_state_t) must be equal to sizeof(md5_state_t).
//---------------------------------------------------------------------------
void TVP_md5_init(TVP_md5_state_t *pms)
{
	md5_init((md5_state_t*)pms);
}
//---------------------------------------------------------------------------
void TVP_md5_append(TVP_md5_state_t *pms, const tjs_uint8 *data, int nbytes)
{
	md5_append((md5_state_t*)pms, (const md5_byte_t*)data, nbytes);
}
//---------------------------------------------------------------------------
void TVP_md5_finish(TVP_md5_state_t *pms, tjs_uint8 *digest)
{
	md5_finish((md5_state_t*)pms, digest);
}
//---------------------------------------------------------------------------
#ifdef _WIN32
HWND TVPGetApplicationWindowHandle()
{
	return Application->GetHandle();
}
#endif
//---------------------------------------------------------------------------
void TVPProcessApplicationMessages()
{
#if 0
	Application->ProcessMessages();
#endif
}
//---------------------------------------------------------------------------
void TVPHandleApplicationMessage()
{
#if 0
	Application->HandleMessage();
#endif
}
//---------------------------------------------------------------------------
bool TVPRegisterGlobalObject(const tjs_char *name, iTJSDispatch2 * dsp)
{
	// register given object to global object
	tTJSVariant val(dsp);
	iTJSDispatch2 *global = TVPGetScriptDispatch();
	tjs_error er;
	try
	{
		er = global->PropSet(TJS_MEMBERENSURE, name, NULL, &val, global);
	}
	catch(...)
	{
		global->Release();
		return false;
	}
	global->Release();
	return TJS_SUCCEEDED(er);
}
//---------------------------------------------------------------------------
bool TVPRemoveGlobalObject(const tjs_char *name)
{
	// remove registration of global object
	iTJSDispatch2 *global = TVPGetScriptDispatch();
	if(!global) return false;
	tjs_error er;
	try
	{
		er = global->DeleteMember(0, name, NULL, global);
	}
	catch(...)
	{
		global->Release();
		return false;
	}
	global->Release();
	return TJS_SUCCEEDED(er);
}
//---------------------------------------------------------------------------
void TVPDoTryBlock(
	tTVPTryBlockFunction tryblock,
	tTVPCatchBlockFunction catchblock,
	tTVPFinallyBlockFunction finallyblock,
	void *data)
{
	try
	{
		tryblock(data);
	}
	catch(const eTJS & e)
	{
		if(finallyblock) finallyblock(data);
		tTVPExceptionDesc desc;
		desc.type = TJS_W("eTJS");
		desc.message = e.GetMessage();
		if(catchblock(data, desc)) throw;
		return;
	}
	catch(...)
	{
		if(finallyblock) finallyblock(data);
		tTVPExceptionDesc desc;
		desc.type = TJS_W("unknown");
		if(catchblock(data, desc)) throw;
		return;
	}
	if(finallyblock) finallyblock(data);
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPGetFileVersionOf
//---------------------------------------------------------------------------
#ifdef _WIN32
bool TVPGetFileVersionOf(const wchar_t* module_filename, tjs_int &major, tjs_int &minor, tjs_int &release, tjs_int &build)
{
	// retrieve file version
	major = minor = release = build = 0;

	VS_FIXEDFILEINFO *FixedFileInfo;
	BYTE *VersionInfo;
	bool got = false;

	UINT dum;
	DWORD dum2;

	tjs_char* filename = new tjs_char[TJS_strlen(module_filename) + 1];
	try
	{
		TJS_strcpy(filename, module_filename);

		DWORD size = ::GetFileVersionInfoSize (filename, &dum2);
		if(size)
		{
			VersionInfo = new BYTE[size + 2];
			try
			{
				if(::GetFileVersionInfo(filename, 0, size, (void*)VersionInfo))
				{
					if(::VerQueryValue((void*)VersionInfo, TJS_W("\\"), (void**)(&FixedFileInfo),
						&dum))
					{
						major   = FixedFileInfo->dwFileVersionMS >> 16;
						minor   = FixedFileInfo->dwFileVersionMS & 0xffff;
						release = FixedFileInfo->dwFileVersionLS >> 16;
						build   = FixedFileInfo->dwFileVersionLS & 0xffff;
						got = true;
					}
				}
			}
			catch(...)
			{
				delete [] VersionInfo;
				throw;
			}
			delete [] VersionInfo;
		}
	}
	catch(...)
	{
		delete [] filename;
		throw;
	}

	delete [] filename;

	return got;
}
//---------------------------------------------------------------------------
#endif



//---------------------------------------------------------------------------
// TVPCreateNativeClass_Plugins
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Plugins()
{
	tTJSNC_Plugins *cls = new tTJSNC_Plugins();


	// setup some platform-specific members
//---------------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/link)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];

	TVPLoadPlugin(name);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/link)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/unlink)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];

	bool res = TVPUnloadPlugin(name);

	if(result) *result = (tjs_int)res;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/unlink)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(getList)
{
	iTJSDispatch2 * array = TJSCreateArrayObject();
	try
	{
#ifdef KRKRSDL2_ENABLE_PLUGINS
		tTVPPluginVectorType::iterator i;
		tjs_int idx = 0;
		for(i = TVPPluginVector.Vector.begin(); i != TVPPluginVector.Vector.end(); i++)
		{
			tTJSVariant val = (*i)->Name.c_str();
			array->PropSetByNum(TJS_MEMBERENSURE, idx++, &val, array);
		}
#endif

		if (result) *result = tTJSVariant(array, array);
	}
	catch(...)
	{
		array->Release();
		throw;
	}
	array->Release();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(cls, getList)
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
	return cls;
}
//---------------------------------------------------------------------------




