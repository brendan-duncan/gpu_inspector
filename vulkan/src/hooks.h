// Hand-written post-call hooks for specific commands (see EXTRA_HOOKS in tools/vkgen/dispatch.py).
// The generated forwarders call Hook_<command>(args) after forwarding and object registration.
#pragma once

#include "vk_hooks.gen.h"
