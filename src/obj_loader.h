// obj_loader.h - OBJ/MTL import with triangulation, smooth-normal generation,
// deterministic material mapping, auto-centering and camera framing.
#pragma once
#include "scene.h"
#include <string>

struct ObjLoadResult {
    bool        ok = false;
    std::string message;   // error or warning text for the UI
    Scene       scene;
};

// Loads an OBJ from a wide (UTF-16) path so non-ASCII directories work.
ObjLoadResult loadObj(const std::wstring& widePath);
