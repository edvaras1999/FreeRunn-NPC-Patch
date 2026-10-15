// Camera.h - keeping the camera still while an action plays.
//
// During a climb the player is not steering, so the camera should not be either:
// the third person zoom is frozen where it was, the camera's own input is
// ignored, and in first person the head is kept from pitching far enough to see
// through the body.

#pragma once

namespace Camera
{
	bool InstallHooks();
}
