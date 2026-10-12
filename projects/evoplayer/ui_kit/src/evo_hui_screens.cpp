/*
 * evo_hui_screens.cpp - EVO's screens on ps5-homebrew-ui. See the header.
 *
 * The look follows the kit's Aurora Shelf design (third_party/ps5-homebrew-ui
 * src/concepts/aurora.cpp): a hero block for the title you were watching, 16:9
 * shelves under it, one focus ring that glides between whatever has the
 * cursor, and entrances that stagger in rather than pop.
 */
#include "evo_hui_tr.hpp"
#include "evo_hui_screens.hpp"

#include "evo_features.h"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../stb_image.h"
#pragma clang diagnostic pop

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace tween = ::hui::tween;
namespace ui = ::hui::ui;

namespace
{

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);

/* EVO theme words are 0xAABBGGRR. */
Color from_evo(std::uint32_t c, bool use_alpha)
{
    const float a = use_alpha ? static_cast<float>((c >> 24) & 0xff) / 255.0f : 1.0f;
    return {static_cast<float>(c & 0xff) / 255.0f, static_cast<float>((c >> 8) & 0xff) / 255.0f,
            static_cast<float>((c >> 16) & 0xff) / 255.0f, a};
}

/* A one-line string cut to width with an ellipsis. */
std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(evo::kit::tr(text), size, width);
}

} // namespace

/* ---------------------------------------------------------------- textures */

TextureCache::~TextureCache()
{
    clear();
}

void TextureCache::clear()
{
    for (auto &kv : entries_)
        host_.release_texture(kv.second.handle);
    entries_.clear();
    px_bytes_ = 0;
}

/*
 * Covers share a 64 MB GPU pool with everything else on screen. Scrolling Home
 * fast uploaded a new texture per poster (and one per half-decoded state of
 * it) faster than the idle ones left, and the pool ran to 62 of its 64 MB.
 * Keep the covers under a budget: the longest-unused go first, and a cover
 * whose pixels changed (same buffer, newer picture) replaces its old upload.
 */
static const std::size_t kCoverBudget = 36u << 20;

void TextureCache::trim(std::size_t incoming)
{
    while (px_bytes_ + incoming > kCoverBudget)
    {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->first.compare(0, 3, "px:") == 0 && it->second.idle_frames > 0 &&
                (oldest == entries_.end() || it->second.idle_frames > oldest->second.idle_frames))
                oldest = it;
        if (oldest == entries_.end())
            return;                              /* all on screen now: nothing to drop */
        host_.release_texture(oldest->second.handle);
        px_bytes_ -= std::min(px_bytes_, oldest->second.bytes);
        entries_.erase(oldest);
    }
}

std::uint32_t TextureCache::pixels(const std::uint32_t *data, int width, int height,
                                   std::uint64_t tag)
{
    if (!data || width <= 0 || height <= 0)
        return 0;
    /* The cover service decodes into its buffers after handing them out, so
     * the same pointer first holds black and later the picture. A sparse hash
     * of the pixels goes into the key: new content, new upload. */
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::uint64_t sample = 1469598103934665603ull;
    for (std::size_t i = 0; i < count; i += 61)
        sample = (sample ^ data[i]) * 1099511628211ull;
    char key[128], head[96];
    std::snprintf(head, sizeof(head), "px:%p:%dx%d:%llx:", static_cast<const void *>(data), width,
                  height, static_cast<unsigned long long>(tag));
    std::snprintf(key, sizeof(key), "%s%llx", head, static_cast<unsigned long long>(sample));
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.idle_frames = 0;
        return it->second.handle;
    }
    /* The same buffer with older pixels: that upload is stale now. */
    const std::size_t hl = std::strlen(head);
    for (auto o = entries_.begin(); o != entries_.end();)
    {
        if (o->first.compare(0, hl, head) == 0)
        {
            host_.release_texture(o->second.handle);
            px_bytes_ -= std::min(px_bytes_, o->second.bytes);
            o = entries_.erase(o);
        }
        else
        {
            ++o;
        }
    }
    Entry e;
    e.bytes = count * 4u;
    trim(e.bytes);
    e.handle = host_.create_texture(width, height, reinterpret_cast<const std::uint8_t *>(data));
    if (e.handle)
        px_bytes_ += e.bytes;
    else
        e.bytes = 0;
    entries_[key] = e;
    return e.handle;
}

std::uint32_t TextureCache::icon(const std::string &path)
{
    if (path.empty())
        return 0;
    const std::string key = "icon:" + path;
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.idle_frames = 0;
        return it->second.handle;
    }
    Entry e;
    std::string bytes;
    if (host_.read_asset(path, &bytes))
    {
        int w = 0, h = 0, n = 0;
        unsigned char *rgba =
            stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                                  static_cast<int>(bytes.size()), &w, &h, &n, 4);
        if (rgba)
        {
            e.handle = host_.create_texture(w, h, rgba);
            stbi_image_free(rgba);
        }
    }
    /* A failed load is cached too (handle 0), so a missing icon is not
     * re-read every frame. */
    entries_[key] = e;
    return e.handle;
}

void TextureCache::tick()
{
    /* Icons stay; covers that left the screen go after about two seconds. */
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (it->first.compare(0, 3, "px:") == 0 && ++it->second.idle_frames > 120)
        {
            host_.release_texture(it->second.handle);
            px_bytes_ -= std::min(px_bytes_, it->second.bytes);
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

/* ----------------------------------------------------------------- palette */

Palette Palette::from(const evo_rmlui_theme_t &t)
{
    Palette p;
    p.bg_top = from_evo(t.bg_top, false);
    p.bg_bottom = from_evo(t.bg_bottom, false);
    p.surface = from_evo(t.surface, true);
    p.surface_sel = from_evo(t.surface_sel, true);
    p.border = from_evo(t.border, true);
    p.accent = from_evo(t.accent, false);
    p.accent_soft = from_evo(t.accent_soft, false);
    p.accent_alt = from_evo(t.accent_alt, false);
    p.text = from_evo(t.text_primary, false);
    p.text_muted = from_evo(t.text_secondary, false);
    p.text_faint = from_evo(t.text_muted, false);
    return p;
}

/* ---------------------------------------------------------------- backdrop */

void draw_backdrop(DrawList &list, const Context &ctx, std::uint32_t picture, float picture_alpha)
{
    const Palette &p = ctx.palette;
    const Rect screen{0, 0, ::hui::gfx::kVirtualWidth, ::hui::gfx::kVirtualHeight};
    list.gradient_rect(screen, 0, p.bg_top, p.bg_bottom);

    /* The picture, washed in from the right: the left side stays dark for
     * the text that sits on it. */
    if (picture && picture_alpha > 0.0f)
    {
        list.image(picture, screen, ::hui::gfx::kFullUv, kWhite.with_alpha(0.55f * picture_alpha));
        list.gradient_rect_h(screen, 0, p.bg_bottom.with_alpha(0.97f), p.bg_bottom.with_alpha(0.35f));
        list.gradient_rect({0, 540, screen.w, 540}, 0, p.bg_bottom.with_alpha(0.0f),
                           p.bg_bottom.with_alpha(0.92f));
    }

    /* Aurora: three large soft lights that drift on slow, unrelated periods. */
    const float t = ctx.time;
    struct Light
    {
        float x, y, r, period, phase;
        Color c;
    };
    const Light lights[] = {
        {1500, 160, 360, 23.0f, 0.0f, p.accent.with_alpha(0.16f)},
        {420, 900, 420, 31.0f, 2.1f, p.accent_alt.with_alpha(0.12f)},
        {1200, 980, 300, 19.0f, 4.2f, p.accent_soft.with_alpha(0.14f)},
    };
    for (const Light &l : lights)
    {
        const float a = 6.2831853f * t / l.period + l.phase;
        const float x = l.x + 120.0f * std::sin(a);
        const float y = l.y + 70.0f * std::cos(a * 0.7f);
        list.shadow({x - l.r, y - l.r, 2 * l.r, 2 * l.r}, l.r, l.r * 0.9f, l.c);
    }
}

/* ----------------------------------------------------------------- the rail */

namespace
{

struct RailItem
{
    const char *label;
    const char *icon;
};

/* ScreenManager's section order; Providers only when the build has them. */
int rail_items(RailItem *out)
{
    int n = 0;
    out[n++] = {"Home", "../icons/icon_home.png"};
    out[n++] = {"Live TV", "../icons/icon_tv.png"};
    if (EVO_ENABLE_EMBY)
        out[n++] = {"Providers", "../icons/icon_emby.png"};
    out[n++] = {"Settings", "../icons/icon_settings.png"};
    out[n++] = {"About", "../icons/icon_about_support.png"};
    out[n++] = {"Quit Kijkr", "../icons/icon_power.png"};
    return n;
}

constexpr float kRailTop = 300.0f;
constexpr float kRailPitch = 88.0f;
constexpr float kRailExpanded = 360.0f;

float rail_item_y(int index, int count)
{
    /* Quit sits apart at the foot of the rail. */
    if (index == count - 1)
        return 1080.0f - 150.0f;
    return kRailTop + static_cast<float>(index) * kRailPitch;
}

} // namespace

void NavRail::update(float dt)
{
    expand_.target = params_.rail_focused ? 1.0f : 0.0f;
    expand_.update(dt, 16.0f);
    RailItem items[8];
    const int count = rail_items(items);
    const int at = params_.rail_focused ? params_.cursor_index : params_.active_section;
    const int index = std::clamp(at, 0, count - 1);
    const float width = tween::lerp(72.0f, kRailExpanded - 56.0f, expand_.value);
    const Rect target{28.0f, rail_item_y(index, count), width, 72.0f};
    if (!cursor_snapped_)
    {
        cursor_.snap(target);
        cursor_snapped_ = true;
    }
    cursor_.target(target);
    cursor_.update(dt, 22.0f);
}

void NavRail::draw(DrawList &list, const Context &ctx) const
{
    if (!params_.visible)
        return;
    const Palette &p = ctx.palette;
    const ui::Fonts &fonts = ctx.fonts;
    const float e = expand_.value;
    RailItem items[8];
    const int count = rail_items(items);

    /* Scrim over the content while the rail is open. */
    if (e > 0.01f)
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.5f * e));

    const float width = tween::lerp(kWidth, kRailExpanded, e);
    const Rect panel{0, 0, width, 1080};
    list.gradient_rect_h(panel, 0, p.bg_bottom.with_alpha(0.94f + 0.05f * e),
                         p.bg_bottom.with_alpha(0.7f + 0.27f * e));
    list.rounded_rect({width - 1.5f, 0, 1.5f, 1080}, 0, kWhite.with_alpha(0.07f));

    /* Logo. */
    const std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
    if (logo)
        list.image(logo, {36, 64, 56, 56}, ::hui::gfx::kFullUv, kWhite);
    if (e > 0.01f)
    {
        list.push_opacity(e);
        ui::text(list, fonts.display, "KIJKR", 112, 104, 34, kWhite);
        list.pop_opacity();
    }

    /* The highlight: a pill that glides to the cursor (rail open) or rests on
     * the active section (rail closed). */
    const Rect pill = cursor_.value();
    if (params_.rail_focused)
    {
        list.glow(pill, 36, 18, p.accent.with_alpha(0.45f));
        list.rounded_rect(pill, 36, p.accent);
    }
    else
    {
        list.rounded_rect(pill, 36, kWhite.with_alpha(0.10f));
    }

    for (int i = 0; i < count; ++i)
    {
        const float y = rail_item_y(i, count);
        const bool cursor = params_.rail_focused && i == params_.cursor_index;
        const bool active = i == params_.active_section;
        const Color ink = cursor ? kInk : (active ? kWhite : kWhite.with_alpha(0.62f));
        const std::uint32_t icon = ctx.textures.icon(items[i].icon);
        if (icon)
            list.image(icon, {48, y + 20, 32, 32}, ::hui::gfx::kFullUv, ink);
        if (!params_.rail_focused && active)
            list.rounded_rect({8, y + 22, 4, 28}, 2, p.accent);
        if (e > 0.01f)
        {
            list.push_opacity(tween::clamp01(e * 1.6f - 0.4f));
            ui::text(list, cursor ? fonts.semibold : fonts.regular, items[i].label, 104, y + 46, 26,
                     ink);
            list.pop_opacity();
        }
    }

    if (params_.show_fps)
    {
        char fps[24];
        std::snprintf(fps, sizeof(fps), "%d FPS", params_.fps);
        const Rect chip{1920 - 180, 1080 - 64, 140, 40};
        list.rounded_rect(chip, 20, Color::rgb(0x000000, 0.55f));
        ui::text(list, fonts.mono, fps, chip.cx(), chip.y + 28, 20, p.accent, Align::center);
    }
}

/* ------------------------------------------------------------------- home */

namespace
{

constexpr float kLeft = NavRail::kWidth + 64.0f; /* content's left edge */
constexpr float kCardW = 320.0f;
constexpr float kCardH = 180.0f; /* the cover service's 16:9 posters */
constexpr float kCardGap = 30.0f;
constexpr float kPosterW = 180.0f; /* a film or series poster, 2:3 */
constexpr float kPosterH = 270.0f;
constexpr float kPosterGap = 24.0f;
constexpr float kRowGap = 94.0f;   /* card bottom to the next row's cards */
constexpr float kRecentY = 636.0f;
constexpr float kLibraryY = 920.0f;
constexpr float kLibW = 250.0f;
constexpr float kLibH = 96.0f;
constexpr float kLibGap = 22.0f;
constexpr float kPageFloor = 1016.0f; /* a row ending lower than this scrolls the page */
constexpr float kShelfView = 1824.0f - kLeft; /* visible shelf width */
const Rect kHeroArt{1176, 164, 640, 360};
const Rect kHeroAction{kLeft, 452, 260, 64};

/* A cover-fit crop of a w x h picture into a box of the given aspect. */
Rect home_cover_uv(int w, int h, float box_w, float box_h)
{
    if (w <= 0 || h <= 0)
        return ::hui::gfx::kFullUv;
    const float img = static_cast<float>(w) / static_cast<float>(h);
    const float box = box_w / box_h;
    if (img > box)
    {
        const float f = box / img;
        return {(1.0f - f) * 0.5f, 0.0f, f, 1.0f};
    }
    const float f = img / box;
    return {0.0f, (1.0f - f) * 0.5f, 1.0f, f};
}

} // namespace

void HomeScreen::set(const evo_rmlui_launch_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    app_name_ = str(p.app_name);
    version_ = str(p.version);
    clock_ = str(p.clock);

    Hero next;
    next.eyebrow = str(p.hero_eyebrow);
    next.title = str(p.hero_title);
    next.detail = str(p.hero_detail);
    next.action = str(p.hero_action);
    next.overview = str(p.hero_overview);
    next.progress = p.hero_progress;
    next.art = p.hero_art;
    next.art_w = p.hero_art_w;
    next.art_h = p.hero_art_h;
    if (next.title != hero_.title && !hero_.title.empty())
    {
        previous_hero_ = hero_;
        hero_fade_.start(0.45f);
    }
    hero_ = next;
    hero_focused_ = p.hero_focused != 0;

    recent_total_ = p.recent_total;
    recent_cursor_ = p.recent_cursor;
    auto tile = [&](const evo_rmlui_launch_tile_t &in) {
        Tile t;
        t.title = str(in.title);
        t.detail = str(in.detail);
        t.icon_path = str(in.icon_path);
        t.progress = in.progress;
        t.art = in.art;
        t.art_w = in.art_w;
        t.art_h = in.art_h;
        t.focused = in.is_focused != 0;
        t.badge = str(in.badge);
        return t;
    };
    recent_.clear();
    for (int i = 0; i < p.recent_visible && i < EVO_RMLUI_TILES; ++i)
        recent_.push_back(tile(p.recent[i]));
    library_.clear();
    for (int i = 0; i < p.library_visible && i < EVO_RMLUI_TILES; ++i)
        library_.push_back(tile(p.library[i]));
    {
        const evo_rmlui_launch_details_t &d = p.details;
        details_.active = d.active != 0;
        details_.eyebrow = str(d.eyebrow);
        details_.title = str(d.title);
        details_.meta = str(d.meta);
        details_.overview = str(d.overview);
        details_.art = d.art;
        details_.art_w = d.art_w;
        details_.art_h = d.art_h;
        details_.progress = d.progress;
        details_.focus = d.focus;
        details_.buttons.clear();
        for (int i = 0; i < d.button_count && i < EVO_RMLUI_DETAIL_BUTTONS; ++i)
            details_.buttons.emplace_back(str(d.buttons[i].label), str(d.buttons[i].detail));
    }
    for (int s = 0; s < EVO_RMLUI_SHELVES; ++s)
    {
        const evo_rmlui_launch_shelf_t &in = p.shelves[s];
        Shelf &out = shelves_[s];
        out.title = str(in.title);
        out.cursor = in.cursor;
        out.portrait = in.portrait != 0;
        out.removable = in.removable != 0;
        out.tiles.clear();
        for (int i = 0; i < in.visible && i < EVO_RMLUI_SHELF_TILES; ++i)
            out.tiles.push_back(tile(in.tiles[i]));
    }
}

void HomeScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
}

/* ---- layout, in page coordinates ---- */

float HomeScreen::rows_top() const
{
    return recent_.empty() ? kRecentY : kRecentY + kCardH + kRowGap;
}

float HomeScreen::shelf_h(int s) const
{
    return shelves_[s].portrait ? kPosterH : kCardH;
}

float HomeScreen::shelf_w(int s) const
{
    return shelves_[s].portrait ? kPosterW : kCardW;
}

float HomeScreen::shelf_y(int s) const
{
    float y = rows_top();
    for (int k = 0; k < s && k < EVO_RMLUI_SHELVES; ++k)
        if (!shelves_[k].tiles.empty())
            y += shelf_h(k) + kRowGap;
    return y;
}

float HomeScreen::library_y() const
{
    bool any = false;
    for (const Shelf &s : shelves_)
        any = any || !s.tiles.empty();
    /* Without rows the library sits where it always did. */
    return any ? shelf_y(EVO_RMLUI_SHELVES) + 10.0f : kLibraryY;
}

Rect HomeScreen::recent_rect(int index, bool settled) const
{
    const float off = settled ? recent_scroll_.position.target : recent_scroll_.offset();
    return {kLeft + static_cast<float>(index) * (kCardW + kCardGap) - off, kRecentY, kCardW, kCardH};
}

Rect HomeScreen::shelf_rect(int s, int index, bool settled) const
{
    const float w = shelf_w(s);
    const float gap = shelves_[s].portrait ? kPosterGap : kCardGap;
    const float off = settled ? shelf_scroll_[s].position.target : shelf_scroll_[s].offset();
    return {kLeft + static_cast<float>(index) * (w + gap) - off, shelf_y(s), w, shelf_h(s)};
}

Rect HomeScreen::library_rect(int index) const
{
    return {kLeft + static_cast<float>(index) * (kLibW + kLibGap), library_y(), kLibW, kLibH};
}

Rect HomeScreen::focus_rect() const
{
    if (hero_focused_)
        return kHeroAction;
    for (std::size_t i = 0; i < recent_.size(); ++i)
        if (recent_[i].focused)
        {
            /* Where the card lands once the shelf stops, not where it is: the
             * ring would otherwise trail the scroll. */
            const Rect r = recent_rect(static_cast<int>(i), true);
            return {r.x - 10, r.y - 10, r.w + 20, r.h + 20};
        }
    for (int s = 0; s < EVO_RMLUI_SHELVES; ++s)
        for (std::size_t i = 0; i < shelves_[s].tiles.size(); ++i)
            if (shelves_[s].tiles[i].focused)
            {
                const Rect r = shelf_rect(s, static_cast<int>(i), true);
                return {r.x - 10, r.y - 10, r.w + 20, r.h + 20};
            }
    for (std::size_t i = 0; i < library_.size(); ++i)
        if (library_[i].focused)
            return library_rect(static_cast<int>(i));
    return {};
}

/* How far down the focused row ends: the page follows it. */
float HomeScreen::focused_bottom() const
{
    for (int s = 0; s < EVO_RMLUI_SHELVES; ++s)
        for (const Tile &t : shelves_[s].tiles)
            if (t.focused)
                return shelf_y(s) + shelf_h(s);
    for (const Tile &t : library_)
        if (t.focused)
            return library_y() + kLibH;
    return 0.0f; /* the hero or Continue Watching: the top of the page */
}

void HomeScreen::update(float dt)
{
    age_ += dt;
    hero_fade_.update(dt);
    if (recent_cursor_ >= 0 && recent_cursor_ < static_cast<int>(recent_.size()))
    {
        const float start = static_cast<float>(recent_cursor_) * (kCardW + kCardGap);
        const float limit = static_cast<float>(recent_.size()) * (kCardW + kCardGap) - kCardGap;
        recent_scroll_.reveal(start, start + kCardW, kShelfView, kCardW * 0.6f, limit);
    }
    recent_scroll_.update(dt, 12.0f);
    for (int s = 0; s < EVO_RMLUI_SHELVES; ++s)
    {
        const Shelf &sh = shelves_[s];
        if (sh.cursor >= 0 && sh.cursor < static_cast<int>(sh.tiles.size()))
        {
            const float w = shelf_w(s);
            const float step = w + (sh.portrait ? kPosterGap : kCardGap);
            const float start = static_cast<float>(sh.cursor) * step;
            const float limit = static_cast<float>(sh.tiles.size()) * step - (step - w);
            shelf_scroll_[s].reveal(start, start + w, kShelfView, w * 0.6f, limit);
        }
        shelf_scroll_[s].update(dt, 12.0f);
    }
    page_.target = std::max(0.0f, focused_bottom() - kPageFloor);
    page_.update(dt, 11.0f);
    details_in_.target = details_.active ? 1.0f : 0.0f;
    details_in_.update(dt, 14.0f);

    const Rect target = focus_rect();
    if (!ring_snapped_ && target.w > 0.0f)
    {
        ring_.snap(target);
        recent_scroll_.position.snap(recent_scroll_.position.target);
        for (auto &sc : shelf_scroll_)
            sc.position.snap(sc.position.target);
        page_.snap(page_.target);
        ring_snapped_ = true;
    }
    if (target.w > 0.0f)
        ring_.target(target);
    ring_.update(dt, 20.0f);
    row_glow_.target = target.w > 0.0f ? 1.0f : 0.0f;
    row_glow_.update(dt, 10.0f);
}

std::uint32_t HomeScreen::art_texture(const Context &ctx, const std::uint32_t *art, int w, int h,
                                      const std::string &tag) const
{
    if (!art)
        return 0;
    /* The hero buffer is reused for every title, so its key carries the title. */
    std::uint64_t hash = 1469598103934665603ull;
    for (char c : tag)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return ctx.textures.pixels(art, w, h, hash);
}

void HomeScreen::draw_hero(DrawList &list, const Context &ctx, const Hero &hero, float alpha,
                           float slide) const
{
    if (alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);

    /* Artwork, floating, with a glow in the accent colour. */
    const float bob = std::sin(ctx.time * 0.8f) * 5.0f;
    const Rect art{kHeroArt.x + slide * 1.5f, kHeroArt.y + bob, kHeroArt.w, kHeroArt.h};
    list.glow(art.inset(24), 40, 80, p.accent.with_alpha(0.28f));
    list.shadow({art.x, art.y + 24, art.w, art.h}, 30, 44, Color::rgb(0x000000, 0.6f));
    const std::uint32_t tex = art_texture(ctx, hero.art, hero.art_w, hero.art_h, hero.title);
    if (tex)
    {
        list.image(tex, art, home_cover_uv(hero.art_w, hero.art_h, art.w, art.h), kWhite, 28);
    }
    else
    {
        list.gradient_rect(art, 28, p.accent_soft, p.bg_bottom);
        const std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
        if (logo)
            list.image(logo, {art.cx() - 56, art.cy() - 56, 112, 112}, ::hui::gfx::kFullUv,
                       kWhite.with_alpha(0.85f));
    }
    list.bordered_rect(art, 28, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.16f));

    const float x = kLeft + slide;
    ui::text(list, fonts.semibold, ui::upper(hero.eyebrow), x, 196, 20, p.accent, Align::left, 4.0f);
    ui::text(list, fonts.display, fit(fonts.display, hero.title, 76, 940), x - 3, 282, 76, kWhite);
    ui::text(list, fonts.regular, fit(fonts.regular, hero.detail, 24, 940), x, 330, 24,
             p.text_faint);
    /* A line of what it is about, when the portal said. */
    float bar_y = 382;
    if (!hero.overview.empty())
    {
        ui::text(list, fonts.regular, fit(fonts.regular, hero.overview, 21, 940), x, 368, 21,
                 p.text_muted);
        bar_y = 404;
    }

    if (hero.progress >= 0)
    {
        const float t = tween::clamp01(static_cast<float>(hero.progress) / 1000.0f);
        const Rect bar{x, bar_y, 420, 8};
        list.rounded_rect(bar, 4, kWhite.with_alpha(0.16f));
        list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * t), bar.h}, 4, p.accent);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%% watched", static_cast<int>(t * 100.0f + 0.5f));
        ui::text(list, fonts.regular, pct, bar.x + bar.w + 24, bar_y + 12, 22, p.text_muted);
    }

    if (!hero.action.empty())
    {
        const Rect play{x, kHeroAction.y, kHeroAction.w, kHeroAction.h};
        if (hero_focused_)
            list.glow(play, 32, 22, p.accent.with_alpha(0.45f));
        list.rounded_rect(play, 32, hero_focused_ ? kWhite : kWhite.with_alpha(0.14f));
        ui::draw_button(list, fonts, hero_focused_ ? ui::GlyphStyle::light() : ui::GlyphStyle::dark(),
                        ui::Button::cross, play.x + 20, play.cy(), 34);
        ui::text(list, fonts.semibold, ui::upper(hero.action), play.x + 68, play.cy() + 9, 24,
                 hero_focused_ ? kInk : kWhite, Align::left, 2.0f);
    }
    list.pop_opacity();
}

/* One poster or cover card, its name on a scrim at the foot. */
void HomeScreen::draw_card(DrawList &list, const Context &ctx, const Tile &t, Rect r,
                           bool portrait) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    if (t.focused)
        r = {r.x - 10, r.y - 10, r.w + 20, r.h + 20};
    const float radius = portrait ? 14.0f : 18.0f;
    list.shadow({r.x, r.y + 14, r.w, r.h}, 20, 24, Color::rgb(0x000000, 0.5f));
    const std::uint32_t tex = art_texture(ctx, t.art, t.art_w, t.art_h, std::string());
    if (tex)
    {
        list.image(tex, r, home_cover_uv(t.art_w, t.art_h, r.w, r.h),
                   kWhite.with_alpha(t.focused ? 1.0f : 0.82f), radius);
    }
    else
    {
        list.gradient_rect(r, radius, p.surface_sel, p.surface);
        const std::uint32_t icon = ctx.textures.icon(t.icon_path);
        if (icon)
            list.image(icon, {r.cx() - 28, r.cy() - 40, 56, 56}, ::hui::gfx::kFullUv,
                       kWhite.with_alpha(0.5f));
    }
    list.gradient_rect({r.x, r.y + r.h * 0.45f, r.w, r.h * 0.55f}, radius,
                       Color::rgb(0x000000, 0.0f), Color::rgb(0x000000, 0.78f));
    {
        /* "Scrubs (2026) S1E4": a long name is shortened, the episode code
         * never is - it is the part that says where you are. */
        const auto &font = t.focused ? fonts.semibold : fonts.regular;
        const float size = portrait ? 19.0f : 22.0f;
        const float pad = portrait ? 12.0f : 18.0f;
        std::string name = t.title, code;
        const std::size_t sp = t.title.rfind(' ');
        if (sp != std::string::npos && sp + 1 < t.title.size())
        {
            const std::string tail = t.title.substr(sp + 1);
            std::size_t k = 0;
            if (k < tail.size() && tail[k] == 'S')
            {
                ++k;
                while (k < tail.size() && std::isdigit(static_cast<unsigned char>(tail[k])))
                    ++k;
            }
            const bool hasE = k < tail.size() && tail[k] == 'E' && k + 1 < tail.size();
            bool digits = hasE;
            for (std::size_t j = k + 1; hasE && j < tail.size(); ++j)
                digits = digits && std::isdigit(static_cast<unsigned char>(tail[j]));
            if (digits)
            {
                name = t.title.substr(0, sp);
                code = tail;
            }
        }
        const float maxw = r.w - 2.0f * pad;
        const float base = r.y + r.h - (t.progress >= 0 ? 24.0f : 18.0f);
        if (code.empty())
        {
            ui::text(list, font, fit(font, t.title, size, maxw), r.x + pad, base, size, kWhite);
        }
        else
        {
            const float cw = font.measure(code, size);
            const std::string shown = fit(font, name, size, std::max(40.0f, maxw - cw - 10));
            ui::text(list, font, shown, r.x + pad, base, size, kWhite);
            ui::text(list, fonts.semibold, code, r.x + pad + font.measure(shown, size) + 10, base,
                     size, p.accent);
        }
        /* The year or "34 MIN LEFT" over the name, on a poster. */
        if (portrait && !t.detail.empty())
            ui::text(list, fonts.regular, fit(fonts.regular, t.detail, 16, maxw), r.x + pad,
                     base - 26, 16, kWhite.with_alpha(0.7f));
    }
    if (!t.badge.empty())
    {
        /* TMDB's score in a pill at the top right. */
        const float bw = fonts.semibold.measure(t.badge, 17) + 18.0f;
        const Rect pill{r.x + r.w - bw - 8.0f, r.y + 8.0f, bw, 28.0f};
        list.rounded_rect(pill, 14, Color::rgb(0x000000, 0.72f));
        ui::text(list, fonts.semibold, t.badge, pill.cx(), pill.y + 20.0f, 17, Color::rgb(0xf5c518),
                 Align::center);
    }
    if (t.progress >= 0)
    {
        const float f = tween::clamp01(static_cast<float>(t.progress) / 1000.0f);
        const float pad = portrait ? 12.0f : 18.0f;
        const Rect bar{r.x + pad, r.y + r.h - 12, r.w - 2.0f * pad, 4};
        list.rounded_rect(bar, 2, kWhite.with_alpha(0.22f));
        list.rounded_rect({bar.x, bar.y, std::max(4.0f, bar.w * f), bar.h}, 2, p.accent);
    }
}

/* A film's or series' page: poster, title, year and score, the story, and
 * what can be done with it (play, a version, favourite, watched). */
void HomeScreen::draw_details(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const Details &d = details_;
    const std::uint32_t tex = art_texture(ctx, d.art, d.art_w, d.art_h, "details:" + d.title);
    draw_backdrop(list, ctx, tex, 1.0f);
    const float in = tween::clamp01(details_in_.value);
    list.push_opacity(0.35f + 0.65f * in);
    const float slide = 40.0f * (1.0f - in);

    /* The poster. */
    const Rect poster{kLeft + slide, 150, 400, 600};
    list.glow(poster.inset(30), 40, 70, p.accent.with_alpha(0.25f));
    list.shadow({poster.x, poster.y + 24, poster.w, poster.h}, 30, 44, Color::rgb(0x000000, 0.6f));
    if (tex)
    {
        list.image(tex, poster, home_cover_uv(d.art_w, d.art_h, poster.w, poster.h), kWhite, 24);
    }
    else
    {
        list.gradient_rect(poster, 24, p.accent_soft, p.bg_bottom);
        const std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
        if (logo)
            list.image(logo, {poster.cx() - 56, poster.cy() - 56, 112, 112}, ::hui::gfx::kFullUv,
                       kWhite.with_alpha(0.8f));
    }
    list.bordered_rect(poster, 24, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.16f));

    /* The text column. */
    const float x = poster.x + poster.w + 64.0f;
    const float w = 1824.0f - x;
    ui::text(list, fonts.semibold, ui::upper(d.eyebrow), x, 196, 20, p.accent, Align::left, 4.0f);
    ui::text(list, fonts.display, fit(fonts.display, d.title, 64, w), x - 2, 270, 64, kWhite);
    ui::text(list, fonts.regular, fit(fonts.regular, d.meta, 26, w), x, 318, 26, p.text_muted);
    float y = 372;
    if (d.progress >= 0)
    {
        const float t = tween::clamp01(static_cast<float>(d.progress) / 1000.0f);
        const Rect bar{x, 344, 420, 8};
        list.rounded_rect(bar, 4, kWhite.with_alpha(0.16f));
        list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * t), bar.h}, 4, p.accent);
        y = 396;
    }
    if (!d.overview.empty())
        ui::paragraph(list, fonts.regular, d.overview, x, y, 24, std::min(w, 1100.0f), 34, kWhite.with_alpha(0.86f),
                      5);

    /* The buttons: five at a time, scrolled with the focus. */
    const int n = static_cast<int>(d.buttons.size());
    const int shown = std::min(n, 5);
    int first = 0;
    if (d.focus >= shown)
        first = d.focus - shown + 1;
    const float bw = std::min(760.0f, w);
    for (int i = 0; i < shown; ++i)
    {
        const int k = first + i;
        const bool f = k == d.focus;
        const Rect b{x, 600.0f + static_cast<float>(i) * 78.0f, bw, 64};
        if (f)
            list.glow(b, 32, 18, p.accent.with_alpha(0.4f));
        list.rounded_rect(b, 32, f ? kWhite : kWhite.with_alpha(0.10f));
        const auto &label = d.buttons[static_cast<size_t>(k)].first;
        const auto &detail = d.buttons[static_cast<size_t>(k)].second;
        const float lw = fonts.semibold.measure(label, 24);
        ui::text(list, fonts.semibold, fit(fonts.semibold, label, 24, bw - 56), b.x + 28, b.cy() + 9, 24,
                 f ? kInk : kWhite);
        if (!detail.empty() && lw + 80 < bw - 56)
            ui::text(list, fonts.regular, fit(fonts.regular, detail, 18, bw - lw - 96), b.x + b.w - 28,
                     b.cy() + 7, 18, f ? kInk.with_alpha(0.7f) : p.text_faint, Align::right);
    }
    if (n > shown)
    {
        char more[32];
        std::snprintf(more, sizeof more, "%d OF %d", d.focus + 1, n);
        ui::text(list, fonts.regular, more, x + bw, 590, 18, p.text_faint, Align::right, 2.0f);
    }
    list.pop_opacity();

    ui::Hint hints[2];
    hints[0] = {ui::Button::cross, "Select"};
    hints[1] = {ui::Button::circle, "Back"};
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = 1046;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, 2, 1824, true, layout);
}

void HomeScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    if (details_.active)
    {
        draw_details(list, ctx);
        return;
    }
    const std::uint32_t hero_tex = art_texture(ctx, hero_.art, hero_.art_w, hero_.art_h, hero_.title);
    draw_backdrop(list, ctx, hero_tex, 1.0f);

    /* Top right: clock and version. */
    {
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in * tween::clamp01(1.0f - page_.value / 120.0f));
        if (!clock_.empty())
            ui::text(list, fonts.semibold, clock_, 1824, 92, 30, kWhite, Align::right);
        if (!version_.empty())
            ui::text(list, fonts.regular, version_, 1824, 124, 18, p.text_faint, Align::right, 2.0f);
        list.pop_opacity();
    }

    /* Everything below scrolls up together once the focus goes past
     * Continue Watching. */
    const float page = page_.value;
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -page);

    /* Hero, cross-fading when the title changes, fading as the page scrolls. */
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in * tween::clamp01(1.0f - page / 520.0f));
        if (hero_fade_.running)
        {
            const float t = hero_fade_.progress();
            draw_hero(list, ctx, previous_hero_, 1.0f - tween::smoothstep(t * 2.2f),
                      -36.0f * tween::cubic_in(tween::clamp01(t * 2.2f)));
            const float arrive = tween::clamp01((t - 0.25f) / 0.75f);
            draw_hero(list, ctx, hero_, tween::smoothstep(arrive),
                      44.0f * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_hero(list, ctx, hero_, 1.0f, 30.0f * (1.0f - in));
        }
        list.pop_opacity();
    }

    /* The focus ring's light goes under the cards (a glow fills its inside). */
    if (row_glow_.value > 0.01f && !hero_focused_)
    {
        list.push_opacity(row_glow_.value);
        list.glow(ring_.value(), 24, 22, p.accent.with_alpha(0.35f + 0.12f * ui::breathe(ctx.time)));
        list.pop_opacity();
    }

    /* Rows are clipped at the rail: they scroll under it. The clip is tall, so
     * a scrolled page never cuts a row off. */
    const Rect rows_clip{NavRail::kWidth, -4000.0f, ::hui::gfx::kVirtualWidth - NavRail::kWidth, 9000.0f};

    /* Jump back in. */
    if (!recent_.empty())
    {
        list.push_clip(rows_clip);
        const float in = tween::stagger(age_, 3, 0.09f, 0.6f);
        list.push_opacity(in);
        const float dy = 40.0f * (1.0f - in);
        ui::text(list, fonts.semibold, "JUMP BACK IN", kLeft, kRecentY - 30 + dy, 20,
                 recent_cursor_ >= 0 ? kWhite : p.text_muted, Align::left, 3.0f);
        if (recent_total_ > static_cast<int>(recent_.size()) || recent_cursor_ >= 0)
        {
            char counter[32];
            if (recent_cursor_ >= 0)
                std::snprintf(counter, sizeof(counter), "%d OF %d", recent_cursor_ + 1, recent_total_);
            else
                std::snprintf(counter, sizeof(counter), "%d ITEMS", recent_total_);
            ui::text(list, fonts.regular, counter, 1824, kRecentY - 30 + dy, 18, p.text_faint,
                     Align::right, 2.0f);
        }
        for (std::size_t i = 0; i < recent_.size(); ++i)
        {
            Rect r = recent_rect(static_cast<int>(i));
            r.y += dy;
            draw_card(list, ctx, recent_[i], r, false);
        }
        list.pop_opacity();
        list.pop_clip();
    }

    /* Favorites, New films, New series. */
    for (int s = 0; s < EVO_RMLUI_SHELVES; ++s)
    {
        const Shelf &sh = shelves_[s];
        if (sh.tiles.empty())
            continue;
        list.push_clip(rows_clip);
        const float in = tween::stagger(age_, 4 + s, 0.09f, 0.6f);
        list.push_opacity(in);
        const float dy = 40.0f * (1.0f - in);
        const float y = shelf_y(s);
        ui::text(list, fonts.semibold, ui::upper(sh.title), kLeft, y - 30 + dy, 20,
                 sh.cursor >= 0 ? kWhite : p.text_muted, Align::left, 3.0f);
        if (sh.cursor >= 0)
        {
            char counter[32];
            std::snprintf(counter, sizeof(counter), "%d OF %d", sh.cursor + 1,
                          static_cast<int>(sh.tiles.size()));
            ui::text(list, fonts.regular, counter, 1824, y - 30 + dy, 18, p.text_faint, Align::right,
                     2.0f);
        }
        for (std::size_t i = 0; i < sh.tiles.size(); ++i)
        {
            Rect r = shelf_rect(s, static_cast<int>(i));
            if (r.x > ::hui::gfx::kVirtualWidth || r.x + r.w < NavRail::kWidth)
                continue; /* off screen */
            r.y += dy;
            draw_card(list, ctx, sh.tiles[i], r, sh.portrait);
        }
        list.pop_opacity();
        list.pop_clip();
    }

    /* Library (Home may leave it to the side menu: no tiles, no row). */
    if (!library_.empty())
    {
        const float in = tween::stagger(age_, 4 + EVO_RMLUI_SHELVES, 0.09f, 0.6f);
        list.push_opacity(in);
        const float dy = 40.0f * (1.0f - in);
        bool any_focus = false;
        for (const Tile &t : library_)
            any_focus = any_focus || t.focused;
        ui::text(list, fonts.semibold, "LIBRARY", kLeft, library_y() - 26 + dy, 20,
                 any_focus ? kWhite : p.text_muted, Align::left, 3.0f);
        for (std::size_t i = 0; i < library_.size(); ++i)
        {
            const Tile &t = library_[i];
            Rect r = library_rect(static_cast<int>(i));
            r.y += dy;
            list.rounded_rect(r, 22, t.focused ? kWhite.with_alpha(0.16f) : p.surface);
            list.bordered_rect(r, 22, Color::rgb(0x000000, 0.0f), 1.0f, kWhite.with_alpha(0.08f));
            const Rect well{r.x + 16, r.y + 16, 64, 64};
            list.rounded_rect(well, 18, t.focused ? p.accent : kWhite.with_alpha(0.08f));
            const std::uint32_t icon = ctx.textures.icon(t.icon_path);
            if (icon)
                list.image(icon, well.inset(16), ::hui::gfx::kFullUv, t.focused ? kInk : kWhite);
            ui::text(list, fonts.semibold, fit(fonts.semibold, t.title, 22, r.w - 112), r.x + 96,
                     r.y + 44, 22, kWhite);
            ui::text(list, fonts.regular, fit(fonts.regular, t.detail, 17, r.w - 112), r.x + 96,
                     r.y + 72, 17, p.text_faint);
        }
        list.pop_opacity();
    }

    /* The focus ring glides between hero button, covers and library tiles. */
    if (row_glow_.value > 0.01f && !hero_focused_)
    {
        const Rect ring = ring_.value();
        list.push_opacity(row_glow_.value);
        list.bordered_rect(ring.inset(-5), 26, Color::rgb(0x000000, 0.0f), 4, kWhite);
        list.pop_opacity();
    }
    list.pop_transform();

    /* Hints, on a band of their own once rows scroll behind them. */
    {
        if (page > 1.0f)
            list.gradient_rect({0, 990, ::hui::gfx::kVirtualWidth, 90}, 0, p.bg_bottom.with_alpha(0.0f),
                               p.bg_bottom.with_alpha(0.95f));
        list.push_opacity(tween::stagger(age_, 6, 0.08f, 0.5f));
        ui::Hint hints[4];
        int n = 0;
        hints[n++] = {ui::Button::cross, "Select"};
        bool on_poster = false, removable = false;
        for (const Shelf &sh : shelves_)
            if (sh.cursor >= 0)
            {
                on_poster = true;
                removable = sh.removable;
            }
        /* Continue Watching (hero or recent shelf): Square removes, Options opens the menu. */
        if (hero_focused_ ? (!hero_.title.empty() && recent_total_ > 0) : (recent_cursor_ >= 0))
        {
            hints[n++] = {ui::Button::square, "Remove"};
            hints[n++] = {ui::Button::options, "Options"};
        }
        else if (on_poster)
        {
            if (removable)
                hints[n++] = {ui::Button::square, "Remove"};
            hints[n++] = {ui::Button::options, "Options"};
        }
        hints[n++] = {ui::Button::circle, "Back"};
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 20;
        layout.cy = 1046;
        ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, 1824, true, layout);
        list.pop_opacity();
    }
}

/* -------------------------------------------------------------------- list */

namespace
{

constexpr float kListTop = 220.0f;
constexpr float kListRowH = 76.0f;
constexpr float kListGap = 8.0f;
constexpr float kListRight = 1824.0f;
constexpr float kMenuW = 640.0f;
constexpr float kMenuItemH = 64.0f;
const Color kDanger = Color::rgb(0xff5a5f);

/* The RmlUi documents name a hint by its glyph file. */
ui::Button button_for(const std::string &glyph)
{
    struct Map
    {
        const char *name;
        ui::Button button;
    };
    static const Map map[] = {{"btn_cross", ui::Button::cross},     {"btn_circle", ui::Button::circle},
                              {"btn_square", ui::Button::square},   {"btn_triangle", ui::Button::triangle},
                              {"btn_dpad", ui::Button::dpad},       {"btn_lstick", ui::Button::left_stick},
                              {"btn_options", ui::Button::options}, {"btn_touchpad", ui::Button::touchpad}};
    for (const Map &m : map)
        if (glyph.find(m.name) != std::string::npos)
            return m.button;
    return ui::Button::none;
}

/* "SELECT" -> "Select": the kit's hints are sentence case. */
std::string title_case(const std::string &s)
{
    std::string out = s;
    bool start = true;
    for (char &c : out)
    {
        if (c >= 'A' && c <= 'Z' && !start)
            c = static_cast<char>(c - 'A' + 'a');
        start = (c == ' ');
    }
    return out;
}

} // namespace

void ListScreen::set(const evo_rmlui_list_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    if (str(p.title) != title_)
    {
        age_ = 0.0f; /* a different list: run the entrance again */
        ring_snapped_ = false;
    }
    title_ = str(p.title);
    subtitle_ = str(p.subtitle);
    total_ = p.total_count;
    cursor_ = p.cursor_index;
    rows_.clear();
    for (int i = 0; i < p.row_count && i < EVO_RMLUI_LIST_ROWS; ++i)
    {
        Row r;
        r.title = str(p.rows[i].title);
        r.detail = str(p.rows[i].detail);
        r.icon = str(p.rows[i].icon_path);
        r.badge = str(p.rows[i].badge);
        r.progress = p.rows[i].progress;
        r.chevron = p.rows[i].has_chevron != 0;
        r.focused = p.rows[i].is_focused != 0;
        rows_.push_back(r);
    }
    empty_ = p.is_empty != 0;
    empty_title_ = str(p.empty_title);
    empty_hint_ = str(p.empty_hint);
    empty_icon_ = str(p.empty_icon);
    hints_.clear();
    for (int i = 0; i < p.hint_count && i < 4; ++i)
    {
        const ui::Button b = button_for(str(p.hints[i].glyph_path));
        if (b != ui::Button::none)
            hints_.emplace_back(static_cast<int>(b), title_case(str(p.hints[i].label)));
    }
    if (menu_.empty() && p.menu_count > 0)
        menu_ring_snapped_ = false;
    menu_.clear();
    for (int i = 0; i < p.menu_count && i < EVO_RMLUI_LIST_MENU_ROWS; ++i)
    {
        MenuItem m;
        m.label = str(p.menu[i].label);
        m.desc = str(p.menu[i].desc);
        m.icon = str(p.menu[i].icon_path);
        m.danger = p.menu[i].danger != 0;
        menu_.push_back(m);
    }
    menu_focus_ = p.menu_focus;
    menu_eyebrow_ = str(p.menu_eyebrow);
    menu_title_ = str(p.menu_title);
    menu_sub_ = str(p.menu_sub);
    menu_icon_ = str(p.menu_icon);
}

void ListScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
    menu_open_.snap(menu_.empty() ? 0.0f : 1.0f);
}

Rect ListScreen::row_rect(int index) const
{
    return {kLeft, kListTop + static_cast<float>(index) * (kListRowH + kListGap), kListRight - kLeft,
            kListRowH};
}

Rect ListScreen::menu_item_rect(int index) const
{
    /* Card centred on the screen; items under its 180 px header. */
    const float h = 180.0f + static_cast<float>(menu_.size()) * (kMenuItemH + 6.0f) + 24.0f;
    const float top = 540.0f - h * 0.5f;
    return {960.0f - kMenuW * 0.5f + 24.0f, top + 180.0f + static_cast<float>(index) * (kMenuItemH + 6.0f),
            kMenuW - 48.0f, kMenuItemH};
}

void ListScreen::update(float dt)
{
    age_ += dt;
    int focused = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].focused)
            focused = static_cast<int>(i);
    if (focused >= 0)
    {
        const Rect target = row_rect(focused);
        if (!ring_snapped_)
        {
            ring_.snap(target);
            ring_snapped_ = true;
        }
        ring_.target(target);
    }
    ring_.update(dt, 22.0f);
    has_focus_.target = focused >= 0 ? 1.0f : 0.0f;
    has_focus_.update(dt, 12.0f);

    menu_open_.target = menu_.empty() ? 0.0f : 1.0f;
    menu_open_.update(dt, 18.0f);
    if (!menu_.empty())
    {
        const Rect target = menu_item_rect(std::clamp(menu_focus_, 0, static_cast<int>(menu_.size()) - 1));
        if (!menu_ring_snapped_)
        {
            menu_ring_.snap(target);
            menu_ring_snapped_ = true;
        }
        menu_ring_.target(target);
        menu_ring_.update(dt, 24.0f);
    }
}

void ListScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    draw_backdrop(list, ctx);

    /* Header. */
    {
        const float in = tween::stagger(age_, 0, 0.05f, 0.45f);
        list.push_opacity(in);
        const float dy = 16.0f * (1.0f - in);
        ui::text(list, fonts.display, fit(fonts.display, title_, 52, 1100), kLeft - 2, 140 + dy, 52,
                 kWhite);
        if (!subtitle_.empty())
            ui::text(list, fonts.semibold, ui::upper(subtitle_), kLeft, 182 + dy, 18, p.text_muted,
                     Align::left, 3.0f);
        if (total_ > 0)
        {
            char counter[32];
            if (total_ > static_cast<int>(rows_.size()) && cursor_ >= 0)
                std::snprintf(counter, sizeof(counter), "%d OF %d", cursor_ + 1, total_);
            else
                std::snprintf(counter, sizeof(counter), "%d %s", total_, total_ == 1 ? "ITEM" : "ITEMS");
            ui::text(list, fonts.regular, counter, kListRight, 140 + dy, 20, p.text_faint, Align::right,
                     2.0f);
        }
        list.pop_opacity();
    }

    if (empty_)
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const float cx = (kLeft + kListRight) * 0.5f;
        const std::uint32_t icon = ctx.textures.icon(empty_icon_);
        if (icon)
            list.image(icon, {cx - 48, 380, 96, 96}, ::hui::gfx::kFullUv, kWhite.with_alpha(0.4f));
        ui::text(list, fonts.display, empty_title_, cx, 560, 40, kWhite, Align::center);
        ui::paragraph(list, fonts.regular, empty_hint_, cx, 610, 24, 900, 34, p.text_muted, 3,
                      Align::center);
        list.pop_opacity();
    }
    else
    {
        /* The highlight's light goes under the rows. */
        if (has_focus_.value > 0.01f)
        {
            list.push_opacity(has_focus_.value);
            list.glow(ring_.value(), 20, 18, p.accent.with_alpha(0.3f));
            list.pop_opacity();
        }
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            const Row &row = rows_[i];
            const float in = tween::stagger(age_, static_cast<int>(i) + 1, 0.03f, 0.4f);
            list.push_opacity(in);
            Rect r = row_rect(static_cast<int>(i));
            r.y += 24.0f * (1.0f - tween::cubic_out(in));
            list.rounded_rect(r, 20, row.focused ? kWhite.with_alpha(0.14f) : p.surface);
            const Rect well{r.x + 12, r.y + 10, 56, 56};
            list.rounded_rect(well, 16, row.focused ? p.accent : kWhite.with_alpha(0.08f));
            const std::uint32_t icon = ctx.textures.icon(row.icon);
            if (icon)
                list.image(icon, well.inset(14), ::hui::gfx::kFullUv, row.focused ? kInk : kWhite);

            float right = r.x + r.w - 24;
            if (row.chevron)
            {
                const std::uint32_t chev = ctx.textures.icon("../icons/icon_chevron.png");
                if (chev)
                    list.image(chev, {right - 24, r.cy() - 12, 24, 24}, ::hui::gfx::kFullUv,
                               kWhite.with_alpha(row.focused ? 0.9f : 0.45f));
                right -= 44;
            }
            if (!row.badge.empty())
            {
                const float w = fonts.semibold.measure(row.badge, 16, 1.5f) + 28;
                const Rect pill{right - w, r.cy() - 16, w, 32};
                if (row.focused)
                    list.bordered_rect(pill, 16, Color::rgb(0x000000, 0.0f), 1.5f, p.accent);
                else
                    list.rounded_rect(pill, 16, kWhite.with_alpha(0.08f));
                ui::text(list, fonts.semibold, row.badge, pill.cx(), pill.y + 22, 16,
                         row.focused ? p.accent : p.text_muted, Align::center, 1.5f);
                right = pill.x - 20;
            }
            const float text_w = right - (r.x + 88);
            ui::text(list, row.focused ? fonts.semibold : fonts.regular,
                     fit(fonts.semibold, row.title, 24, text_w), r.x + 88, r.y + 35, 24, kWhite);
            ui::text(list, fonts.regular, fit(fonts.regular, row.detail, 18, text_w), r.x + 88, r.y + 61,
                     18, p.text_faint);
            if (row.progress >= 0)
            {
                const float f = tween::clamp01(static_cast<float>(row.progress) / 1000.0f);
                const Rect bar{r.x + 88, r.y + r.h - 6, text_w, 3};
                list.rounded_rect(bar, 1.5f, kWhite.with_alpha(0.12f));
                list.rounded_rect({bar.x, bar.y, std::max(3.0f, bar.w * f), bar.h}, 1.5f, p.accent);
            }
            list.pop_opacity();
        }
        if (has_focus_.value > 0.01f && menu_open_.value < 0.99f)
        {
            list.push_opacity(has_focus_.value * (1.0f - menu_open_.value));
            list.bordered_rect(ring_.value().inset(-3), 23, Color::rgb(0x000000, 0.0f), 3, kWhite);
            list.pop_opacity();
        }
    }

    /* Hints. */
    if (!hints_.empty())
    {
        ui::Hint hints[4];
        int n = 0;
        for (const auto &h : hints_)
            hints[n++] = {static_cast<ui::Button>(h.first), h.second.c_str()};
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 20;
        layout.cy = 1046;
        list.push_opacity(tween::stagger(age_, 6, 0.06f, 0.4f));
        ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, kListRight, true, layout);
        list.pop_opacity();
    }

    /* OPTIONS menu. */
    const float m = menu_open_.value;
    if (m > 0.01f && !menu_.empty())
    {
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.45f * m));
        list.push_transform(0.96f + 0.04f * m, 960, 540, 0, 0);
        list.push_opacity(m);
        const Rect first = menu_item_rect(0);
        const float top = first.y - 180.0f;
        const float h = 180.0f + static_cast<float>(menu_.size()) * (kMenuItemH + 6.0f) + 24.0f;
        const Rect card{960.0f - kMenuW * 0.5f, top, kMenuW, h};
        list.shadow({card.x, card.y + 24, card.w, card.h}, 32, 48, Color::rgb(0x000000, 0.6f));
        list.rounded_rect(card, 32, p.bg_top.with_alpha(0.96f));
        list.bordered_rect(card, 32, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.14f));
        const Rect well{card.x + 32, card.y + 36, 64, 64};
        list.rounded_rect(well, 18, p.accent);
        const std::uint32_t icon = ctx.textures.icon(menu_icon_);
        if (icon)
            list.image(icon, well.inset(16), ::hui::gfx::kFullUv, kInk);
        const float tx = well.x + well.w + 20;
        ui::text(list, fonts.semibold, ui::upper(menu_eyebrow_), tx, card.y + 54, 16, p.accent, Align::left,
                 3.0f);
        ui::text(list, fonts.display, fit(fonts.display, menu_title_, 32, card.x + card.w - 32 - tx), tx,
                 card.y + 92, 32, kWhite);
        ui::text(list, fonts.regular, fit(fonts.regular, menu_sub_, 19, card.w - 64), card.x + 32,
                 card.y + 144, 19, p.text_muted);

        list.rounded_rect(menu_ring_.value(), 20, kWhite);
        for (std::size_t i = 0; i < menu_.size(); ++i)
        {
            const MenuItem &item = menu_[i];
            const Rect r = menu_item_rect(static_cast<int>(i));
            const bool focused = static_cast<int>(i) == menu_focus_;
            const Color ink = focused ? kInk : (item.danger ? kDanger : kWhite);
            const std::uint32_t ic = ctx.textures.icon(item.icon);
            if (ic)
                list.image(ic, {r.x + 18, r.cy() - 14, 28, 28}, ::hui::gfx::kFullUv, ink);
            ui::text(list, fonts.semibold, item.label, r.x + 62, r.y + 30, 22, ink);
            ui::text(list, fonts.regular, fit(fonts.regular, item.desc, 16, r.w - 80), r.x + 62, r.y + 52, 16,
                     focused ? kInk.with_alpha(0.7f) : p.text_faint);
        }
        list.pop_opacity();
        list.pop_transform();
    }
}

bool ListScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (!menu_.empty())
    {
        for (std::size_t i = 0; i < menu_.size(); ++i)
            out->focusable.emplace_back("menu-" + std::to_string(i), menu_[i].label);
        const int at = std::clamp(menu_focus_, 0, static_cast<int>(menu_.size()) - 1);
        out->id = "menu-" + std::to_string(at);
        out->text = menu_[static_cast<std::size_t>(at)].label;
        out->index = at + 1;
        out->total = static_cast<int>(menu_.size());
        out->rect = menu_item_rect(at);
        return true;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i)
        out->focusable.emplace_back("row-" + std::to_string(i), rows_[i].title);
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        if (!rows_[i].focused)
            continue;
        out->id = "row-" + std::to_string(i);
        out->text = rows_[i].title;
        out->index = cursor_ >= 0 ? cursor_ + 1 : static_cast<int>(i) + 1;
        out->total = total_ > 0 ? total_ : static_cast<int>(rows_.size());
        out->rect = row_rect(static_cast<int>(i));
        return true;
    }
    out->focusable.clear();
    return false;
}

/* ------------------------------------------------------------ focus report */

bool NavRail::focus(FocusInfo *out) const
{
    if (!params_.visible || !params_.rail_focused)
        return false;
    RailItem items[8];
    const int count = rail_items(items);
    const int at = std::clamp(params_.cursor_index, 0, count - 1);
    out->id = "nav-" + std::to_string(at);
    out->text = items[at].label;
    out->index = at + 1;
    out->total = count;
    out->rect = {28.0f, rail_item_y(at, count), kRailExpanded - 56.0f, 72.0f};
    out->focusable.clear();
    for (int i = 0; i < count; ++i)
        out->focusable.emplace_back("nav-" + std::to_string(i), items[i].label);
    return true;
}

bool HomeScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (details_.active)
    {
        for (std::size_t i = 0; i < details_.buttons.size(); ++i)
            out->focusable.emplace_back("detail-" + std::to_string(i), details_.buttons[i].first);
        if (details_.focus >= 0 && details_.focus < static_cast<int>(details_.buttons.size()))
        {
            out->id = "detail-" + std::to_string(details_.focus);
            out->text = details_.title + ": " + details_.buttons[static_cast<size_t>(details_.focus)].first;
            out->index = details_.focus + 1;
            out->total = static_cast<int>(details_.buttons.size());
            out->rect = {kLeft + 464.0f, 600.0f, 760.0f, 64.0f};
            return true;
        }
    }
    if (hero_focused_)
    {
        out->id = "hero-action";
        out->text = hero_.action + " " + hero_.title;
        out->index = out->total = 1;
        out->rect = kHeroAction;
        out->focusable.emplace_back(out->id, out->text);
        return true;
    }
    auto row = [&](const std::vector<Tile> &tiles, const char *prefix, bool recent) {
        for (std::size_t i = 0; i < tiles.size(); ++i)
            out->focusable.emplace_back(std::string(prefix) + std::to_string(i), tiles[i].title);
        for (std::size_t i = 0; i < tiles.size(); ++i)
        {
            if (!tiles[i].focused)
                continue;
            out->id = std::string(prefix) + std::to_string(i);
            out->text = tiles[i].title;
            out->index = static_cast<int>(i) + 1;
            out->total = recent ? std::max(recent_total_, static_cast<int>(tiles.size()))
                                : static_cast<int>(tiles.size());
            out->rect = recent ? recent_rect(static_cast<int>(i)) : library_rect(static_cast<int>(i));
            out->rect.y -= page_.value;
            return true;
        }
        out->focusable.clear();
        return false;
    };
    if (row(recent_, "recent-", true))
        return true;
    for (int s = 0; s < EVO_RMLUI_SHELVES; ++s)
    {
        const Shelf &sh = shelves_[s];
        out->focusable.clear();
        for (std::size_t i = 0; i < sh.tiles.size(); ++i)
            out->focusable.emplace_back("shelf" + std::to_string(s) + "-" + std::to_string(i),
                                        sh.tiles[i].title);
        for (std::size_t i = 0; i < sh.tiles.size(); ++i)
        {
            if (!sh.tiles[i].focused)
                continue;
            out->id = "shelf" + std::to_string(s) + "-" + std::to_string(i);
            out->text = sh.title + ": " + sh.tiles[i].title;
            out->index = static_cast<int>(i) + 1;
            out->total = static_cast<int>(sh.tiles.size());
            out->rect = shelf_rect(s, static_cast<int>(i));
            out->rect.y -= page_.value;
            return true;
        }
    }
    out->focusable.clear();
    return row(library_, "library-", false);
}

} // namespace evo::kit
