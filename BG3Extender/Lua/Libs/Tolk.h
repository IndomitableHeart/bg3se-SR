#pragma once

namespace bg3se::lua::tolk {

    void InitializeTolkGlobally();
    void ShutdownTolkGlobally();
    void SetAccessibilityEnabled(bool enabled);

} // namespace bg3se::lua::tolk