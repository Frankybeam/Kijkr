#pragma once
/*
 * EVO's words in the chosen language (see evo_i18n.h). The kit's fonts.hpp
 * (third_party/ps5-homebrew-ui/src/ui/fonts.hpp) carries the hook; with an
 * older copy of that file EVO still builds, only without translations.
 */
#include "ui/fonts.hpp"

#include <string_view>

namespace evo::kit
{
inline std::string_view tr(std::string_view text)
{
#ifdef HUI_HAS_TEXT_FILTER
    return hui::ui::filtered(text);
#else
    return text;
#endif
}
} // namespace evo::kit
