// A shared object that carries the plugin entry points but makes no plugin instance.

// A DLL exports only what it marks; without the mark the loader would find no entry point and pass the file over
#if defined(_WIN32)
#define GR4CP_PLUGIN_ENTRY_POINT extern "C" __declspec(dllexport)
#else
#define GR4CP_PLUGIN_ENTRY_POINT extern "C"
#endif

GR4CP_PLUGIN_ENTRY_POINT void gr_plugin_make(void** plugin) {
    *plugin = nullptr;
}

GR4CP_PLUGIN_ENTRY_POINT void gr_plugin_free(void*) {}
