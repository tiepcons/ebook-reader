#include "App.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int show)
{
    return eb::run_app(inst, cmdline, show);
}
