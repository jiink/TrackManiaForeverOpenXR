#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#define DirectInput8Create TMOXR_SDK_DECLARATION_DirectInput8Create
#include <dinput.h>
#undef DirectInput8Create

#include <cstdio>
#include <cstring>
#include <array>

struct EnumerationResult {
    GUID instance{};
    bool found = false;
};

BOOL CALLBACK FindVirtualGamepad(const DIDEVICEINSTANCEA* instance, void* context) {
    auto* result = static_cast<EnumerationResult*>(context);
    if (std::strstr(instance->tszProductName, "Meta Quest Touch")) {
        result->instance = instance->guidInstance;
        result->found = true;
    }
    return DIENUM_CONTINUE;
}

int main(int argumentCount, char** arguments) {
    if (argumentCount != 2) return 2;
    HMODULE module = LoadLibraryA(arguments[1]);
    if (!module) return 3;
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "DirectInput8Create"));
    if (!create) return 4;
    IDirectInput8A* input = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&input), nullptr)) || !input) return 5;
    EnumerationResult enumeration;
    if (FAILED(input->EnumDevices(DI8DEVCLASS_GAMECTRL, FindVirtualGamepad, &enumeration, DIEDFL_ATTACHEDONLY)) || !enumeration.found) return 6;
    IDirectInputDevice8A* device = nullptr;
    if (FAILED(input->CreateDevice(enumeration.instance, &device, nullptr)) || !device) return 7;
    std::array<DIOBJECTDATAFORMAT, 16> objects{};
    objects[0] = {const_cast<GUID*>(&GUID_XAxis), DIJOFS_X, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0), 0};
    objects[1] = {const_cast<GUID*>(&GUID_YAxis), DIJOFS_Y, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1), 0};
    objects[2] = {const_cast<GUID*>(&GUID_ZAxis), DIJOFS_Z, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2), 0};
    objects[3] = {const_cast<GUID*>(&GUID_RxAxis), DIJOFS_RX, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3), 0};
    objects[4] = {const_cast<GUID*>(&GUID_RyAxis), DIJOFS_RY, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(4), 0};
    objects[5] = {const_cast<GUID*>(&GUID_POV), DIJOFS_POV(0), DIDFT_POV | DIDFT_MAKEINSTANCE(0), 0};
    for (DWORD button = 0; button < 10; ++button) {
        objects[6 + button] = {const_cast<GUID*>(&GUID_Button), static_cast<DWORD>(DIJOFS_BUTTON(button)),
                               static_cast<DWORD>(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(button)), 0};
    }
    DIDATAFORMAT format{sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS,
                        sizeof(DIJOYSTATE2), static_cast<DWORD>(objects.size()), objects.data()};
    if (FAILED(device->SetDataFormat(&format)) || FAILED(device->Acquire()) || FAILED(device->Poll())) return 8;
    DIJOYSTATE2 state{};
    if (FAILED(device->GetDeviceState(sizeof(state), &state))) return 9;
    const bool centered = state.lX >= 32760 && state.lX <= 32775 && state.lY >= 32760 && state.lY <= 32775 &&
        state.lZ >= 32760 && state.lZ <= 32775;
    std::printf("virtual gamepad: X=%ld Y=%ld Z=%ld centered=%d\n", state.lX, state.lY, state.lZ, centered ? 1 : 0);
    device->Release();
    input->Release();
    FreeLibrary(module);
    return centered ? 0 : 10;
}
