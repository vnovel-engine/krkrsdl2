//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "MenuItem" class implementation - SDL2 / libretro platform half
// See MenuItemImpl.h: item state and the item tree are real; there is no
// window menu to show them in.
#include "tjsCommHead.h"

#include "EventIntf.h"
#include "MenuItemImpl.h"
#include "Present.h"

#include "MsgIntf.h"
#include "WindowIntf.h"

//---------------------------------------------------------------------------
// tTJSNI_MenuItem
//---------------------------------------------------------------------------
tTJSNI_MenuItem::tTJSNI_MenuItem()
{
	/* Defaults match the VCL TMenuItem upstream created these from. */
	Checked = false;
	Enabled = true;
	Group = 0;
	Radio = false;
	Visible = true;
	Index = 0;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_MenuItem::Construct(tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *tjs_obj)
{
	inherited::Construct(numparams, param, tjs_obj);

	/* The second parameter is the caption whenever it was not the Window
	 * (upstream's rule), so a KAGMenuItem that calls super.MenuItem(window,
	 * caption) can read item.caption back. */
	if(!Window && numparams >= 2)
	{
		Caption = *param[1];
	}

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_MenuItem::Invalidate()
{
	/* No platform object to delete; inherited tree teardown is the whole of it. */
	inherited::Invalidate();
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::CanDeliverEvents() const
{
	/* Refuse delivery while this item or any ancestor is disabled (a disabled
	 * entry cannot be picked, so its click never happened). The walk is over
	 * the item tree, which carries the same state upstream read from the VCL
	 * item. The base OnClick() and tTJSNI_BaseWindow still apply their own
	 * window/Window checks. */
	const tTJSNI_BaseMenuItem *item = this;
	while(item)
	{
		const tTJSNI_MenuItem *self = static_cast<const tTJSNI_MenuItem *>(item);
		if(!self->GetEnabled()) return false;
		item = item->GetParent();
	}
	return true;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Add(tTJSNI_MenuItem * item)
{
	/* The item tree is the only store; every read the game performs is against
	 * it (children, parent, root, KAGWindow::internalSetMenuAccessibleAll). */
	AddChild(item);
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Insert(tTJSNI_MenuItem *item, tjs_int index)
{
	/* Position is kept in the tree via the ordered InsertChild; no bound
	 * shortcut or accelerator. */
	InsertChild(item, index);
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Remove(tTJSNI_MenuItem *item)
{
	/* TVPNotChildMenuItem is the guard the game's bookkeeping relies on;
	 * RemoveChild is silent when the item is not a child. */
	if(FindChild(item) == -1) TVPThrowExceptionMessage(TVPNotChildMenuItem);
	RemoveChild(item);
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_MenuItem::GetIndex() const
{
	/* A field, not a derivation (matches upstream's MenuIndex). */
	return Index;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetIndex(tjs_int newIndex)
{
	Index = newIndex;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetCaption(const ttstr & caption)
{
	/* Only the value is kept; nothing renders it. The "&S" hotkey handling the
	 * game's captions rely on is upstream's VCL behaviour, which has no effect
	 * here. */
	Caption = caption;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::GetCaption(ttstr & caption) const
{
	caption = Caption;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetChecked(bool b)
{
	Checked = b;
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetChecked() const
{
	return Checked;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetEnabled(bool b)
{
	Enabled = b;
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetEnabled() const
{
	return Enabled;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetGroup(tjs_int g)
{
	Group = g;
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_MenuItem::GetGroup() const
{
	return Group;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetRadio(bool b)
{
	Radio = b;
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetRadio() const
{
	return Radio;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetShortcut(const ttstr & shortcut)
{
	/* The string is kept verbatim so it round-trips identically; nothing can
	 * honour an accelerator here. */
	Shortcut = shortcut;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::GetShortcut(ttstr & shortcut) const
{
	shortcut = Shortcut;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetVisible(bool b)
{
	/* Item-level value only: the game's `menu.visible` (the KAG `menu` object
	 * is the window's own menu) has no bar to toggle here. The core's own
	 * overlay menu is a separate surface driven by the host, not from here.
	 * `rightClickMenuItem.visible = false` hides that entry. */
	Visible = b;
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetVisible() const
{
	return Visible;
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_MenuItem::TrackPopup(tjs_uint32 flags, tjs_int x, tjs_int y) const
{
	/* No-op: no HMENU, no window, no popup to track. Returns 0, upstream's
	 * value for "the popup did not come up". A no-op rather than a throw
	 * because the game calls it inside its right-click handler. The TJS
	 * `popup` method requires three arguments; a caller reaching this uses the
	 * native signature, not the game's `Menu.popup(x, y)` polyfill. */
	(void)flags;
	(void)x;
	(void)y;
	return 0;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTJSNC_MenuItem::CreateNativeInstance
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_MenuItem::CreateNativeInstance()
{
	return new tTJSNI_MenuItem();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPCreateNativeClass_MenuItem
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_MenuItem()
{
	tTJSNativeClass *cls = new tTJSNC_MenuItem();
	static tjs_uint32 TJS_NCM_CLASSID;
	TJS_NCM_CLASSID = tTJSNC_MenuItem::ClassID;

	/* `MenuItem.HMENU` is omitted: there is no handle, and reporting always-NULL
	 * would mislead a plugin into thinking a menu exists. */

	return cls;
}
//---------------------------------------------------------------------------
