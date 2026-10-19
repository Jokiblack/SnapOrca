#include "MixedFilamentDialog.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/event.h>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <vector>
#include <string>
#include "slic3r/GUI/GUI_Utils.hpp"
#include <wx/dc.h>
#include <wx/dcmemory.h>
#include <utility>
#include <wx/chartype.h>
#include <tuple>
#include <wx/arrstr.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/scrolwin.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>
#include <wx/tglbtn.h>
#include <wx/wrapsizer.h>
#include <wx/tokenzr.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GradientCurveEditor.hpp"
#include "FilamentBitmapUtils.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DropDown.hpp"
#include "Widgets/Label.hpp"
// Snapmaker: Full Spectrum mixed filament dialog
#include "MixedColorMatchHelpers.hpp"
#include "MixedGradientSelector.hpp"
#include "MixedColorMatchPanel.hpp"
#include "MixedFilamentBadge.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "PresetBundle.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/StaticBox.hpp"
#include <wx/statline.h>
#include <wx/bmpbuttn.h>
#include <wx/clrpicker.h>
#include <wx/colordlg.h>
#include <numeric>
#include <sstream>

namespace Slic3r {
namespace GUI {

static constexpr int MAX_COMPONENTS = 3;
static constexpr int MIN_COMPONENT_RATIO = 10;

// Section headings and the placeholder text share one muted tone; light key, resolved at each use.
static const wxColour COLOR_LABEL_MUTED("#6B6A6A");

// Lightweight self-painting label used for both dual-color and triple-color
// ratio percentage display.  Hover shows a rounded-rect background; click
// fires wxEVT_LEFT_DOWN which the owning dialog binds to start_ratio_editor.
class RatioLabelPanel : public wxPanel
{
public:
    RatioLabelPanel(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(_L("Click to edit ratio"));
        SetFont(::Label::Body_10);

        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { m_hovered = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { m_hovered = false; Refresh(); e.Skip(); });
        Bind(wxEVT_PAINT, &RatioLabelPanel::on_paint, this);
    }

    void SetLabel(const wxString& text) override
    {
        if (m_text == text) return;
        m_text = text;
        update_best_size();
        Refresh();
    }
    wxString GetLabel() const override { return m_text; }

private:
    void update_best_size()
    {
        wxClientDC dc(this);
        dc.SetFont(GetFont());
        wxSize ts = dc.GetTextExtent(m_text);
        int pad_x = FromDIP(4), pad_y = FromDIP(3);
        SetMinSize(wxSize(ts.GetWidth() + pad_x * 2, ts.GetHeight() + pad_y * 2));
        InvalidateBestSize();
    }

    void on_paint(wxPaintEvent&)
    {
        wxBufferedPaintDC dc(this);
        wxSize sz = GetClientSize();

        wxColour parent_bg = GetParent() ? GetParent()->GetBackgroundColour()
                                         : StateColor::darkModeColorFor(*wxWHITE);
        dc.SetBrush(wxBrush(parent_bg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        if (m_hovered) {
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#F8F8F8"))));
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#CECECE")), 1));
            dc.DrawRoundedRectangle(0, 0, sz.GetWidth(), sz.GetHeight(), FromDIP(3));
        }

        dc.SetFont(GetFont());
        dc.SetTextForeground(m_hovered ? StateColor::darkModeColorFor(wxColour("#009688"))
                                       : StateColor::darkModeColorFor(wxColour("#262E30")));
        wxSize ts = dc.GetTextExtent(m_text);
        int x = (sz.GetWidth()  - ts.GetWidth())  / 2;
        int y = (sz.GetHeight() - ts.GetHeight()) / 2;
        dc.DrawText(m_text, x, y);
    }

    wxString m_text;
    bool     m_hovered{false};
};

static wxColour blend_colors(const wxColour& a, const wxColour& b, double ratio_a)
{
    unsigned char r, g, bl;
    Slic3r::filament_mixer_lerp(a.Red(), a.Green(), a.Blue(),
                                b.Red(), b.Green(), b.Blue(),
                                static_cast<float>(1.0 - ratio_a),
                                &r, &g, &bl);
    return wxColour(r, g, bl);
}


// ---- Constructors ----

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                         const std::vector<std::string>& physical_colors,
                                         const std::vector<std::string>& physical_names,
                                         const std::vector<std::string>& physical_types)
    : DPIDialog(parent, wxID_ANY, _L("Add Mixed Filament"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_physical_colors(physical_colors)
    , m_physical_names(physical_names)
    , m_physical_types(physical_types)
{
    m_result.components = {1, (physical_colors.size() >= 2) ? 2u : 1u};
    m_result.ratios     = {50, 50};
    build_ui();
    wxGetApp().UpdateDlgDarkUI(this);
}

MixedFilamentDialog::~MixedFilamentDialog()
{
    // Backstop: a child must never be destroyed while it still holds the mouse
    // capture. wxWidgets only asserts about this (compiled out in release), and
    // the macOS port never unwinds its capture stack, so the stale entry would
    // make wxNSWindow::sendEvent swallow every mouse event in the application.
    if (m_ratio_bar && m_ratio_bar->HasCapture())
        m_ratio_bar->ReleaseMouse();
    if (m_triangle_panel && m_triangle_panel->HasCapture())
        m_triangle_panel->ReleaseMouse();
}

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                         const MixedFilamentResult& existing,
                                         const std::vector<std::string>& physical_colors,
                                         const std::vector<std::string>& physical_names,
                                         const std::vector<std::string>& physical_types)
    : DPIDialog(parent, wxID_ANY, _L("Edit Mixed Filament"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_result(existing)
    , m_physical_colors(physical_colors)
    , m_physical_names(physical_names)
    , m_physical_types(physical_types)
{
    if (m_result.components.size() < 2) {
        m_result.components = {1, (physical_colors.size() >= 2) ? 2u : 1u};
        m_result.ratios     = {50, 50};
    }
    if (m_result.ratios.size() >= 3) {
        int sum = 0;
        for (int r : m_result.ratios) sum += r;
        if (sum > 0) {
            m_tri_wx = (double)m_result.ratios[0] / sum;
            m_tri_wy = (double)m_result.ratios[1] / sum;
            m_tri_wz = (double)m_result.ratios[2] / sum;
        }
    }
    build_ui();
    wxGetApp().UpdateDlgDarkUI(this);
}

void MixedFilamentDialog::on_dpi_changed(const wxRect&)
{
    int h = (num_components() >= 3) ? FromDIP(680) : FromDIP(580);
    SetSize(FromDIP(439), h);
    Refresh();
}

wxColour MixedFilamentDialog::comp_colour(size_t i) const
{
    unsigned int c = comp(i);
    if (c >= 1 && c <= m_physical_colors.size())
        return wxColour(m_physical_colors[c - 1]);
    return wxColour("#D9D9D9");
}

static wxBitmap make_alpha_bitmap(int w, int h,
                                  const std::function<void(wxDC& dc)>& draw_fn)
{
    wxBitmap bmp(w, h);
    wxMemoryDC memdc;
#ifdef __WXOSX__
    bmp.UseAlpha();
    memdc.SelectObject(bmp);
#else
    {
        wxImage img(w, h);
        img.InitAlpha();
        memset(img.GetAlpha(), 0, w * h);
        bmp = wxBitmap(std::move(img));
    }
    memdc.SelectObject(bmp);
#endif
    {
#ifdef __WXMSW__
        wxGCDC dc(memdc);
#else
        wxDC& dc = memdc;
#endif
        draw_fn(dc);
    }
    memdc.SelectObject(wxNullBitmap);
    return bmp;
}

wxBitmap MixedFilamentDialog::make_swatch_bitmap(size_t idx)
{
    int swatch_sz = FromDIP(20);
    int pad_left  = FromDIP(2);
    int pad_right = FromDIP(6);
    int bmp_w = pad_left + swatch_sz + pad_right;
    int bmp_h = swatch_sz;

    // Reuse the sidebar clr_picker swatch (get_extruder_color_icon) so the
    // checkerboard (transparent.svg tiling), border and label style match the
    // sidebar exactly, instead of a self-drawn rounded rect / programmatic grid.
    std::string color_hex = "#D9D9D9";
    if (idx < m_physical_colors.size())
        color_hex = m_physical_colors[idx];
    std::string label = std::to_string(idx + 1);

    wxBitmap* icon = get_extruder_color_icon(color_hex, label, swatch_sz, swatch_sz);

    return make_alpha_bitmap(bmp_w, bmp_h, [&](wxDC& dc) {
        if (icon && icon->IsOk())
            dc.DrawBitmap(*icon, pad_left, 0);
    });
}

void MixedFilamentDialog::apply_uniform_label_width(wxStaticText* lbl)
{
    // A material row places the combo right after the label, so the combo x follows the label
    // width and the rows drift apart with fonts that render digits at different advances (which
    // is what macOS does).  Reserve the width of the widest row label on every row instead.
    // The label itself is used as the measuring device on purpose: SetMinSize overrides the
    // control's own best size rather than being merged with it, and on macOS the native cell is
    // wider than the plain text extent, so a wxDC-measured width would clip the text.
    const wxString text = lbl->GetLabel();
    int w = 0;
    for (int i = 1; i <= MAX_COMPONENTS; ++i) {
        lbl->SetLabel(wxString::Format(_L("Filament %d"), i));
        lbl->InvalidateBestSize();
        w = std::max(w, lbl->GetBestSize().x);
    }
    lbl->SetLabel(text);
    lbl->InvalidateBestSize();
    lbl->SetMinSize(wxSize(w, -1));
}

void MixedFilamentDialog::append_material_row()
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* lbl = new wxStaticText(this, wxID_ANY,
                                 wxString::Format(_L("Filament %d"), (int)(m_combo_filaments.size() + 1)));
    lbl->SetFont(::Label::Body_12);
    apply_uniform_label_width(lbl);
    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    auto* combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(FromDIP(166), FromDIP(24)), 0, nullptr, wxCB_READONLY);
    combo->SetKeepDropArrow(true);
    combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_filament_changed(); });
    row->Add(combo, 1, wxALIGN_CENTER_VERTICAL);

    m_combo_filaments.push_back(combo);
    m_combo_to_physical.push_back({});
    m_material_rows_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(9));
}

void MixedFilamentDialog::reset_manual_ratio_state()
{
    m_ratio_manual_order.clear();
    if (m_ratio_editor_panel)
        m_ratio_editor_panel->Hide();
    // Restore any label hidden by an in-flight editor so it can never be left
    // permanently invisible if the editor is dismissed without a commit.
    if (m_ratio_editor_anchor) {
        m_ratio_editor_anchor->Show();
        m_ratio_editor_anchor = nullptr;
    }
}

void MixedFilamentDialog::refresh_ratio_labels()
{
    if (m_label_ratio_a)
        m_label_ratio_a->SetLabel(wxString::Format(wxT("%d%%"), ratio(0)));
    if (m_label_ratio_b)
        m_label_ratio_b->SetLabel(wxString::Format(wxT("%d%%"), ratio(1)));
    if (m_ratio_sizer)
        m_ratio_sizer->Layout();
    if (m_triangle_panel)
        m_triangle_panel->Refresh();
}

void MixedFilamentDialog::sync_triangle_weights_from_ratios()
{
    if (m_result.ratios.size() < 3)
        return;

    int sum = 0;
    for (int r : m_result.ratios)
        sum += r;
    if (sum <= 0)
        return;

    m_tri_wx = (double)m_result.ratios[0] / sum;
    m_tri_wy = (double)m_result.ratios[1] / sum;
    m_tri_wz = (double)m_result.ratios[2] / sum;
}

void MixedFilamentDialog::apply_manual_ratio(size_t idx, int value)
{
    const size_t n = num_components();
    if (idx >= n)
        return;
    if (m_result.ratios.size() != n)
        m_result.ratios.assign(n, n > 0 ? 100 / (int)n : 0);
    bool manual_stale = false;
    for (size_t o : m_ratio_manual_order) {
        if (o >= n) { manual_stale = true; break; }
    }
    if (manual_stale)
        reset_manual_ratio_state();

    int max_value = (int)(100 - (n - 1) * MIN_COMPONENT_RATIO);
    value = std::clamp(value, MIN_COMPONENT_RATIO, std::max(MIN_COMPONENT_RATIO, max_value));

    if (n == 2) {
        if (idx == 0) {
            m_result.ratios[0] = value;
            m_result.ratios[1] = 100 - value;
        } else {
            m_result.ratios[1] = value;
            m_result.ratios[0] = 100 - value;
        }
        m_result.ratios[0] = std::clamp(m_result.ratios[0], MIN_COMPONENT_RATIO, 100 - MIN_COMPONENT_RATIO);
        m_result.ratios[1] = 100 - m_result.ratios[0];
    } else if (n >= 3) {
        m_result.ratios[idx] = value;
        int remaining = 100 - value;

        std::vector<size_t> others;
        int others_sum = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i == idx) continue;
            others.push_back(i);
            others_sum += m_result.ratios[i];
        }

        if (!others.empty()) {
            if (others_sum > 0) {
                int assigned = 0;
                for (size_t k = 0; k < others.size(); ++k) {
                    int nv = (int)((double)remaining * m_result.ratios[others[k]] / others_sum + 0.5);
                    nv = std::max(nv, MIN_COMPONENT_RATIO);
                    m_result.ratios[others[k]] = nv;
                    assigned += nv;
                }
                while (assigned != remaining) {
                    if (assigned > remaining) {
                        int pick = -1;
                        for (size_t k = 0; k < others.size(); ++k)
                            if (m_result.ratios[others[k]] > MIN_COMPONENT_RATIO
                                && (pick < 0 || m_result.ratios[others[k]] > m_result.ratios[others[pick]]))
                                pick = (int)k;
                        if (pick < 0) break;
                        --m_result.ratios[others[pick]]; --assigned;
                    } else {
                        int pick = 0;
                        for (size_t k = 1; k < others.size(); ++k)
                            if (m_result.ratios[others[k]] > m_result.ratios[others[pick]])
                                pick = (int)k;
                        ++m_result.ratios[others[pick]]; ++assigned;
                    }
                }
            } else {
                int base = remaining / (int)others.size();
                for (size_t k = 0; k < others.size(); ++k)
                    m_result.ratios[others[k]] = base;
                m_result.ratios[others.back()] += remaining - base * (int)others.size();
            }
        }
    }

    refresh_ratio_labels();
    sync_triangle_weights_from_ratios();
    update_preview();
}

void MixedFilamentDialog::apply_dragged_triangle_ratio(int r0, int r1, int r2)
{
    if (m_result.ratios.size() < 3)
        return;

    int ratios[3] = {
        std::clamp(r0, MIN_COMPONENT_RATIO, 100),
        std::clamp(r1, MIN_COMPONENT_RATIO, 100),
        std::clamp(r2, MIN_COMPONENT_RATIO, 100)
    };

    int sum = ratios[0] + ratios[1] + ratios[2];
    while (sum > 100) {
        int idx = 0;
        for (int i = 1; i < 3; ++i) {
            if (ratios[i] > ratios[idx])
                idx = i;
        }
        if (ratios[idx] <= MIN_COMPONENT_RATIO)
            break;
        --ratios[idx];
        --sum;
    }
    while (sum < 100) {
        int idx = 0;
        for (int i = 1; i < 3; ++i) {
            if (ratios[i] < ratios[idx])
                idx = i;
        }
        ++ratios[idx];
        ++sum;
    }

    m_result.ratios[0] = ratios[0];
    m_result.ratios[1] = ratios[1];
    m_result.ratios[2] = ratios[2];
    sync_triangle_weights_from_ratios();
    reset_manual_ratio_state();
    update_preview();
}

void MixedFilamentDialog::start_ratio_editor(size_t idx, wxWindow* anchor, const wxRect& anchor_rect)
{
    if (!anchor || idx >= m_result.ratios.size())
        return;
    if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown())
        commit_ratio_editor(true);

    if (!m_ratio_editor_panel) {
        wxColour bg = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
        wxColour fg = StateColor::darkModeColorFor(wxColour("#262E30"));

        m_ratio_editor_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_SIMPLE);
        m_ratio_editor_panel->SetBackgroundColour(bg);

        auto* hsizer = new wxBoxSizer(wxHORIZONTAL);

        m_ratio_editor = new wxTextCtrl(m_ratio_editor_panel, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxTE_PROCESS_ENTER | wxTE_RIGHT | wxBORDER_NONE);
        m_ratio_editor->SetFont(::Label::Body_10);
        m_ratio_editor->SetMaxLength(3);
        m_ratio_editor->SetBackgroundColour(bg);
        m_ratio_editor->SetForegroundColour(fg);
        // Default wxTextCtrl best width (~140px) is too wide for the sizer to
        // shrink, which would push the "%" suffix out of the panel.  Size the
        // editor for the *widest* three digits rather than the largest accepted
        // value: SetMaxLength above lets anything up to "888" be typed, and the
        // macOS system font renders digits at different advances, so "100" is
        // narrower than what the user can actually enter.  GetSizeFromTextSize()
        // then adds the platform's own text field margins; on macOS those margins
        // are what clipped the digits.
        {
            wxClientDC mdc(m_ratio_editor);
            mdc.SetFont(::Label::Body_10);
            int digits_w = mdc.GetTextExtent(wxT("888")).GetWidth();
            m_ratio_editor->SetMinSize(m_ratio_editor->GetSizeFromTextSize(digits_w));
        }

        auto* pct_label = new wxStaticText(m_ratio_editor_panel, wxID_ANY, wxT("%"));
        pct_label->SetFont(::Label::Body_10);
        pct_label->SetForegroundColour(fg);
        pct_label->SetBackgroundColour(bg);
        pct_label->SetMinSize(pct_label->GetBestSize());

        hsizer->Add(m_ratio_editor, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
        hsizer->Add(pct_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
        m_ratio_editor_panel->SetSizer(hsizer);
        m_ratio_editor_panel->Hide();

        m_ratio_editor->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { commit_ratio_editor(true); });
        m_ratio_editor->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
            commit_ratio_editor(true);
            e.Skip();
        });
        m_ratio_editor->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_ESCAPE)
                commit_ratio_editor(false);
            else
                e.Skip();
        });
    }

    m_ratio_editor_idx = idx;

    // Keep the editor in the same window hierarchy as the clicked label so the
    // z-order is reliable and the editor fully covers the anchor (dual-color
    // labels live on the dialog, triple-color labels live on the triangle
    // panel).
    wxWindow* target_parent = anchor->GetParent();
    if (target_parent && m_ratio_editor_panel->GetParent() != target_parent)
        m_ratio_editor_panel->Reparent(target_parent);

    // Hide the label being edited to avoid its (hover-state) text leaking out
    // next to the editor; restored on commit.
    m_ratio_editor_anchor = anchor;
    anchor->Hide();

    wxPoint pos = anchor->GetPosition() + anchor_rect.GetTopLeft();
    // Match the editor to the label (hover box) size so the inline editor and
    // the hover state look identical, but never go below what the digits and
    // the "%" suffix need: the sizer takes any missing width out of the
    // stretchable editor, which would clip the value.
    wxSize needed = m_ratio_editor_panel->ClientToWindowSize(
        m_ratio_editor_panel->GetSizer()->CalcMin());
    wxSize size = anchor->GetSize();
    size.SetWidth(std::max(size.GetWidth(), needed.GetWidth()));
    size.SetHeight(std::max(size.GetHeight(), needed.GetHeight()));
    // An editor wider than the label must still stay inside its parent, or the
    // corner labels of the triangle picker would have it clipped at the edge.
    if (wxWindow* editor_parent = m_ratio_editor_panel->GetParent()) {
        wxSize avail = editor_parent->GetClientSize();
        pos.x = std::clamp(pos.x, 0, std::max(0, avail.GetWidth()  - size.GetWidth()));
        pos.y = std::clamp(pos.y, 0, std::max(0, avail.GetHeight() - size.GetHeight()));
    }
    m_ratio_editor_panel->SetSize(wxRect(pos, size));
    m_ratio_editor_panel->Layout();
    m_ratio_editor->SetValue(wxString::Format(wxT("%d"), ratio(idx)));
    m_ratio_editor_panel->Show();
    m_ratio_editor_panel->Raise();
    m_ratio_editor->SetFocus();
    m_ratio_editor->SelectAll();
    m_ratio_editor_panel->Refresh();
    Update();
}

void MixedFilamentDialog::commit_ratio_editor(bool apply)
{
    if (!m_ratio_editor_panel || !m_ratio_editor_panel->IsShown() || m_ratio_editor_committing)
        return;

    m_ratio_editor_committing = true;

    // Restore the hidden anchor before applying the ratio, so any sizer layout
    // triggered by refresh_ratio_labels() accounts for the visible label.
    m_ratio_editor_panel->Hide();
    if (m_ratio_editor_anchor) {
        m_ratio_editor_anchor->Show();
        m_ratio_editor_anchor = nullptr;
    }

    if (apply) {
        wxString value = m_ratio_editor->GetValue();
        value.Trim(true);
        value.Trim(false);
        if (value.EndsWith(wxT("%")))
            value.RemoveLast();

        long parsed = 0;
        if (value.ToLong(&parsed))
            apply_manual_ratio(m_ratio_editor_idx, (int)parsed);
        else
            refresh_ratio_labels();
    }

    m_ratio_editor_committing = false;
}

void MixedFilamentDialog::commit_ratio_editor_from_background(wxMouseEvent& e)
{
    if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown()) {
        wxPoint mouse_in_panel = m_ratio_editor_panel->ScreenToClient(wxGetMousePosition());
        if (!m_ratio_editor_panel->GetClientRect().Contains(mouse_in_panel))
            commit_ratio_editor(true);
    }
    e.Skip();
}

// ---- UI Construction ----

void MixedFilamentDialog::build_ui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    Bind(wxEVT_LEFT_DOWN, &MixedFilamentDialog::commit_ratio_editor_from_background, this);
    SetSize(FromDIP(439), FromDIP(580));

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    auto* top_sizer = new wxBoxSizer(wxHORIZONTAL);
    top_sizer->Add(create_preview_panel(), 0, wxALL, FromDIP(20));

    m_right_sizer = new wxBoxSizer(wxVERTICAL);
    m_right_sizer->Add(create_material_selection(), 0, wxEXPAND);
    m_right_sizer->Add(create_gradient_section(), 0, wxEXPAND | wxTOP, FromDIP(7));

    m_ratio_sizer = create_ratio_slider();
    m_right_sizer->Add(m_ratio_sizer, 0, wxEXPAND | wxTOP, FromDIP(7));

    m_triangle_sizer = create_triangle_picker();
    m_right_sizer->Add(m_triangle_sizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(7));

    top_sizer->Add(m_right_sizer, 1, wxTOP | wxRIGHT | wxBOTTOM, FromDIP(20));
    main_sizer->Add(top_sizer, 0, wxEXPAND);

    main_sizer->Add(create_recommendation_grid(), 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(25));

    // Warning panel: red bordered box with exclamation icon + text
    m_warning_sizer = new wxBoxSizer(wxVERTICAL);
    m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(48)));
    m_warning_panel->SetMinSize(wxSize(-1, FromDIP(48)));
    m_warning_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_warning_panel->Bind(wxEVT_PAINT, &MixedFilamentDialog::paint_warning_panel, this);
    m_warning_sizer->Add(m_warning_panel, 0, wxEXPAND);
    main_sizer->Add(m_warning_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(25));
    m_warning_panel->Hide();

    main_sizer->Add(create_button_panel(), 0, wxALIGN_RIGHT | wxALL, FromDIP(20));

    SetSizer(main_sizer);

    rebuild_all_combos();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();

    Layout();
    CentreOnParent();
}

wxBoxSizer* MixedFilamentDialog::create_preview_panel()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_preview_canvas = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(129), FromDIP(129)));
    m_preview_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_preview_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_preview_canvas);
        wxSize sz = m_preview_canvas->GetClientSize();
        size_t n = num_components();

        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        if (n == 0) return;

        int swatch_sz = FromDIP(80);
        int x0 = (sz.GetWidth() - swatch_sz) / 2;
        int y0 = (sz.GetHeight() - swatch_sz) / 2;
        double radius = FromDIP(6);

        if (m_result.gradient_enabled && n == 2) {
            Slic3r::GradientCurve curve;
            if (!m_result.gradient_curve.empty()) {
                curve.points = m_result.gradient_curve;
            } else {
                double yStart = (m_result.gradient_direction == 0) ? kGradientMaxRatio : kGradientMinRatio;
                double yEnd   = (m_result.gradient_direction == 0) ? kGradientMinRatio : kGradientMaxRatio;
                curve.points = {{0.0, yStart, NAN, NAN}, {1.0, yEnd, NAN, NAN}};
            }

            // Same sampler the sidebar, extruder icons and paint gizmo swatches use, so this
            // preview and every swatch drawn for the filament agree on what it looks like.
            auto ramp = sample_gradient_ramp(comp_colour(0), comp_colour(1), curve, std::max(80, swatch_sz));
            fill_gradient_ramp_rect(dc, wxRect(x0, y0, swatch_sz, swatch_sz), ramp);

            // Mask corners: overdraw a thick background-colored rounded rect frame
            // so the inner edge forms the desired rounded corners.
            // Known limitation: this assumes the panel background equals
            // darkModeColorFor(white).  wxGraphicsContext::Clip(path) is not
            // available in our wxWidgets build (only Clip(wxRegion) exists).
            int r = static_cast<int>(radius);
            wxColour bg = StateColor::darkModeColorFor(*wxWHITE);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(bg, r * 2));
            dc.DrawRoundedRectangle(x0 - r, y0 - r, swatch_sz + r * 2, swatch_sz + r * 2, radius * 2);
        } else {
            std::vector<wxColour> cols;
            std::vector<double>   weights;
            for (size_t i = 0; i < n; ++i) {
                cols.push_back(comp_colour(i));
                weights.push_back(ratio(i) / 100.0);
            }
            wxColour mixed = blend_n_colors(cols, weights);
            dc.SetBrush(wxBrush(mixed));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(x0, y0, swatch_sz, swatch_sz, radius);
        }
    });

    sizer->Add(m_preview_canvas, 0, wxALIGN_CENTER);

    auto* label = new wxStaticText(this, wxID_ANY, _L("Effect Preview"));
    label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_MUTED));
    label->SetFont(::Label::Body_13);
    sizer->Add(label, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));

    return sizer;
}

wxBoxSizer* MixedFilamentDialog::create_material_selection()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Summary panel — draws N components dynamically
    m_summary_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(234), FromDIP(40)));
    m_summary_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_summary_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_summary_panel);
        wxSize sz = m_summary_panel->GetClientSize();

        wxColour sum_bg   = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
        wxColour sum_text = StateColor::darkModeColorFor(wxColour("#262E30"));
        dc.SetBrush(wxBrush(sum_bg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        int swatch_sz = FromDIP(20);
        int y_center = (sz.GetHeight() - swatch_sz) / 2;
        int x = FromDIP(13);

        dc.SetFont(::Label::Body_13);

        auto draw_summary_swatch = [&](size_t comp_idx) {
            unsigned int c = comp(comp_idx);
            std::string color_hex = "#D9D9D9";
            if (c >= 1 && c <= m_physical_colors.size())
                color_hex = m_physical_colors[c - 1];
            std::string label = std::to_string(c);
            wxBitmap* icon = get_extruder_color_icon(color_hex, label, swatch_sz, swatch_sz);
            if (icon && icon->IsOk())
                dc.DrawBitmap(*icon, x, y_center);
            x += swatch_sz + FromDIP(4);
        };

        if (m_result.gradient_enabled && num_components() == 2) {
            size_t idx_a = (m_result.gradient_direction == 0) ? 0 : 1;
            size_t idx_b = 1 - idx_a;
            draw_summary_swatch(idx_a);

            dc.SetTextForeground(sum_text);
            wxString arrow = wxT("\u2192");
            wxSize arrow_sz = dc.GetTextExtent(arrow);
            dc.DrawText(arrow, x, y_center + (swatch_sz - arrow_sz.GetHeight()) / 2);
            x += arrow_sz.GetWidth() + FromDIP(4);

            draw_summary_swatch(idx_b);
        } else {
            for (size_t i = 0; i < num_components(); ++i) {
                if (i > 0) {
                    dc.SetTextForeground(sum_text);
                    wxString plus = wxT("+");
                    wxSize plus_sz = dc.GetTextExtent(plus);
                    dc.DrawText(plus, x, y_center + (swatch_sz - plus_sz.GetHeight()) / 2);
                    x += plus_sz.GetWidth() + FromDIP(4);
                }
                draw_summary_swatch(i);

                dc.SetTextForeground(sum_text);
                wxString pct = wxString::Format(wxT("%d%%"), ratio(i));
                wxSize pct_sz = dc.GetTextExtent(pct);
                dc.DrawText(pct, x, y_center + (swatch_sz - pct_sz.GetHeight()) / 2);
                x += pct_sz.GetWidth() + FromDIP(4);
            }
        }
    });
    sizer->Add(m_summary_panel, 0, wxEXPAND);

    auto* sel_label = new wxStaticText(this, wxID_ANY, _L("Select Mixed Materials"));
    sel_label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_MUTED));
    sel_label->SetFont(::Label::Body_12);
    sizer->Add(sel_label, 0, wxTOP, FromDIP(6));

    m_material_rows_sizer = new wxBoxSizer(wxVERTICAL);

    m_combo_filaments.clear();
    m_combo_to_physical.clear();
    for (size_t i = 0; i < m_result.components.size(); ++i)
        append_material_row();

    sizer->Add(m_material_rows_sizer, 0, wxEXPAND);

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_btn_add_material = new Button(this, _L("+ Add Material"));
    m_btn_add_material->SetBackgroundColor(wxColour("#F8F8F8"));
    m_btn_add_material->SetBorderColor(wxColour("#EEEEEE"));
    // The disabled tone rides on the StateColor so Enable() alone repaints it, the way m_btn_ok does.
    m_btn_add_material->SetTextColor(StateColor(
        std::make_pair(wxColour("#ACACAC"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#262E30"), (int) StateColor::Normal)));
    m_btn_add_material->SetMinSize(wxSize(-1, FromDIP(24)));
    m_btn_add_material->SetCursor(wxCursor(wxCURSOR_HAND));
    m_btn_add_material->EnableTooltipEvenDisabled();
    m_btn_add_material->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_add_material(); });
    btn_sizer->Add(m_btn_add_material, 1, wxRIGHT, FromDIP(6));

    m_btn_remove_material = new Button(this, _L("- Delete Material"));
    m_btn_remove_material->SetBackgroundColor(wxColour("#F8F8F8"));
    m_btn_remove_material->SetBorderColor(wxColour("#EEEEEE"));
    m_btn_remove_material->SetTextColor(wxColour("#262E30"));
    m_btn_remove_material->SetMinSize(wxSize(-1, FromDIP(24)));
    m_btn_remove_material->SetCursor(wxCursor(wxCURSOR_HAND));
    m_btn_remove_material->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_remove_material(); });
    m_btn_remove_material->Hide();
    btn_sizer->Add(m_btn_remove_material, 1, 0, 0);

    sizer->Add(btn_sizer, 0, wxEXPAND | wxTOP, FromDIP(9));

    return sizer;
}

wxBoxSizer* MixedFilamentDialog::create_ratio_slider()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* ratio_label = new wxStaticText(this, wxID_ANY, _L("Ratio"));
    ratio_label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_MUTED));
    ratio_label->SetFont(::Label::Body_12);
    sizer->Add(ratio_label, 0, wxBOTTOM, FromDIP(4));

    m_ratio_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(27)));
    m_ratio_bar->SetMinSize(wxSize(-1, FromDIP(27)));
    m_ratio_bar->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_ratio_bar->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_ratio_bar);
        wxSize sz = m_ratio_bar->GetClientSize();
        draw_mixed_ratio_blend_bar(dc, wxRect(0, 0, sz.GetWidth(), sz.GetHeight()),
                                   comp_colour(0), comp_colour(1), ratio(1) / 100.0);
    });

    m_ratio_bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown())
            commit_ratio_editor(true);
        m_ratio_dragging = true;
        if (!m_ratio_bar->HasCapture())
            m_ratio_bar->CaptureMouse();
        int new_ratio = 100 - (int)(e.GetX() * 100.0 / m_ratio_bar->GetClientSize().GetWidth() + 0.5);
        on_ratio_changed(std::max(MIN_COMPONENT_RATIO, std::min(100 - MIN_COMPONENT_RATIO, new_ratio)));
    });

    m_ratio_bar->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (!m_ratio_dragging) return;
        int new_ratio = 100 - (int)(e.GetX() * 100.0 / m_ratio_bar->GetClientSize().GetWidth() + 0.5);
        on_ratio_changed(std::max(MIN_COMPONENT_RATIO, std::min(100 - MIN_COMPONENT_RATIO, new_ratio)));
    });

    // Key the release off the capture itself, not off the drag flag: the two can fall out of
    // sync (a lost capture clears the flag on its own), and a capture that outlives the widget
    // wedges mouse input for the whole application.
    m_ratio_bar->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_ratio_dragging = false;
        if (m_ratio_bar->HasCapture())
            m_ratio_bar->ReleaseMouse();
    });

    m_ratio_bar->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_ratio_dragging = false;
    });

    sizer->Add(m_ratio_bar, 0, wxEXPAND);

    auto* pct_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_label_ratio_a = new RatioLabelPanel(this);
    m_label_ratio_a->SetLabel(wxString::Format(wxT("%d%%"), ratio(0)));
    m_label_ratio_b = new RatioLabelPanel(this);
    m_label_ratio_b->SetLabel(wxString::Format(wxT("%d%%"), ratio(1)));
    auto bind_ratio_click = [this](RatioLabelPanel* label, size_t idx) {
        label->Bind(wxEVT_LEFT_DOWN, [this, label, idx](wxMouseEvent&) {
            wxRect rect(wxPoint(0, 0), label->GetClientSize());
            start_ratio_editor(idx, label, rect);
        });
    };
    bind_ratio_click(m_label_ratio_a, 0);
    bind_ratio_click(m_label_ratio_b, 1);
    pct_sizer->Add(m_label_ratio_a, 0);
    pct_sizer->AddStretchSpacer(1);
    pct_sizer->Add(m_label_ratio_b, 0);
    sizer->Add(pct_sizer, 0, wxEXPAND | wxTOP, FromDIP(2));

    return sizer;
}

// ---- Triangle (ternary) ratio picker ----
// The barycentric utilities (TriPoint, tri_contains, tri_barycentric, tri_clamp) live in
// FilamentBitmapUtils so the Publish dialog can mirror this picker read-only.

wxBoxSizer* MixedFilamentDialog::create_triangle_picker()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    int panel_w = FromDIP(160);
    int panel_h = FromDIP(160);
    m_triangle_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(panel_w, panel_h));
    m_triangle_panel->SetMinSize(wxSize(panel_w, panel_h));
    m_triangle_panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_triangle_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto get_vertices = [this]() -> std::tuple<TriPoint, TriPoint, TriPoint> {
        wxSize sz = m_triangle_panel->GetClientSize();
        double pw = sz.GetWidth(), ph = sz.GetHeight();
        double margin = FromDIP(20);
        double avail = std::min(pw, ph) - 2 * margin;
        double side = avail;
        double tri_h = side * std::sqrt(3.0) / 2.0;
        double cx = pw / 2.0;
        double top_y = (ph - tri_h) / 2.0;
        double bot_y = top_y + tri_h;
        TriPoint v0 = {cx, top_y};                             // top
        TriPoint v1 = {cx - side / 2.0, bot_y};               // bottom-left
        TriPoint v2 = {cx + side / 2.0, bot_y};               // bottom-right
        return {v0, v1, v2};
    };

    m_triangle_panel->Bind(wxEVT_PAINT, [this, get_vertices](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_triangle_panel);
        wxSize sz = m_triangle_panel->GetClientSize();
        auto [v0, v1, v2] = get_vertices();

        // Draw the background, cached barycentric fill, outline and drag-handle marker through the
        // shared picker painter (same geometry the read-only Publish preview uses).
        draw_mixed_triangle_picker(dc, sz,
                                   {comp_colour(0), comp_colour(1), comp_colour(2)},
                                   {m_tri_wx, m_tri_wy, m_tri_wz},
                                   {StateColor::darkModeColorFor(*wxWHITE),
                                    StateColor::darkModeColorFor(wxColour("#CECECE")),
                                    StateColor::darkModeColorFor(wxColour("#262E30")),
                                    StateColor::darkModeColorFor(COLOR_LABEL_MUTED)});

        if (m_result.ratios.size() >= 3) {
            dc.SetFont(::Label::Body_10);
            wxSize ts0 = dc.GetTextExtent(wxString::Format(wxT("%d%%"), m_result.ratios[0]));
            int top_label_y = std::max(0, (int)(v0.y - ts0.GetHeight() - FromDIP(4)));

            dc.SetFont(::Label::Body_12);
            dc.SetTextForeground(StateColor::darkModeColorFor(COLOR_LABEL_MUTED));
            dc.DrawText(_L("Ratio"), FromDIP(2), top_label_y);

            // Position the real RatioLabelPanel children
            for (int i = 0; i < 3 && i < (int)m_triangle_ratio_labels.size(); ++i) {
                if (!m_triangle_ratio_labels[i]) continue;
                m_triangle_ratio_labels[i]->SetLabel(
                    wxString::Format(wxT("%d%%"), m_result.ratios[i]));
                wxSize lsz = m_triangle_ratio_labels[i]->GetMinSize();
                int lx = 0, ly = 0;
                if (i == 0) {
                    lx = (int)(v0.x - lsz.GetWidth() / 2);
                    ly = top_label_y;
                } else if (i == 1) {
                    lx = (int)(v1.x - lsz.GetWidth() / 2);
                    ly = (int)(v1.y + FromDIP(3));
                } else {
                    lx = (int)(v2.x - lsz.GetWidth() / 2);
                    ly = (int)(v2.y + FromDIP(3));
                }
                m_triangle_ratio_labels[i]->SetSize(lx, ly, lsz.GetWidth(), lsz.GetHeight());
            }
        }
    });

    auto handle_mouse = [this, get_vertices](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_vertices();
        TriPoint p = {(double)e.GetX(), (double)e.GetY()};

        if (is_down) {
            // Only start dragging when the press lands inside the triangle;
            // clicks outside the triangle must not change the mix ratio.
            if (!tri_contains(p, v0, v1, v2))
                return;
            m_tri_dragging = true;
            if (!m_triangle_panel->HasCapture())
                m_triangle_panel->CaptureMouse();
        }

        if (!m_tri_dragging) return;

        TriPoint clamped = tri_clamp(p, v0, v1, v2);
        tri_barycentric(clamped, v0, v1, v2, m_tri_wx, m_tri_wy, m_tri_wz);

        int r0 = (int)(m_tri_wx * 100 + 0.5);
        int r1 = (int)(m_tri_wy * 100 + 0.5);
        int r2 = 100 - r0 - r1;
        r0 = std::clamp(r0, 0, 100);
        r1 = std::clamp(r1, 0, 100);
        r2 = std::clamp(r2, 0, 100);

        apply_dragged_triangle_ratio(r0, r1, r2);
    };

    // Create 3 RatioLabelPanel children on the triangle panel
    m_triangle_ratio_labels.fill(nullptr);
    for (int i = 0; i < 3; ++i) {
        auto* lbl = new RatioLabelPanel(m_triangle_panel);
        lbl->SetLabel(wxString::Format(wxT("%d%%"),
                      (i < (int)m_result.ratios.size()) ? m_result.ratios[i] : 33));
        size_t idx = (size_t)i;
        lbl->Bind(wxEVT_LEFT_DOWN, [this, lbl, idx](wxMouseEvent&) {
            wxRect rect(wxPoint(0, 0), lbl->GetClientSize());
            start_ratio_editor(idx, lbl, rect);
        });
        m_triangle_ratio_labels[i] = lbl;
    }

    m_triangle_panel->Bind(wxEVT_LEFT_DOWN, [this, handle_mouse](wxMouseEvent& e) {
        if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown())
            commit_ratio_editor(true);
        handle_mouse(e, true);
    });
    m_triangle_panel->Bind(wxEVT_MOTION, [this, handle_mouse](wxMouseEvent& e) {
        if (m_tri_dragging)
            handle_mouse(e, false);
    });
    m_triangle_panel->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_tri_dragging = false;
        if (m_triangle_panel->HasCapture())
            m_triangle_panel->ReleaseMouse();
    });
    m_triangle_panel->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_tri_dragging = false;
    });

    sizer->Add(m_triangle_panel, 0);

    return sizer;
}

wxBoxSizer* MixedFilamentDialog::create_gradient_section()
{
    m_gradient_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_chk_gradient = new ::CheckBox(this);
    m_chk_gradient->SetValue(m_result.gradient_enabled);
    m_chk_gradient->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { e.Skip(); on_gradient_toggled(); });
    m_gradient_sizer->Add(m_chk_gradient, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));

    m_label_gradient = new wxStaticText(this, wxID_ANY, _L("Gradient Effect"));
    m_label_gradient->SetFont(::Label::Body_13);
    m_gradient_sizer->Add(m_label_gradient, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    m_combo_gradient_dir = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                        wxSize(FromDIP(152), FromDIP(24)), 0, nullptr, wxCB_READONLY);
    m_combo_gradient_dir->SetKeepDropArrow(true);
    update_gradient_direction_items();
    m_combo_gradient_dir->SetSelection(m_result.gradient_direction);
    m_combo_gradient_dir->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_gradient_direction_changed(); });
    m_combo_gradient_dir->Show(m_result.gradient_enabled);

    m_gradient_sizer->Add(m_combo_gradient_dir, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(m_gradient_sizer, 0, wxEXPAND);

    // Custom curve editor: visible only when gradient is on and exactly 2 components are mixed.
    m_curve_sizer = new wxBoxSizer(wxVERTICAL);
    m_curve_editor = new GradientCurveEditor(this, comp_colour(0), comp_colour(1));
    if (!m_result.gradient_curve.empty())
        m_curve_editor->set_points(m_result.gradient_curve);
    else
        m_curve_editor->reset_to_linear((m_result.gradient_direction == 0) ? 0.9 : 0.1,
                                        (m_result.gradient_direction == 0) ? 0.1 : 0.9);
    m_curve_editor->Bind(wxEVT_GRADIENT_CURVE_CHANGED,
        [this](wxCommandEvent&) { on_gradient_curve_changed(); });
    m_curve_sizer->Add(m_curve_editor, 0, wxEXPAND | wxTOP, FromDIP(4));

    outer->Add(m_curve_sizer, 0, wxEXPAND | wxTOP, FromDIP(6));
    const bool curve_visible = m_result.gradient_enabled && num_components() == 2;
    m_curve_sizer->ShowItems(curve_visible);

    // Per-part gradient toggle sits BELOW the curve editor.
    m_per_part_gradient_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_chk_per_part_gradient = new ::CheckBox(this);
    m_chk_per_part_gradient->SetValue(m_result.per_part_gradient);
    m_chk_per_part_gradient->Bind(wxEVT_TOGGLEBUTTON,
        [this](wxCommandEvent& e) { e.Skip(); on_per_part_gradient_toggled(); });
    m_per_part_gradient_sizer->Add(m_chk_per_part_gradient, 0,
        wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));

    m_label_per_part_gradient = new wxStaticText(this, wxID_ANY, _L("Enable per-part gradient effect"));
    m_label_per_part_gradient->SetFont(::Label::Body_13);
    m_per_part_gradient_sizer->Add(m_label_per_part_gradient, 0,
        wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    outer->Add(m_per_part_gradient_sizer, 0, wxEXPAND | wxTOP, FromDIP(2));
    m_per_part_gradient_sizer->ShowItems(m_result.gradient_enabled);

    return outer;
}

wxBoxSizer* MixedFilamentDialog::create_recommendation_grid()
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* rec_label = new wxStaticText(this, wxID_ANY, _L("Mixing Recommendations"));
    rec_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#ACACAC")));
    rec_label->SetFont(::Label::Body_10);
    title_sizer->Add(rec_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    auto* rec_line = new wxPanel(this, wxID_ANY);
    rec_line->SetMinSize(wxSize(-1, 1));
    rec_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#EEEEEE")));
    title_sizer->Add(rec_line, 1, wxALIGN_CENTER_VERTICAL);

    outer->Add(title_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    m_recommendation_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(116)));
    m_recommendation_scroll->SetScrollRate(0, FromDIP(20));
    m_recommendation_scroll->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F8F8")));

    m_recommendation_grid = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
    auto* scroll_inner_sizer = new wxBoxSizer(wxVERTICAL);
    scroll_inner_sizer->Add(m_recommendation_grid, 1, wxEXPAND | wxLEFT | wxTOP, FromDIP(8));
    m_recommendation_scroll->SetSizer(scroll_inner_sizer);

    rebuild_recommendation_items();

    outer->Add(m_recommendation_scroll, 1, wxEXPAND | wxTOP, FromDIP(4));
    return outer;
}

void MixedFilamentDialog::rebuild_recommendation_items()
{
    if (!m_recommendation_scroll || !m_recommendation_grid)
        return;

    static constexpr int MAX_RECOMMENDATIONS = 100;

    m_recommendation_scroll->Freeze();
    m_recommendation_grid->Clear(true);

    size_t n = m_physical_colors.size();
    int count = 0;

    // Group physical filaments by type (only same-type combos are recommended)
    std::map<std::string, std::vector<size_t>> type_groups;
    for (size_t i = 0; i < n; ++i) {
        std::string t = (i < m_physical_types.size()) ? m_physical_types[i] : "PLA";
        // Skip support filaments (type ends with "-S")
        if (t.size() >= 2 && t.compare(t.size() - 2, 2, "-S") == 0)
            continue;
        type_groups[t].push_back(i);
    }

    if (num_components() >= 3) {
        // Three-color: C(g,3) x 3 variants per same-type group
        for (auto& [type, indices] : type_groups) {
            if (count >= MAX_RECOMMENDATIONS) break;
            size_t g = indices.size();
            for (size_t ai = 0; ai < g && count < MAX_RECOMMENDATIONS; ++ai) {
                for (size_t bi = ai + 1; bi < g && count < MAX_RECOMMENDATIONS; ++bi) {
                    for (size_t ci = bi + 1; ci < g && count < MAX_RECOMMENDATIONS; ++ci) {
                        size_t idx[3] = {indices[ai], indices[bi], indices[ci]};
                        // 3 variants: each filament takes the 50% role in turn
                        for (int dominant = 0; dominant < 3 && count < MAX_RECOMMENDATIONS; ++dominant) {
                            size_t i0 = idx[(dominant + 1) % 3]; // 25%
                            size_t i1 = idx[(dominant + 2) % 3]; // 25%
                            size_t i2 = idx[dominant];           // 50%

                            wxColour ca(m_physical_colors[i0]);
                            wxColour cb(m_physical_colors[i1]);
                            wxColour cc(m_physical_colors[i2]);
                            wxColour mixed = blend_n_colors({ca, cb, cc}, {0.25, 0.25, 0.50});

                            auto* item = new wxPanel(m_recommendation_scroll, wxID_ANY,
                                                     wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)));
                            item->SetBackgroundColour(mixed);
                            item->SetCursor(wxCursor(wxCURSOR_HAND));

                            unsigned int ca_1 = (unsigned int)(i0 + 1);
                            unsigned int cb_1 = (unsigned int)(i1 + 1);
                            unsigned int cc_1 = (unsigned int)(i2 + 1);
                            item->Bind(wxEVT_LEFT_UP, [this, ca_1, cb_1, cc_1](wxMouseEvent&) {
                                on_recommendation_clicked_triple(ca_1, cb_1, cc_1);
                            });
                            item->SetToolTip(wxString::Format(wxT("%s + %s + %s"),
                                wxString::FromUTF8(m_physical_names[i0]),
                                wxString::FromUTF8(m_physical_names[i1]),
                                wxString::FromUTF8(m_physical_names[i2])));

                            m_recommendation_grid->Add(item, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
                            ++count;
                        }
                    }
                }
            }
        }
    } else {
        // Two-color: C(g,2) per same-type group
        for (auto& [type, indices] : type_groups) {
            if (count >= MAX_RECOMMENDATIONS) break;
            size_t g = indices.size();
            for (size_t ai = 0; ai < g && count < MAX_RECOMMENDATIONS; ++ai) {
                for (size_t bi = ai + 1; bi < g && count < MAX_RECOMMENDATIONS; ++bi) {
                    size_t i = indices[ai];
                    size_t j = indices[bi];

                    wxColour ca(m_physical_colors[i]);
                    wxColour cb(m_physical_colors[j]);
                    wxColour mixed = blend_colors(ca, cb, 0.5);

                    auto* item = new wxPanel(m_recommendation_scroll, wxID_ANY,
                                             wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)));
                    item->SetBackgroundColour(mixed);
                    item->SetCursor(wxCursor(wxCURSOR_HAND));

                    unsigned int comp_a = (unsigned int)(i + 1);
                    unsigned int comp_b = (unsigned int)(j + 1);
                    item->Bind(wxEVT_LEFT_UP, [this, comp_a, comp_b](wxMouseEvent&) {
                        on_recommendation_clicked(comp_a, comp_b);
                    });
                    item->SetToolTip(wxString::Format(wxT("%s + %s"),
                        wxString::FromUTF8(m_physical_names[i]),
                        wxString::FromUTF8(m_physical_names[j])));

                    m_recommendation_grid->Add(item, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
                    ++count;
                }
            }
        }
    }

    m_recommendation_scroll->SetScrollbars(0, FromDIP(20), 0, 1);
    m_recommendation_scroll->FitInside();
    m_recommendation_scroll->Layout();
    m_recommendation_scroll->Thaw();
}

wxBoxSizer* MixedFilamentDialog::create_button_panel()
{
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    m_btn_cancel = new Button(this, _L("Cancel"));
    m_btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    m_btn_ok = new Button(this, _L("OK"));
    m_btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

    sizer->Add(m_btn_cancel, 0, wxRIGHT, FromDIP(12));
    sizer->Add(m_btn_ok, 0);

    return sizer;
}

void MixedFilamentDialog::rebuild_all_combos()
{
    m_combo_to_physical.resize(m_combo_filaments.size());

    for (size_t i = 0; i < m_combo_filaments.size(); ++i) {
        std::set<unsigned int> others_selected;
        std::set<std::string>  others_types;
        for (size_t k = 0; k < m_result.components.size(); ++k) {
            if (k == i) continue;
            unsigned int phys = m_result.components[k];
            others_selected.insert(phys);
            if (phys >= 1 && phys <= m_physical_types.size())
                others_types.insert(m_physical_types[phys - 1]);
        }

        auto* combo = m_combo_filaments[i];
        combo->Clear();
        m_combo_to_physical[i].clear();

        int restore_sel = -1;
        unsigned int cur_phys = (i < m_result.components.size()) ? m_result.components[i] : 0;

        if (cur_phys == 0) {
            combo->Append(_L("-- Select --"));
            m_combo_to_physical[i].push_back(0);
            restore_sel = 0;
        }

        for (size_t j = 0; j < m_physical_names.size(); ++j) {
            unsigned int phys_1based = (unsigned int)(j + 1);

            if (others_selected.count(phys_1based))
                continue;

            int style = 0;
            if (!others_types.empty() && !m_physical_types.empty()) {
                std::string this_type = (j < m_physical_types.size()) ? m_physical_types[j] : "PLA";
                if (others_types.find(this_type) == others_types.end())
                    style = DD_ITEM_STYLE_DIMMED;
            }

            int idx = combo->Append(wxString::FromUTF8(m_physical_names[j]), make_swatch_bitmap(j), style);
            m_combo_to_physical[i].push_back(phys_1based);

            if (phys_1based == cur_phys)
                restore_sel = idx;
        }

        if (restore_sel >= 0)
            combo->SetSelection(restore_sel);
        else if (combo->GetCount() > 0)
            combo->SetSelection(0);
    }
}

void MixedFilamentDialog::refresh_curve_editor_colors()
{
    if (m_curve_editor)
        m_curve_editor->set_colors(comp_colour(0), comp_colour(1));
}

// ---- Event Handlers ----

void MixedFilamentDialog::on_filament_changed()
{
    for (size_t i = 0; i < m_combo_filaments.size() && i < m_result.components.size(); ++i) {
        int sel = m_combo_filaments[i]->GetSelection();
        if (sel >= 0 && i < m_combo_to_physical.size() && sel < (int)m_combo_to_physical[i].size())
            m_result.components[i] = m_combo_to_physical[i][sel];
    }

    refresh_curve_editor_colors();
    rebuild_all_combos();
    update_gradient_direction_items();
    update_preview();
    update_ok_button_state();
}

void MixedFilamentDialog::on_ratio_changed(int new_ratio_a)
{
    if (m_result.ratios.size() < 2) return;
    m_result.ratios[0] = new_ratio_a;
    m_result.ratios[1] = 100 - new_ratio_a;

    reset_manual_ratio_state();
    refresh_ratio_labels();

    update_preview();
}

void MixedFilamentDialog::on_gradient_toggled()
{

    m_result.gradient_enabled = m_chk_gradient->GetValue();

    if (m_ratio_sizer)
        m_ratio_sizer->ShowItems(!m_result.gradient_enabled && num_components() == 2);
    if (m_combo_gradient_dir)
        m_combo_gradient_dir->Show(m_result.gradient_enabled);
    if (m_per_part_gradient_sizer)
        m_per_part_gradient_sizer->ShowItems(m_result.gradient_enabled);
    if (m_curve_sizer)
        m_curve_sizer->ShowItems(m_result.gradient_enabled && num_components() == 2);
    if (!m_result.gradient_enabled) {
        m_result.per_part_gradient = false;
        if (m_chk_per_part_gradient) m_chk_per_part_gradient->SetValue(false);
    }

    // Toggling the curve editor changes the right column height (and width when
    // turning gradient on), so the dialog must follow or the recommendation list
    // gets squeezed off-screen. Same trick as 2-color -> 3-color switching.
    const wxSize new_size = compute_dialog_size();
    if (GetSize() != new_size) {
        const wxRect old_rect = GetRect();
        const wxPoint center(old_rect.x + old_rect.width / 2,
                             old_rect.y + old_rect.height / 2);
        SetSize(new_size);
        SetPosition(wxPoint(center.x - new_size.x / 2,
                            center.y - new_size.y / 2));
    }

    Layout();
    Refresh();
}

void MixedFilamentDialog::on_gradient_direction_changed()
{
    if (!m_combo_gradient_dir) return;
    m_result.gradient_direction = m_combo_gradient_dir->GetSelection();

    // Mirror the user's custom curve around y=0.5 instead of resetting it, so
    // shape work (added anchors, bent segments) survives a direction toggle.
    // reverse() flips y and tangent signs consistently; default two-point
    // linear curves end up matching the new direction exactly (0.9->0.1 <-> 0.1->0.9).
    if (m_curve_editor) {
        m_curve_editor->reverse();
        m_result.gradient_curve = m_curve_editor->get_points();
    }
    update_preview();
}

void MixedFilamentDialog::on_gradient_curve_changed()
{
    if (m_curve_editor)
        m_result.gradient_curve = m_curve_editor->get_points();
    update_preview();
}

void MixedFilamentDialog::on_per_part_gradient_toggled()
{
    if (m_chk_per_part_gradient)
        m_result.per_part_gradient = m_chk_per_part_gradient->GetValue();
}

void MixedFilamentDialog::on_add_material()
{
    size_t n = num_components();
    if (n >= (size_t)MAX_COMPONENTS) return;

    unsigned int new_comp = 0;
    for (size_t j = 0; j < m_physical_names.size(); ++j) {
        unsigned int candidate = (unsigned int)(j + 1);
        bool used = false;
        for (auto c : m_result.components)
            if (c == candidate) { used = true; break; }
        if (!used) { new_comp = candidate; break; }
    }
    if (new_comp == 0) return;
    m_result.components.push_back(new_comp);

    int each = 100 / (int)(n + 1);
    m_result.ratios.clear();
    int assigned = 0;
    for (size_t i = 0; i < n; ++i) {
        m_result.ratios.push_back(each);
        assigned += each;
    }
    m_result.ratios.push_back(100 - assigned);

    if (m_result.ratios.size() >= 3) {
        int sum = 0;
        for (int r : m_result.ratios) sum += r;
        if (sum > 0) {
            m_tri_wx = (double)m_result.ratios[0] / sum;
            m_tri_wy = (double)m_result.ratios[1] / sum;
            m_tri_wz = (double)m_result.ratios[2] / sum;
        }
    }
    reset_manual_ratio_state();

    append_material_row();

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    rebuild_recommendation_items();

    Layout();
    Refresh();
}

void MixedFilamentDialog::on_remove_material()
{
    if (num_components() <= 2)
        return;

    m_result.components.resize(2);
    m_result.ratios = {50, 50};
    m_tri_wx = 0.5;
    m_tri_wy = 0.5;
    m_tri_wz = 0.0;

    reset_manual_ratio_state();
    refresh_ratio_labels();

    if (m_material_rows_sizer && m_material_rows_sizer->GetItemCount() > 2) {
        auto* sizer_item = m_material_rows_sizer->GetItem(m_material_rows_sizer->GetItemCount() - 1);
        if (sizer_item && sizer_item->GetSizer())
            sizer_item->GetSizer()->Clear(true);
        m_material_rows_sizer->Remove(m_material_rows_sizer->GetItemCount() - 1);
    }

    if (m_combo_filaments.size() > 2)
        m_combo_filaments.pop_back();
    if (m_combo_to_physical.size() > 2)
        m_combo_to_physical.pop_back();

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    rebuild_recommendation_items();
    Layout();
    Refresh();
}

void MixedFilamentDialog::on_recommendation_clicked(unsigned int comp_a, unsigned int comp_b)
{
    while (m_material_rows_sizer->GetItemCount() > 2) {
        auto* sizer_item = m_material_rows_sizer->GetItem(m_material_rows_sizer->GetItemCount() - 1);
        if (sizer_item && sizer_item->GetSizer())
            sizer_item->GetSizer()->Clear(true);
        m_material_rows_sizer->Remove(m_material_rows_sizer->GetItemCount() - 1);
    }

    while (m_combo_filaments.size() > 2)
        m_combo_filaments.pop_back();
    while (m_combo_to_physical.size() > 2)
        m_combo_to_physical.pop_back();

    m_result.components = {comp_a, comp_b};
    m_result.ratios = {50, 50};

    reset_manual_ratio_state();
    refresh_ratio_labels();

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_gradient_direction_items();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    Layout();
    Refresh();
}

void MixedFilamentDialog::on_recommendation_clicked_triple(unsigned int a, unsigned int b, unsigned int c)
{
    // Ensure we have exactly 3 combo rows
    if (num_components() < 3) {
        // Need to add a 3rd combo row
        while (m_combo_filaments.size() < 3)
            append_material_row();
    } else if (num_components() > 3) {
        while (m_material_rows_sizer->GetItemCount() > 3) {
            auto* sizer_item = m_material_rows_sizer->GetItem(m_material_rows_sizer->GetItemCount() - 1);
            if (sizer_item && sizer_item->GetSizer())
                sizer_item->GetSizer()->Clear(true);
            m_material_rows_sizer->Remove(m_material_rows_sizer->GetItemCount() - 1);
        }
        while (m_combo_filaments.size() > 3)
            m_combo_filaments.pop_back();
        while (m_combo_to_physical.size() > 3)
            m_combo_to_physical.pop_back();
    }

    m_result.components = {a, b, c};
    m_result.ratios = {25, 25, 50};
    m_tri_wx = 0.25;
    m_tri_wy = 0.25;
    m_tri_wz = 0.50;
    reset_manual_ratio_state();

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_gradient_direction_items();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    Layout();
    Refresh();
}

void MixedFilamentDialog::update_preview()
{
    if (m_preview_canvas)  m_preview_canvas->Refresh();
    if (m_summary_panel)   m_summary_panel->Refresh();
    if (m_ratio_bar)       m_ratio_bar->Refresh();
    if (m_triangle_panel)  m_triangle_panel->Refresh();
}

void MixedFilamentDialog::paint_warning_panel(wxPaintEvent&)
{
    wxBufferedPaintDC dc(m_warning_panel);
    wxSize sz = m_warning_panel->GetClientSize();

    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#F8F8F8"))));
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D01B1B")), 1));
    dc.DrawRoundedRectangle(0, 0, sz.GetWidth(), sz.GetHeight(), FromDIP(4));

    int x = FromDIP(10);
    int cy = sz.GetHeight() / 2;

    int icon_r = FromDIP(7);
    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#D01B1B"))));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawCircle(x + icon_r, cy, icon_r);
    dc.SetFont(::Label::Body_10);
    dc.SetTextForeground(*wxWHITE);
    wxSize ex = dc.GetTextExtent(wxT("!"));
    dc.DrawText(wxT("!"), x + icon_r - ex.GetWidth() / 2, cy - ex.GetHeight() / 2);
    x += icon_r * 2 + FromDIP(6);

    if (m_type_mismatch_msg.empty()) return;

    dc.SetFont(::Label::Body_12);
    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#D01B1B")));
    wxString msg = m_type_mismatch_msg;
    int avail_w = sz.GetWidth() - x - FromDIP(10);
    wxSize ts = dc.GetTextExtent(msg);
    if (ts.GetWidth() <= avail_w) {
        dc.DrawText(msg, x, cy - ts.GetHeight() / 2);
    } else {
        wxArrayString lines;
        wxString cur_line;
        wxArrayString words;
        wxStringTokenizer tkz(msg, wxT(" "), wxTOKEN_RET_EMPTY_ALL);
        while (tkz.HasMoreTokens()) words.Add(tkz.GetNextToken());
        if (words.empty()) words.Add(msg);
        for (size_t w = 0; w < words.size(); ++w) {
            wxString test = cur_line.empty() ? words[w] : cur_line + wxT(" ") + words[w];
            if (dc.GetTextExtent(test).GetWidth() > avail_w && !cur_line.empty()) {
                lines.Add(cur_line);
                cur_line = words[w];
            } else {
                cur_line = test;
            }
        }
        if (!cur_line.empty()) lines.Add(cur_line);
        if (lines.empty()) lines.Add(msg);
        int line_h = dc.GetTextExtent(wxT("Mg")).GetHeight();
        int total_h = (int)lines.size() * line_h;
        int y0 = (sz.GetHeight() - total_h) / 2;
        for (size_t l = 0; l < lines.size(); ++l)
            dc.DrawText(lines[l], x, y0 + (int)l * line_h);
    }
}

void MixedFilamentDialog::update_ok_button_state()
{
    if (!m_btn_ok) return;

    bool has_type_mismatch = false;
    if (!m_physical_types.empty() && m_result.components.size() >= 2) {
        std::map<std::string, std::vector<unsigned int>> type_groups;
        for (size_t i = 0; i < m_result.components.size(); ++i) {
            unsigned int phys = m_result.components[i];
            if (phys < 1 || phys > m_physical_types.size()) continue;
            type_groups[m_physical_types[phys - 1]].push_back(phys);
        }
        has_type_mismatch = type_groups.size() > 1;
        if (has_type_mismatch) {
            wxString parts;
            for (auto it = type_groups.begin(); it != type_groups.end(); ++it) {
                if (!parts.empty())
                    parts += "; ";
                wxString slots;
                for (size_t j = 0; j < it->second.size(); ++j) {
                    if (!slots.empty()) slots += ", ";
                    slots += std::to_string(it->second[j]);
                }
                parts += wxString::Format(_L("Slot %s (%s)"), slots, wxString::FromUTF8(it->first));
            }
            m_type_mismatch_msg = wxString::Format(_L("Different filament types cannot be mixed: %s. Please select the same filament type."), parts);
        } else {
            m_type_mismatch_msg.clear();
        }
    } else {
        m_type_mismatch_msg.clear();
    }

    bool has_unselected = false;
    for (unsigned int c : m_result.components) {
        if (c == 0) { has_unselected = true; break; }
    }

    bool can_confirm = !has_type_mismatch && !has_unselected;
    // Enable() alone repaints the button: its StateColor carries the disabled grey.
    m_btn_ok->Enable(can_confirm);
    if (has_unselected)
        m_btn_ok->SetToolTip(_L("Please select a filament for all components"));
    else if (has_type_mismatch)
        m_btn_ok->SetToolTip(_L("Cannot mix different filament types"));
    else
        m_btn_ok->SetToolTip(wxEmptyString);

    if (m_warning_panel) {
        m_warning_panel->Show(has_type_mismatch);
        // Force a repaint: when the panel is already visible and only the
        // mismatch text changes (e.g. PETG -> ABS), Show()/Layout() do not
        // generate a paint event, so paint_warning_panel keeps the stale text.
        m_warning_panel->Refresh();
        Layout();
    }
}

void MixedFilamentDialog::update_gradient_direction_items()
{
    if (!m_combo_gradient_dir) return;

    int prev_sel = m_combo_gradient_dir->GetSelection();
    m_combo_gradient_dir->Clear();

    if (num_components() < 2) return;

    auto make_direction_bitmap = [this](size_t idx_from, size_t idx_to) -> wxBitmap {
        int swatch_sz = FromDIP(20);
        int arrow_w   = FromDIP(16);
        int gap       = FromDIP(4);
        int bmp_w = swatch_sz + gap + arrow_w + gap + swatch_sz;
        int bmp_h = swatch_sz;

        wxColour dir_text = StateColor::darkModeColorFor(wxColour("#262E30"));

        return make_alpha_bitmap(bmp_w, bmp_h, [&](wxDC& dc) {
            dc.SetFont(::Label::Body_13);

            auto draw_swatch = [&](int x, size_t idx) {
                std::string color_hex = "#D9D9D9";
                if (idx < m_physical_colors.size())
                    color_hex = m_physical_colors[idx];
                std::string label = std::to_string(idx + 1);
                wxBitmap* icon = get_extruder_color_icon(color_hex, label, swatch_sz, swatch_sz);
                if (icon && icon->IsOk())
                    dc.DrawBitmap(*icon, x, 0);
            };

            int x = 0;
            draw_swatch(x, idx_from);
            x += swatch_sz + gap;

            dc.SetTextForeground(dir_text);
            wxString arrow = wxT("\u2192");
            wxSize arrow_sz = dc.GetTextExtent(arrow);
            dc.DrawText(arrow, x + (arrow_w - arrow_sz.GetWidth()) / 2,
                               (bmp_h - arrow_sz.GetHeight()) / 2);
            x += arrow_w + gap;

            draw_swatch(x, idx_to);
        });
    };

    size_t idx_a = (comp(0) >= 1) ? comp(0) - 1 : 0;
    size_t idx_b = (comp(1) >= 1) ? comp(1) - 1 : 1;

    m_combo_gradient_dir->Append(wxT(" "), make_direction_bitmap(idx_a, idx_b));
    m_combo_gradient_dir->Append(wxT(" "), make_direction_bitmap(idx_b, idx_a));

    if (prev_sel >= 0 && prev_sel < (int)m_combo_gradient_dir->GetCount())
        m_combo_gradient_dir->SetSelection(prev_sel);
    else
        m_combo_gradient_dir->SetSelection(0);
}

wxSize MixedFilamentDialog::compute_dialog_size() const
{
    const bool is_three      = (num_components() >= 3);
    const bool curve_visible = !is_three && m_result.gradient_enabled;

    int w = FromDIP(439);
    int h = FromDIP(580);
    if (is_three) {
        h = FromDIP(680);
    } else if (curve_visible) {
        // Wider so the gradient editor can show "Material Ratio" intact;
        // +40 over the 3-color height to fit the curve editor while keeping the
        // recommendation list visible (it can still scroll if needed).
        w = FromDIP(470);
        h = FromDIP(720);
    }
    return wxSize(w, h);
}

void MixedFilamentDialog::update_component_count_ui()
{
    bool is_two   = (num_components() == 2);
    bool is_three = (num_components() >= 3);

    // Toggle ratio slider vs triangle picker
    if (m_ratio_sizer)
        m_ratio_sizer->ShowItems(is_two && !m_result.gradient_enabled);
    if (m_triangle_sizer)
        m_triangle_sizer->ShowItems(is_three);

    // 3-color: hide gradient entirely, force off
    if (m_gradient_sizer) {
        bool show_gradient = is_two;
        m_chk_gradient->Show(show_gradient);
        if (m_label_gradient) m_label_gradient->Show(show_gradient);
        m_combo_gradient_dir->Show(show_gradient && m_result.gradient_enabled);
        if (m_per_part_gradient_sizer)
            m_per_part_gradient_sizer->ShowItems(show_gradient && m_result.gradient_enabled);
        if (m_curve_sizer)
            m_curve_sizer->ShowItems(show_gradient && m_result.gradient_enabled);
    }
    if (is_three) {
        m_result.gradient_enabled = false;
        if (m_chk_gradient) m_chk_gradient->SetValue(false);
        m_result.per_part_gradient = false;
        if (m_chk_per_part_gradient) m_chk_per_part_gradient->SetValue(false);
    }

    if (m_btn_add_material) {
        bool can_add = (num_components() < (size_t)MAX_COMPONENTS && m_physical_colors.size() > num_components());
        m_btn_add_material->Enable(can_add);
        m_btn_add_material->SetToolTip(can_add ? wxString()
            : (is_three ? _L("Maximum 3 materials for mixing") : _L("Maximum number of components reached")));
    }

    if (m_btn_remove_material) {
        m_btn_remove_material->Show(is_three);
        m_btn_remove_material->Enable(is_three);
        m_btn_remove_material->SetToolTip(is_three ? _L("Remove the third material") : wxString());
    }

    const wxSize new_size = compute_dialog_size();
    const wxRect old_rect = GetRect();
    const wxPoint center(old_rect.x + old_rect.width / 2,
                         old_rect.y + old_rect.height / 2);
    SetSize(new_size);
    SetPosition(wxPoint(center.x - new_size.x / 2,
                        center.y - new_size.y / 2));
    Layout();
}

} // namespace GUI
} // namespace Slic3r

// ===========================================================================
// Snapmaker: Full Spectrum mixed filament dialog (SnapmakerMixedFilamentDialog)
// ===========================================================================

namespace Slic3r { namespace GUI {

static constexpr int SWATCH_SIZE  = 16;
static constexpr int PREVIEW_SIZE = 140;
static constexpr int STRIP_HEIGHT = 24;

// ---------------------------------------------------------------------------
// Custom Figma-style range slider for match mode "Min mix ratio"
// ---------------------------------------------------------------------------
class MatchRangeSlider : public wxPanel
{
public:
    MatchRangeSlider(wxWindow* parent, int value, int minVal, int maxVal)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
        , m_value(value), m_min(minVal), m_max(maxVal)
    {
        SetInitialSize(wxSize(FromDIP(110), FromDIP(22)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        Bind(wxEVT_PAINT, &MatchRangeSlider::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &MatchRangeSlider::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &MatchRangeSlider::on_left_up, this);
        Bind(wxEVT_MOTION, &MatchRangeSlider::on_motion, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &MatchRangeSlider::on_capture_lost, this);
    }

    int value() const { return m_value; }
    void set_value(int v) { m_value = std::clamp(v, m_min, m_max); Refresh(); }

private:
    int m_value, m_min, m_max;
    bool m_dragging = false;

    int value_from_x(int x) const {
        int tw = std::max(4, GetClientSize().x - FromDIP(10));
        int tx = FromDIP(5);
        float frac = (float)(x - tx) / (float)tw;
        return m_min + (int)(frac * (m_max - m_min) + 0.5f);
    }

    void fire_event() {
        wxCommandEvent evt(wxEVT_SLIDER, GetId());
        evt.SetEventObject(this);
        ProcessWindowEvent(evt);
    }

    void on_paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        wxSize sz = GetClientSize();
        // Erase entire background to prevent ghosting
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        int track_h = FromDIP(3);
        int track_y = (sz.y - track_h) / 2;
        int thumb_d = FromDIP(10);
        int track_x = FromDIP(5);
        int track_w = std::max(4, sz.x - thumb_d);

        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#EBEBEB"))));
        dc.DrawRoundedRectangle(track_x, track_y, track_w, track_h, FromDIP(2));

        float frac = (float)(m_value - m_min) / (float)std::max(1, m_max - m_min);
        int active_w = std::max(0, (int)(track_w * frac));
        if (active_w > 0) {
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#009688"))));
            dc.DrawRoundedRectangle(track_x, track_y, active_w, track_h, FromDIP(2));
        }

        int thumb_x = track_x + active_w - thumb_d / 2;
        thumb_x = std::clamp(thumb_x, track_x - thumb_d/2, track_x + track_w - thumb_d/2);
        int thumb_y = (sz.y - thumb_d) / 2;
        dc.SetBrush(wxBrush(wxColour("#FFFFFF")));
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#009688")), FromDIP(1)));
        dc.DrawEllipse(thumb_x, thumb_y, thumb_d, thumb_d);
    }

    void on_left_down(wxMouseEvent& e) {
        m_dragging = true;
        if (!HasCapture()) CaptureMouse();
        int v = value_from_x(e.GetX());
        v = std::clamp(v, m_min, m_max);
        if (v != m_value) { m_value = v; Refresh(); fire_event(); }
    }

    void on_left_up(wxMouseEvent&) {
        if (m_dragging) { m_dragging = false; if (HasCapture()) ReleaseMouse(); }
    }

    void on_motion(wxMouseEvent& e) {
        if (!m_dragging) return;
        int v = value_from_x(e.GetX());
        v = std::clamp(v, m_min, m_max);
        if (v != m_value) { m_value = v; Refresh(); fire_event(); }
    }

    void on_capture_lost(wxMouseCaptureLostEvent&) { m_dragging = false; }
};

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

SnapmakerMixedFilamentDialog::SnapmakerMixedFilamentDialog(wxWindow* parent,
                                     const std::vector<std::string>& filament_colours)
    : DPIDialog(parent, wxID_ANY, _L("Add Mix"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_filament_colours(filament_colours)
{
    m_result.component_a   = 1;
    m_result.component_b   = 2;
    m_result.mix_b_percent = 50;
    build_ui();
}

SnapmakerMixedFilamentDialog::SnapmakerMixedFilamentDialog(wxWindow* parent,
                                     const std::vector<std::string>& filament_colours,
                                     const Slic3r::MixedFilament& existing)
    : DPIDialog(parent, wxID_ANY, _L("Edit Mix"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_filament_colours(filament_colours)
    , m_result(existing)
{
    // Use saved UI mode when available (new format with cm token).
    // Fall back to heuristics for legacy rows without cm token.
    if (existing.ui_mode >= 0 && existing.ui_mode <= 3) {
        m_current_mode = existing.ui_mode;
    } else if (existing.gradient_enabled) {
        m_current_mode = MODE_GRADIENT;
    } else if (!MixedFilamentManager::normalize_manual_pattern(existing.manual_pattern).empty()) {
        m_current_mode = MODE_CYCLE;
    } else if (!existing.gradient_component_ids.empty() &&
             existing.distribution_mode == int(MixedFilament::Simple)) {
        m_current_mode = MODE_MATCH;
    } else if (!existing.gradient_component_ids.empty() &&
             existing.distribution_mode == int(MixedFilament::LayerCycle)) {
        // Legacy heuristic: distinguish match 3-color from ratio 3-color.
        // Fall back to MODE_MATCH when ratio_a/ratio_b are both ≤ 1.
        if (existing.ratio_a <= 1 && existing.ratio_b <= 1)
            m_current_mode = MODE_MATCH;
        else
            m_current_mode = MODE_RATIO;
    } else {
        m_current_mode = MODE_RATIO;
    }

    // Determine gradient direction from existing configuration
    // Direction 0: A→B (component_a starts dominant, transitions to component_b)
    //              Implemented as: gradient_start > gradient_end (e.g., 0.8 → 0.2)
    // Direction 1: B→A (component_b starts dominant, transitions to component_a)
    //              Implemented as: gradient_start < gradient_end (e.g., 0.2 → 0.8)
    m_gradient_direction = (existing.gradient_start >= existing.gradient_end) ? 0 : 1;

    // Restore tri-picker weights from slash-separated weight string
    if (m_current_mode == MODE_RATIO && !existing.gradient_component_weights.empty()) {
        std::vector<int> vals;
        const char *p = existing.gradient_component_weights.c_str();
        while (*p) {
            char *end;
            int v = (int)std::strtol(p, &end, 10);
            if (end == p) break;
            vals.push_back(v);
            p = end;
            if (*p == '/') ++p;
        }
        if (vals.size() >= 3) {
            int total = 0;
            for (int v : vals) total += v;
            if (total > 0) {
                m_tri_wx = vals[0] / (double)total;
                m_tri_wy = vals[1] / (double)total;
                m_tri_wz = vals[2] / (double)total;
                // Clamp each weight to be at least 10% and at most 90%
                constexpr double MIN_WEIGHT = 0.10;
                constexpr double MAX_WEIGHT = 0.90;
                m_tri_wx = std::clamp(m_tri_wx, MIN_WEIGHT, MAX_WEIGHT);
                m_tri_wy = std::clamp(m_tri_wy, MIN_WEIGHT, MAX_WEIGHT);
                m_tri_wz = std::clamp(m_tri_wz, MIN_WEIGHT, MAX_WEIGHT);
                // Renormalize after clamping
                double sum = m_tri_wx + m_tri_wy + m_tri_wz;
                if (sum > 0) { m_tri_wx /= sum; m_tri_wy /= sum; m_tri_wz /= sum; }
            }
        }
    }

    build_ui();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

wxBitmap SnapmakerMixedFilamentDialog::make_color_bitmap(const wxColour& c, int size)
{
    wxBitmap bmp(size, size);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(c));
    dc.Clear();
    return bmp;
}

int SnapmakerMixedFilamentDialog::max_filaments_for_mode(int mode) const
{
    if (mode == MODE_RATIO)    return 3;
    if (mode == MODE_GRADIENT) return 2;
    return 4;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::build_ui()
{
    m_mode_btn_selected = m_current_mode;
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));

    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    const int M = FromDIP(8);
    const int V_GAP = FromDIP(16);

    // ---- Segmented mode buttons (Figma: white 380×60 panel, inner #F8F7F7 segment bg, tabs 80×28, 4px gap) ----
    {
        // Outer white panel (#FFFFFF bg, bottom border #F0F0F0, px=20 py=12)
        auto* mode_outer = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        mode_outer->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        auto* mode_outer_sizer = new wxBoxSizer(wxVERTICAL);

        // Inner segment container (#F8F7F7 bg, 36px height, 4px radius)
        m_mode_btn_container = new StaticBox(mode_outer, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_mode_btn_container->SetCornerRadius(FromDIP(4));
        m_mode_btn_container->SetBorderWidth(0);
        m_mode_btn_container->SetMinSize(wxSize(-1, FromDIP(36)));
        m_mode_btn_container->SetBackgroundColor(
            StateColor(std::pair(wxColour("#F8F7F7"), (int)StateColor::Normal)));
        auto* mode_sizer = new wxBoxSizer(wxHORIZONTAL);
        const std::vector<wxString> mode_names = { _L("Ratio"), _L("Cycle"), _L("Match"), _L("Gradient") };

        // Unselected: transparent (no bg) — container's #F8F7F7 shows through
        auto seg_bg  = StateColor();
        auto seg_fg  = StateColor(std::pair(wxColour("#4A4A4A"), (int)StateColor::Normal));
        auto sel_bg  = StateColor(std::pair(wxColour("#009688"), (int)StateColor::Normal));
        auto sel_fg  = StateColor(std::pair(wxColour("#FEFEFE"), (int)StateColor::Normal));

        for (int i = 0; i < 4; ++i) {
            auto* btn = new Button(m_mode_btn_container, mode_names[i]);
            btn->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
            btn->SetPaddingSize(wxSize(FromDIP(6), FromDIP(2)));
            btn->SetCornerRadius(FromDIP(4));
            btn->SetBorderWidth(0);
            btn->SetFont(Label::Body_12);
            if (i == (int)m_current_mode) {
                btn->SetBackgroundColor(sel_bg);
                btn->SetTextColor(sel_fg);
                btn->SetCanFocus(false);
            } else {
                btn->SetBackgroundColor(seg_bg);
                btn->SetTextColor(seg_fg);
            }
            btn->Bind(wxEVT_BUTTON, [this, i, seg_bg, seg_fg, sel_bg, sel_fg](wxCommandEvent&) {
                if (i == m_mode_btn_selected) return;
                if (m_mode_btn_selected >= 0 && m_mode_btn_selected < (int)m_mode_buttons.size()) {
                    m_mode_buttons[m_mode_btn_selected]->SetBackgroundColor(seg_bg);
                    m_mode_buttons[m_mode_btn_selected]->SetTextColor(seg_fg);
                    m_mode_buttons[m_mode_btn_selected]->SetCanFocus(true);
                }
                m_mode_btn_selected = i;
                m_mode_buttons[i]->SetBackgroundColor(sel_bg);
                m_mode_buttons[i]->SetTextColor(sel_fg);
                m_mode_buttons[i]->SetCanFocus(false);
                on_mode_changed(i);
            });
            m_mode_buttons.push_back(btn);
            mode_sizer->Add(btn, 1, wxEXPAND | wxALL, FromDIP(4));
        }
        m_mode_btn_container->SetSizer(mode_sizer);

        mode_outer_sizer->Add(m_mode_btn_container, 1, wxEXPAND | wxALL, FromDIP(12));
        mode_outer->SetSizer(mode_outer_sizer);

        // Bottom divider (Figma: #F0F0F0, 1px)
        auto* mode_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        mode_divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));

        top_sizer->Add(mode_outer, 0, wxEXPAND);
        top_sizer->Add(mode_divider, 0, wxEXPAND);
    }

    // Add/remove & swap buttons are created inside Card A (see filament card block below)

    // Error banner (red, blocks confirm)
    {
        m_error_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL);
        m_error_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FDE8E8")));
        m_error_panel->Hide();
        auto* err_sizer = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap error_bmp(m_error_panel, "error_icon_red_exclamation", 14);
        auto* error_icon = new wxStaticBitmap(m_error_panel, wxID_ANY, error_bmp.bmp());
        err_sizer->Add(error_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        err_sizer->AddSpacer(FromDIP(4));
        m_error_text = new Label(m_error_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_error_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D32F2F")));
        err_sizer->Add(m_error_text, 1, wxALL, FromDIP(8));
        m_error_panel->SetSizer(err_sizer);
        top_sizer->Add(m_error_panel, 0, wxEXPAND);
    }

    // Warning banner (orange, advisory, confirm stays enabled)
    {
        m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL);
        m_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFF3EB")));
        m_warning_panel->Hide();
        auto* warn_sizer = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap warn_bmp(m_warning_panel, "icon_warning_triangle", 14);
        auto* warn_icon = new wxStaticBitmap(m_warning_panel, wxID_ANY, warn_bmp.bmp());
        warn_sizer->Add(warn_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        warn_sizer->AddSpacer(FromDIP(4));
        m_warning_text = new Label(m_warning_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF842D")));
        warn_sizer->Add(m_warning_text, 1, wxALL, FromDIP(8));
        m_warning_panel->SetSizer(warn_sizer);
        top_sizer->Add(m_warning_panel, 0, wxEXPAND);
    }
    // ---- Scrolled content area ----
    m_scrolled_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
                                               wxSize(FromDIP(380), FromDIP(400)),
                                               wxVSCROLL | wxBORDER_NONE);
    m_scrolled_content->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    m_scrolled_content->SetScrollRate(0, FromDIP(8));
    // Prevent wxScrolledWindow from auto-scrolling to focused children.
    // Without this, clicking a partially-visible widget first scrolls it
    // into view instead of handling the click, requiring a second click.
    m_scrolled_content->Bind(wxEVT_CHILD_FOCUS, [](wxChildFocusEvent&) {
        // Do not evt.Skip() — that would invoke the default handler which scrolls.
    });
    auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);
    scroll_sizer->AddSpacer(FromDIP(16));

    // ======== Card 1: Match Input (match mode: filament badges + target color) ========
    {
        m_match_input_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_NONE);
        m_match_input_card->SetCornerRadius(FromDIP(4));
        m_match_input_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_match_input_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_match_input_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_match_input_card->SetBorderWidth(FromDIP(1));
        m_match_input_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        auto* card1_sizer = new wxBoxSizer(wxVERTICAL);

        auto* filament_label = new wxStaticText(m_match_input_card, wxID_ANY, _L("Filaments"));
        filament_label->SetFont(Label::Body_14);
        filament_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        filament_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        card1_sizer->Add(filament_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        card1_sizer->AddSpacer(FromDIP(12));

        m_match_badges_panel = new wxPanel(m_match_input_card, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_NONE);
        m_match_badges_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_badges_sizer = new wxWrapSizer(wxHORIZONTAL);
        MixedFilamentDisplayContext ctx;
        ctx.num_physical = m_filament_colours.size();
        ctx.physical_colors = m_filament_colours;
        for (int fid = 0; fid < (int)m_filament_colours.size(); ++fid) {
            MixedFilament mf;
            mf.display_color = m_filament_colours[fid];
            mf.custom = true;
            auto* badge = new MixedFilamentBadge(m_match_badges_panel, wxID_ANY, fid + 1,
                                                  mf, ctx, true, 20);
            badge->SetCanFocus(false);
            m_match_badges_sizer->Add(badge, 0, wxRIGHT | wxBOTTOM, FromDIP(12));
        }
        m_match_badges_panel->SetSizer(m_match_badges_sizer);
        card1_sizer->Add(m_match_badges_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // Target Color label
        auto* target_label = new wxStaticText(m_match_input_card, wxID_ANY, _L("Target Color"));
        target_label->SetFont(Label::Body_14);
        target_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        target_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        card1_sizer->Add(target_label, 0, wxLEFT | wxRIGHT, FromDIP(16));
        card1_sizer->AddSpacer(FromDIP(12));

        wxColour default_target("#26A69A");

        // Color panel + Hex input
        auto* target_row = new wxBoxSizer(wxHORIZONTAL);
        m_match_target_picker = new wxPanel(m_match_input_card, wxID_ANY, wxDefaultPosition,
                                             wxSize(FromDIP(108), FromDIP(24)));
        m_match_target_picker->SetBackgroundColour(default_target);
        m_match_target_picker->SetMinSize(wxSize(FromDIP(108), FromDIP(24)));
        m_match_target_picker->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            wxColourData data;
            data.SetColour(m_match_target_picker->GetBackgroundColour());
            wxColourDialog dlg(this, &data);
            if (dlg.ShowModal() == wxID_OK) {
                wxColour c = dlg.GetColourData().GetColour();
                if (c.IsOk()) {
                    m_match_target_picker->SetBackgroundColour(c);
                    m_match_target_picker->Refresh();
                    if (m_match_hex_input)
                        m_match_hex_input->ChangeValue(c.GetAsString(wxC2S_HTML_SYNTAX).Mid(1));
                    if (m_match_panel) m_match_panel->set_target_color(c);
                    if (m_match_hex_error) {
                        m_match_hex_error = false;
                        if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                        if (m_error_panel) m_error_panel->Hide();
                        if (m_btn_confirm) m_btn_confirm->Enable();
                        Layout();
                    }
                }
            }
        });
        target_row->Add(m_match_target_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

        auto* hex_wrapper = new wxPanel(m_match_input_card, wxID_ANY, wxDefaultPosition,
                                         wxSize(FromDIP(109), FromDIP(24)));
        m_match_hex_wrapper = hex_wrapper;
        hex_wrapper->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        hex_wrapper->SetMinSize(wxSize(FromDIP(109), FromDIP(24)));
        hex_wrapper->SetBackgroundStyle(wxBG_STYLE_PAINT);
        hex_wrapper->Bind(wxEVT_PAINT, [this](wxPaintEvent& evt) {
            wxWindow* w = dynamic_cast<wxWindow*>(evt.GetEventObject());
            if (!w) return;
            wxPaintDC dc(w);
            wxSize sz = w->GetClientSize();
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(m_match_hex_error ? StateColor::darkModeColorFor(wxColour("#FF0000")) : StateColor::darkModeColorFor(wxColour("#009688")), 1));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        });
        auto* hex_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto* hex_label = new wxStaticText(hex_wrapper, wxID_ANY, "Hex:");
        hex_label->SetFont(Label::Body_12);
        hex_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
        hex_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        hex_sizer->Add(hex_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(9));
        hex_sizer->AddSpacer(FromDIP(4));

        auto* hash_label = new wxStaticText(hex_wrapper, wxID_ANY, "#");
        hash_label->SetFont(Label::Body_12);
        hash_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
        hash_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        hex_sizer->Add(hash_label, 0, wxALIGN_CENTER_VERTICAL);
        hex_sizer->AddSpacer(FromDIP(2));

        m_match_hex_input = new wxTextCtrl(hex_wrapper, wxID_ANY,
                                           default_target.GetAsString(wxC2S_HTML_SYNTAX).Mid(1),
                                           wxDefaultPosition, wxSize(FromDIP(52), -1),
                                           wxTE_PROCESS_ENTER | wxBORDER_NONE);
        m_match_hex_input->SetFont(Label::Body_12);
        m_match_hex_input->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        m_match_hex_input->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_hex_input->SetMaxLength(6);
        // Prevent wxEVT_TEXT_ENTER from propagating to dialog default button
        m_match_hex_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            wxString val = m_match_hex_input->GetValue();
            val.Trim(true).Trim(false);
            wxColour parsed;
            if (try_parse_color_match_hex(val, parsed)) {
                m_match_hex_error = false;
                if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                if (m_error_panel) m_error_panel->Hide();
                if (m_btn_confirm) m_btn_confirm->Enable();
                Layout();
                if (m_match_target_picker) { m_match_target_picker->SetBackgroundColour(parsed); m_match_target_picker->Refresh(); }
                if (m_match_panel) m_match_panel->set_target_color(parsed);
            } else {
                m_match_hex_error = true;
                if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                set_error(_L("Please enter a valid 6-digit Hex value."));
            }
        });
        m_match_hex_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (m_match_hex_error) {
                m_match_hex_error = false;
                if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                if (m_error_panel) m_error_panel->Hide();
                if (m_btn_confirm) m_btn_confirm->Enable();
                Layout();
            }
        });
        hex_sizer->Add(m_match_hex_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(9));
        hex_wrapper->SetSizer(hex_sizer);
        target_row->Add(hex_wrapper, 0, wxALIGN_CENTER_VERTICAL);

        card1_sizer->Add(target_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_match_input_card->SetSizer(card1_sizer);
        scroll_sizer->Add(m_match_input_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }
    // ======== Card A: Filament Selection ========
    {
        m_filament_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxBORDER_NONE);
        m_filament_card->SetCornerRadius(FromDIP(4));
        m_filament_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_filament_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_filament_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_filament_card->SetBorderWidth(FromDIP(1));
        m_filament_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_filament_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title row with add/remove/swap buttons
        auto* card_title_row = new wxBoxSizer(wxHORIZONTAL);
        m_filament_card_title = new wxStaticText(m_filament_card, wxID_ANY, _L("Filament Selection"));
        m_filament_card_title->SetFont(Label::Body_14);
        m_filament_card_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        m_filament_card_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        card_title_row->Add(m_filament_card_title, 1, wxALIGN_CENTER_VERTICAL);

        m_btn_swap_gradient_dir = new ScalableButton(m_filament_card, wxID_ANY, "reverse_arrow");
        m_btn_swap_gradient_dir->SetToolTip(_L("Swap filaments"));
        m_btn_swap_gradient_dir->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
        m_btn_swap_gradient_dir->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_filament_rows.size() < 2) return;
            std::vector<int> selections;
            for (int i = 0; i < m_filament_rows.size(); ++i)
                selections.push_back(get_filament_index((int)i));
            std::swap(selections[0], selections[1]);
            m_gradient_direction = 0;
            rebuild_all_combos_with_selections(selections);
            update_preview();
            update_compatibility_warning();
        });
        card_title_row->Add(m_btn_swap_gradient_dir, 0, wxALIGN_CENTER_VERTICAL);

        // Remove filament button
        m_btn_remove_filament = new ScalableButton(m_filament_card, wxID_ANY, "icon_minus");
        m_btn_remove_filament->SetToolTip(_L("Remove last filament"));
        m_btn_remove_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            sync_rows_to_result();
            int new_count = std::max(2, (int)m_filament_rows.size() - 1);
            if (new_count == 2) {
                m_tri_wx = 1.0/3.0; m_tri_wy = 1.0/3.0; m_tri_wz = 1.0/3.0;
            }
            resize_gradient_ids(new_count);
            wxWeakRef<wxWindow> weak_self(this);
            CallAfter([weak_self]() {
                if (!weak_self) return;
                auto* self = static_cast<SnapmakerMixedFilamentDialog*>(weak_self.get());
                self->rebuild_filament_rows();
                self->update_compatibility_warning();
                self->Layout(); self->Fit();
            });
        });
        card_title_row->Add(m_btn_remove_filament, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        // Add filament button
        m_btn_add_filament = new ScalableButton(m_filament_card, wxID_ANY, "icon_plus");
        m_btn_add_filament->SetToolTip(_L("Add one filament"));
        m_btn_add_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            sync_rows_to_result();
            resize_gradient_ids((int)m_filament_rows.size() + 1);
            wxWeakRef<wxWindow> weak_self(this);
            CallAfter([weak_self]() {
                if (!weak_self) return;
                auto* self = static_cast<SnapmakerMixedFilamentDialog*>(weak_self.get());
                self->rebuild_filament_rows();
                self->update_compatibility_warning();
                self->Layout(); self->Fit();
            });
        });
        card_title_row->Add(m_btn_add_filament, 0, wxALIGN_CENTER_VERTICAL);
        m_filament_card_sizer->Add(card_title_row, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(16));

        m_filament_card_sizer->AddSpacer(FromDIP(12));

        // Filament rows panel
        m_filament_rows_panel = new wxPanel(m_filament_card);
        m_filament_rows_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_filament_rows_sizer = new wxBoxSizer(wxVERTICAL);
        m_filament_rows_panel->SetSizer(m_filament_rows_sizer);
        m_filament_card_sizer->Add(m_filament_rows_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_filament_card->SetSizer(m_filament_card_sizer);
        m_filament_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_filament_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Card B: Mix Ratio (Ratio mode) ========
    {
        m_ratio_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxBORDER_NONE);
        m_ratio_card->SetCornerRadius(FromDIP(4));
        m_ratio_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_ratio_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_ratio_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_ratio_card->SetBorderWidth(FromDIP(1));
        m_ratio_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_ratio_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title
        auto* ratio_title = new wxStaticText(m_ratio_card, wxID_ANY, _L("Mixing Ratio"));
        ratio_title->SetFont(Label::Body_14);
        ratio_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        ratio_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_ratio_card_sizer->Add(ratio_title, 0, wxTOP | wxLEFT | wxRIGHT, FromDIP(16));

        // Compute initial values
        int initial_val = m_result.mix_b_percent;
        wxColour col_a = (!m_filament_colours.empty())
            ? parse_mixed_color(m_filament_colours[std::max(0, (int)m_result.component_a - 1)])
            : wxColour(128, 128, 128);
        wxColour col_b = (m_filament_colours.size() > 1)
            ? parse_mixed_color(m_filament_colours[std::max(0, (int)m_result.component_b - 1)])
            : col_a;

        // ---- Gradient bar (2-color) ----
        m_ratio_gradient_spacer = m_ratio_card_sizer->AddSpacer(FromDIP(16));
        m_gradient_selector = new MixedGradientSelector(m_ratio_card, col_a, col_b, initial_val);
        m_ratio_card_sizer->Add(m_gradient_selector, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // ---- Tri picker (3-color) ----
        m_ratio_tri_spacer = m_ratio_card_sizer->AddSpacer(FromDIP(12));
        build_tri_picker(m_ratio_card);
        m_ratio_card_sizer->Add(m_tri_picker, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT, FromDIP(16));

        // ---- Legend panel (dynamic swatches + percentage labels) ----
        m_ratio_card_sizer->AddSpacer(FromDIP(6));
        m_legend_panel = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_legend_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_legend_panel->SetSizer(m_legend_sizer);
        m_ratio_card_sizer->Add(m_legend_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // ---- Divider ----
        {
            m_ratio_card_sizer->AddSpacer(FromDIP(6));
            auto* divider = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            m_ratio_card_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
            m_ratio_card_sizer->AddSpacer(FromDIP(6));
        }

        // ---- Strip preview panel ----
        m_strip_panel = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_strip_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
        m_strip_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_strip_panel);
            draw_strip(dc, m_strip_panel);
        });

        // ---- Dual-column preview: stripe (left) + blend (right) ----
        {
            auto* dual_preview_row = new wxBoxSizer(wxHORIZONTAL);

            // Left: preview label + stripe
            auto* left_col = new wxBoxSizer(wxVERTICAL);
            auto* preview_lbl = new wxStaticText(m_ratio_card, wxID_ANY, _L("Preview"));
            preview_lbl->SetFont(Label::Body_12);
            preview_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            preview_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            left_col->Add(preview_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            left_col->Add(m_strip_panel, 1, wxEXPAND);
            dual_preview_row->Add(left_col, 1, wxEXPAND | wxRIGHT, FromDIP(8));

            // Right: blend result label + blend color panel
            auto* right_col = new wxBoxSizer(wxVERTICAL);
            auto* blend_lbl = new wxStaticText(m_ratio_card, wxID_ANY, _L("Mix Effect"));
            blend_lbl->SetFont(Label::Body_12);
            blend_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            blend_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            right_col->Add(blend_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            m_preview_blend_panel = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_preview_blend_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
            m_preview_blend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_preview_blend_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_preview_blend_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_preview_blend_panel);
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
                wxSize sz = m_preview_blend_panel->GetClientSize();
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
                dc.DrawRectangle(0, 0, sz.x, sz.y);
            });
            right_col->Add(m_preview_blend_panel, 1, wxEXPAND);
            dual_preview_row->Add(right_col, 1, wxEXPAND);

            m_ratio_card_sizer->Add(dual_preview_row, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }

        m_ratio_card->SetSizer(m_ratio_card_sizer);
        m_ratio_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_ratio_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Match Mix Ratio Card (match mode's own card) ========
    {
        m_match_ratio_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_NONE);
        m_match_ratio_card->SetCornerRadius(FromDIP(4));
        m_match_ratio_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_match_ratio_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_match_ratio_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_match_ratio_card->SetBorderWidth(FromDIP(1));
        m_match_ratio_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_match_ratio_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title: 混合比例 (Figma: 14px Medium)
        auto* match_title = new wxStaticText(m_match_ratio_card, wxID_ANY, _L("Mixing Ratio"));
        match_title->SetFont(Label::Body_14);
        match_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        match_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_ratio_card_sizer->Add(match_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        wxColour def_ca = (!m_filament_colours.empty())
            ? parse_mixed_color(m_filament_colours[0]) : wxColour(128,128,128);
        wxColour def_cb = (m_filament_colours.size() > 1)
            ? parse_mixed_color(m_filament_colours[1]) : def_ca;
        m_match_gradient_selector = new MixedGradientSelector(m_match_ratio_card, def_ca, def_cb, 50);
        m_match_gradient_selector->set_min_max(m_match_min_pct, 100 - m_match_min_pct);
        m_match_gradient_spacer = m_match_ratio_card_sizer->AddSpacer(FromDIP(16));
        m_match_ratio_card_sizer->Add(m_match_gradient_selector, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        m_match_tri_spacer = m_match_ratio_card_sizer->AddSpacer(FromDIP(12));
        build_match_tri_picker(m_match_ratio_card);
        m_match_ratio_card_sizer->Add(m_match_tri_picker, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT, FromDIP(16));

        // Legend panel (gap-[6px] below tri-picker per Figma)
        m_match_ratio_card_sizer->AddSpacer(FromDIP(6));
        m_match_legend_panel = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_match_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_legend_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_match_legend_panel->SetSizer(m_match_legend_sizer);
        m_match_ratio_card_sizer->Add(m_match_legend_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // Range slider row: 最低混色比例
        {
            m_match_range_row = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_match_range_row->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            auto* range_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto* range_label = new wxStaticText(m_match_range_row, wxID_ANY, _L("Min Mix Ratio"));
            range_label->SetFont(Label::Body_14);
            range_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            range_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            range_sizer->Add(range_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            m_match_range_slider = new MatchRangeSlider(m_match_range_row, m_match_min_pct, 0, 50);
            range_sizer->Add(m_match_range_slider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            m_match_range_value = new wxStaticText(m_match_range_row, wxID_ANY,
                                                    wxString::Format("%d%%", m_match_min_pct));
            m_match_range_value->SetFont(Label::Body_12);
            m_match_range_value->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            m_match_range_value->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            range_sizer->Add(m_match_range_value, 0, wxALIGN_CENTER_VERTICAL);
            range_sizer->AddStretchSpacer();
            m_match_range_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
                const int new_min = std::clamp(m_match_range_slider->value(), 0, 50);
                m_match_range_value->SetLabel(wxString::Format("%d%%", new_min));
                if (m_match_gradient_selector)
                    m_match_gradient_selector->set_min_max(new_min, 100 - new_min);
                bool visibility_changed = false;
                for (int w : m_swatch_min_weights) {
                    if ((w >= m_match_min_pct) != (w >= new_min)) {
                        visibility_changed = true;
                        break;
                    }
                }
                m_match_min_pct = new_min;
                if (visibility_changed)
                    rebuild_swatch_sizer();
                if (m_match_panel)
                    m_match_panel->set_min_component_percent(new_min);
            });
            m_match_range_row->SetSizer(range_sizer);
            m_match_ratio_card_sizer->Add(m_match_range_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        }

        // Divider
        {
            m_match_ratio_card_sizer->AddSpacer(FromDIP(16));
            auto* divider = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            m_match_ratio_card_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
            m_match_ratio_card_sizer->AddSpacer(FromDIP(6));
        }

        // Preview section
        m_match_strip_panel = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_match_strip_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
        m_match_strip_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_match_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_match_strip_panel);
            draw_strip(dc, m_match_strip_panel);
        });

        {
            auto* dual_preview_row = new wxBoxSizer(wxHORIZONTAL);
            auto* left_col = new wxBoxSizer(wxVERTICAL);
            auto* preview_lbl = new wxStaticText(m_match_ratio_card, wxID_ANY, _L("Preview"));
            preview_lbl->SetFont(Label::Body_12);
            preview_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            preview_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            left_col->Add(preview_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            left_col->Add(m_match_strip_panel, 0, wxEXPAND);
            dual_preview_row->Add(left_col, 0, wxEXPAND | wxRIGHT, FromDIP(8));

            auto* right_col = new wxBoxSizer(wxVERTICAL);
            auto* blend_lbl = new wxStaticText(m_match_ratio_card, wxID_ANY, _L("Mix Effect"));
            blend_lbl->SetFont(Label::Body_12);
            blend_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            blend_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            right_col->Add(blend_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            m_match_blend_panel = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_match_blend_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
            m_match_blend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_match_blend_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_match_blend_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_match_blend_panel);
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
                wxSize sz = m_match_blend_panel->GetClientSize();
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
                dc.DrawRectangle(0, 0, sz.x, sz.y);
            });
            right_col->Add(m_match_blend_panel, 0, wxEXPAND);
            dual_preview_row->Add(right_col, 0, wxEXPAND);

            m_match_ratio_card_sizer->Add(dual_preview_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }

        m_match_gradient_selector->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            // Sync tri weights to slider for strip preview + legend
            int val = std::clamp(m_match_gradient_selector->value(), m_match_min_pct, 100 - m_match_min_pct);
            m_match_tri_wx = (100 - val) / 100.0;
            m_match_tri_wy = val / 100.0;
            m_match_tri_weights = {m_match_tri_wx, m_match_tri_wy};
            update_match_legend_labels();
            if (m_match_strip_panel)   m_match_strip_panel->Refresh();
            if (m_match_blend_panel)   m_match_blend_panel->Refresh();
            update_compatibility_warning();
        });

        m_match_ratio_card->SetSizer(m_match_ratio_card_sizer);
        scroll_sizer->Add(m_match_ratio_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Gradient Effect Card: 混色效果 (shown in gradient mode) ========
    {
        m_gradient_effect_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                                wxDefaultSize, wxBORDER_NONE);
        m_gradient_effect_card->SetCornerRadius(FromDIP(4));
        m_gradient_effect_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_gradient_effect_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_gradient_effect_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_gradient_effect_card->SetBorderWidth(FromDIP(1));
        m_gradient_effect_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_gradient_effect_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Mix Effect title (same font as Blended Color)
        auto* effect_title = new wxStaticText(m_gradient_effect_card, wxID_ANY, _L("Mix Effect"));
        effect_title->SetFont(Label::Body_14);
        effect_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        effect_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_gradient_effect_card_sizer->Add(effect_title, 0, wxTOP | wxLEFT | wxRIGHT, FromDIP(16));
        m_gradient_effect_card_sizer->AddSpacer(FromDIP(8));

        // Gradient preview panel
        m_preview_panel = new wxPanel(m_gradient_effect_card, wxID_ANY, wxDefaultPosition,
                                      wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
        m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
        m_preview_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_preview_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_preview_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_preview_panel);
            if (m_current_mode == MODE_GRADIENT && m_filament_rows.size() >= 2) {
                wxSize sz = m_preview_panel->GetClientSize();
                int ia = std::max(0, std::min(get_filament_index(0), (int)m_filament_colours.size()-1));
                int ib = std::max(0, std::min(get_filament_index(1), (int)m_filament_colours.size()-1));
                wxColour ca = parse_mixed_color(m_filament_colours[ia]);
                wxColour cb = parse_mixed_color(m_filament_colours[ib]);
                if (m_gradient_direction != 0)
                    std::swap(ca, cb);
                wxImage img(sz.GetWidth(), sz.GetHeight());
                unsigned char* data = img.GetData();
                // Vertical gradient: top = cb-last, bottom = ca-first
                // (t=1.0 at top→pure cb, t=0 at bottom→pure ca, matching gradient_start/end direction 0)
                for (int y = 0; y < sz.GetHeight(); ++y) {
                    float t = (sz.GetHeight() > 1) ? 1.0f - float(y) / float(sz.GetHeight() - 1) : 0.5f;
                    wxColour c = blend_pair_filament_mixer(ca, cb, t);
                    for (int x = 0; x < sz.GetWidth(); ++x) {
                        int idx = (y * sz.GetWidth() + x) * 3;
                        data[idx]   = c.Red();
                        data[idx+1] = c.Green();
                        data[idx+2] = c.Blue();
                    }
                }
                dc.DrawBitmap(wxBitmap(img), 0, 0, false);
            } else {
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
            }
            wxSize sz = m_preview_panel->GetClientSize();
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        });
        m_gradient_effect_card_sizer->Add(m_preview_panel, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_gradient_effect_card->SetSizer(m_gradient_effect_card_sizer);
        m_gradient_effect_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_gradient_effect_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ---- Match mode panel ----
    {
        m_match_panel_sizer = new wxBoxSizer(wxVERTICAL);
        wxColour initial = (m_current_mode == MODE_MATCH && !m_result.display_color.empty())
            ? parse_mixed_color(m_result.display_color)
            : StateColor::darkModeColorFor(wxColour("#26A69A"));
        m_match_panel = new MixedColorMatchPanel(m_scrolled_content, m_filament_colours, initial);
        m_match_panel_sizer->Add(m_match_panel, 0, wxEXPAND | wxALL, M);
        scroll_sizer->Add(m_match_panel_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Card: Cycle Pattern ========
    {
        m_cycle_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxBORDER_NONE);
        m_cycle_card->SetCornerRadius(FromDIP(4));
        m_cycle_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_cycle_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_cycle_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_cycle_card->SetBorderWidth(FromDIP(1));
        m_cycle_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_cycle_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title
        auto* cycle_title = new wxStaticText(m_cycle_card, wxID_ANY, _L("Filaments"));
        cycle_title->SetFont(Label::Body_14);
        cycle_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        cycle_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_cycle_card_sizer->Add(cycle_title, 0, wxALL, FromDIP(16));

        // Filament quick buttons (20×20 badges, wrapping)
        {
            MixedFilamentDisplayContext ctx;
            ctx.num_physical = m_filament_colours.size();
            ctx.physical_colors = m_filament_colours;

            auto* btn_row = new wxWrapSizer(wxHORIZONTAL);
            for (int fid = 0; fid < (int)m_filament_colours.size(); ++fid) {
                MixedFilament mf;
                mf.display_color = m_filament_colours[fid];
                mf.custom = true;

                auto* badge = new MixedFilamentBadge(m_cycle_card, wxID_ANY, fid + 1, mf, ctx, true, 20);
                badge->SetToolTip(wxString::Format(_L("Append filament %d to pattern"), fid + 1));
                badge->Bind(wxEVT_BUTTON, [this, fid](wxCommandEvent&) {
                    if (m_pattern_ctrl) {
                        if (fid + 1 >= 10) {
                            m_pattern_ctrl->AppendText(wxString::Format("[%d]", fid + 1));
                        } else {
                            m_pattern_ctrl->AppendText(wxString::Format("%d", fid + 1));
                        }
                        validate_cycle_pattern();
                    }
                });
                btn_row->Add(badge, 0, wxRIGHT | wxBOTTOM, FromDIP(8));
                m_pattern_quick_buttons.push_back(badge);
            }
            m_cycle_card_sizer->Add(btn_row, 0, wxLEFT | wxRIGHT, FromDIP(16));
            m_cycle_card_sizer->AddSpacer(FromDIP(12));
        }

        // Pattern input (Figma: 293×30, #F0F0F0 1px border)
        {
            const std::string init_pattern = MixedFilamentManager::normalize_manual_pattern(
                m_current_mode == MODE_CYCLE ? m_result.manual_pattern : std::string());
            // StaticBox wrapper for exact #F0F0F0 border color
            auto* input_wrapper = new StaticBox(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            input_wrapper->SetCornerRadius(0);
            input_wrapper->SetBorderWidth(FromDIP(1));
            input_wrapper->SetBorderColor(
                StateColor(std::pair(wxColour("#F0F0F0"), (int)StateColor::Normal)));
            input_wrapper->SetBackgroundColor(
                StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
            auto* wrapper_sizer = new wxBoxSizer(wxHORIZONTAL);
            m_pattern_ctrl = new wxTextCtrl(input_wrapper, wxID_ANY,
                                            from_u8(init_pattern.empty() ? "12" : init_pattern),
                                            wxDefaultPosition, wxDefaultSize,
                                            wxTE_PROCESS_ENTER | wxBORDER_NONE);
            m_pattern_ctrl->SetFont(Label::Body_14);
            m_pattern_ctrl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_pattern_ctrl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            m_pattern_ctrl->SetMargins(FromDIP(8), FromDIP(8));
            input_wrapper->SetMinSize(wxSize(FromDIP(293), FromDIP(30)));
            wrapper_sizer->Add(m_pattern_ctrl, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(1));
            input_wrapper->SetSizer(wrapper_sizer);
            m_pattern_ctrl->SetToolTip(_L("Allowed Input: Only digits, square brackets ([ and ]), and commas (,)."));
            m_pattern_ctrl->SetMaxLength(512);

            m_pattern_ctrl->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
                validate_cycle_pattern();
                event.Skip();
            });
            m_pattern_ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
                validate_cycle_pattern();
            });
            m_cycle_card_sizer->Add(input_wrapper, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        }

        // Divider
        {
            auto* divider = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            m_cycle_card_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, M);
        }

        // Dual-column preview: stripe (left) + blend (right)
        {
            auto* dual_preview_row = new wxBoxSizer(wxHORIZONTAL);

            // Left: preview label + stripe
            auto* left_col = new wxBoxSizer(wxVERTICAL);
            auto* preview_lbl = new wxStaticText(m_cycle_card, wxID_ANY, _L("Preview"));
            preview_lbl->SetFont(Label::Body_12);
            preview_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            preview_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            left_col->Add(preview_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));

            m_cycle_strip_panel = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_cycle_strip_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(140)));
            m_cycle_strip_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_cycle_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_cycle_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_cycle_strip_panel);
                draw_strip(dc, m_cycle_strip_panel);
            });
            left_col->Add(m_cycle_strip_panel, 1, wxEXPAND);
            dual_preview_row->Add(left_col, 1, wxEXPAND | wxRIGHT, FromDIP(8));

            // Right: blend effect label + blend color panel
            auto* right_col = new wxBoxSizer(wxVERTICAL);
            auto* blend_lbl = new wxStaticText(m_cycle_card, wxID_ANY, _L("Mix Effect"));
            blend_lbl->SetFont(Label::Body_12);
            blend_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            blend_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            right_col->Add(blend_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));

            m_cycle_blend_panel = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_cycle_blend_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(140)));
            m_cycle_blend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_cycle_blend_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_cycle_blend_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_cycle_blend_panel);
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
                wxSize sz = m_cycle_blend_panel->GetClientSize();
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
                dc.DrawRectangle(0, 0, sz.x, sz.y);
            });
            right_col->Add(m_cycle_blend_panel, 1, wxEXPAND);
            dual_preview_row->Add(right_col, 1, wxEXPAND);

            m_cycle_card_sizer->Add(dual_preview_row, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }

        // Legend panel (dynamic swatches + percentage labels)
        m_cycle_legend_panel = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_cycle_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_cycle_legend_sizer = new wxFlexGridSizer(5, FromDIP(12), FromDIP(6));
        m_cycle_legend_panel->SetSizer(m_cycle_legend_sizer);
        m_cycle_card_sizer->Add(m_cycle_legend_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_cycle_card->SetSizer(m_cycle_card_sizer);
        m_cycle_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_cycle_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Card C: Recommended Swatches ========
    {
        m_swatch_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, wxBORDER_NONE);
        m_swatch_card->SetCornerRadius(FromDIP(4));
        m_swatch_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_swatch_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_swatch_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_swatch_card->SetBorderWidth(FromDIP(1));
        m_swatch_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_swatch_card_sizer = new wxBoxSizer(wxVERTICAL);

        auto* swatch_title = new wxStaticText(m_swatch_card, wxID_ANY, _L("Mixing Recommendations"));
        swatch_title->SetFont(Label::Body_14);
        swatch_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        swatch_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_swatch_card_sizer->Add(swatch_title, 0, wxALL, FromDIP(16));

        m_swatch_grid_panel = new wxPanel(m_swatch_card, wxID_ANY, wxDefaultPosition,
                                          wxDefaultSize);
        m_swatch_grid_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        build_swatch_grid();
        m_swatch_card_sizer->Add(m_swatch_grid_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_swatch_card->SetSizer(m_swatch_card_sizer);
        m_swatch_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_swatch_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    m_scrolled_content->SetSizer(scroll_sizer);
    top_sizer->Add(m_scrolled_content, 1, wxEXPAND);

    // ---- Bottom button panel: white bg + #F0F0F0 top border, padding 13/12/20 ----
    {
        auto* btn_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        btn_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

        auto* panel_sizer = new wxBoxSizer(wxVERTICAL);

        // #F0F0F0 top border (1px)
        auto* top_line = new wxPanel(btn_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        top_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
        panel_sizer->Add(top_line, 0, wxEXPAND);

        // 13px top padding - 1px line = 12px remaining top padding
        panel_sizer->AddSpacer(FromDIP(12));

        auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

        m_btn_cancel  = new Button(btn_panel, _L("Cancel"));
        m_btn_cancel->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_cancel->SetCornerRadius(FromDIP(4));
        m_btn_cancel->SetBorderWidth(FromDIP(1));
        m_btn_cancel->SetBorderColorNormal(wxColour("#D1D5DC"));
        m_btn_cancel->SetBackgroundColorNormal(wxColour("#FFFFFF"));
        m_btn_cancel->SetTextColorNormal(wxColour("#242424"));

        m_btn_confirm = new Button(btn_panel, _L("OK"));
        m_btn_confirm->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_confirm->SetCornerRadius(FromDIP(4));
        m_btn_confirm->SetBorderWidth(0);
        m_btn_confirm->SetBackgroundColor(StateColor(
            std::pair(wxColour("#DFDFDF"), (int)StateColor::Disabled),
            std::pair(wxColour("#019687"), (int)StateColor::Normal)));
        m_btn_confirm->SetTextColor(StateColor(
            std::pair(wxColour("#6B6A6A"), (int)StateColor::Disabled),
            std::pair(wxColour("#FEFEFE"), (int)StateColor::Normal)));

        btn_sizer->Add(m_btn_cancel,  1, wxRIGHT, FromDIP(8));
        btn_sizer->Add(m_btn_confirm, 1);
        panel_sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

        // 12px bottom padding
        panel_sizer->AddSpacer(FromDIP(12));

        btn_panel->SetSizer(panel_sizer);
        top_sizer->Add(btn_panel, 0, wxEXPAND);
    }

    SetSizer(top_sizer);
    SetMinClientSize(wxSize(FromDIP(380), FromDIP(666)));
    SetMaxClientSize(wxSize(FromDIP(380), FromDIP(666)));

    // Initialize match state before rebuild (avoids gradient/tri flicker)
    on_mode_changed(m_current_mode);

    rebuild_filament_rows();
    update_compatibility_warning();

    // Bind slider events
    m_gradient_selector->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        int val = m_gradient_selector->value();
        update_legend_text();
        if (m_preview_panel) m_preview_panel->Refresh();
        if (m_preview_blend_panel) m_preview_blend_panel->Refresh();
        if (m_strip_panel)   m_strip_panel->Refresh();
        update_compatibility_warning();
    });

    // --- Match-mode event wiring (does not affect ratio/cycle/gradient) ---
    if (m_match_panel) {
        m_match_panel->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            if (m_current_mode != MODE_MATCH) return;
            auto recipe = m_match_panel->selected_recipe();
            if (!recipe.valid) return;

            int num_physical = (int)m_filament_colours.size();
            auto weights = expand_color_match_recipe_weights(recipe, num_physical);

            std::vector<std::pair<int, int>> sorted;
            for (int i = 0; i < (int)weights.size(); ++i)
                if (weights[i] > 0) sorted.push_back({i, weights[i]});
            std::sort(sorted.begin(), sorted.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });

            // Use actual recipe filaments (no padding) — visibility switches based on count
            if ((int)sorted.size() >= 3) {
                sorted.resize(3);
                m_match_tri_indices = {sorted[0].first, sorted[1].first, sorted[2].first};
                int total = sorted[0].second + sorted[1].second + sorted[2].second;
                if (total > 0) {
                    m_match_tri_wx = sorted[0].second / (double)total;
                    m_match_tri_wy = sorted[1].second / (double)total;
                    m_match_tri_wz = sorted[2].second / (double)total;
                }
                m_match_tri_weights = {m_match_tri_wx, m_match_tri_wy, m_match_tri_wz};
            } else if ((int)sorted.size() >= 2) {
                if (sorted.size() >= 2) {
                    m_match_tri_indices = {sorted[0].first, sorted[1].first};
                    if (m_match_gradient_selector) {
                        int total = sorted[0].second + sorted[1].second;
                        int b_pct = (total > 0) ? (sorted[1].second * 100 / total) : 50;
                        m_match_gradient_selector->set_value(b_pct);
                    }
                    m_match_tri_weights = {sorted[0].second / 100.0, sorted[1].second / 100.0};
                }
            }

            if (m_match_gradient_selector && m_match_tri_indices.size() >= 2) {
                int ia = std::max(0, std::min(m_match_tri_indices[0], (int)m_filament_colours.size() - 1));
                int ib = std::max(0, std::min(m_match_tri_indices[1], (int)m_filament_colours.size() - 1));
                m_match_gradient_selector->set_colors(
                    parse_mixed_color(m_filament_colours[ia]),
                    parse_mixed_color(m_filament_colours[ib]));
            }

            rebuild_match_legend();
            update_ratio_or_tri_visibility();  // switch gradient/tri based on recipe filament count
            if (m_match_tri_picker)  m_match_tri_picker->Refresh();
            if (m_match_strip_panel) m_match_strip_panel->Refresh();
            if (m_match_blend_panel) m_match_blend_panel->Refresh();
            update_compatibility_warning();
        });
    }

    // --- End of match-mode event wiring ---

    m_btn_cancel->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_current_mode == MODE_CYCLE) {
            validate_cycle_pattern();
            if (!m_btn_confirm->IsEnabled()) return;
        }
        collect_result();
        EndModal(wxID_OK);
    });

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

    Fit();
    CentreOnParent();

    m_scrolled_content->FitInside();
    m_scrolled_content->Scroll(0, 0);

}

// ---------------------------------------------------------------------------
// Barycentric triangle utilities
// ---------------------------------------------------------------------------

struct TriPt { double x, y; };

static double tri_area2(TriPt a, TriPt b, TriPt c)
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

static void tri_bary(TriPt p, TriPt v0, TriPt v1, TriPt v2,
                     double& w0, double& w1, double& w2)
{
    double total = tri_area2(v0, v1, v2);
    if (std::abs(total) < 1e-9) { w0 = w1 = w2 = 1.0 / 3.0; return; }
    w0 = tri_area2(p, v1, v2) / total;
    w1 = tri_area2(v0, p, v2) / total;
    w2 = 1.0 - w0 - w1;
}

static TriPt tri_clamp_pt(TriPt p, TriPt v0, TriPt v1, TriPt v2)
{
    double w0, w1, w2;
    tri_bary(p, v0, v1, v2, w0, w1, w2);
    return { w0 * v0.x + w1 * v1.x + w2 * v2.x,
             w0 * v0.y + w1 * v1.y + w2 * v2.y };
}

// ---------------------------------------------------------------------------

int SnapmakerMixedFilamentDialog::get_filament_index(int row_idx) const
{
    if (row_idx < 0 || row_idx >= (int)m_filament_rows.size()) return 0;
    const auto& row = m_filament_rows[row_idx];
    int cb_idx = row.combo->GetSelection();
    if (cb_idx < 0 || cb_idx >= (int)row.filament_indices.size()) return 0;
    return row.filament_indices[cb_idx];
}

// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::populate_combo(ComboBox* cb, const std::set<int>& exclude_ids, int select_id,
                                         std::vector<int>& out_filament_indices)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const std::vector<std::string>& filament_presets = preset_bundle ? preset_bundle->filament_presets : std::vector<std::string>();

    cb->Clear();
    out_filament_indices.clear();

    int select_idx = -1;
    for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
        if (exclude_ids.find(j) != exclude_ids.end()) continue;

        wxString display_name;
        if (preset_bundle && j < (int)filament_presets.size()) {
            const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[j]);
            if (preset) display_name = from_u8(preset->label(false));
        }
        if (display_name.empty()) display_name = wxString::Format("F%d", j + 1);

        wxBitmap* badge_icon = get_extruder_color_icon(m_filament_colours[j], std::to_string(j + 1), FromDIP(20), FromDIP(20));
        cb->Append(display_name, badge_icon ? badge_icon->ConvertToImage() : wxNullImage);

        if (j == select_id) select_idx = out_filament_indices.size();
        out_filament_indices.push_back(j);
    }

    if (select_idx >= 0) cb->SetSelection(select_idx);
}

// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::refresh_all_combos()
{
    for (size_t i = 0; i < m_filament_rows.size(); ++i) {
        std::set<int> used_by_others;
        for (size_t k = 0; k < m_filament_rows.size(); ++k)
            if (k != i) used_by_others.insert(get_filament_index((int)k));

        populate_combo(m_filament_rows[i].combo, used_by_others, get_filament_index((int)i),
                       m_filament_rows[i].filament_indices);
    }

    for (size_t i = 0; i < m_filament_rows.size(); ++i)
        set_combo_combined_icon(m_filament_rows[i].combo, get_filament_index((int)i));
}

// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::rebuild_all_combos_with_selections(const std::vector<int>& selections)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const std::vector<std::string>& filament_presets = preset_bundle ? preset_bundle->filament_presets : std::vector<std::string>();

    for (size_t i = 0; i < m_filament_rows.size(); ++i) {
        auto& row = m_filament_rows[i];
        ComboBox* cb = row.combo;

        // Build set of filaments already used by other rows
        std::set<int> used_by_others;
        for (size_t k = 0; k < m_filament_rows.size(); ++k) {
            if (k != i && k < selections.size()) {
                used_by_others.insert(selections[k]);
            }
        }

        // Clear and rebuild combo box
        cb->Clear();
        row.filament_indices.clear();

        for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
            // Skip filaments already used by other rows
            if (used_by_others.find(j) != used_by_others.end()) continue;

            wxString display_name;
            if (preset_bundle && j < (int)filament_presets.size()) {
                const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[j]);
                if (preset) {
                    display_name = from_u8(preset->label(false));
                }
            }
            if (display_name.empty()) {
                display_name = wxString::Format("F%d", j + 1);
            }
            wxBitmap* icon = get_extruder_color_icon(m_filament_colours[j], std::to_string(j + 1), FromDIP(20), FromDIP(20));
            cb->Append(display_name, icon ? icon->ConvertToImage() : wxNullImage);
            row.filament_indices.push_back(j);
        }

        // Restore selection from the provided selections vector
        int sel = (i < selections.size()) ? selections[i] : 0;
        for (int k = 0; k < (int)row.filament_indices.size(); ++k) {
            if (row.filament_indices[k] == sel) {
                cb->SetSelection(k);
                break;
            }
        }
    }

    for (size_t i = 0; i < m_filament_rows.size(); ++i)
        set_combo_combined_icon(m_filament_rows[i].combo, get_filament_index((int)i));
}

// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::set_combo_combined_icon(ComboBox* cb, int filament_idx)
{
    if (!cb || filament_idx < 0 || filament_idx >= (int)m_filament_colours.size()) return;
    const int pad = FromDIP(8), arr_w = FromDIP(8), badge_w = FromDIP(20), h = FromDIP(20), gap = FromDIP(8), text_gap = FromDIP(8);
    const int total_w = pad + arr_w + gap + badge_w + text_gap;
    // Transparent background via wxImage alpha
    wxImage img(total_w, h, true);
    img.InitAlpha();
    memset(img.GetAlpha(), 0, total_w * h);

    auto set_rgba = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        if (x < 0 || x >= total_w || y < 0 || y >= h) return;
        int pos = y * total_w + x;
        img.GetData()[pos * 3] = r;
        img.GetData()[pos * 3 + 1] = g;
        img.GetData()[pos * 3 + 2] = b;
        img.GetAlpha()[pos] = a;
    };

    // Arrow: paste SVG (transparent background, only arrow pixels opaque)
    ScalableBitmap ab(cb, "drop_down", arr_w);
    if (ab.bmp().IsOk()) {
        wxImage aimg = ab.bmp().ConvertToImage();
        if (!aimg.HasAlpha()) aimg.InitAlpha();
        int ax = pad, ay = (h - aimg.GetHeight()) / 2;
        for (int y = 0; y < aimg.GetHeight() && ay + y < h; ++y)
            for (int x = 0; x < aimg.GetWidth() && ax + x < total_w; ++x) {
                unsigned char* s = aimg.GetData() + (y * aimg.GetWidth() + x) * 3;
                unsigned char a = aimg.HasAlpha() ? *(aimg.GetAlpha() + y * aimg.GetWidth() + x) : 255;
                if (a > 0) set_rgba(ax + x, ay + y, s[0], s[1], s[2], a);
            }
    }

    // Badge: use get_extruder_color_icon (16×16 with number, opaque)
    const int bx = pad + arr_w + gap;
    wxBitmap* badge_bmp = get_extruder_color_icon(m_filament_colours[filament_idx],
        std::to_string(filament_idx + 1), FromDIP(20), FromDIP(20));
    if (badge_bmp) {
        wxImage bimg = badge_bmp->ConvertToImage();
        int by = (h - bimg.GetHeight()) / 2;
        for (int y = 0; y < bimg.GetHeight() && by + y < h; ++y)
            for (int x = 0; x < bimg.GetWidth() && bx + x < total_w; ++x) {
                unsigned char* s = bimg.GetData() + (y * bimg.GetWidth() + x) * 3;
                set_rgba(bx + x, by + y, s[0], s[1], s[2], 255);
            }
    }

    cb->SetIcon(wxBitmap(img));
    // SetIcon triggers Rescale→messureSize which recalculates height; re-lock to 30
    cb->SetMinSize(wxSize(-1, FromDIP(30)));
    cb->SetMaxSize(wxSize(-1, FromDIP(30)));
}

// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::rebuild_legend()
{
    if (!m_legend_sizer) return;

    m_legend_sizer->Clear(true);
    m_legend_labels.clear();

    int n = (int)m_filament_rows.size();
    if (n < 2) return;

    // Compute weights
    std::vector<int> weights(n, 0);
    if (n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        weights[0] = 100 - val;
        weights[1] = val;
    } else if (n == 3) {
        weights[0] = (int)(m_tri_wx * 100 + 0.5);
        weights[1] = (int)(m_tri_wy * 100 + 0.5);
        weights[2] = 100 - weights[0] - weights[1];
        if (weights[2] < 0) weights[2] = 0;
    } else {
        for (int i = 0; i < n; ++i)
            weights[i] = 100 / n;
    }

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = m_filament_colours.size();
    ctx.physical_colors = m_filament_colours;

    for (int i = 0; i < n; ++i) {
        int idx = get_filament_index(i);
        idx = std::max(0, std::min(idx, (int)m_filament_colours.size() - 1));

        // Badge + label pair
        auto* pair = new wxBoxSizer(wxHORIZONTAL);

        MixedFilament mf;
        mf.display_color = m_filament_colours[idx];
        mf.custom = true;
        auto* badge = new MixedFilamentBadge(m_legend_panel, wxID_ANY, idx + 1, mf, ctx, true, 12);
        pair->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        auto* lbl = new wxStaticText(m_legend_panel, wxID_ANY, wxString::Format("%d%%", weights[i]));
        lbl->SetFont(Label::Body_12);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        pair->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        m_legend_labels.push_back(lbl);

        m_legend_sizer->Add(pair, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    }

    m_legend_panel->Layout();
}

void SnapmakerMixedFilamentDialog::rebuild_match_legend()
{
    if (!m_match_legend_sizer) return;
    m_match_legend_sizer->Clear(true);
    m_match_legend_labels.clear();
    if (m_match_tri_indices.empty()) return;

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = m_filament_colours.size();
    ctx.physical_colors = m_filament_colours;

    int num_physical = (int)m_filament_colours.size();
    std::vector<int> weights;
    // Always use tri_weights (same source as compute_preview_color / draw_strip)
    for (double w : m_match_tri_weights)
        weights.push_back((int)(w * 100 + 0.5));

    int total = 0;
    for (int w : weights) total += w;
    if (total > 0) {
        for (size_t i = 0; i < weights.size(); ++i)
            weights[i] = weights[i] * 100 / total;
    }

    for (size_t i = 0; i < m_match_tri_indices.size(); ++i) {
        int fid = m_match_tri_indices[i];
        fid = std::max(0, std::min(fid, (int)m_filament_colours.size() - 1));
        auto* pair = new wxBoxSizer(wxHORIZONTAL);
        MixedFilament mf;
        mf.display_color = m_filament_colours[fid];
        mf.custom = true;
        auto* badge = new MixedFilamentBadge(m_match_legend_panel, wxID_ANY, fid + 1, mf, ctx, true, 12);
        pair->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        int pct = (i < weights.size()) ? weights[i] : 0;
        auto* lbl = new wxStaticText(m_match_legend_panel, wxID_ANY, wxString::Format("%d%%", pct));
        lbl->SetFont(Label::Body_12);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        pair->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        m_match_legend_labels.push_back(lbl);
        m_match_legend_sizer->Add(pair, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    }
    m_match_legend_panel->Layout();
}

void SnapmakerMixedFilamentDialog::update_match_legend_labels()
{
    if (m_match_legend_labels.empty() || m_match_tri_indices.empty()) return;

    std::vector<int> weights;
    int nf = (int)m_match_tri_indices.size();
    if (nf >= 3) {
        weights = {(int)(m_match_tri_wx * 100 + 0.5), (int)(m_match_tri_wy * 100 + 0.5),
                   (int)(m_match_tri_wz * 100 + 0.5)};
    } else {
        for (double w : m_match_tri_weights)
            weights.push_back((int)(w * 100 + 0.5));
    }

    int total = 0;
    for (int w : weights) total += w;
    if (total > 0) {
        for (size_t i = 0; i < weights.size(); ++i)
            weights[i] = weights[i] * 100 / total;
    }

    for (size_t i = 0; i < m_match_legend_labels.size() && i < weights.size(); ++i)
        m_match_legend_labels[i]->SetLabel(wxString::Format("%d%%", weights[i]));
}

// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::update_legend_text()
{
    if (m_legend_labels.empty()) return;

    int n = (int)m_filament_rows.size();
    if ((int)m_legend_labels.size() != n) return;

    // Compute weights (same logic as rebuild_legend, no widget recreation)
    std::vector<int> weights(n, 0);
    if (n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        weights[0] = 100 - val;
        weights[1] = val;
    } else if (n == 3) {
        weights[0] = (int)(m_tri_wx * 100 + 0.5);
        weights[1] = (int)(m_tri_wy * 100 + 0.5);
        weights[2] = 100 - weights[0] - weights[1];
        if (weights[2] < 0) weights[2] = 0;
    } else {
        for (int i = 0; i < n; ++i)
            weights[i] = 100 / n;
    }

    for (int i = 0; i < n; ++i)
        m_legend_labels[i]->SetLabel(wxString::Format("%d%%", weights[i]));
}

// ---------------------------------------------------------------------------

void SnapmakerMixedFilamentDialog::rebuild_filament_rows()
{
    m_filament_rows_sizer->Clear(true);
    m_filament_rows.clear();

    const int max_idx = std::max(0, (int)m_filament_colours.size() - 1);
    std::vector<int> sels;

    // Decode gradient_component_ids (supports both legacy single-char and extended /-separated formats)
    std::vector<unsigned int> decoded_ids = MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids);

    // Detect "all-ids" format (match recipe): gradient_component_ids contains
    // component_a as its first entry, meaning all filaments are listed there.
    const bool all_ids_format = !decoded_ids.empty() &&
        decoded_ids[0] == m_result.component_a;

    if (all_ids_format) {
        for (unsigned int id : decoded_ids)
            sels.push_back(std::clamp(int(id - 1), 0, max_idx));
    } else {
        sels.push_back(std::clamp((int)m_result.component_a - 1, 0, max_idx));
        sels.push_back(std::clamp((int)m_result.component_b - 1, 0, max_idx));
        for (unsigned int id : decoded_ids) {
            sels.push_back(std::clamp(int(id - 1), 0, max_idx));
        }
    }

    int count = (int)sels.size();
    if (m_current_mode == MODE_GRADIENT)
        count = 2;
    else
        count = std::max(2, std::min(count, max_filaments_for_mode(m_current_mode)));

    for (int i = 0; i < count; ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* row_lbl = new wxStaticText(m_filament_rows_panel, wxID_ANY,
                                         wxString::Format(_L("Filament %d"), i + 1),
                                         wxDefaultPosition, wxSize(FromDIP(50), -1));
        row_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        row_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        row->Add(row_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

        // Note: sw (color swatch panel) is kept for internal state tracking but hidden
        // The color is now shown inside the ComboBox via get_extruder_color_icon
        auto* sw = new wxPanel(m_filament_rows_panel, wxID_ANY, wxDefaultPosition,
                               wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetMinSize(wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetBackgroundStyle(wxBG_STYLE_PAINT);
        sw->Hide();  // Hide the external swatch, color is shown in ComboBox

        auto* cb = new ComboBox(m_filament_rows_panel, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(FromDIP(200), FromDIP(30)),
                                0, nullptr, wxCB_READONLY);
        cb->SetMinSize(wxSize(-1, FromDIP(30)));
        cb->SetMaxSize(wxSize(-1, FromDIP(30)));

        // Build set of filaments already used by other rows (based on sels array)
        std::set<int> used_by_others;
        for (int k = 0; k < count; ++k) {
            if (k != i && k < (int)sels.size())
                used_by_others.insert(sels[k]);
        }

        int sel = (i < (int)sels.size()) ? sels[i] : i;
        sel = std::max(0, std::min(sel, (int)m_filament_colours.size() - 1));

        FilamentRow fr;
        fr.swatch = sw;
        fr.combo = cb;

        populate_combo(cb, used_by_others, sel, fr.filament_indices);

        wxColour init_col = parse_mixed_color(m_filament_colours[sel]);
        sw->Bind(wxEVT_PAINT, [sw, init_col](wxPaintEvent&) mutable {
            wxAutoBufferedPaintDC dc(sw);
            dc.SetBackground(wxBrush(init_col));
            dc.Clear();
        });

        // Add to m_filament_rows before binding so we can use row index
        m_filament_rows.push_back(fr);
        int row_idx = (int)m_filament_rows.size() - 1;

        cb->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            sync_rows_to_result();
            refresh_all_combos();
            rebuild_legend();
            if (m_tri_picker) m_tri_picker->Refresh();
            update_preview();
            update_compatibility_warning();
        });

        row->Add(cb, 1, wxEXPAND);

        m_filament_rows_sizer->Add(row, 0, wxEXPAND | (i < count - 1 ? wxBOTTOM : 0), FromDIP(12));
    }

    m_filament_rows_panel->Layout();
    update_ratio_or_tri_visibility();
    rebuild_legend();

    if (m_swatch_grid_panel) {
        m_swatch_grid_panel->DestroyChildren();
        build_swatch_grid();
    }

    // Refresh card containers so backgrounds repaint after child changes
    if (m_filament_card) {
        m_filament_card->Layout();
        m_filament_card->Refresh();
    }
    if (m_ratio_card) {
        m_ratio_card->Layout();
        m_ratio_card->Refresh();
    }

    // Set combined arrow+icon for newly created combos
    for (size_t i = 0; i < m_filament_rows.size(); ++i)
        set_combo_combined_icon(m_filament_rows[i].combo, get_filament_index((int)i));

    update_preview();
}

void SnapmakerMixedFilamentDialog::build_tri_picker(wxWindow* parent)
{
    if (!parent) parent = this;
    static constexpr int TRI_SIZE = 160;
    m_tri_picker = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                               wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_tri_picker->SetMinSize(wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_tri_picker->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto get_verts = [this]() -> std::tuple<TriPt, TriPt, TriPt> {
        wxSize sz = m_tri_picker->GetClientSize();
        double pw = sz.GetWidth(), ph = sz.GetHeight();
        double margin = FromDIP(3);
        double side = std::min(pw, ph) - 2 * margin;
        double tri_h = side * std::sqrt(3.0) / 2.0;
        double cx = pw / 2.0;
        double top_y = (ph - tri_h) / 2.0;
        double bot_y = top_y + tri_h;
        return { {cx, top_y}, {cx - side / 2.0, bot_y}, {cx + side / 2.0, bot_y} };
    };

    auto safe_col = [&](int row) -> wxColour {
        if (row >= (int)m_filament_rows.size()) return wxColour(128,128,128);
        int s = get_filament_index(row);
        s = std::max(0, std::min(s, (int)m_filament_colours.size()-1));
        return parse_mixed_color(m_filament_colours[s]);
    };

    m_tri_picker->Bind(wxEVT_PAINT, [this, get_verts, safe_col](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_tri_picker);
        wxSize sz = m_tri_picker->GetClientSize();
        auto [v0, v1, v2] = get_verts();

        wxColour c0 = safe_col(0), c1 = safe_col(1), c2 = safe_col(2);

        int min_y = (int)std::min({v0.y, v1.y, v2.y});
        int max_y = (int)std::max({v0.y, v1.y, v2.y});
        int min_x = (int)std::min({v0.x, v1.x, v2.x});
        int max_x = (int)std::max({v0.x, v1.x, v2.x});

        // Create image for gradient rendering (more efficient than DrawPoint)
        const int img_w = sz.GetWidth();
        const int img_h = sz.GetHeight();
        wxImage img(img_w, img_h);
        
        // Fill with background color matching card
        const wxColour bg = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        img.SetRGB(wxRect(0, 0, img_w, img_h), bg.Red(), bg.Green(), bg.Blue());
        unsigned char* data = img.GetData();

        // Draw gradient only inside triangle
        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                TriPt p = {(double)px, (double)py};
                double w0, w1, w2;
                tri_bary(p, v0, v1, v2, w0, w1, w2);
                // Skip points outside triangle (negative barycentric coordinates)
                constexpr double EPSILON = 0.001;
                if (w0 < -EPSILON || w1 < -EPSILON || w2 < -EPSILON) continue;
                if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;

                const int idx = (py * img_w + px) * 3;
                data[idx]     = (unsigned char)std::clamp((int)(c0.Red()   * w0 + c1.Red()   * w1 + c2.Red()   * w2), 0, 255);
                data[idx + 1] = (unsigned char)std::clamp((int)(c0.Green() * w0 + c1.Green() * w1 + c2.Green() * w2), 0, 255);
                data[idx + 2] = (unsigned char)std::clamp((int)(c0.Blue()  * w0 + c1.Blue()  * w1 + c2.Blue()  * w2), 0, 255);
            }
        }

        // Draw the image
        wxBitmap bmp(img);
        dc.DrawBitmap(bmp, 0, 0);

        // Draw triangle outline (#D9D9D9 per Figma)
        wxPoint pts[3] = {{(int)v0.x, (int)v0.y}, {(int)v1.x, (int)v1.y}, {(int)v2.x, (int)v2.y}};
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawPolygon(3, pts);

        // Draw colored circles at each vertex (6×6, #D9D9D9 border per Figma)
        const int dot_r = FromDIP(3);
        wxPen dot_border(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1);
        dc.SetPen(dot_border);
        dc.SetBrush(wxBrush(c0));
        dc.DrawCircle(wxPoint((int)v0.x, (int)v0.y), dot_r);
        dc.SetBrush(wxBrush(c1));
        dc.DrawCircle(wxPoint((int)v1.x, (int)v1.y), dot_r);
        dc.SetBrush(wxBrush(c2));
        dc.DrawCircle(wxPoint((int)v2.x, (int)v2.y), dot_r);

        // Draw selection circle (transparent fill, white border per Figma)
        double hx = m_tri_wx*v0.x + m_tri_wy*v1.x + m_tri_wz*v2.x;
        double hy = m_tri_wx*v0.y + m_tri_wy*v1.y + m_tri_wz*v2.y;
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#FFFFFF")), 1));
        dc.DrawCircle((int)hx, (int)hy, FromDIP(5));

    });

    auto handle_mouse = [this, get_verts](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_verts();
        TriPt p = {(double)e.GetX(), (double)e.GetY()};
        if (is_down) {
            m_tri_dragging = true;
            if (!m_tri_picker->HasCapture()) m_tri_picker->CaptureMouse();
        }
        if (!m_tri_dragging) return;
        TriPt clamped = tri_clamp_pt(p, v0, v1, v2);
        tri_bary(clamped, v0, v1, v2, m_tri_wx, m_tri_wy, m_tri_wz);
        // Clamp each weight to be at least 10% and at most 90%, iterate to renormalize
        constexpr double MIN_WEIGHT = 0.10;
        constexpr double MAX_WEIGHT = 0.90;
        for (int i = 0; i < 4; ++i) {
            m_tri_wx = std::clamp(m_tri_wx, MIN_WEIGHT, MAX_WEIGHT);
            m_tri_wy = std::clamp(m_tri_wy, MIN_WEIGHT, MAX_WEIGHT);
            m_tri_wz = std::clamp(m_tri_wz, MIN_WEIGHT, MAX_WEIGHT);
            double sum = m_tri_wx + m_tri_wy + m_tri_wz;
            if (sum > 0) { m_tri_wx /= sum; m_tri_wy /= sum; m_tri_wz /= sum; }
        }
        update_preview();
        m_tri_picker->Refresh();
    };

    m_tri_picker->Bind(wxEVT_LEFT_DOWN, [handle_mouse](wxMouseEvent& e) { handle_mouse(e, true); });
    m_tri_picker->Bind(wxEVT_MOTION,    [handle_mouse](wxMouseEvent& e) { handle_mouse(e, false); });
    m_tri_picker->Bind(wxEVT_LEFT_UP,   [this](wxMouseEvent&) {
        if (m_tri_dragging) {
            m_tri_dragging = false;
            if (m_tri_picker->HasCapture()) m_tri_picker->ReleaseMouse();
            update_compatibility_warning();
        }
    });
    m_tri_picker->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_tri_dragging = false;
    });
}

void SnapmakerMixedFilamentDialog::build_match_tri_picker(wxWindow* parent)
{
    static constexpr int TRI_SIZE = 160;
    m_match_tri_picker = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                                     wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_match_tri_picker->SetMinSize(wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_match_tri_picker->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto get_verts = [this]() -> std::tuple<TriPt, TriPt, TriPt> {
        wxSize sz = m_match_tri_picker->GetClientSize();
        double pw = sz.GetWidth(), ph = sz.GetHeight();
        double margin = FromDIP(3);
        double side = std::min(pw, ph) - 2 * margin;
        double tri_h = side * std::sqrt(3.0) / 2.0;
        double cx = pw / 2.0;
        double top_y = (ph - tri_h) / 2.0;
        double bot_y = top_y + tri_h;
        return { {cx, top_y}, {cx - side / 2.0, bot_y}, {cx + side / 2.0, bot_y} };
    };

    auto safe_col = [this](int row) -> wxColour {
        if (row >= (int)m_match_tri_indices.size()) return wxColour(128,128,128);
        int s = m_match_tri_indices[row];
        s = std::max(0, std::min(s, (int)m_filament_colours.size()-1));
        return parse_mixed_color(m_filament_colours[s]);
    };

    m_match_tri_picker->Bind(wxEVT_PAINT, [this, get_verts, safe_col](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_match_tri_picker);
        wxSize sz = m_match_tri_picker->GetClientSize();
        auto [v0, v1, v2] = get_verts();
        wxColour c0 = safe_col(0), c1 = safe_col(1), c2 = safe_col(2);

        int min_y = (int)std::min({v0.y, v1.y, v2.y});
        int max_y = (int)std::max({v0.y, v1.y, v2.y});
        int min_x = (int)std::min({v0.x, v1.x, v2.x});
        int max_x = (int)std::max({v0.x, v1.x, v2.x});

        const int img_w = sz.GetWidth(), img_h = sz.GetHeight();
        wxImage img(img_w, img_h);
        const wxColour bg = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        img.SetRGB(wxRect(0, 0, img_w, img_h), bg.Red(), bg.Green(), bg.Blue());
        unsigned char* data = img.GetData();

        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                TriPt p = {(double)px, (double)py};
                double w0, w1, w2;
                tri_bary(p, v0, v1, v2, w0, w1, w2);
                constexpr double EPSILON = 0.001;
                if (w0 < -EPSILON || w1 < -EPSILON || w2 < -EPSILON) continue;
                if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;
                const int idx = (py * img_w + px) * 3;
                data[idx]     = (unsigned char)std::clamp((int)(c0.Red()   * w0 + c1.Red()   * w1 + c2.Red()   * w2), 0, 255);
                data[idx + 1] = (unsigned char)std::clamp((int)(c0.Green() * w0 + c1.Green() * w1 + c2.Green() * w2), 0, 255);
                data[idx + 2] = (unsigned char)std::clamp((int)(c0.Blue()  * w0 + c1.Blue()  * w1 + c2.Blue()  * w2), 0, 255);
            }
        }
        wxBitmap bmp(img);
        dc.DrawBitmap(bmp, 0, 0);

        wxPoint pts[3] = {{(int)v0.x, (int)v0.y}, {(int)v1.x, (int)v1.y}, {(int)v2.x, (int)v2.y}};
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawPolygon(3, pts);

        // Colored circles at each vertex (6×6, #D9D9D9 border)
        const int dot_r = FromDIP(3);
        wxPen dot_border(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1);
        dc.SetPen(dot_border);
        dc.SetBrush(wxBrush(c0));
        dc.DrawCircle(wxPoint((int)v0.x, (int)v0.y), dot_r);
        dc.SetBrush(wxBrush(c1));
        dc.DrawCircle(wxPoint((int)v1.x, (int)v1.y), dot_r);
        dc.SetBrush(wxBrush(c2));
        dc.DrawCircle(wxPoint((int)v2.x, (int)v2.y), dot_r);

        // Selection circle (transparent fill, white border)
        double hx = m_match_tri_wx*v0.x + m_match_tri_wy*v1.x + m_match_tri_wz*v2.x;
        double hy = m_match_tri_wx*v0.y + m_match_tri_wy*v1.y + m_match_tri_wz*v2.y;
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#FFFFFF")), 1));
        dc.DrawCircle((int)hx, (int)hy, FromDIP(5));
    });

    auto handle_mouse = [this, get_verts](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_verts();
        TriPt p = {(double)e.GetX(), (double)e.GetY()};
        if (is_down) { m_match_tri_dragging = true; if (!m_match_tri_picker->HasCapture()) m_match_tri_picker->CaptureMouse(); }
        if (!m_match_tri_dragging) return;
        TriPt clamped = tri_clamp_pt(p, v0, v1, v2);
        tri_bary(clamped, v0, v1, v2, m_match_tri_wx, m_match_tri_wy, m_match_tri_wz);
        // Use dynamic min_component_percent (0-50%) instead of hard-coded 10%
        {
            const double min_w = std::max(0.0, m_match_min_pct / 100.0);
            const double max_w = 1.0 - 2.0 * min_w;  // each weight ≤ 100% - 2x min (leaves room for 2 others)
            m_match_tri_wx = std::clamp(m_match_tri_wx, min_w, max_w);
            m_match_tri_wy = std::clamp(m_match_tri_wy, min_w, max_w);
            m_match_tri_wz = std::clamp(m_match_tri_wz, min_w, max_w);
            double sum = m_match_tri_wx + m_match_tri_wy + m_match_tri_wz;
            if (sum > 0) { m_match_tri_wx /= sum; m_match_tri_wy /= sum; m_match_tri_wz /= sum; }
        }
        update_match_legend_labels();
        if (m_match_strip_panel) m_match_strip_panel->Refresh();
        if (m_match_blend_panel) m_match_blend_panel->Refresh();
        m_match_tri_picker->Refresh();
    };

    m_match_tri_picker->Bind(wxEVT_LEFT_DOWN, [handle_mouse](wxMouseEvent& e) { handle_mouse(e, true); });
    m_match_tri_picker->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_match_tri_dragging = false;
        if (m_match_tri_picker->HasCapture()) m_match_tri_picker->ReleaseMouse();
        update_compatibility_warning();
    });
    m_match_tri_picker->Bind(wxEVT_MOTION, [handle_mouse](wxMouseEvent& e) { handle_mouse(e, false); });
    m_match_tri_picker->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_match_tri_dragging = false;
    });
}

void SnapmakerMixedFilamentDialog::update_ratio_or_tri_visibility()
{
    bool is_ratio_mode = (m_current_mode == MODE_RATIO);
    bool is_cycle_mode = (m_current_mode == MODE_CYCLE);
    bool is_match_mode = (m_current_mode == MODE_MATCH);
    bool is_gradient_mode = (m_current_mode == MODE_GRADIENT);
    int  n             = (int)m_filament_rows.size();
    bool show_slider   = is_ratio_mode && (n == 2);
    bool show_tri      = is_ratio_mode && (n == 3);

    // Card A: Filament Selection card
    bool show_filament_rows = !is_match_mode && !is_cycle_mode;
    if (m_filament_card)      m_filament_card->Show(show_filament_rows);
    if (m_filament_rows_panel) m_filament_rows_panel->Show(show_filament_rows);

    // Card 1: Match input card (match mode only)
    if (m_match_input_card) m_match_input_card->Show(is_match_mode);

    // Card B: Ratio card — shown for both 2-color and 3-color ratio modes
    if (m_ratio_card) {
        bool show_ratio_card = is_ratio_mode && !is_match_mode && !is_gradient_mode;
        m_ratio_card->Show(show_ratio_card);
    }
    // Show/hide gradient bar vs tri-picker within card
    if (m_ratio_gradient_spacer) m_ratio_gradient_spacer->Show(show_slider);
    if (m_gradient_selector)     m_gradient_selector->Show(show_slider);
    if (m_ratio_tri_spacer)      m_ratio_tri_spacer->Show(show_tri);
    if (m_tri_picker)            m_tri_picker->Show(show_tri);
    // Gradient effect card (shown in gradient mode)
    if (m_gradient_effect_card)
        m_gradient_effect_card->Show(is_gradient_mode && !is_match_mode);

    // Match Mix Ratio card (match mode only, separate from ratio card)
    if (m_match_ratio_card) m_match_ratio_card->Show(is_match_mode);
    if (is_match_mode) {
        int n_tri = (int)m_match_tri_indices.size();
        bool match_show_slider = (n_tri == 2);
        bool match_show_tri    = (n_tri >= 3);
        if (m_match_gradient_spacer)   m_match_gradient_spacer->Show(match_show_slider);
        if (m_match_gradient_selector) m_match_gradient_selector->Show(match_show_slider);
        if (m_match_tri_spacer)        m_match_tri_spacer->Show(match_show_tri);
        if (m_match_tri_picker)        m_match_tri_picker->Show(match_show_tri);
    }

    // Cycle card
    if (m_cycle_card) m_cycle_card->Show(is_cycle_mode);

    // Match panel UI: never shown (backend only)
    if (m_match_panel_sizer) m_match_panel_sizer->ShowItems(false);

    // Card C: Swatches card
    bool show_swatches = !is_cycle_mode;
    if (m_swatch_card) m_swatch_card->Show(show_swatches);

    // Update add/remove button visibility
    bool can_remove = !is_match_mode && !is_gradient_mode && !is_cycle_mode && (n > 2);
    bool show_add = !is_match_mode && !is_gradient_mode && !is_cycle_mode && n < max_filaments_for_mode(m_current_mode);
    bool can_add = n < (int)m_filament_colours.size();
    if (m_btn_remove_filament) m_btn_remove_filament->Show(can_remove);
    if (m_btn_add_filament) {
        m_btn_add_filament->Show(show_add);
        m_btn_add_filament->Enable(can_add);
    }

    // Update filament card title visibility
    if (m_filament_card_title) m_filament_card_title->Show(show_filament_rows);

    const bool show_gradient_swap = is_gradient_mode && (n == 2);
    if (m_btn_swap_gradient_dir)
        m_btn_swap_gradient_dir->Show(show_gradient_swap);

    // Refresh match card layout when gradient/tri picker visibility changes
    if (is_match_mode && m_match_ratio_card) {
        m_match_ratio_card->Layout();
        m_match_ratio_card->Refresh();
        if (m_scrolled_content) m_scrolled_content->Layout();
        Layout();
        // Force badge panel repaint after re-layout prevents StaticBox bg from covering badges
        if (m_match_input_card) m_match_input_card->Refresh();
        if (m_match_badges_panel) m_match_badges_panel->Refresh();
    }
}

void SnapmakerMixedFilamentDialog::resize_gradient_ids(int target_count)
{
    int extra = target_count - 2;
    if (extra <= 0) { m_result.gradient_component_ids.clear(); return; }

    // Decode existing gradient IDs
    std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids);

    // Build set of already used filament IDs (1-based)
    std::set<unsigned int> used;
    used.insert(m_result.component_a);
    used.insert(m_result.component_b);
    for (unsigned int id : ids)
        used.insert(id);

    // Find unused filaments for new slots
    while ((int)ids.size() < extra) {
        unsigned int new_filament = 0;
        for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
            unsigned int fid = (unsigned int)(j + 1);
            if (used.find(fid) == used.end()) {
                new_filament = fid;
                used.insert(fid);
                break;
            }
        }
        if (new_filament >= 1)
            ids.push_back(new_filament);
        else
            break;  // No unused filament available, stop filling
    }
    ids.resize((size_t)extra);
    m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
}

void SnapmakerMixedFilamentDialog::sync_rows_to_result()
{
    if (m_filament_rows.empty()) return;
    int a = get_filament_index(0);
    int b = (m_filament_rows.size() > 1) ? get_filament_index(1) : a;
    m_result.component_a = (unsigned int)(std::max(0, a) + 1);
    m_result.component_b = (unsigned int)(std::max(0, b) + 1);
    std::vector<unsigned int> extra_ids;
    for (int i = 2; i < (int)m_filament_rows.size(); ++i) {
        int s = get_filament_index(i);
        if (s >= 0)
            extra_ids.push_back((unsigned int)(s + 1));
    }
    m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(extra_ids);
}

void SnapmakerMixedFilamentDialog::update_compatibility_warning()
{
    if (!m_error_panel || !m_error_text || !m_warning_panel || !m_warning_text || !m_btn_confirm)
        return;

    // Collect all filament IDs referenced by the current mix
    std::vector<unsigned int> fids;

    if (m_current_mode == MODE_MATCH) {
        if (m_match_panel && m_match_panel->has_valid_recipe()) {
            auto recipe = m_match_panel->selected_recipe();
            if (recipe.component_a >= 1) fids.push_back(recipe.component_a - 1);
            if (recipe.component_b >= 1) fids.push_back(recipe.component_b - 1);
            if (!recipe.gradient_component_ids.empty()) {
                for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids)) {
                    if (id >= 1) fids.push_back(id - 1);
                }
            }
        } else {
            for (int idx : m_match_tri_indices)
                fids.push_back((unsigned int)idx);
        }
    } else if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        sync_rows_to_result();
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(raw);
        auto parsed = parse_cycle_pattern(norm, (int)m_filament_colours.size());
        for (unsigned int id : parsed.ids) {
            unsigned int idx = id - 1;
            if (std::find(fids.begin(), fids.end(), idx) == fids.end())
                fids.push_back(idx);
        }
    } else {
        sync_rows_to_result();
        if (m_result.component_a >= 1) fids.push_back(m_result.component_a - 1);
        if (m_result.component_b >= 1) fids.push_back(m_result.component_b - 1);
        if (!m_result.gradient_component_ids.empty()) {
            for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids)) {
                if (id >= 1) fids.push_back(id - 1);
            }
        }
    }

    if (!is_filament_compatible(fids)) {
        if (auto pair = find_incompatible_filament_pair(fids)) {
            set_error(wxString::Format(_L("Filament %d and Filament %d cannot be mixed. Please select filaments of the same type."), pair->first, pair->second));
        } else {
            set_error(_L("Different filament types cannot be mixed. Please correct the settings."));
        }
    } else if (wxString warning_msg = get_ratio_warning_msg(); !warning_msg.empty()) {
        display_warning(warning_msg);
        if (m_btn_confirm) m_btn_confirm->Enable();
    } else if (m_current_mode == MODE_CYCLE && fids.size() == 1) {
        display_warning(_L("Same filament colors cannot produce new colors. Please select different colors for mixing."));
        if (m_btn_confirm) m_btn_confirm->Enable();
    } else if (m_current_mode == MODE_CYCLE && fids.size() > 4) {
        display_warning(_L("Excessive filaments in the mix may affect the result. Please use with caution."));
        if (m_btn_confirm) m_btn_confirm->Enable();
    } else {
        m_error_panel->Hide();
        m_warning_panel->Hide();
        m_btn_confirm->Enable();
    }
    Layout();
}

void SnapmakerMixedFilamentDialog::display_warning(const wxString& msg)
{
    if (!m_warning_panel || !m_warning_text || !m_error_panel)
        return;
    m_error_panel->Hide();
    m_warning_panel->Show();
    Layout(); // land panel width first, so the auto-wrap label's CalcMin gets a real width
    m_warning_text->SetLabel(msg);
    Layout(); // re-wrap with correct width; single Layout on macOS may calc height from stale zero-width
}

void SnapmakerMixedFilamentDialog::set_error(const wxString& msg)
{
    if (!m_error_panel || !m_error_text || !m_warning_panel)
        return;
    m_warning_panel->Hide();
    m_error_panel->Show();
    Layout(); // land panel width first, so the auto-wrap label's CalcMin gets a real width
    m_error_text->SetLabel(msg);
    if (m_btn_confirm) m_btn_confirm->Disable();
    Layout(); // re-wrap with correct width; single Layout on macOS may calc height from stale zero-width
}

wxString SnapmakerMixedFilamentDialog::get_ratio_warning_msg()
{
    static constexpr double HIGH_RATIO_THRESHOLD = 0.667;

    if (m_filament_rows.empty() || m_filament_colours.empty())
        return wxEmptyString;

    const int num_physical = (int)m_filament_colours.size();
    std::vector<double> ratios(num_physical, 0.0);
    double total = 0.0;

    switch (m_current_mode) {
    case MODE_RATIO:
        if (m_filament_rows.size() == 2 && m_gradient_selector) {
            int val = m_gradient_selector->value();
            int ia = get_filament_index(0);
            int ib = get_filament_index(1);
            if (ia < num_physical) { ratios[ia] = (100.0 - val) / 100.0; total += ratios[ia]; }
            if (ib < num_physical) { ratios[ib] = val / 100.0;        total += ratios[ib]; }
        } else if (m_filament_rows.size() == 3) {
            for (int i = 0; i < 3; ++i) {
                int idx = get_filament_index(i);
                if (idx < num_physical) {
                    double w = (i == 0) ? m_tri_wx : (i == 1) ? m_tri_wy : m_tri_wz;
                    ratios[idx] = w;
                    total += w;
                }
            }
        }
        break;
    case MODE_GRADIENT:
        if (m_filament_rows.size() >= 2) {
            int ia = get_filament_index(0);
            int ib = get_filament_index(1);
            if (ia < num_physical) { ratios[ia] = 0.5; total += 0.5; }
            if (ib < num_physical) { ratios[ib] = 0.5; total += 0.5; }
        }
        break;
    case MODE_MATCH:
        {
            int n_tri = (int)m_match_tri_indices.size();
            if (n_tri == 2) {
                double w0, w1;
                if (m_match_gradient_selector) {
                    int val = m_match_gradient_selector->value();
                    w0 = (100.0 - val) / 100.0;
                    w1 = val / 100.0;
                } else {
                    w0 = m_match_tri_wx;
                    w1 = m_match_tri_wy;
                }
                for (int i = 0; i < 2 && i < n_tri; ++i) {
                    int idx = std::clamp(m_match_tri_indices[i], 0, num_physical - 1);
                    double w = (i == 0) ? w0 : w1;
                    ratios[idx] = w;
                    total += w;
                }
            } else if (n_tri >= 3) {
                for (int i = 0; i < n_tri && i < 3; ++i) {
                    int idx = std::clamp(m_match_tri_indices[i], 0, num_physical - 1);
                    double w = (i == 0) ? m_match_tri_wx : (i == 1) ? m_match_tri_wy : m_match_tri_wz;
                    ratios[idx] = w;
                    total += w;
                }
            }
        }
        break;
    case MODE_CYCLE:
        return wxEmptyString;
    }

    if (total <= 0.0)
        return wxEmptyString;

    double max_ratio = 0.0;
    int    max_idx   = -1;
    for (int i = 0; i < num_physical; ++i) {
        double ratio = ratios[i] / total;
        if (ratio > max_ratio) {
            max_ratio = ratio;
            max_idx   = i;
        }
    }
    if (max_idx >= 0 && max_ratio > HIGH_RATIO_THRESHOLD) {
        return wxString::Format(_L("Filament %d ratio is too high. Mix may be affected."), max_idx + 1);

    }

    return wxEmptyString;
}

std::string SnapmakerMixedFilamentDialog::compute_preview_color()
{
    if (m_filament_colours.empty()) return "#808080";
    if (m_filament_rows.empty() && m_current_mode != MODE_MATCH) return "#808080";

    // Match mode: compute from current tri/gradient weights (during drag or no recipe)
    if (m_current_mode == MODE_MATCH && !m_match_tri_indices.empty()) {
        int nf = (int)m_match_tri_indices.size();
        if (nf == 2 && m_match_gradient_selector) {
            int val = m_match_gradient_selector->value();
            int ia = std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1);
            int ib = std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1);
            return MixedFilamentManager::blend_color(m_filament_colours[ia], m_filament_colours[ib], 100 - val, val);
        }
        if (nf >= 3) {
            wxColour c0 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1)]);
            wxColour c1 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1)]);
            wxColour c2 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[2], 0, (int)m_filament_colours.size() - 1)]);
            int r = (int)(c0.Red()*m_match_tri_wx + c1.Red()*m_match_tri_wy + c2.Red()*m_match_tri_wz + 0.5);
            int g = (int)(c0.Green()*m_match_tri_wx + c1.Green()*m_match_tri_wy + c2.Green()*m_match_tri_wz + 0.5);
            int b = (int)(c0.Blue()*m_match_tri_wx + c1.Blue()*m_match_tri_wy + c2.Blue()*m_match_tri_wz + 0.5);
            return wxString::Format("#%02X%02X%02X", std::clamp(r,0,255), std::clamp(g,0,255), std::clamp(b,0,255)).ToStdString();
        }
    }

    int n   = (int)m_filament_rows.size();
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;

    auto safe_idx = [&](int raw) {
        return std::max(0, std::min(raw, (int)m_filament_colours.size() - 1));
    };

    // Cycle mode: blend by pattern frequency using the same logic as Plater.cpp
    if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);
        if (!normalized.empty()) {
            const size_t num_physical = m_filament_colours.size();

            // In cycle mode, tokens "1"/"2" map directly to physical filaments 1/2,
            // and tokens "3".."9" map directly to physical filaments 3..9.
            std::vector<unsigned int> sequence;
            MixedFilament dummy_mf;
            dummy_mf.component_a = 1;
            dummy_mf.component_b = 2;
            const std::vector<std::string> group_strs = MixedFilamentManager::split_pattern_groups(normalized);
            for (const std::string &group : group_strs) {
                const std::vector<std::string> tokens =
                    MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
                for (const std::string &token : tokens) {
                    const unsigned int extruder_id =
                        MixedFilamentManager::physical_filament_from_token(token, dummy_mf, num_physical);
                    if (extruder_id >= 1 && extruder_id <= num_physical)
                        sequence.push_back(extruder_id);
                }
            }

            if (!sequence.empty()) {
                std::vector<wxColour> palette;
                palette.reserve(m_filament_colours.size());
                for (const auto& s : m_filament_colours)
                    palette.push_back(parse_mixed_color(s));
                wxColour result = blend_sequence_filament_mixer(palette, sequence);
                return wxString::Format("#%02X%02X%02X", result.Red(), result.Green(), result.Blue()).ToStdString();
            }
        }
    }

    if (n == 2) {
        int ia = safe_idx(get_filament_index(0));
        int ib = safe_idx(get_filament_index(1));
        int w = (m_current_mode == MODE_RATIO) ? val : 50;
        return MixedFilamentManager::blend_color(
            m_filament_colours[ia], m_filament_colours[ib], 100 - w, w);
    }

    // 3-filament ratio mode: linear weighted average matching tri picker rendering
    if (n == 3 && m_current_mode == MODE_RATIO) {
        auto get_col = [&](int row) {
            return parse_mixed_color(m_filament_colours[safe_idx(get_filament_index(row))]);
        };
        wxColour c0 = get_col(0), c1 = get_col(1), c2 = get_col(2);
        int r = (int)(c0.Red()   * m_tri_wx + c1.Red()   * m_tri_wy + c2.Red()   * m_tri_wz + 0.5);
        int g = (int)(c0.Green() * m_tri_wx + c1.Green() * m_tri_wy + c2.Green() * m_tri_wz + 0.5);
        int b = (int)(c0.Blue()  * m_tri_wx + c1.Blue()  * m_tri_wy + c2.Blue()  * m_tri_wz + 0.5);
        return wxString::Format("#%02X%02X%02X",
            std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)).ToStdString();
    }

    int per = 100 / n;
    int rem = 100 - per * n;
    std::vector<std::pair<std::string, int>> cp;
    for (int i = 0; i < n; ++i) {
        int idx = safe_idx(get_filament_index(i));
        cp.push_back(std::make_pair(m_filament_colours[idx], per + (i == 0 ? rem : 0)));
    }
    return MixedFilamentManager::blend_color_multi(cp);
}

void SnapmakerMixedFilamentDialog::draw_strip(wxDC& dc, wxPanel* panel)
{
    wxSize sz = panel->GetClientSize();
    if (sz.x <= 0 || sz.y <= 0) { dc.SetBackground(*wxGREY_BRUSH); dc.Clear(); return; }
    if (m_filament_rows.empty() && m_current_mode != MODE_MATCH) {
        dc.SetBackground(*wxGREY_BRUSH); dc.Clear(); return;
    }
    int n = (int)m_filament_rows.size();
    static constexpr int STRIP_SEGMENTS = 20;
    bool vertical = (m_current_mode == MODE_RATIO && (n == 2 || n == 3))
                 || (m_current_mode == MODE_CYCLE)
                 || (m_current_mode == MODE_MATCH);
    int total_px = vertical ? sz.y : sz.x;

    // In 2-color ratio mode, build a pattern reflecting the A:B ratio
    std::vector<int> pattern;
    if (m_current_mode == MODE_RATIO && n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        const int pct_b = std::clamp(val, 0, 100);
        const int pct_a = 100 - pct_b;
        int ratio_a = 1, ratio_b = 0;
        if (pct_b >= 100) {
            ratio_a = 0; ratio_b = 1;
        } else if (pct_b > 0) {
            const bool b_major = pct_b >= pct_a;
            const int major = b_major ? pct_b : pct_a;
            const int minor = b_major ? pct_a : pct_b;
            const int g = std::gcd(major, minor);
            ratio_a = b_major ? (minor / g) : (major / g);
            ratio_b = b_major ? (major / g) : (minor / g);
            // Cap total stripes to 20 for visual clarity in preview
            if (ratio_a + ratio_b > 20) {
                const double scale = 20.0 / (ratio_a + ratio_b);
                ratio_a = std::max(1, (int)std::round(ratio_a * scale));
                ratio_b = std::max(1, (int)std::round(ratio_b * scale));
            }
        }
        const int cycle = std::max(1, ratio_a + ratio_b);
        for (int pos = 0; pos < cycle; ++pos) {
            const int b_before = (pos * ratio_b) / cycle;
            const int b_after  = ((pos + 1) * ratio_b) / cycle;
            pattern.push_back((b_after > b_before) ? 1 : 0);
        }
    } else if (m_current_mode == MODE_RATIO && n == 3) {
        int w0 = std::max(1, (int)std::round(m_tri_wx * 100));
        int w1 = std::max(1, (int)std::round(m_tri_wy * 100));
        int w2 = std::max(1, 100 - w0 - w1);
        int g = std::gcd(std::gcd(w0, w1), w2);
        if (g > 1) { w0 /= g; w1 /= g; w2 /= g; }
        const int counts[3] = {w0, w1, w2};
        const int total = w0 + w1 + w2;
        int emitted[3] = {0, 0, 0};
        for (int pos = 0; pos < total; ++pos) {
            int best = 0;
            double best_score = -1e9;
            for (int i = 0; i < 3; ++i) {
                double score = double(pos + 1) * double(counts[i]) / double(total) - double(emitted[i]);
                if (score > best_score) { best_score = score; best = i; }
            }
            ++emitted[best];
            pattern.push_back(best);
        }
    } else if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        // Decode pattern using the same logic as MixedFilamentManager
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);
        const size_t num_physical = m_filament_colours.size();

        // In cycle mode, tokens "1"/"2" map directly to physical filaments 1/2.
        MixedFilament dummy_mf;
        dummy_mf.component_a = 1;
        dummy_mf.component_b = 2;
        const std::vector<std::string> group_strs = MixedFilamentManager::split_pattern_groups(normalized);
        for (const std::string &group : group_strs) {
            const std::vector<std::string> tokens =
                MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
            for (const std::string &token : tokens) {
                const unsigned int extruder_id =
                    MixedFilamentManager::physical_filament_from_token(token, dummy_mf, num_physical);
                if (extruder_id >= 1 && extruder_id <= num_physical)
                    pattern.push_back(-(int)extruder_id);
            }
        }
        if (pattern.empty())
            for (int i = 0; i < n; ++i) pattern.push_back(i);
    } else if (m_current_mode == MODE_MATCH) {
        // Use current tri weights directly (not recipe, to reflect drag state)
        if (!m_match_tri_indices.empty()) {
            // Build pattern from current tri weights, skip zero-weight filaments
            {
                std::vector<double> wts = {m_match_tri_wx, m_match_tri_wy};
                if (m_match_tri_indices.size() >= 3) wts.push_back(m_match_tri_wz);
                std::vector<int> counts, ids;
                for (int i = 0; i < (int)wts.size() && i < (int)m_match_tri_indices.size(); ++i) {
                    int cnt = (int)std::round(wts[i] * 20.0);
                    if (cnt > 0) {
                        counts.push_back(cnt);
                        ids.push_back(m_match_tri_indices[i]);
                    }
                }
                if (!counts.empty()) {
                    int total = std::accumulate(counts.begin(), counts.end(), 0);
                    std::vector<int> emitted(counts.size(), 0);
                    for (int pos = 0; pos < total; ++pos) {
                        int best = 0; double best_score = -1e9;
                        for (int i = 0; i < (int)counts.size(); ++i) {
                            double score = double(pos + 1) * double(counts[i]) / double(total) - double(emitted[i]);
                            if (score > best_score) { best_score = score; best = i; }
                        }
                        ++emitted[best];
                        pattern.push_back(-(ids[best] + 1));
                    }
                }
            }
        }
        if (pattern.empty()) {
            for (int i = 0; i < (int)m_filament_colours.size(); ++i)
                pattern.push_back(-(i + 1));
        }
    } else {
        for (int i = 0; i < n; ++i) pattern.push_back(i);
    }

    if (pattern.empty())
        return;

    for (int s = 0; s < STRIP_SEGMENTS; ++s) {
        int entry = pattern[s % (int)pattern.size()];
        int idx;
        if (entry < 0) {
            idx = std::max(0, std::min(-entry - 1, (int)m_filament_colours.size() - 1));
        } else {
            int row = std::min(entry, n - 1);
            idx = get_filament_index(row);
            idx = std::max(0, std::min(idx, (int)m_filament_colours.size() - 1));
        }
        dc.SetBrush(wxBrush(parse_mixed_color(m_filament_colours[idx])));
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (vertical) {
            int y0 = s * total_px / STRIP_SEGMENTS;
            int y1 = (s + 1) * total_px / STRIP_SEGMENTS;
            dc.DrawRectangle(0, total_px - y1, sz.x, y1 - y0);
        }
        else {
            int x0 = s * total_px / STRIP_SEGMENTS;
            int x1 = (s + 1) * total_px / STRIP_SEGMENTS;
            dc.DrawRectangle(x0, 0, x1 - x0, sz.y);
        }
    }

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
    dc.DrawRectangle(0, 0, sz.x, sz.y);
}

void SnapmakerMixedFilamentDialog::build_swatch_grid()
{
    int n = (int)m_filament_colours.size();
    if (n < 2) return;

    // Limit recommended swatches to the first 6 physical filaments.
    constexpr int kMaxSwatchFilaments = 6;

    bool is_ratio_3 = (m_current_mode == MODE_RATIO) && ((int)m_filament_rows.size() == 3);
    if(is_ratio_3) {
        // In 3-row ratio mode, we can show triple combinations, but limit to the first 6 filaments to avoid combinatorial explosion.
        n = std::min(n, kMaxSwatchFilaments);
    }

    // Build candidates using the same preset logic as match mode.
    // For ratio mode with 2 rows: pair candidates at 25/50/75.
    // For ratio mode with 3 rows: pair + triple candidates.
    // For other modes: pairs at 50 only.
    struct Candidate {
        wxColour color;
        wxString tooltip;
        // For pair: row indices and b_pct. For triple: row indices and weights.
        int rows[3] = {0, 0, 0};
        int n_rows   = 2;
        int b_pct    = 50;       // used when n_rows == 2
        double wx    = 1.0/3.0;  // used when n_rows == 3
        double wy    = 1.0/3.0;
        double wz    = 1.0/3.0;
    };

    std::vector<Candidate> candidates;

    if (m_current_mode == MODE_RATIO) {
        std::vector<wxColour> palette;
        palette.reserve(n);
        for (const auto& s : m_filament_colours)
            palette.push_back(parse_mixed_color(s));

        if (is_ratio_3) {
            // Three-color mode: only show triple candidates
            auto make_triple_candidate = [&](int i, int j, int k,
                                             const std::vector<int>& input_weights) -> Candidate {
                std::vector<unsigned int> ids = {(unsigned)(i+1), (unsigned)(j+1), (unsigned)(k+1)};
                auto recipe = build_multi_color_match_candidate(palette, ids, input_weights);
                if (!recipe.valid) return {};

                Candidate c;
                c.color  = recipe.preview_color;
                c.n_rows = 3;
                // Use original input order for rows (i, j, k correspond to rows 0, 1, 2)
                c.rows[0] = i;
                c.rows[1] = j;
                c.rows[2] = k;
                // Weights correspond to the original input order
                c.wx = input_weights[0] / 100.0;
                c.wy = input_weights[1] / 100.0;
                c.wz = input_weights[2] / 100.0;
                c.tooltip = wxString::Format("F%d(%d%%)+F%d(%d%%)+F%d(%d%%)",
                    i+1, input_weights[0],
                    j+1, input_weights[1],
                    k+1, input_weights[2]);
                return c;
            };

            const std::vector<int> eq = normalize_color_match_weights({1, 1, 1}, 3);
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    for (int k = j + 1; k < n; ++k) {
                        auto c_eq = make_triple_candidate(i, j, k, eq);
                        if (c_eq.n_rows == 3) candidates.push_back(c_eq);

                        for (int dom = 0; dom < 3; ++dom) {
                            std::vector<int> dw = {25, 25, 25};
                            dw[dom] = 50;
                            auto c_dom = make_triple_candidate(i, j, k, dw);
                            if (c_dom.n_rows == 3) candidates.push_back(c_dom);
                        }
                    }
                }
            }
        } else {
            // Two-color mode: only show pair candidates at 50:50
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    auto recipe = build_pair_color_match_candidate(palette, i + 1, j + 1, 50);
                    if (!recipe.valid) continue;
                    Candidate c;
                    c.color   = recipe.preview_color;
                    c.tooltip = wxString::Format("F%d(50%%) + F%d(50%%)", i+1, j+1);
                    c.rows[0] = i; c.rows[1] = j;
                    c.n_rows  = 2;
                    c.b_pct   = 50;
                    candidates.push_back(c);
                }
            }
        }
    } else if (m_current_mode == MODE_MATCH) {
        const auto presets = build_color_match_presets(m_filament_colours, m_match_min_pct);
        for (const auto& preset : presets) {
            if (!preset.valid) continue;
            Candidate c;
            c.color   = preset.preview_color;
            c.tooltip = from_u8(summarize_color_match_recipe(preset));
            auto decoded = MixedFilamentManager::decode_gradient_component_ids(preset.gradient_component_ids);
            if (decoded.size() >= 2) {
                c.rows[0] = (int)decoded[0] - 1; c.rows[1] = (int)decoded[1] - 1;
                c.n_rows = 2; c.b_pct = preset.mix_b_percent;
                if (decoded.size() >= 3) {
                    c.rows[2] = (int)decoded[2] - 1; c.n_rows = 3;
                    auto w = decode_color_match_gradient_weights(preset.gradient_component_weights, 3);
                    if (w.size() >= 3) { c.wx = w[0]/100.0; c.wy = w[1]/100.0; c.wz = w[2]/100.0; }
                }
            } else {
                c.rows[0] = (int)preset.component_a - 1; c.rows[1] = (int)preset.component_b - 1;
                c.n_rows = 2; c.b_pct = preset.mix_b_percent;
            }
            candidates.push_back(c);
        }
    } else if (m_current_mode == MODE_GRADIENT) {
        // Gradient mode: A→B and B→A direction pairs per filament pair.
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                // 50:50 blend is symmetric — compute once, reuse for both directions.
                const wxColour blended = parse_mixed_color(
                    MixedFilamentManager::blend_color(m_filament_colours[i], m_filament_colours[j], 50, 50));

                // Direction A→B: Fi → Fj
                Candidate c_ab;
                c_ab.color   = blended;
                c_ab.tooltip = wxString::Format(L"F%d → F%d", i + 1, j + 1);
                c_ab.rows[0] = i; c_ab.rows[1] = j;
                c_ab.n_rows  = 2;
                c_ab.b_pct   = 50;
                candidates.push_back(c_ab);

                // Direction B→A: Fj → Fi
                Candidate c_ba;
                c_ba.color   = blended;
                c_ba.tooltip = wxString::Format(L"F%d → F%d", j + 1, i + 1);
                c_ba.rows[0] = j; c_ba.rows[1] = i;
                c_ba.n_rows  = 2;
                c_ba.b_pct   = 50;
                candidates.push_back(c_ba);
            }
        }
    } else {
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                std::string blended = MixedFilamentManager::blend_color(
                    m_filament_colours[i], m_filament_colours[j], 50, 50);
                Candidate c;
                c.color   = parse_mixed_color(blended);
                c.tooltip = wxString::Format("F%d + F%d", i+1, j+1);
                c.rows[0] = i; c.rows[1] = j;
                c.n_rows  = 2;
                c.b_pct   = 50;
                candidates.push_back(c);
            }
        }
    }

    if (candidates.empty()) return;
    m_swatch_min_weights.clear();
    auto* grid = new wxGridSizer(10, FromDIP(6), FromDIP(6));

    // Build display context for MixedFilamentBadge
    MixedFilamentDisplayContext ctx;
    ctx.num_physical = m_filament_colours.size();
    ctx.physical_colors = m_filament_colours;

    int badge_idx = 0;
    for (const auto& cand : candidates) {
        {
            std::vector<unsigned int> fids;
            for (int r = 0; r < cand.n_rows; ++r)
                fids.push_back(static_cast<unsigned int>(cand.rows[r]));
            if (!is_filament_compatible(fids))
                continue;
        }

        // Build a MixedFilament for the badge's display_color
        MixedFilament mf;
        mf.component_a = (unsigned int)(cand.rows[0] + 1);
        mf.component_b = (unsigned int)(cand.rows[1] + 1);
        mf.custom = true;
        mf.gradient_enabled = (m_current_mode == MODE_GRADIENT);
        mf.display_color = wxString::Format("#%02X%02X%02X",
            cand.color.Red(), cand.color.Green(), cand.color.Blue()).ToStdString();

        auto* badge = new MixedFilamentBadge(m_swatch_grid_panel, wxID_ANY, ++badge_idx, mf, ctx, false, 24);
        badge->SetToolTip(cand.tooltip);

        badge->Bind(wxEVT_BUTTON, [this, cand](wxCommandEvent&) {
            if (m_current_mode == MODE_MATCH) {
                // Directly update tri picker with preset config (no target update, no recipe search)
                if (cand.n_rows >= 3) {
                    m_match_tri_indices = {cand.rows[0], cand.rows[1], cand.rows[2]};
                    m_match_tri_wx = cand.wx; m_match_tri_wy = cand.wy; m_match_tri_wz = cand.wz;
                    m_match_tri_weights = {cand.wx, cand.wy, cand.wz};
                } else if (cand.n_rows == 2) {
                    m_match_tri_indices = {cand.rows[0], cand.rows[1]};
                    m_match_tri_weights = {(double)(100 - cand.b_pct) / 100.0, (double)cand.b_pct / 100.0};
                    m_match_tri_wx = m_match_tri_weights[0]; m_match_tri_wy = m_match_tri_weights[1];
                    if (m_match_gradient_selector) {
                        m_match_gradient_selector->set_value(cand.b_pct);
                        int ia = std::clamp(cand.rows[0], 0, (int)m_filament_colours.size() - 1);
                        int ib = std::clamp(cand.rows[1], 0, (int)m_filament_colours.size() - 1);
                        m_match_gradient_selector->set_colors(
                            parse_mixed_color(m_filament_colours[ia]),
                            parse_mixed_color(m_filament_colours[ib]));
                    }
                }
                if (m_match_gradient_selector && cand.n_rows >= 3 && m_match_tri_indices.size() >= 2) {
                    int ia = std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1);
                    int ib = std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1);
                    m_match_gradient_selector->set_colors(
                        parse_mixed_color(m_filament_colours[ia]),
                        parse_mixed_color(m_filament_colours[ib]));
                }
                rebuild_match_legend();
                update_ratio_or_tri_visibility();  // switch gradient/tri based on actual filament count
                if (m_match_tri_picker)  m_match_tri_picker->Refresh();
                if (m_match_strip_panel) m_match_strip_panel->Refresh();
                if (m_match_blend_panel) m_match_blend_panel->Refresh();
                update_compatibility_warning();
                return;
            }
            std::vector<int> selections;
            for (int r = 0; r < cand.n_rows; ++r)
                selections.push_back(cand.rows[r]);

            if (selections.size() >= 2) {
                m_result.component_a = (unsigned int)(selections[0] + 1);
                m_result.component_b = (unsigned int)(selections[1] + 1);
            }

            // Rebuild all combos with the candidate's selections
            for (size_t i = 0; i < m_filament_rows.size(); ++i) {
                std::set<int> used_by_others;
                for (size_t k = 0; k < m_filament_rows.size(); ++k)
                    if (k != i && k < selections.size())
                        used_by_others.insert(selections[k]);
                int sel = (i < selections.size()) ? selections[i] : 0;
                populate_combo(m_filament_rows[i].combo, used_by_others, sel,
                               m_filament_rows[i].filament_indices);
                set_combo_combined_icon(m_filament_rows[i].combo, sel);
            }

            if (cand.n_rows == 2 && m_current_mode == MODE_RATIO && m_gradient_selector) {
                m_gradient_selector->set_value(cand.b_pct);
                rebuild_legend();
            } else if (cand.n_rows == 2 && m_current_mode == MODE_GRADIENT) {
                // Direction is encoded in rows[] ordering; reset to 0 so
                // the gradient draws component_a (rows[0]) → component_b (rows[1]).
                m_gradient_direction = 0;
            } else if (cand.n_rows == 3) {
                m_tri_wx = cand.wx;
                m_tri_wy = cand.wy;
                m_tri_wz = cand.wz;
                rebuild_legend();
            }
            update_preview();
            update_compatibility_warning();
        });

        // Single-filament badges are always visible regardless of the min-mix slider.
        int min_w = 100;
        if (cand.n_rows == 2)
            min_w = std::min(100 - cand.b_pct, cand.b_pct);
        else if (cand.n_rows == 3)
            min_w = static_cast<int>(std::min({cand.wx, cand.wy, cand.wz}) * 100.0 + 0.5);
        // n_rows == 1 or unknown: badge is always visible (sentinel 100).
        m_swatch_min_weights.push_back(min_w);

        grid->Add(badge, 0, wxALIGN_CENTER);
    }

    m_swatch_grid_panel->SetSizer(grid);
    m_swatch_grid_panel->Layout();
}

/**
 * Rebuild the swatch grid sizer in match mode without recreating widgets.
 * Hides badges whose minimum component weight falls below m_match_min_pct
 * and reflows the remaining visible badges into a new grid sizer.
 */
void SnapmakerMixedFilamentDialog::rebuild_swatch_sizer()
{
    if (m_current_mode != MODE_MATCH) return;
    auto& children = m_swatch_grid_panel->GetChildren();

    // Guard: the grid panel children must correspond 1:1 with swatch badges.
    if (children.size() != m_swatch_min_weights.size()) return;

    const int cols = 10;
    auto* new_grid = new wxGridSizer(cols, FromDIP(6), FromDIP(6));
    for (size_t i = 0; i < children.size() && i < m_swatch_min_weights.size(); ++i) {
        bool visible = m_swatch_min_weights[i] >= m_match_min_pct;
        children[i]->Show(visible);
        if (visible)
            new_grid->Add(children[i], 0, wxALIGN_CENTER);
    }

    // SetSizer replaces and auto-deletes the old sizer.
    // Windows survive because Add() defaults to no ownership.
    m_swatch_grid_panel->SetSizer(new_grid);
    m_swatch_grid_panel->Layout();
    m_swatch_card->Layout();
    m_swatch_card->Refresh();
    m_scrolled_content->FitInside();
    m_scrolled_content->Refresh();
}

void SnapmakerMixedFilamentDialog::on_mode_changed(int mode_index)
{
    sync_rows_to_result();
    if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        m_result.manual_pattern = MixedFilamentManager::normalize_manual_pattern(raw);
    }
    m_current_mode = mode_index;
    int max_f = max_filaments_for_mode(mode_index);
    if ((int)m_filament_rows.size() > max_f)
        resize_gradient_ids(max_f);
    if (mode_index == MODE_MATCH) {
        int num_physical = (int)m_filament_colours.size();
        if (m_match_state_persisted) {
            // Re-entering Match mode in the same session: the match member state
            // (m_match_tri_indices/weights/wx/wy/wz), the gradient/tri widgets, the
            // target color, and m_match_min_pct all survive mode switches on their
            // own — sync_rows_to_result() only touches filament-row data, never the
            // match members. Re-initializing from m_result here would reset the
            // user's adjustments to the 2:1:1 default (the original bug), so we only
            // refresh the legend from the retained state.
            rebuild_match_legend();
        } else {
            // First entry this session: initialize from m_result (Edit dialog) or
            // fall back to the default 2:1:1 ratio (Add dialog).
            m_match_tri_indices.clear();
            m_match_tri_weights.clear();
            // Restore saved data if editing, otherwise use default 2:1:1
            auto saved_ids = MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids);
            if (saved_ids.size() >= 3) {
                // Restore from saved match data
                for (unsigned int id : saved_ids) {
                    int idx = int(id - 1);
                    if (idx >= 0 && idx < num_physical)
                        m_match_tri_indices.push_back(idx);
                }
                if (m_match_tri_indices.size() >= 3) {
                    auto w = decode_color_match_gradient_weights(m_result.gradient_component_weights, (int)m_match_tri_indices.size());
                    double total = 0;
                    for (int v : w) total += v;
                    if (total > 0) {
                        m_match_tri_wx = w[0] / total; m_match_tri_wy = w[1] / total; m_match_tri_wz = w[2] / total;
                    }
                    m_match_tri_weights = {m_match_tri_wx, m_match_tri_wy, m_match_tri_wz};
                }
            } else if (saved_ids.size() == 2) {
                // 2-color saved match
                for (unsigned int id : saved_ids) {
                    int idx = int(id - 1);
                    if (idx >= 0 && idx < num_physical)
                        m_match_tri_indices.push_back(idx);
                }
                m_match_tri_weights = {(double)(100 - m_result.mix_b_percent) / 100.0, (double)m_result.mix_b_percent / 100.0};
                m_match_tri_wx = m_match_tri_weights[0]; m_match_tri_wy = m_match_tri_weights[1];
            } else if (m_result.custom && m_result.component_a > 0 && m_result.component_b > 0 && num_physical >= 2) {
                // Edit dialog: 2-color match restored from component_a/b (no gradient_component_ids stored)
                m_match_tri_indices = {(int)m_result.component_a - 1, (int)m_result.component_b - 1};
                m_match_tri_weights = {(double)(100 - m_result.mix_b_percent) / 100.0, (double)m_result.mix_b_percent / 100.0};
                m_match_tri_wx = m_match_tri_weights[0]; m_match_tri_wy = m_match_tri_weights[1];
            } else if (num_physical >= 3) {
                m_match_tri_indices = {0, 1, 2};
                m_match_tri_weights = {2.0/4.0, 1.0/4.0, 1.0/4.0};
                m_match_tri_wx = 2.0/4.0; m_match_tri_wy = 1.0/4.0; m_match_tri_wz = 1.0/4.0;
            } else if (num_physical == 2) {
                m_match_tri_indices = {0, 1};
                m_match_tri_weights = {0.5, 0.5};
            }
            // Set gradient_selector colors
            if (m_match_gradient_selector && m_match_tri_indices.size() >= 2) {
                int ia = std::clamp(m_match_tri_indices[0], 0, num_physical - 1);
                int ib = std::clamp(m_match_tri_indices[1], 0, num_physical - 1);
                m_match_gradient_selector->set_colors(
                    parse_mixed_color(m_filament_colours[ia]),
                    parse_mixed_color(m_filament_colours[ib]));
                if (m_match_tri_indices.size() == 2)
                    m_match_gradient_selector->set_value((int)(m_match_tri_weights[1] * 100 + 0.5));
            }
            // Default target color using K-M blend (same method as recipe search)
            wxColour default_target("#26A69A");
            std::vector<wxColour> palette;
            palette.reserve(num_physical);
            for (const auto& s : m_filament_colours)
                palette.push_back(parse_mixed_color(s));
            if (num_physical >= 3) {
                auto recipe = build_multi_color_match_candidate(palette, {1u, 2u, 3u}, {50, 25, 25}, m_match_min_pct);
                if (recipe.valid) default_target = recipe.preview_color;
            } else if (num_physical == 2) {
                auto recipe = build_pair_color_match_candidate(palette, 1u, 2u, 50, m_match_min_pct);
                if (recipe.valid) default_target = recipe.preview_color;
            }
            if (m_match_target_picker) m_match_target_picker->SetBackgroundColour(default_target);
            if (m_match_hex_input) m_match_hex_input->ChangeValue(default_target.GetAsString(wxC2S_HTML_SYNTAX).Mid(1));
            m_match_state_persisted = true;
            rebuild_match_legend();
        }
    } else if (mode_index == MODE_CYCLE && m_pattern_ctrl) {
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(m_result.manual_pattern);
        m_pattern_ctrl->SetValue(from_u8(norm.empty() ? "12" : norm));
        rebuild_cycle_legend();
    }
    rebuild_filament_rows();
    update_ratio_or_tri_visibility();
    update_preview();
    // Clear any stale banners before re-evaluating for the new mode
    if (m_error_panel && m_warning_panel && m_btn_confirm) {
        m_error_panel->Hide();
        m_warning_panel->Hide();
        if (mode_index == MODE_CYCLE)
            validate_cycle_pattern();
        else
            update_compatibility_warning();
    }
    Layout();
    if (IsShown()) Fit();
}

void SnapmakerMixedFilamentDialog::update_gradient_selector_colors()
{
    if (!m_gradient_selector || m_filament_rows.size() < 2) return;
    int ia = get_filament_index(0);
    int ib = get_filament_index(1);
    ia = std::max(0, std::min(ia, (int)m_filament_colours.size()-1));
    ib = std::max(0, std::min(ib, (int)m_filament_colours.size()-1));
    wxColour ca = parse_mixed_color(m_filament_colours[ia]);
    wxColour cb = parse_mixed_color(m_filament_colours[ib]);
    m_gradient_selector->set_colors(ca, cb);

    update_legend_text();
}

void SnapmakerMixedFilamentDialog::rebuild_cycle_legend()
{
    if (!m_cycle_legend_sizer || !m_pattern_ctrl) return;

    m_cycle_legend_sizer->Clear(true);
    m_cycle_legend_labels.clear();

    const std::string raw = into_u8(m_pattern_ctrl->GetValue());
    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);

    if (!normalized.empty()) {
        const int num_physical = (int)m_filament_colours.size();
        if (num_physical >= 2) {
            // Decode pattern to filament IDs (same logic as draw_strip / collect_result)
            MixedFilament dummy_mf;
            dummy_mf.component_a = 1;
            dummy_mf.component_b = 2;
            std::vector<unsigned int> sequence;
            const std::vector<std::string> group_strs = MixedFilamentManager::split_pattern_groups(normalized);
            for (const std::string& group : group_strs) {
                const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
                for (const auto& token : tokens) {
                    unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, dummy_mf, num_physical);
                    if (eid >= 1 && eid <= (unsigned)num_physical) sequence.push_back(eid);
                }
            }

            if (!sequence.empty()) {
                // Count occurrences
                std::map<unsigned int, int> counts;
                for (unsigned int eid : sequence) counts[eid]++;

                const int total = (int)sequence.size();

                // Compute floor percentages first, then distribute remainder via largest remainders
                // to guarantee sum=100. This matches summarize_cycle_pattern_text in Plater.
                std::vector<std::pair<unsigned int, int>> sorted_cnts(counts.begin(), counts.end());
                std::sort(sorted_cnts.begin(), sorted_cnts.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });

                std::vector<int> pcts(sorted_cnts.size());
                int sum_pct = 0;
                for (size_t i = 0; i < sorted_cnts.size(); ++i) {
                    pcts[i] = int((static_cast<long long>(sorted_cnts[i].second) * 100) / total);
                    sum_pct += pcts[i];
                }

                if (sum_pct < 100) {
                    std::vector<std::pair<size_t, int>> rem;
                    rem.reserve(sorted_cnts.size());
                    for (size_t i = 0; i < sorted_cnts.size(); ++i)
                        rem.emplace_back(i, int((static_cast<long long>(sorted_cnts[i].second) * 100) % total));
                    std::sort(rem.begin(), rem.end(), [](const auto& a, const auto& b) {
                        if (a.second != b.second) return a.second > b.second;
                        return a.first < b.first;
                    });
                    for (int extra = 100 - sum_pct; extra > 0; --extra) {
                        pcts[rem.front().first]++;
                        rem.erase(rem.begin());
                    }
                }

                for (size_t i = 0; i < sorted_cnts.size(); ++i) {
                    unsigned int eid = sorted_cnts[i].first;
                    int pct         = pcts[i];
                    int idx = (int)eid - 1;
                    if (idx < 0 || idx >= num_physical) continue;

                        // Badge + label pair
                        auto* pair = new wxBoxSizer(wxHORIZONTAL);

                        MixedFilamentDisplayContext ctx;
                        ctx.num_physical = m_filament_colours.size();
                        ctx.physical_colors = m_filament_colours;
                        MixedFilament mf;
                        mf.display_color = m_filament_colours[idx];
                        mf.custom = true;
                        auto* badge = new MixedFilamentBadge(m_cycle_legend_panel, wxID_ANY, eid, mf, ctx, true, 12);
                        pair->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

                        auto* lbl = new wxStaticText(m_cycle_legend_panel, wxID_ANY, wxString::Format("%d%%", pct));
                        lbl->SetFont(Label::Body_12);
                        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
                        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
                        pair->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
                        m_cycle_legend_labels.push_back(lbl);

                        m_cycle_legend_sizer->Add(pair, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
                }
            }
        }
    }

    m_cycle_legend_panel->Layout();
    if (m_cycle_card) {
        m_cycle_card->Layout();
        m_cycle_card->Refresh();
    }
    if (m_scrolled_content) {
        m_scrolled_content->Layout();
        m_scrolled_content->FitInside();
        m_scrolled_content->Refresh();
    }
}

void SnapmakerMixedFilamentDialog::validate_cycle_pattern()
{
    if (!m_pattern_ctrl) return;

    const std::string raw = into_u8(m_pattern_ctrl->GetValue());

    bool has_invalid_chars = false;
    std::string filtered;
    filtered.reserve(raw.size());
    for (char c : raw) {
        if (c == ',' || c == '[' || c == ']' ||
            (c >= '0' && c <= '9')) {
            filtered.push_back(c);
        } else {
            has_invalid_chars = true;
        }
    }

    const bool has_leading_trailing_comma = !raw.empty() && (raw.front() == ',' || raw.back() == ',');

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(filtered);
    const bool is_malformed = !filtered.empty() && normalized.empty();

    if (has_leading_trailing_comma) {
        set_error(_L("Leading or trailing commas are not allowed."));
        return;
    }

    if (has_invalid_chars) {
        set_error(_L("Invalid characters found. Only digits, square brackets ([ and ]), and commas (,) are allowed."));
        return;
    }

    if (is_malformed) {
        set_error(_L("Unrecognized pattern format. Please check the pattern syntax."));
        return;
    }

    // Validate that all directly-referenced physical filament IDs exist.
    // H4: only validate when there are at least 2 physical filaments and a non-empty pattern.
    const int num_physical = (int)m_filament_colours.size();
    if (num_physical >= 2 && !normalized.empty()) {
        auto parsed = parse_cycle_pattern(normalized, num_physical);
        if (parsed.invalid_id != 0) {
            set_error(
                wxString::Format(_L("Filament %d not recognized. Please re-enter."), (int)parsed.invalid_id));
            return;
        }
        if (!parsed.invalid_token.empty()) {
            set_error(_L("Unrecognized pattern format. Please check the pattern syntax."));
            return;
        }
    }

    update_compatibility_warning();

    const std::string final_text = normalized.empty() ? "12" : normalized;

    if (m_cycle_card) m_cycle_card->Freeze();

    if (final_text != raw) {
        m_pattern_ctrl->ChangeValue(from_u8(final_text));
    }

    if (m_cycle_strip_panel)  m_cycle_strip_panel->Refresh();
    if (m_cycle_blend_panel)  m_cycle_blend_panel->Refresh();
    rebuild_cycle_legend();

    if (m_cycle_card) {
        m_cycle_card->Thaw();
        m_cycle_card->Refresh();
    }
}

void SnapmakerMixedFilamentDialog::update_preview()
{
    if ((int)m_filament_rows.size() == 2)
        update_gradient_selector_colors();
    update_legend_text();
    if (m_preview_panel)       m_preview_panel->Refresh();
    if (m_preview_blend_panel) m_preview_blend_panel->Refresh();
    if (m_strip_panel)         m_strip_panel->Refresh();
    if (m_tri_picker)          m_tri_picker->Refresh();
    if (m_cycle_strip_panel)   m_cycle_strip_panel->Refresh();
    if (m_cycle_blend_panel)   m_cycle_blend_panel->Refresh();
    // Match mode widgets
    if (m_match_tri_picker)    m_match_tri_picker->Refresh();
    if (m_match_strip_panel)   m_match_strip_panel->Refresh();
    if (m_match_blend_panel)   m_match_blend_panel->Refresh();
}

void SnapmakerMixedFilamentDialog::collect_result()
{
    sync_rows_to_result();
    m_result.ui_mode = m_current_mode;
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;
    m_result.mix_b_percent = val;
    // Default: drop Z-gradient state. Only MODE_GRADIENT re-enables it below.
    m_result.gradient_enabled = false;
    switch (m_current_mode) {
    case MODE_RATIO:
        m_result.gradient_component_weights.clear();
        if ((int)m_filament_rows.size() == 3) {
            m_result.distribution_mode = int(MixedFilament::LayerCycle);
            m_result.manual_pattern.clear();
            // Store all 3 filament ids (1-based) so gradient_component_ids has 3 entries,
            // which lets resolve() enter the weighted gradient branch.
            {
                std::vector<unsigned int> all_ids;
                for (int i = 0; i < 3; ++i) {
                    int s = get_filament_index(i);
                    if (s >= 0)
                        all_ids.push_back((unsigned int)(s + 1));
                }
                m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(all_ids);
            }
            int r0 = (int)(m_tri_wx * 100 + 0.5);
            int r1 = (int)(m_tri_wy * 100 + 0.5);
            int r2 = 100 - r0 - r1;
            m_result.gradient_component_weights =
                std::to_string(r0) + "/" + std::to_string(r1) + "/" + std::to_string(r2);
        } else {
            m_result.distribution_mode = int(MixedFilament::Simple);
            m_result.gradient_component_ids.clear();
            m_result.manual_pattern.clear();
            const int pct_b = std::clamp(val, 0, 100);
            int ratio_a = 1, ratio_b = 0;
            if (pct_b >= 100) {
                ratio_a = 0; ratio_b = 1;
            } else if (pct_b > 0) {
                const int pct_a      = 100 - pct_b;
                const bool b_is_major = pct_b >= pct_a;
                const int major_pct   = b_is_major ? pct_b : pct_a;
                const int minor_pct   = b_is_major ? pct_a : pct_b;
                const int g = std::gcd(major_pct, minor_pct);
                ratio_a = b_is_major ? (minor_pct / g) : (major_pct / g);
                ratio_b = b_is_major ? (major_pct / g) : (minor_pct / g);
            }
            m_result.ratio_a = std::max(0, ratio_a);
            m_result.ratio_b = std::max(0, ratio_b);
        }
        break;
    case MODE_CYCLE: {
        const std::string raw = m_pattern_ctrl ? into_u8(m_pattern_ctrl->GetValue()) : std::string();
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(raw);
        m_result.distribution_mode = int(MixedFilament::Simple);
        m_result.manual_pattern = norm.empty() ? "12" : norm;
        m_result.gradient_component_ids.clear();
        m_result.gradient_component_weights.clear();
        // In cycle mode, tokens "1"/"2" map directly to physical filaments 1/2.
        m_result.component_a = 1;
        m_result.component_b = 2;
        break;
    }
    case MODE_MATCH: {
        // Always save current tri/gradient state — reflects what user actually set
        m_result.distribution_mode = int(MixedFilament::Simple);
        m_result.manual_pattern.clear();
        m_result.ratio_a = 1;
        m_result.ratio_b = 1;
        if (!m_match_tri_indices.empty()) {
            m_result.component_a = (unsigned)(m_match_tri_indices[0] + 1);
            m_result.component_b = (unsigned)(m_match_tri_indices.size() >= 2 ? m_match_tri_indices[1] + 1 : 1);
            if (m_match_tri_indices.size() >= 3) {
                int w0 = (int)(m_match_tri_wx * 100 + 0.5);
                int w1 = (int)(m_match_tri_wy * 100 + 0.5);
                int w2 = std::max(0, 100 - w0 - w1);
                m_result.gradient_component_ids.clear();
                std::string weights_str;
                std::vector<unsigned int> match_ids;
                // Only include filaments with actual weight > 0
                if (w0 > 0) { match_ids.push_back((unsigned int)(m_match_tri_indices[0] + 1)); weights_str += std::to_string(w0); }
                if (w1 > 0) { match_ids.push_back((unsigned int)(m_match_tri_indices[1] + 1)); if (!weights_str.empty()) weights_str += "/"; weights_str += std::to_string(w1); }
                if (w2 > 0) { match_ids.push_back((unsigned int)(m_match_tri_indices[2] + 1)); if (!weights_str.empty()) weights_str += "/"; weights_str += std::to_string(w2); }
                m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(match_ids);
                m_result.gradient_component_weights = weights_str;
                if (match_ids.size() >= 3) {
                    m_result.distribution_mode = int(MixedFilament::LayerCycle);
                    m_result.mix_b_percent = 50;
                } else if (match_ids.size() == 2) {
                    m_result.distribution_mode = int(MixedFilament::Simple);
                    int total_w = 0;
                    if (w0 > 0) total_w += w0;
                    if (w1 > 0) total_w += w1;
                    if (w2 > 0) total_w += w2;
                    m_result.mix_b_percent = (total_w > 0) ? (100 * (w1 > 0 ? w1 : w2) / total_w) : 50;
                }
            } else {
                m_result.mix_b_percent = m_match_gradient_selector ? m_match_gradient_selector->value() : 50;
                m_result.gradient_component_ids.clear();
                // Store for constructor detection (Plater skips display for Simple mode)
                m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids({m_result.component_a, m_result.component_b});
                m_result.gradient_component_weights.clear();
            }
        } else {
            m_result.component_a = 1;
            m_result.component_b = 2;
            m_result.mix_b_percent = 50;
            m_result.gradient_component_ids.clear();
            m_result.gradient_component_weights.clear();
        }
        // Compute display_color from all filaments (not just component_a/b)
        {
            int nf = (int)m_match_tri_indices.size();
            if (nf == 2 && m_match_gradient_selector) {
                int ia = std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1);
                int ib = std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1);
                int val = m_match_gradient_selector->value();
                m_result.display_color = MixedFilamentManager::blend_color(m_filament_colours[ia], m_filament_colours[ib], 100 - val, val);
            } else if (nf >= 3) {
                wxColour c0 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1)]);
                wxColour c1 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1)]);
                wxColour c2 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[2], 0, (int)m_filament_colours.size() - 1)]);
                int r = (int)(c0.Red()*m_match_tri_wx + c1.Red()*m_match_tri_wy + c2.Red()*m_match_tri_wz + 0.5);
                int g = (int)(c0.Green()*m_match_tri_wx + c1.Green()*m_match_tri_wy + c2.Green()*m_match_tri_wz + 0.5);
                int b = (int)(c0.Blue()*m_match_tri_wx + c1.Blue()*m_match_tri_wy + c2.Blue()*m_match_tri_wz + 0.5);
                m_result.display_color = wxString::Format("#%02X%02X%02X", std::clamp(r,0,255), std::clamp(g,0,255), std::clamp(b,0,255)).ToStdString();
            }
        }
        break;
    }
    case MODE_GRADIENT:
        m_result.distribution_mode = int(MixedFilament::LayerCycle);
        // 2-filament Z gradient does not need extra component IDs/weights.
        m_result.gradient_component_ids.clear();
        m_result.gradient_component_weights.clear();
        m_result.manual_pattern.clear();
        m_result.gradient_enabled = true;
        
        // Gradient direction mapping:
        // Direction 0: A→B (component_a starts dominant at 80%, ends at 20%)
        //              gradient_start=0.8, gradient_end=0.2 (start > end)
        // Direction 1: B→A (component_a starts at 20%, ends dominant at 80%)
        //              gradient_start=0.2, gradient_end=0.8 (start < end)
        // 
        // The gradient_start/end values represent the ratio of component_a.
        // Component_b ratio is always (1 - component_a_ratio).
        
        if (m_gradient_direction == 0) {
            // A→B: component_a goes from dominant to minority
            m_result.gradient_start = MixedFilament::k_default_gradient_dominant;
            m_result.gradient_end   = MixedFilament::k_default_gradient_minority;
        } else {
            // B→A: component_a goes from minority to dominant
            m_result.gradient_start = MixedFilament::k_default_gradient_minority;
            m_result.gradient_end   = MixedFilament::k_default_gradient_dominant;
        }
        
        // Mid-blend layer ratio (50%) keeps non-gradient consumers consistent.
        m_result.mix_b_percent = 50;
        m_result.ratio_a = 1;
        m_result.ratio_b = 1;
        // Force at least 2 sublayers per layer so the LocalZ planner emits the
        // two sub-layers needed to realize the per-layer gradient ratio.
        if (m_result.local_z_max_sublayers < 2)
            m_result.local_z_max_sublayers = 2;
        break;
    }
    m_result.custom = true;
}

void SnapmakerMixedFilamentDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    if (m_preview_panel)
        m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    if (m_strip_panel)
        m_strip_panel->SetMinSize(wxSize(-1, FromDIP(STRIP_HEIGHT)));
    Layout(); 
    Fit();
}

}} // namespace Slic3r::GUI
