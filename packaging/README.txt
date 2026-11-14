ALPHA VIEWS VR  v@VERSION@
by GmoLargey  -  https://github.com/GmoLargey/Alpha-Views-VR
If you want to support me: https://ko-fi.com/virtuallyreal
Free, open-source software under the GNU GPL version 3 (see LICENSE.txt).

See your real room through "windows" in any SteamVR (OpenVR) game.

Alpha Views VR is the SteamVR companion of Alpha Views XR. It places windows
over the game: around your wheel, keyboard, desk, drink, whatever you want to
see. Each window is filled with a solid key colour (green, blue or pink), which
a colour-key passthrough streamer such as Virtual Desktop replaces with your
real room. On headsets whose camera SteamVR can use (e.g. Valve Index, or a
Reverb G2 with the Oasis driver), a window can show the camera picture instead.

A window can be a rounded rectangle, a circle or any free shape you draw, and
it can be a BOX with depth. It lives in the SteamVR dashboard and starts with
SteamVR by itself.

Pictures in this folder:
  Guide.png       the guide panel you see in edit mode (controls, handles)
  Dashboard.png   the Alpha Views panel in the SteamVR dashboard


=============================================================================
1. BEFORE YOU START
=============================================================================
You need:
  - Windows 10/11 (64-bit) and SteamVR.
  - One of these, for the windows to show your room:
      a) A streamer with colour-key passthrough, e.g. Virtual Desktop. Set its
         passthrough colour key to the same colour as Alpha Views VR (green by
         default, see section 4).
      b) A headset with a camera that SteamVR can use (Valve Index, Reverb G2
         with the Oasis driver, ...): choose "camera" as the colour.

Alpha Views VR works with any SteamVR game, as it is drawn by SteamVR on top
of the game. OpenXR games that run through SteamVR work too.


=============================================================================
2. INSTALL
=============================================================================
  1. Unzip this folder somewhere permanent, e.g. C:\Program Files\AlphaViewsVR
     or Documents\AlphaViewsVR (SteamVR remembers where it is: don't move it
     afterwards, or run it once more from the new place).
  2. Start SteamVR.
  3. Run AlphaViewsVR.exe once.

That's it. It registers itself with SteamVR, turns on "start with SteamVR",
and from then on it starts whenever SteamVR does. There is no window on the
desktop: it runs in the background and lives in the SteamVR dashboard.

If you run it while SteamVR isn't running, it tells you to start SteamVR first.

To check it's registered: SteamVR Settings > Startup / Shutdown > Choose
Startup Overlay Apps lists "Alpha Views VR".


=============================================================================
3. THE DASHBOARD PANEL
=============================================================================
Open the SteamVR dashboard (the menu button on your controller). The bottom
bar has an "Alpha Views" icon: it opens the Alpha Views panel (Dashboard.png):

  Edit windows         turn edit mode on (close the dashboard to edit)
  Windows: on / off    show or hide all the windows
  Colour               green, blue, pink (and camera, if your headset has one)
  Guide panel          show the guide panel in edit mode, or not
  Start with SteamVR   on / off
  Delete all windows   hold for 2 seconds

Its status shows the game you are in, how many windows it has, and the camera.


=============================================================================
4. EDIT MODE
=============================================================================
Turn it on with "Edit windows" in the dashboard (then close the dashboard), or
Ctrl+Alt+E on the keyboard.

In edit mode SteamVR's laser pointer comes out of your controller, and the
game gets no controller input until you finish. Point the laser and pull the
trigger (or the grip):

  On a window            select it and move it: it rides on the laser
  Thumbstick up / down   while holding a window: push it away / pull it in
  The handles            on the selected window: grab and drag (below)

The GUIDE PANEL (Guide.png) appears in front of you. Grab it anywhere that
isn't a button to move it. Its buttons:

  New window       a new window in front of you
  Delete           the selected window
  Colour           green / blue / pink / camera
  Move all         hold it and move: every window comes with you
  Preset 1 2 3     tap: load the windows saved there; hold 1.5 s: save them
  Done             save and leave edit mode

HANDLES on the selected window:

  Blue (centre)        move
  Orange (corners)     resize; shrink a window until tiny to delete it
  Magenta              round that corner
  Above the window:
    Lock               lock the window's angle (it then only moves, not turns)
    Shape              switch between rectangle and free shape
    Circle             switch between rectangle and circle (rectangles)
    Draw (pencil)      draw a new outline (free shapes, see below)
    Snap               straighten it: upright, or level (tap again: the other)
  Beside the window:
    Green box          tap: make it a 20 cm deep box, or flat again. Hold it
                       and use the thumbstick: up deeper, down shallower (up
                       to 100 m)
    Box faces          cycle which faces of a box show (box windows only):
                         orange   top and far wall
                         blue     far wall and floor
                         purple   all but the front and the floor
                         green    the whole box
                       The faces belong to the box: "front" is the window's
                       front, "top" the face along the window's top edge.

FREE SHAPES: the orange dots are the outline's points (drag them; tap one to
remove it), the small "+" between two points adds a point. To draw a new
outline: tap the pencil, then hold the trigger anywhere you point and trace
the shape; let go to finish (or end on the yellow start dot). The window takes
the new shape where you drew it.

KEYBOARD (Ctrl+Alt + key, works in or out of edit mode):

  E        edit mode on / off
  N        new window
  X        delete all windows
  P        windows on / off
  C        colour
  G        guide panel on / off
  1 2 3    load preset (with Shift: save it)


=============================================================================
5. PER GAME, PRESETS AND FILES
=============================================================================
Every game has its own windows: Alpha Views VR sees which game is running and
loads its windows (and saves them as you change them). Each game also has its
own three presets.

Files (%LOCALAPPDATA%\AlphaViewsVR):
  log.txt                          what happened (useful if something's wrong)
  settings.cfg                     colour and guide panel setting
  windows_<game>.cfg               each game's windows
  windows_<game>_preset1..3.cfg    its presets


=============================================================================
6. GOOD TO KNOW
=============================================================================
  - SteamVR overlays can't see the game's depth: a window is always drawn in
    front of the game (nothing in the game can come in front of it), and there
    are no "keep boxes" or hands-behind-objects as in Alpha Views XR. Boxes are
    real 3D boxes though: each face is drawn where it really is.
  - SteamVR allows 128 overlays per app: rounded and circular boxes use at
    most 24 sides each. With very many box windows some faces may not show
    (the log says so).
  - In edit mode, the selected box is drawn as one picture (so its handles stay
    in front); the others, and all boxes outside edit mode, show real depth.
  - The camera option shows the camera picture projected into the windows; how
    good it looks depends on your headset's camera and driver.
  - SteamVR's dashboard has to be closed by you (the menu button): SteamVR
    doesn't let an app close it.


=============================================================================
7. UNINSTALL
=============================================================================
  1. In the Alpha Views dashboard panel, turn "Start with SteamVR" off (or in
     SteamVR Settings > Startup / Shutdown > Choose Startup Overlay Apps).
  2. Close SteamVR, then delete the Alpha Views VR folder.
  3. Optional: delete %LOCALAPPDATA%\AlphaViewsVR (your windows and log).


=============================================================================
8. TROUBLESHOOTING
=============================================================================
  - No Alpha Views icon in the dashboard: run AlphaViewsVR.exe once while
    SteamVR is running. Check log.txt.
  - The windows show the key colour but not your room: set your streamer's
    passthrough colour key to the same colour (Colour button / Ctrl+Alt+C).
  - "camera" isn't offered as a colour: SteamVR has no camera for your headset
    (the dashboard panel's status says so).
  - The laser doesn't do anything: edit mode must be on, and the dashboard
    closed.


=============================================================================
LICENCE
=============================================================================
Alpha Views VR is free software under the GNU General Public License, version
3 (GPL-3.0-only): see LICENSE.txt. It comes with ABSOLUTELY NO WARRANTY. The
source code is at https://github.com/GmoLargey/Alpha-Views-VR and in
AlphaViewsVR-@VERSION@-source.zip, published with each
release. It uses Valve's OpenVR API (BSD-3-Clause): see THIRD_PARTY_NOTICES.txt.
