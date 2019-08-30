#include "InputHandler.h"
#include "Utils.h"
#include "Server.h"
#include "HeldBlockRenderer.h"
#include "Game.h"
#include "Platform.h"
#include "ExtMath.h"
#include "Camera.h"
#include "Inventory.h"
#include "World.h"
#include "Event.h"
#include "Window.h"
#include "Entity.h"
#include "Chat.h"
#include "Funcs.h"
#include "Screens.h"
#include "Block.h"
#include "Menus.h"
#include "Gui.h"
#include "Protocol.h"
#include "AxisLinesRenderer.h"

static bool input_buttonsDown[3];
static int input_pickingId = -1;
static const short normDists[10]   = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };
static const short classicDists[4] = { 8, 32, 128, 512 };
static TimeMS input_lastClick;
static float input_fovIndex = -1.0f;
#ifdef CC_BUILD_WEB
static bool suppressEscape;
#endif
enum MouseButton_ { MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE };

/*########################################################################################################################*
*-----------------------------------------------------Mouse helpers-------------------------------------------------------*
*#########################################################################################################################*/
static void MouseStateUpdate(int button, bool pressed) {
	struct Entity* p;
	/* defer getting the targeted entity, as it's a costly operation */
	if (input_pickingId == -1) {
		p = &LocalPlayer_Instance.Base;
		input_pickingId = Entities_GetCloset(p);
	}

	input_buttonsDown[button] = pressed;
	CPE_SendPlayerClick(button, pressed, (EntityID)input_pickingId, &Game_SelectedPos);	
}

static void MouseStateChanged(int button, bool pressed) {
	if (pressed) {
		/* Can send multiple Pressed events */
		MouseStateUpdate(button, true);
	} else {
		if (!input_buttonsDown[button]) return;
		MouseStateUpdate(button, false);
	}
}

static void MouseStatePress(int button) {
	input_lastClick = DateTime_CurrentUTC_MS();
	input_pickingId = -1;
	MouseStateChanged(button, true);
}

static void MouseStateRelease(int button) {
	input_pickingId = -1;
	MouseStateChanged(button, false);
}

void InputHandler_OnScreensChanged(void) {
	input_lastClick = DateTime_CurrentUTC_MS();

	if (Server.SupportsPlayerClick) {
		input_pickingId = -1;
		MouseStateChanged(MOUSE_LEFT,   false);
		MouseStateChanged(MOUSE_RIGHT,  false);
		MouseStateChanged(MOUSE_MIDDLE, false);
	}
}

static bool TouchesSolid(BlockID b) { return Blocks.Collide[b] == COLLIDE_SOLID; }
static bool PushbackPlace(struct AABB* blockBB) {
	struct Entity* p        = &LocalPlayer_Instance.Base;
	struct HacksComp* hacks = &LocalPlayer_Instance.Hacks;
	Face closestFace;
	bool insideMap;

	Vec3 pos = p->Position;
	struct AABB playerBB;
	struct LocationUpdate update;

	/* Offset position by the closest face */
	closestFace = Game_SelectedPos.Closest;
	if (closestFace == FACE_XMAX) {
		pos.X = blockBB->Max.X + 0.5f;
	} else if (closestFace == FACE_ZMAX) {
		pos.Z = blockBB->Max.Z + 0.5f;
	} else if (closestFace == FACE_XMIN) {
		pos.X = blockBB->Min.X - 0.5f;
	} else if (closestFace == FACE_ZMIN) {
		pos.Z = blockBB->Min.Z - 0.5f;
	} else if (closestFace == FACE_YMAX) {
		pos.Y = blockBB->Min.Y + 1 + ENTITY_ADJUSTMENT;
	} else if (closestFace == FACE_YMIN) {
		pos.Y = blockBB->Min.Y - p->Size.Y - ENTITY_ADJUSTMENT;
	}

	/* Exclude exact map boundaries, otherwise player can get stuck outside map */
	/* Being vertically above the map is acceptable though */
	insideMap =
		pos.X > 0.0f && pos.Y >= 0.0f && pos.Z > 0.0f &&
		pos.X < World.Width && pos.Z < World.Length;
	if (!insideMap) return false;

	AABB_Make(&playerBB, &pos, &p->Size);
	if (!hacks->Noclip && Entity_TouchesAny(&playerBB, TouchesSolid)) {
		/* Don't put player inside another block */
		return false;
	}

	LocationUpdate_MakePos(&update, pos, false);
	p->VTABLE->SetLocation(p, &update, false);
	return true;
}

static bool IntersectsOthers(Vec3 pos, BlockID block) {
	struct AABB blockBB, entityBB;
	struct Entity* entity;
	int id;

	Vec3_Add(&blockBB.Min, &pos, &Blocks.MinBB[block]);
	Vec3_Add(&blockBB.Max, &pos, &Blocks.MaxBB[block]);
	
	for (id = 0; id < ENTITIES_SELF_ID; id++) {
		entity = Entities.List[id];
		if (!entity) continue;

		Entity_GetBounds(entity, &entityBB);
		entityBB.Min.Y += 1.0f / 32.0f; /* when player is exactly standing on top of ground */
		if (AABB_Intersects(&entityBB, &blockBB)) return true;
	}
	return false;
}

static bool CheckIsFree(BlockID block) {
	struct Entity* p        = &LocalPlayer_Instance.Base;
	struct HacksComp* hacks = &LocalPlayer_Instance.Hacks;

	Vec3 pos, nextPos;
	struct AABB blockBB, playerBB;
	struct LocationUpdate update;

	/* Non solid blocks (e.g. water/flowers) can always be placed on players */
	if (Blocks.Collide[block] != COLLIDE_SOLID) return true;

	IVec3_ToVec3(&pos, &Game_SelectedPos.TranslatedPos);
	if (IntersectsOthers(pos, block)) return false;
	
	nextPos = LocalPlayer_Instance.Interp.Next.Pos;
	Vec3_Add(&blockBB.Min, &pos, &Blocks.MinBB[block]);
	Vec3_Add(&blockBB.Max, &pos, &Blocks.MaxBB[block]);

	/* NOTE: Need to also test against next position here, otherwise player can */
	/* fall through the block at feet as collision is performed against nextPos */
	Entity_GetBounds(p, &playerBB);
	playerBB.Min.Y = min(nextPos.Y, playerBB.Min.Y);

	if (hacks->Noclip || !AABB_Intersects(&playerBB, &blockBB)) return true;
	if (hacks->CanPushbackBlocks && hacks->PushbackPlacing && hacks->Enabled) {
		return PushbackPlace(&blockBB);
	}

	playerBB.Min.Y += 0.25f + ENTITY_ADJUSTMENT;
	if (AABB_Intersects(&playerBB, &blockBB)) return false;

	/* Push player upwards when they are jumping and trying to place a block underneath them */
	nextPos.Y = pos.Y + Blocks.MaxBB[block].Y + ENTITY_ADJUSTMENT;
	LocationUpdate_MakePos(&update, nextPos, false);
	p->VTABLE->SetLocation(p, &update, false);
	return true;
}

static void InputHandler_DeleteBlock(void) {
	IVec3 pos;
	BlockID old;
	/* always play delete animations, even if we aren't deleting a block */
	HeldBlockRenderer_ClickAnim(true);

	pos = Game_SelectedPos.BlockPos;
	if (!Game_SelectedPos.Valid || !World_Contains(pos.X, pos.Y, pos.Z)) return;

	old = World_GetBlock(pos.X, pos.Y, pos.Z);
	if (Blocks.Draw[old] == DRAW_GAS || !Blocks.CanDelete[old]) return;

	Game_ChangeBlock(pos.X, pos.Y, pos.Z, BLOCK_AIR);
	Event_RaiseBlock(&UserEvents.BlockChanged, pos, old, BLOCK_AIR);
}

static void InputHandler_PlaceBlock(void) {
	IVec3 pos;
	BlockID old, block;
	pos = Game_SelectedPos.TranslatedPos;
	if (!Game_SelectedPos.Valid || !World_Contains(pos.X, pos.Y, pos.Z)) return;

	old   = World_GetBlock(pos.X, pos.Y, pos.Z);
	block = Inventory_SelectedBlock;
	if (AutoRotate_Enabled) block = AutoRotate_RotateBlock(block);

	if (Game_CanPick(old) || !Blocks.CanPlace[block]) return;
	/* air-ish blocks can only replace over other air-ish blocks */
	if (Blocks.Draw[block] == DRAW_GAS && Blocks.Draw[old] != DRAW_GAS) return;
	if (!CheckIsFree(block)) return;

	Game_ChangeBlock(pos.X, pos.Y, pos.Z, block);
	Event_RaiseBlock(&UserEvents.BlockChanged, pos, old, block);
}

static void InputHandler_PickBlock(void) {
	IVec3 pos;
	BlockID cur;
	pos = Game_SelectedPos.BlockPos;
	if (!World_Contains(pos.X, pos.Y, pos.Z)) return;

	cur = World_GetBlock(pos.X, pos.Y, pos.Z);
	if (Blocks.Draw[cur] == DRAW_GAS) return;
	if (!(Blocks.CanPlace[cur] || Blocks.CanDelete[cur])) return;
	Inventory_PickBlock(cur);
}

void InputHandler_PickBlocks(void) {
	bool left, middle, right;
	TimeMS now = DateTime_CurrentUTC_MS();
	int delta  = (int)(now - input_lastClick);

	if (delta < 250) return; /* 4 times per second */
	input_lastClick = now;
	if (Gui_GetInputGrab()) return;

	left   = KeyBind_IsPressed(KEYBIND_DELETE_BLOCK);
	middle = KeyBind_IsPressed(KEYBIND_PICK_BLOCK);
	right  = KeyBind_IsPressed(KEYBIND_PLACE_BLOCK);

	if (Server.SupportsPlayerClick) {
		input_pickingId = -1;
		MouseStateChanged(MOUSE_LEFT,   left);
		MouseStateChanged(MOUSE_RIGHT,  right);
		MouseStateChanged(MOUSE_MIDDLE, middle);
	}

	if (left) {
		InputHandler_DeleteBlock();
	} else if (right) {
		InputHandler_PlaceBlock();
	} else if (middle) {
		InputHandler_PickBlock();
	}
}


/*########################################################################################################################*
*------------------------------------------------------Key helpers--------------------------------------------------------*
*#########################################################################################################################*/
static bool InputHandler_IsShutdown(Key key) {
	if (key == KEY_F4 && Key_IsAltPressed()) return true;

	/* On OSX, Cmd+Q should also terminate the process */
#ifdef CC_BUILD_OSX
	return key == 'Q' && Key_IsWinPressed();
#else
	return false;
#endif
}

static void InputHandler_Toggle(Key key, bool* target, const char* enableMsg, const char* disableMsg) {
	*target = !(*target);
	if (*target) {
		Chat_Add2("%c. &ePress &a%c &eto disable.",   enableMsg,  Input_Names[key]);
	} else {
		Chat_Add2("%c. &ePress &a%c &eto re-enable.", disableMsg, Input_Names[key]);
	}
}

static void InputHandler_CycleDistanceForwards(const short* viewDists, int count) {
	int i, dist;
	for (i = 0; i < count; i++) {
		dist = viewDists[i];

		if (dist > Game_UserViewDistance) {
			Game_UserSetViewDistance(dist); return;
		}
	}
	Game_UserSetViewDistance(viewDists[0]);
}

static void InputHandler_CycleDistanceBackwards(const short* viewDists, int count) {
	int i, dist;
	for (i = count - 1; i >= 0; i--) {
		dist = viewDists[i];

		if (dist < Game_UserViewDistance) {
			Game_UserSetViewDistance(dist); return;
		}
	}
	Game_UserSetViewDistance(viewDists[count - 1]);
}

bool InputHandler_SetFOV(int fov) {
	struct HacksComp* h = &LocalPlayer_Instance.Hacks;
	if (!h->Enabled || !h->CanUseThirdPersonCamera) return false;

	Game_ZoomFov = fov;
	Game_SetFov(fov);
	return true;
}

static bool InputHandler_DoFovZoom(float deltaPrecise) {
	struct HacksComp* h;
	if (!KeyBind_IsPressed(KEYBIND_ZOOM_SCROLL)) return false;

	h = &LocalPlayer_Instance.Hacks;
	if (!h->Enabled || !h->CanUseThirdPersonCamera) return false;

	if (input_fovIndex == -1.0f) input_fovIndex = (float)Game_ZoomFov;
	input_fovIndex -= deltaPrecise * 5.0f;

	Math_Clamp(input_fovIndex, 1.0f, Game_DefaultFov);
	return InputHandler_SetFOV((int)input_fovIndex);
}

static void InputHandler_CheckZoomFov(void* obj) {
	struct HacksComp* h = &LocalPlayer_Instance.Hacks;
	if (!h->Enabled || !h->CanUseThirdPersonCamera) Game_SetFov(Game_DefaultFov);
}

static bool HandleBlockKey(Key key) {
	if (Gui_GetInputGrab()) return false;

	if (key == KeyBinds[KEYBIND_DELETE_BLOCK]) {
		MouseStatePress(MOUSE_LEFT);
		InputHandler_DeleteBlock();
	} else if (key == KeyBinds[KEYBIND_PLACE_BLOCK]) {
		MouseStatePress(MOUSE_RIGHT);
		InputHandler_PlaceBlock();
	} else if (key == KeyBinds[KEYBIND_PICK_BLOCK]) {
		MouseStatePress(MOUSE_MIDDLE);
		InputHandler_PickBlock();
	} else {
		return false;
	}
	return true;
}

static bool HandleNonClassicKey(Key key) {
	if (key == KeyBinds[KEYBIND_HIDE_GUI]) {
		Game_HideGui = !Game_HideGui;
	} else if (key == KeyBinds[KEYBIND_SMOOTH_CAMERA]) {
		InputHandler_Toggle(key, &Camera.Smooth,
			"  &eSmooth camera is &aenabled",
			"  &eSmooth camera is &cdisabled");
	} else if (key == KeyBinds[KEYBIND_AXIS_LINES]) {
		InputHandler_Toggle(key, &AxisLinesRenderer_Enabled,
			"  &eAxis lines (&4X&e, &2Y&e, &1Z&e) now show",
			"  &eAxis lines no longer show");
	} else if (key == KeyBinds[KEYBIND_AUTOROTATE]) {
		InputHandler_Toggle(key, &AutoRotate_Enabled,
			"  &eAuto rotate is &aenabled",
			"  &eAuto rotate is &cdisabled");
	} else if (key == KeyBinds[KEYBIND_THIRD_PERSON]) {
		Camera_CycleActive();
	} else if (key == KeyBinds[KEYBIND_DROP_BLOCK]) {
		if (Inventory_CheckChangeSelected() && Inventory_SelectedBlock != BLOCK_AIR) {
			/* Don't assign SelectedIndex directly, because we don't want held block
			switching positions if they already have air in their inventory hotbar. */
			Inventory_Set(Inventory.SelectedIndex, BLOCK_AIR);
			Event_RaiseVoid(&UserEvents.HeldBlockChanged);
		}
	} else if (key == KeyBinds[KEYBIND_IDOVERLAY]) {
		TexIdsOverlay_Show();
	} else if (key == KeyBinds[KEYBIND_BREAK_LIQUIDS]) {
		InputHandler_Toggle(key, &Game_BreakableLiquids,
			"  &eBreakable liquids is &aenabled",
			"  &eBreakable liquids is &cdisabled");
	} else {
		return false;
	}
	return true;
}

static bool HandleCoreKey(Key key) {
	if (key == KeyBinds[KEYBIND_HIDE_FPS]) {
		Gui_ShowFPS = !Gui_ShowFPS;
	} else if (key == KeyBinds[KEYBIND_FULLSCREEN]) {
		int state = Window_GetWindowState();

		if (state == WINDOW_STATE_FULLSCREEN) {
			Window_ExitFullscreen();
		} else if (state != WINDOW_STATE_MINIMISED) {
			Window_EnterFullscreen();
		}
	} else if (key == KeyBinds[KEYBIND_FOG]) {
		const short* viewDists = Gui_ClassicMenu ? classicDists : normDists;
		int count = Gui_ClassicMenu ? Array_Elems(classicDists) : Array_Elems(normDists);

		if (Key_IsShiftPressed()) {
			InputHandler_CycleDistanceBackwards(viewDists, count);
		} else {
			InputHandler_CycleDistanceForwards(viewDists, count);
		}
	} else if (key == KEY_F5 && Game_ClassicMode) {
		int weather = Env.Weather == WEATHER_SUNNY ? WEATHER_RAINY : WEATHER_SUNNY;
		Env_SetWeather(weather);
	} else {
		if (Game_ClassicMode) return false;
		return HandleNonClassicKey(key);
	}
	return true;
}

static void HandleHotkeyDown(Key key) {
	struct HotkeyData* hkey;
	String text;
	int i = Hotkeys_FindPartial(key);

	if (i == -1) return;
	hkey = &HotkeysList[i];
	text = StringsBuffer_UNSAFE_Get(&HotkeysText, hkey->TextIndex);

	if (!hkey->StaysOpen) {
		Chat_Send(&text, false);
	} else if (!Gui_GetInputGrab()) {
		HUDScreen_OpenInput(&text);
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Base handlers-------------------------------------------------------*
*#########################################################################################################################*/
static void HandleMouseWheel(void* obj, float delta) {
	struct Screen* s;
	int i;
	struct Widget* widget;
	bool hotbar;
	
	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->VTABLE->HandlesMouseScroll(s, delta)) return;
	}

	hotbar = Key_IsAltPressed() || Key_IsControlPressed() || Key_IsShiftPressed();
	if (!hotbar && Camera.Active->Zoom(delta)) return;
	if (InputHandler_DoFovZoom(delta) || !Inventory.CanChangeSelected) return;

	widget = HUDScreen_GetHotbar();
	Elem_HandlesMouseScroll(widget, delta);
}

static void HandlePointerMove(void* obj, int xDelta, int yDelta) {
	struct Screen* s;
	int i;

	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->VTABLE->HandlesMouseMove(s, Mouse_X, Mouse_Y)) return;
	}
}

static void HandlePointerDown(void* obj, int btn) {
	struct Screen* s;
	int i;

	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->VTABLE->HandlesMouseDown(s, Mouse_X, Mouse_Y, btn)) return;
	}
}

static void HandlePointerUp(void* obj, int btn) {
	struct Screen* s;
	int i;

	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->VTABLE->HandlesMouseUp(s, Mouse_X, Mouse_Y, btn)) return;
	}
}

static void HandleInputDown(void* obj, int key, bool was) {
	struct Screen* s;
	int i;

#ifndef CC_BUILD_WEB
	if (key == KEY_ESCAPE && (s = Gui_GetClosable())) {
		/* Don't want holding down escape to go in and out of pause menu */
		if (!was) Gui_Remove(s);
		return;
	}
#endif

	if (InputHandler_IsShutdown(key)) {
		/* TODO: Do we need a separate exit function in Game class? */
		Window_Close(); return;
	} else if (key == KeyBinds[KEYBIND_SCREENSHOT] && !was) {
		Game_ScreenshotRequested = true; return;
	}
	
	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->VTABLE->HandlesKeyDown(s, key)) return;
	}

	if ((key == KEY_ESCAPE || key == KEY_PAUSE) && !Gui_GetInputGrab()) {
#ifdef CC_BUILD_WEB
		/* Can't do this in KeyUp, because pressing escape without having */
		/* explicitly disabled mouse lock means a KeyUp event isn't sent. */
		/* But switching to pause screen disables mouse lock, causing a KeyUp */
		/* event to be sent, triggering the active->closable case which immediately
		/* closes the pause screen. Hence why the next KeyUp must be supressed. */
		suppressEscape = true;
#endif
		PauseScreen_Show(); return;
	}

	/* These should not be triggered multiple times when holding down */
	if (was) return;
	if (HandleBlockKey(key)) {
	} else if (HandleCoreKey(key)) {
	} else if (LocalPlayer_HandlesKey(key)) {
	} else { HandleHotkeyDown(key); }
}

static void HandleInputUp(void* obj, int key) {
	struct Screen* s;
	int i;

	if (key == KeyBinds[KEYBIND_ZOOM_SCROLL]) Game_SetFov(Game_DefaultFov);
#ifdef CC_BUILD_WEB
	/* When closing menus (which reacquires mouse focus) in key down, */
	/* this still leaves the cursor visible. But if this is instead */
	/* done in key up, the cursor disappears as expected. */
	if (key == KEY_ESCAPE && (s = Gui_GetClosable())) {
		if (suppressEscape) { suppressEscape = false; return; }
		Gui_Remove(s); return;
	}
#endif

	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->VTABLE->HandlesKeyUp(s, key)) return;
	}

	if (Gui_GetInputGrab()) return;
	if (key == KeyBinds[KEYBIND_DELETE_BLOCK]) MouseStateRelease(MOUSE_LEFT);
	if (key == KeyBinds[KEYBIND_PLACE_BLOCK])  MouseStateRelease(MOUSE_RIGHT);
	if (key == KeyBinds[KEYBIND_PICK_BLOCK])   MouseStateRelease(MOUSE_MIDDLE);
}

static void HandleKeyPress(void* obj, int keyChar) {
	struct Screen* s;
	int i;

	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->VTABLE->HandlesKeyPress(s, keyChar)) return;
	}
}

void InputHandler_Init(void) {
	Event_RegisterMouseMove(&MouseEvents.Moved, NULL, HandlePointerMove);
	Event_RegisterInt(&MouseEvents.Down,        NULL, HandlePointerDown);
	Event_RegisterInt(&MouseEvents.Up,          NULL, HandlePointerUp);
	Event_RegisterInt(&InputEvents.Down,        NULL, HandleInputDown);
	Event_RegisterInt(&InputEvents.Up,          NULL, HandleInputUp);
	Event_RegisterInt(&InputEvents.Press,       NULL, HandleKeyPress);
	Event_RegisterFloat(&InputEvents.Wheel,     NULL, HandleMouseWheel);

	Event_RegisterVoid(&UserEvents.HackPermissionsChanged, NULL, InputHandler_CheckZoomFov);
	KeyBind_Init();
	Hotkeys_Init();
}
