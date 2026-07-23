#include "Updater.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    return openreplay::updater::Run(instance);
}
