#include "App.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    return eb::run_app(inst, show);
}
