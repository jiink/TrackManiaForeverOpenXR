#pragma once

namespace tmoxr {

// Meta's 32-bit OpenXR runtime (the "oculus-runtime" modules) looks up
// kernelbase!SetThreadDescription with GetProcAddress and then calls it through
// a __cdecl function pointer, although the function is __stdcall. Both sides
// pop the arguments, so the naming helper restores the wrong callee-saved
// registers and the runtime's first worker thread jumps to a garbage address
// inside xrCreateSession. 64-bit processes have one calling convention and are
// unaffected.
//
// The runtime treats a missing SetThreadDescription as "leave the thread
// unnamed", so this redirects GetProcAddress in the import tables of the loaded
// runtime modules to a wrapper which reports that one function as missing.
//
// Call after the runtime is loaded (xrCreateInstance) and before
// xrCreateSession. Safe to call repeatedly. Returns the number of import slots
// redirected by this call.
unsigned HideThreadDescriptionFromMetaRuntime();

} // namespace tmoxr
