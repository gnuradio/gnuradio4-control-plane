// A shared object that carries the plugin entry points but makes no plugin instance.
extern "C" void gr_plugin_make(void** plugin) {
    *plugin = nullptr;
}

extern "C" void gr_plugin_free(void*) {}
