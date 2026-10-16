//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "MenuItem" class implementation - SDL2 / libretro platform half
//---------------------------------------------------------------------------
// Upstream supplied this half twice, both times as the platform directory's own
// MenuItemImpl.{h,cpp} - the class tTJSNI_MenuItem that MenuItemIntf.h pulls in
// through its `#include "MenuItemImpl.h"`:
//
//     visual/win32/MenuItemImpl.{h,cpp}     the VCL one: a TMenuItem on the
//                                           window's TMainMenu, TrackPopupMenuEx
//
// This tree's platform is SDL2 (see src/config/src_list/kirikirisdl2/
// includes.txt: src/core/visual/sdl2 is the include directory WindowImpl.h -
// and now MenuItemImpl.h - resolve from), so this file is the analogue for it,
// and it lives beside the window implementation it belongs to:
//
//     src/core/visual/sdl2/MenuItemImpl.{h,cpp}
//
// WHAT IS INERT, AND WHY (nothing here pretends to render)
// --------------------------------------------------------
// There is no window menu in this target: a libretro core owns no window
// chrome, has no menu bar, and no HWND to hang one on. The vendored NP2kai menu
// (deps/np2kai-menu, driven by the core's src/menu.cpp) is the native menu that
// exists, and it cannot be bound from here:
//
//   * Its item tree is built once, by menusys_create(), from a static MSYSITEM
//     array; there is no runtime append/insert/remove for a menu (menudlg_append
//     belongs to the modal *dialog* API, a different singleton). So Add/Insert/
//     Remove cannot be expressed against it at all - which is why those three
//     maintain the inherited item tree instead, and say so below.
//   * menusys_* is compiled into the *core* (Makefile.common's NP2KAI_MENU
//     block), not into this engine archive, and the menu is only presented while
//     the core's own open flag is set (vnovel_menu_open keeps it). Materialising
//     this tree into it therefore needs a core-side entry point that does not
//     exist yet; calling the vendored API directly from here would instead
//     duplicate the core's menu session state (its VRAM, geometry and open flag)
//     and put the two out of step.
//
// So: item state and the item tree are real and are exactly what the game reads
// and writes; popup() is a documented no-op; and there is no menu bar for
// `visible` to hide. Each is marked at its definition.
//---------------------------------------------------------------------------
#ifndef MenuItemImplH
#define MenuItemImplH

#include "MenuItemIntf.h"

//---------------------------------------------------------------------------
// tTJSNI_MenuItem : MenuItem Native Instance
//---------------------------------------------------------------------------
class tTJSNI_MenuItem : public tTJSNI_BaseMenuItem
{
	typedef tTJSNI_BaseMenuItem inherited;

	/* Item state. Upstream kept this in the VCL TMenuItem it owned and mirrored
	 * it through the property accessors (Caption, Checked, Enabled, GroupIndex,
	 * RadioItem, ShortCut, Visible, MenuIndex); with no such object here the
	 * state lives in this instance and the accessors are the only way in. What
	 * the game observes is therefore identical; what upstream additionally did
	 * with it (paint it, gray it out, reorder it, bind an accelerator) has
	 * nothing to act on. */
	ttstr Caption;
	ttstr Shortcut;
	bool Checked;
	bool Enabled;
	tjs_int Group;
	bool Radio;
	bool Visible;
	tjs_int Index;

public:
	tTJSNI_MenuItem();
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

protected:
	bool CanDeliverEvents() const;
		// tTJSNI_BaseMenuItem::CanDeliverEvents override

public:
	void Add(tTJSNI_MenuItem * item);
	void Insert(tTJSNI_MenuItem *item, tjs_int index);
	void Remove(tTJSNI_MenuItem *item);

	void SetCaption(const ttstr & caption);
	void GetCaption(ttstr & caption) const;

	void SetChecked(bool b);
	bool GetChecked() const;

	void SetEnabled(bool b);
	bool GetEnabled() const;

	void SetGroup(tjs_int g);
	tjs_int GetGroup() const;

	void SetRadio(bool b);
	bool GetRadio() const;

	void SetShortcut(const ttstr & shortcut);
	void GetShortcut(ttstr & shortcut) const;

	void SetVisible(bool b);
	bool GetVisible() const;

	tjs_int GetIndex() const;
	void SetIndex(tjs_int newIndex);

	tjs_int TrackPopup(tjs_uint32 flags, tjs_int x, tjs_int y) const;
};
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
#endif
