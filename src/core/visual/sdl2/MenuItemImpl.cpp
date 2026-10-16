//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "MenuItem" class implementation - SDL2 / libretro platform half
//---------------------------------------------------------------------------
// See the header for what is inert and why. In one line: item state and the
// item tree are real, and there is no window menu for them to be shown in -
// this target has no menu bar, and the vendored NP2kai menu cannot be grown
// from a TJS item tree (its menu is built from a static array at open time, and
// its runtime-appendable surface is a positioned dialog).
//---------------------------------------------------------------------------
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
	/* Upstream seeded these from the VCL TMenuItem it had just created (whose
	 * own defaults are these). There is no such object here, so this is where
	 * they start. */
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

	/* Upstream created (or attached to) the platform menu item here and fetched
	 * the initial caption. Nothing is created: this target has no menu object
	 * to create. The caption fetch is kept, because it is what gives a
	 * KAGMenuItem its caption - the game calls
	 * `super.MenuItem(window, caption)` and then reads `item.caption` back
	 * (MainWindow.tjs: rightClickMenuItem.caption), and upstream's rule is that
	 * the second parameter is the caption whenever it was not the Window. */
	if(!Window && numparams >= 2)
	{
		Caption = *param[1];
	}

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_MenuItem::Invalidate()
{
	/* Upstream deleted the VCL object here when it owned it. There is nothing
	 * to delete - the inherited tree teardown is the whole of it. */
	inherited::Invalidate();
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::CanDeliverEvents() const
{
	/* Upstream walked the VCL item and its ancestors, refusing to deliver while
	 * any of them was disabled - a disabled menu entry cannot be picked, so its
	 * click never happened. The same walk is done here over the item tree, which
	 * is what carries the same state; upstream's extra `if(!MenuItem) return
	 * false` guard was "the platform menu item is gone", which cannot happen to
	 * an item that is only ever this instance.
	 *
	 * What is deliberately NOT reproduced: upstream's parent (tTJSNI_BaseWindow)
	 * checks the window, and the base OnClick() additionally requires the root
	 * item's Window and that window's own CanDeliverEvents(); both are unchanged
	 * and do apply. This target's window has no notion of focus beyond that. */
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
	/* Upstream also appended the item to the platform menu. There is no platform
	 * menu to append it to here, so the item tree is the whole effect. This is
	 * not a degraded Add: every read the game performs - children, parent, root,
	 * the container walk in KAGWindow::internalSetMenuAccessibleAll - is against
	 * this tree. What is absent is only the menu entry the user never sees,
	 * because no menu is displayed. */
	AddChild(item);
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Insert(tTJSNI_MenuItem *item, tjs_int index)
{
	/* Same as Add, but preserving the requested position. Upstream got the
	 * ordering from the platform menu (TMenuItem::Insert(index, ...)); with the
	 * tree being the only store, the position has to be kept in it, which is why
	 * the base grew an ordered InsertChild. Everything else about Insert is
	 * unchanged, including the absence of any bound shortcut or accelerator. */
	InsertChild(item, index);
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Remove(tTJSNI_MenuItem *item)
{
	/* Upstream located the item in the platform menu and raised
	 * TVPNotChildMenuItem when it was not there, which is the only guard the
	 * game's own bookkeeping relies on. The tree answers the same question, and
	 * the same exception is raised. RemoveChild itself is upstream's: void, and
	 * silent when the item is not a child. */
	if(FindChild(item) == -1) TVPThrowExceptionMessage(TVPNotChildMenuItem);
	RemoveChild(item);
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_MenuItem::GetIndex() const
{
	/* Upstream returned the platform item's MenuIndex - a field, not a
	 * derivation. Kept a field for the same reason: there is no platform item to
	 * ask, and the value only means something once something reorders the menu,
	 * which nothing here does. */
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
	/* Upstream also set the platform item's Caption (and turned VCL's automatic
	 * hotkey rewriting off, which is the "&S" handling the game's captions rely
	 * on). Nothing renders a caption here, so only the value is kept - which is
	 * what the game reads back. */
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
	/* Upstream ran the string through VCL's TextToShortCut, whose whole purpose
	 * was to turn it into an accelerator the platform menu would honour. Nothing
	 * can honour an accelerator here, so the string is kept verbatim - which
	 * round-trips through GetShortcut identically for every value the game sets,
	 * and avoids VCL's ShortCutToText rewriting the one it reads back. */
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
	/* Upstream, for an item that had a Window, toggled the window's menu BAR
	 * (SetMenuBarVisible) rather than the item - that is what `menu.visible =
	 * true` in the game's Config.tjs means: the KAG `menu` object is the
	 * window's own menu, and hiding it hides the bar.
	 *
	 * This target has no window frame, so there is no bar here to toggle. The
	 * core's own menu is a different thing entirely - an overlay it draws at
	 * the present boundary, shown when the user's hotkey asks for it - and it
	 * is deliberately NOT driven from here: a game's window-menu request and
	 * the host's own menu have nothing to do with each other, and wiring one to
	 * the other meant the host's menu could only be reached when a game
	 * happened to ask for a menu bar of its own.
	 *
	 * What remains is the item-level meaning, which is real and is what the
	 * scripts use (`rightClickMenuItem.visible = false` hides that entry): the
	 * value is kept and read back, so a script that sets it sees what it set. */
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
	/* INERT, deliberately and completely.
	 *
	 * Upstream converted the client coordinates to screen coordinates and called
	 * TrackPopupMenuEx on the item's HMENU, returning its BOOL. There is no
	 * HMENU, no window to convert against, and no popup to track, so this
	 * displays nothing and returns 0 - upstream's own value for "the popup did
	 * not come up" (it returns 0 both when the item is missing and when
	 * TrackPopupMenuEx fails).
	 *
	 * It is a no-op rather than a rejected call because the game calls it inside
	 * its right-click handler; throwing would replace a menu that does not appear
	 * with an error dialog. Note also that the TJS `popup` method requires three
	 * arguments, which the game's own `Menu.popup(x, y)` polyfill does not
	 * provide - so a caller reaching this is using the native signature, not the
	 * polyfill's. */
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

	/* Upstream exposed the platform handle as `MenuItem.HMENU` here. There is no
	 * handle, so there is nothing to expose and the property is omitted rather
	 * than reported as a value that is always NULL: a plugin reading it would be
	 * told "a menu with no handle" when the truth is "no menu". */

	return cls;
}
//---------------------------------------------------------------------------
