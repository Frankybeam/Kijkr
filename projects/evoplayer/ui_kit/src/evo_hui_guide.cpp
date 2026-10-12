/*
 * evo_hui_guide.cpp - the TV guide grid on ps5-homebrew-ui. See
 * evo_hui_guide.hpp.
 */
#include "evo_hui_guide.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <cstdio>

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace ui = ::hui::ui;

namespace
{
const Color kWhite = Color::rgb(0xffffff);
const Color kLive = Color::rgb(0xff4d4d);
const Color kArchive = Color::rgb(0x3ccf91);

constexpr float kLeft = NavRail::kWidth + 56.0f;
constexpr float kRight = 1864.0f;
constexpr float kChanW = 300.0f;
constexpr float kAxisY = 226.0f;
constexpr float kRowsY = 250.0f;
constexpr float kRowH = 70.0f;
constexpr float kRowGap = 8.0f;

std::string clock_text(long long t, int offset)
{
    long long s = (t + offset) % 86400;
    if (s < 0) s += 86400;
    char b[8];
    std::snprintf(b, sizeof b, "%02d:%02d", static_cast<int>(s / 3600), static_cast<int>((s / 60) % 60));
    return b;
}
} // namespace

void GuideGridScreen::set(const evo_hui_guide_params_t &p)
{
    auto S = [](const char *s) { return std::string(s ? s : ""); };
    title_ = S(p.title);
    crumb_ = S(p.crumb);
    sel_title_ = S(p.sel_title);
    sel_time_ = S(p.sel_time);
    sel_state_ = S(p.sel_state);
    sel_desc_ = S(p.sel_desc);
    note_ = S(p.note);
    hint_accept_ = S(p.hint_accept);
    now_ = p.now;
    win_start_ = p.win_start;
    win_end_ = p.win_end > p.win_start ? p.win_end : p.win_start + 7200;
    utc_offset_ = p.utc_offset;
    first_ = p.first;
    total_ = p.total;
    rows_.clear();
    const int n = std::min(p.row_count, EVO_HUI_GUIDE_ROWS);
    for (int i = 0; i < n; ++i)
    {
        const evo_hui_guide_row_t &in = p.rows[i];
        Row r;
        r.name = S(in.name);
        r.number = S(in.number);
        r.focused = in.focused != 0;
        r.archive = in.archive != 0;
        r.focus_prog = in.focus_prog;
        const int m = std::min(in.prog_count, EVO_HUI_GUIDE_PROGS);
        for (int j = 0; j < m; ++j)
        {
            Prog g;
            g.start = in.progs[j].start;
            g.stop = in.progs[j].stop;
            g.title = S(in.progs[j].title);
            g.catchup = in.progs[j].catchup != 0;
            r.progs.push_back(std::move(g));
        }
        rows_.push_back(std::move(r));
    }
}

void GuideGridScreen::enter()
{
    age_ = 0.0f;
}

void GuideGridScreen::update(float dt)
{
    age_ += dt;
}

void GuideGridScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &pal = ctx.palette;
    list.push_opacity(std::min(1.0f, age_ * 5.0f));

    /* header */
    ui::text(list, fonts.semibold, title_, kLeft, 120, 14, pal.accent, Align::left, 2.0f);
    ui::text(list, fonts.display, fonts.display.font->fit(crumb_, 34, 1100), kLeft, 166, 34, kWhite);
    if (total_ > 0)
    {
        char counter[48];
        const int last = std::min(first_ + static_cast<int>(rows_.size()), total_);
        std::snprintf(counter, sizeof counter, "%d-%d / %d", first_ + 1, last, total_);
        ui::text(list, fonts.semibold, counter, kRight, 166, 18, pal.text_faint, Align::right);
    }

    /* the clock along the top, a mark every half hour */
    const float gx = kLeft + kChanW + 12.0f;
    const float gw = kRight - gx;
    const double span = static_cast<double>(win_end_ - win_start_);
    auto xat = [&](long long t) {
        const double f = static_cast<double>(t - win_start_) / span;
        return gx + static_cast<float>(std::max(0.0, std::min(1.0, f))) * gw;
    };
    const long long first_mark = ((win_start_ + utc_offset_ + 1799) / 1800) * 1800 - utc_offset_;
    for (long long t = first_mark; t < win_end_; t += 1800)
    {
        const float x = xat(t);
        ui::text(list, fonts.semibold, clock_text(t, utc_offset_), x + 6, kAxisY - 6, 16, pal.text_muted);
        list.line(x, kAxisY, x, kRowsY + EVO_HUI_GUIDE_ROWS * (kRowH + kRowGap), 1.0f, kWhite.with_alpha(0.08f));
    }

    /* rows */
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const Row &r = rows_[i];
        const float y = kRowsY + static_cast<float>(i) * (kRowH + kRowGap);
        Rect ch{kLeft, y, kChanW, kRowH};
        list.rounded_rect(ch, 14, r.focused ? pal.surface_sel : pal.surface);
        if (r.focused)
            list.bordered_rect(ch, 14, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.6f));
        ui::text(list, fonts.semibold, r.number, ch.x + 18, ch.cy() + 7, 16, pal.text_faint);
        const float nx = ch.x + 64;
        const float nw = ch.w - 64 - (r.archive ? 34 : 14);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(r.name, 19, nw), nx, ch.cy() + 7, 19, kWhite);
        if (r.archive)
            list.circle(ch.x + ch.w - 20, ch.cy(), 5, kArchive);

        list.push_clip({gx, y, gw, kRowH});
        if (r.progs.empty())
        {
            Rect e{gx, y, gw, kRowH};
            list.rounded_rect(e, 12, kWhite.with_alpha(0.03f));
            ui::text(list, fonts.regular, "No programme information", gx + 18, e.cy() + 6, 16, pal.text_faint);
        }
        for (std::size_t j = 0; j < r.progs.size(); ++j)
        {
            const Prog &g = r.progs[j];
            if (g.stop <= win_start_ || g.start >= win_end_)
                continue;
            const float x0 = xat(g.start) + 2.0f, x1 = xat(g.stop) - 2.0f;
            if (x1 - x0 < 4.0f)
                continue;
            Rect b{x0, y, x1 - x0, kRowH};
            const bool on_now = g.start <= now_ && now_ < g.stop;
            const bool past = g.stop <= now_;
            const bool sel = r.focused && static_cast<int>(j) == r.focus_prog;
            Color fill = on_now ? pal.accent.with_alpha(0.22f) : kWhite.with_alpha(past ? 0.035f : 0.07f);
            if (sel)
                fill = pal.surface_sel;
            list.rounded_rect(b, 12, fill);
            if (sel)
                list.bordered_rect(b, 12, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.9f));
            const float tx = std::max(b.x, gx) + 14.0f;
            const float tw = b.x + b.w - tx - 10.0f;
            if (tw > 24.0f)
            {
                const Color tc = past && !g.catchup ? pal.text_faint : kWhite;
                ui::text(list, fonts.semibold, fonts.semibold.font->fit(g.title, 17, tw), tx, b.y + 30, 17, tc);
                std::string when = clock_text(g.start, utc_offset_);
                ui::text(list, fonts.regular, fonts.regular.font->fit(when, 14, tw), tx, b.y + 54, 14,
                         pal.text_faint);
            }
            if (past && g.catchup)
                list.circle(b.x + b.w - 14, b.y + 14, 4, kArchive);
            if (on_now)
            {
                const float p = static_cast<float>(now_ - g.start) / static_cast<float>(g.stop - g.start);
                list.rounded_rect({b.x + 6, b.y + b.h - 6, (b.w - 12) * p, 3}, 1.5f, pal.accent);
            }
        }
        list.pop_clip();
    }

    /* now */
    if (now_ >= win_start_ && now_ < win_end_)
    {
        const float x = xat(now_);
        const float bottom = kRowsY + static_cast<float>(rows_.size()) * (kRowH + kRowGap) - kRowGap;
        list.line(x, kAxisY + 4, x, bottom, 2.0f, kLive.with_alpha(0.9f));
        list.circle(x, kAxisY + 4, 5, kLive);
    }

    /* the focused programme */
    const float dy = 892.0f;
    if (!sel_title_.empty())
    {
        float sx = kLeft;
        if (!sel_state_.empty())
        {
            const float bw = fonts.semibold.measure(sel_state_, 13, 1.2f) + 22;
            const bool live = sel_state_.rfind("On now", 0) == 0;
            const bool arch = sel_state_.rfind("Catch-up", 0) == 0;
            Rect bp{sx, dy - 20, bw, 28};
            list.rounded_rect(bp, 14, live ? kLive.with_alpha(0.3f) : arch ? kArchive.with_alpha(0.25f)
                                                                           : kWhite.with_alpha(0.1f));
            ui::text(list, fonts.semibold, sel_state_, bp.cx(), bp.y + 19, 13, kWhite, Align::center, 1.2f);
            sx += bw + 14;
        }
        ui::text(list, fonts.semibold, sel_time_, sx, dy, 18, pal.text_muted);
        ui::text(list, fonts.display, fonts.display.font->fit(sel_title_, 28, kRight - kLeft), kLeft, dy + 40, 28,
                 kWhite);
        if (!sel_desc_.empty())
            ui::paragraph(list, fonts.regular, sel_desc_, kLeft, dy + 72, 16, kRight - kLeft - 300, 22,
                          pal.text_muted, 2);
    }
    else if (!note_.empty())
    {
        ui::paragraph(list, fonts.regular, note_, kLeft, dy + 10, 18, kRight - kLeft - 300, 26, pal.text_muted, 2);
    }

    ui::Hint hints[5];
    int nh = 0;
    if (!hint_accept_.empty())
        hints[nh++] = {ui::Button::cross, hint_accept_.c_str()};
    hints[nh++] = {ui::Button::l2, "EARLIER"};
    hints[nh++] = {ui::Button::r2, "LATER"};
    hints[nh++] = {ui::Button::circle, "CLOSE"};
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = 1030.0f;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, nh, 1876.0f, true, layout);
    list.pop_opacity();
}

} // namespace evo::kit
