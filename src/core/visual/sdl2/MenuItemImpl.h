//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "MenuItem" class implementation - SDL2 / libretro platform half
//
// This target has no window chrome, no menu bar, and no HWND. The vendored
// NP2kai menu (deps/np2kai-menu) is built from a static array at open time and
// has no runtime append/insert/remove surface for a menu, so Add/Insert/Remove
// cannot be expressed against it and maintain the inherited item tree here.
// popup() is a no-op (returns 0, upstream's value for "the popup did not come
// up"); visible has no bar to hide but the item-level value is kept so scripts
// that set it read it back.
#ifndef MenuItemImplH
#define MenuItemImplH

#include "MenuItemIntf.h"

//---------------------------------------------------------------------------
// tTJSNI_MenuItem : MenuItem Native Instance
//---------------------------------------------------------------------------
class tTJSNI_MenuItem : public tTJSNI_BaseMenuItem
{
	typedef tTJSNI_BaseMenuItem inherited;

	/* Item state, stored here because this target has no platform menu object.
	 * The accessors are the only way in; the game observes the same values
	 * upstream mirrored through its VCL TMenuItem. */
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
