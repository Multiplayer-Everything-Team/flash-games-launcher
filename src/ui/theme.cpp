#include "theme.h"

#include <cmath>
#include <vector>

#include "core/log.h"
#include "core/strings.h"

namespace fl::ui {

using namespace Gdiplus;

Gdiplus::Color gc(const fl::Color& c, uint8_t alpha) {
    return Gdiplus::Color(static_cast<BYTE>(c.a * alpha / 255), c.r, c.g, c.b);
}

fl::Color mix(const fl::Color& a, const fl::Color& b, float t) {
    auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

float luminance(const fl::Color& c) { return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f; }

void roundRectPath(GraphicsPath& path, const RectF& r, float radius) {
    float d = std::min({radius * 2, r.Width, r.Height});
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void fillRound(Graphics& g, const RectF& r, float radius, const fl::Color& c) {
    GraphicsPath p;
    roundRectPath(p, r, radius);
    SolidBrush b(gc(c));
    g.FillPath(&b, &p);
}

void strokeRound(Graphics& g, const RectF& r, float radius, const fl::Color& c, float width) {
    GraphicsPath p;
    RectF in(r.X + width / 2, r.Y + width / 2, r.Width - width, r.Height - width);
    roundRectPath(p, in, radius);
    Pen pen(gc(c), width);
    g.DrawPath(&pen, &p);
}

void drawText(Graphics& g, const std::wstring& text, RECT r, HFONT font, const fl::Color& c, UINT flags) {
    HDC dc = g.GetHDC();
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c.ref());
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
    g.ReleaseHDC(dc);
}

int textHeight(HDC dc, HFONT font, const std::wstring& text, int width, UINT flags) {
    if (text.empty()) return 0;
    RECT r{0, 0, std::max(width, 1), 0};
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom - r.top;
}

int textWidth(HDC dc, HFONT font, const std::wstring& text) {
    RECT r{0, 0, 0, 0};
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.right - r.left;
}

std::wstring fitText(HDC dc, HFONT font, const std::wstring& text, int width, int maxHeight) {
    if (textHeight(dc, font, text, width) <= maxHeight) return text;
    if (textHeight(dc, font, L"Ag", width) > maxHeight) return {};
    // Binary search over word boundaries for the longest prefix that fits with the ellipsis.
    std::vector<size_t> cuts;
    for (size_t i = 1; i < text.size(); ++i)
        if (text[i] == L' ' && text[i - 1] != L' ') cuts.push_back(i);
    size_t lo = 0, hi = cuts.size();
    std::wstring best;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        std::wstring candidate = text.substr(0, cuts[mid]);
        while (!candidate.empty() && (candidate.back() == L',' || candidate.back() == L'.' || candidate.back() == L':'))
            candidate.pop_back();
        candidate += L"\u2026";
        if (textHeight(dc, font, candidate, width) <= maxHeight) {
            best = candidate;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best;
}

std::unique_ptr<Bitmap> loadImageFile(const std::wstring& path) {
    if (path.empty()) return nullptr;
    std::unique_ptr<Bitmap> decoded(Bitmap::FromFile(path.c_str()));
    if (!decoded || decoded->GetLastStatus() != Ok || decoded->GetWidth() == 0) {
        logger().warn(L"Could not load image " + path + L" (use PNG, JPG or BMP)");
        return nullptr;
    }
    // Copy into a premultiplied bitmap: faster to draw, and the file is no longer locked.
    auto copy = std::make_unique<Bitmap>(decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppPARGB);
    Graphics g(copy.get());
    g.DrawImage(decoded.get(), 0, 0, decoded->GetWidth(), decoded->GetHeight());
    return copy;
}

static std::wstring initialsOf(const std::wstring& name) {
    std::wstring out;
    for (const auto& word : split(name, L' ')) {
        if (word.empty()) continue;
        if (iswdigit(word[0])) out += word;  // "Gun Mayhem 2" -> "GM2"
        else if (iswalnum(word[0])) out += static_cast<wchar_t>(towupper(word[0]));
        if (out.size() >= 3) break;
    }
    return out.empty() ? L"?" : out.substr(0, 3);
}

std::unique_ptr<Bitmap> coverArt(const std::wstring& name, const fl::Color& from, const fl::Color& to, int w, int h) {
    auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    Graphics g(bmp.get());
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    REAL W = static_cast<REAL>(w), H = static_cast<REAL>(h);
    LinearGradientBrush br(PointF(0, 0), PointF(W, H), gc(from), gc(to));
    g.FillRectangle(&br, 0.0f, 0.0f, W, H);
    // Soft circles give each cover some depth without any copyrighted art.
    unsigned seed = 0;
    for (wchar_t ch : name) seed = seed * 31 + ch;
    for (int i = 0; i < 4; ++i) {
        seed = seed * 1103515245u + 12345u;
        REAL size = H * (0.6f + (seed % 100) / 100.0f);
        REAL x = W * ((seed >> 8) % 100) / 100.0f - size / 2;
        REAL y = H * ((seed >> 16) % 100) / 100.0f - size / 2;
        SolidBrush b(Gdiplus::Color(i % 2 ? 22 : 30, 255, 255, 255));
        g.FillEllipse(&b, x, y, size, size);
    }
    LinearGradientBrush shade(PointF(0, H * 0.45f), PointF(0, H), Gdiplus::Color(0, 0, 0, 0), Gdiplus::Color(90, 0, 0, 0));
    g.FillRectangle(&shade, 0.0f, H * 0.45f, W, H * 0.55f + 1);
    std::wstring text = initialsOf(name);
    FontFamily fam(L"Segoe UI Black");
    FontFamily fallback(L"Segoe UI");
    Font font(fam.IsAvailable() ? &fam : &fallback, H * (text.size() > 2 ? 0.34f : 0.42f), FontStyleBold, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    RectF r(0, 0, W, H);
    RectF shadow(0, H * 0.025f, W, H);
    SolidBrush dim(Gdiplus::Color(70, 0, 0, 0));
    g.DrawString(text.c_str(), -1, &font, shadow, &fmt, &dim);
    SolidBrush white(Gdiplus::Color(245, 255, 255, 255));
    g.DrawString(text.c_str(), -1, &font, r, &fmt, &white);
    return bmp;
}

void drawCover(Graphics& g, Bitmap& img, const RectF& box, float radius, bool roundBottom) {
    float w = static_cast<float>(img.GetWidth()), h = static_cast<float>(img.GetHeight());
    float scale = std::max(box.Width / w, box.Height / h);
    RectF src((w - box.Width / scale) / 2, (h - box.Height / scale) / 2, box.Width / scale, box.Height / scale);
    GraphicsPath p;
    if (roundBottom) {
        roundRectPath(p, box, radius);
    } else {
        // Rounded top only: the card body continues below the art.
        float d = std::min({radius * 2, box.Width, box.Height});
        p.AddArc(box.X, box.Y, d, d, 180, 90);
        p.AddArc(box.X + box.Width - d, box.Y, d, d, 270, 90);
        p.AddLine(box.X + box.Width, box.Y + box.Height, box.X, box.Y + box.Height);
        p.CloseFigure();
    }
    Region old;
    g.GetClip(&old);
    g.SetClip(&p, CombineModeIntersect);
    g.DrawImage(&img, box, src.X, src.Y, src.Width, src.Height, UnitPixel);
    g.SetClip(&old);
}

// ---- Theme -----------------------------------------------------------------

static bool fontInstalled(const std::wstring& family) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, family.c_str(), _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

Theme::~Theme() {
    release();
    if (iconBig) DestroyIcon(iconBig);
    if (iconSmall) DestroyIcon(iconSmall);
}

void Theme::init(const Config& cfg) {
    cfg_ = &cfg;
    configured_ = cfg.branding.colors;
    family_ = cfg.branding.font;
    if (!fontInstalled(family_)) {
        logger().warn(L"Font '" + family_ + L"' is not available; using Segoe UI.");
        family_ = L"Segoe UI";
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    iconBig = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                            GetSystemMetrics(SM_CYICON), 0));
    iconSmall = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), 0));
}

void Theme::release() {
    for (HFONT* f : {&body, &strong, &caption, &tiny, &title, &brand, &heading}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    for (HBRUSH* b : {&brBackground, &brSurface, &brSidebar}) {
        if (*b) DeleteObject(*b);
        *b = nullptr;
    }
}

void Theme::build(UINT newDpi) {
    dpi = newDpi ? newDpi : 96;
    release();
    HIGHCONTRASTW hc{sizeof hc};
    highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    c = configured_;
    if (highContrast) {
        auto sys = [](int idx) {
            COLORREF v = GetSysColor(idx);
            return fl::Color{GetRValue(v), GetGValue(v), GetBValue(v), 255};
        };
        c.background = c.surface = c.surfaceHover = c.sidebar = sys(COLOR_WINDOW);
        c.text = c.sidebarText = c.border = c.success = c.warning = c.error = c.online = sys(COLOR_WINDOWTEXT);
        c.textMuted = c.sidebarTextMuted = sys(COLOR_WINDOWTEXT);
        c.primary = c.primaryHover = c.focus = sys(COLOR_HIGHLIGHT);
        c.onPrimary = sys(COLOR_HIGHLIGHTTEXT);
    }
    dark = luminance(c.background) < 0.5f;
    float pt = cfg_ ? cfg_->branding.fontSize : 10.0f;
    auto make = [&](float size, int weight) {
        LOGFONTW lf{};
        lf.lfHeight = -static_cast<LONG>(size * dpi / 72.0f + 0.5f);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcsncpy_s(lf.lfFaceName, family_.c_str(), _TRUNCATE);
        return CreateFontIndirectW(&lf);
    };
    body = make(pt, FW_NORMAL);
    strong = make(pt, FW_SEMIBOLD);
    caption = make(pt - 1.0f, FW_NORMAL);
    tiny = make(pt - 2.0f, FW_BOLD);
    heading = make(pt + 1.5f, FW_SEMIBOLD);
    title = make(pt * 1.9f, FW_BOLD);
    brand = make(pt * 1.3f, FW_BOLD);
    brBackground = CreateSolidBrush(c.background.ref());
    brSurface = CreateSolidBrush(c.surface.ref());
    brSidebar = CreateSolidBrush(c.sidebar.ref());
}

}  // namespace fl::ui
