// HSPassthrough.log next to the DLL (and ReShade's log once it is registered).
#pragma once

namespace hslog
{
	void init(void *module);
	void info(const char *format, ...);
}
