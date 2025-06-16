option("profile")
    set_default(false)
    set_description("Enable application wide profiling.")
    add_defines("TRACY_ENABLE=1", { public = true })

option("lua_bindings")
    set_default(true)
    set_showmenu(true)
    set_description("Enable Lua bindings")

