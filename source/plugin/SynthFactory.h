#pragma once

#include <string>
#include "SynthType.h"
#include "synthLib/device.h"

namespace retromulator
{
    class SynthFactory
    {
    public:
        // Create a device for the given synth type.
        // romPath may be a file path or directory; each synth's RomLoader
        // will search the path if needed.
        static synthLib::Device* create(SynthType type, const std::string& romPath = {});

        // Which board the 88emu core boots. Switching it means recreating the device:
        // the models differ in CPU, chipset and ROM set, so it is not a live parameter.
        // Values are emu88Lib::DeviceModel; -1 picks the first board whose ROMs are complete.
        static void setEmu88Model(int model);
        static int  getEmu88Model();
    };
}
