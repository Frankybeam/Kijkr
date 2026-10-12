/*
 * evo_hui_guide.hpp - the TV guide grid on ps5-homebrew-ui: channels down the
 * side, their programmes along a clock, the focused programme described
 * underneath. ProviderHostScreen keeps the data and the focus; this draws it.
 */
#ifndef EVO_HUI_GUIDE_HPP
#define EVO_HUI_GUIDE_HPP

#include "evo_rmlui_bridge.h"
#include "evo_hui_screens.hpp"

#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

#include <string>
#include <vector>

namespace evo::kit
{

class GuideGridScreen
{
  public:
    void set(const evo_hui_guide_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;

  private:
    struct Prog
    {
        long long start = 0, stop = 0;
        std::string title;
        bool catchup = false;
    };
    struct Row
    {
        std::string name, number;
        bool focused = false, archive = false;
        int focus_prog = -1;
        std::vector<Prog> progs;
    };

    std::string title_, crumb_, sel_title_, sel_time_, sel_state_, sel_desc_, note_, hint_accept_;
    long long now_ = 0, win_start_ = 0, win_end_ = 0;
    int utc_offset_ = 0, first_ = 0, total_ = 0;
    std::vector<Row> rows_;
    float age_ = 0.0f;
};

} // namespace evo::kit

#endif /* EVO_HUI_GUIDE_HPP */
