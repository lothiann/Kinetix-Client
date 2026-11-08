// (c) Kinetix. Helpers shared between clickgui.cpp and array.cpp (the Array*
// HUD translation unit). Keep this header tiny — everything here must stay
// usable from both files.

#pragma once

#include <base/color.h>

// Color with a stored ARGB alpha multiplied by the UI fade alpha — so an
// AA channel below ff actually renders translucent (WithAlpha alone would
// overwrite the stored alpha).
inline ColorRGBA KxColorWithAlpha(unsigned RGBA, float Alpha)
{
	const float StoredA = ((RGBA >> 24) & 0xff) / 255.0f;
	return ColorRGBA(RGBA, true).WithAlpha(Alpha * StoredA);
}
