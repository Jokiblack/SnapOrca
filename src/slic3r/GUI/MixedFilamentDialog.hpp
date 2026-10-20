#ifndef slic3r_MixedFilamentDialog_hpp_
#define slic3r_MixedFilamentDialog_hpp_

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <wx/bitmap.h>
#include <wx/gdicmn.h>
#include <wx/event.h>
#include <wx/colour.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/tglbtn.h>
#include <wx/stattext.h>

#include "GUI_Utils.hpp"
#include "libslic3r/FilamentMixer.hpp"
// Snapmaker: mixed filament model and dialog dependencies
#include "libslic3r/MixedFilament.hpp"
#include <wx/wx.h>
#include <wx/statline.h>
#include <wx/dcbuffer.h>
#include <wx/wrapsizer.h>
#include <wx/clrpicker.h>
#include <set>

class Button;
class CheckBox;
class ComboBox;
class wxMouseEvent;
class wxScrolledWindow;
class wxTextCtrl;
class wxWrapSizer;
// Snapmaker
class ScalableButton;
class RadioGroup;
class Label;
class StaticBox;

namespace Slic3r {
namespace GUI {

class GradientCurveEditor;
class RatioLabelPanel;

struct MixedFilamentResult {
    std::vector<unsigned int> components;   // 1-based physical filament indices
    std::vector<int>          ratios;       // percentages, sum = 100
    bool gradient_enabled   = false;
    int  gradient_direction = 0;            // 0 = A→B, 1 = B→A  (only for 2-color)
    bool per_part_gradient  = false;        // valid only when gradient_enabled == true
    // Optional Photoshop-style custom curve overriding the linear A→B gradient.
    // Empty -> use linear (gradient_direction). Non-empty -> cubic Hermite over [0,1]^2
    // with optional per-anchor tangent overrides (see GradientAnchor).
    std::vector<GradientAnchor> gradient_curve;
};

class MixedFilamentDialog : public DPIDialog
{
public:
    MixedFilamentDialog(wxWindow* parent,
                        const std::vector<std::string>& physical_colors,
                        const std::vector<std::string>& physical_names,
                        const std::vector<std::string>& physical_types = {});

    MixedFilamentDialog(wxWindow* parent,
                        const MixedFilamentResult& existing,
                        const std::vector<std::string>& physical_colors,
                        const std::vector<std::string>& physical_names,
                        const std::vector<std::string>& physical_types = {});

    ~MixedFilamentDialog();

    MixedFilamentResult get_result() const { return m_result; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    wxBoxSizer* create_preview_panel();
    wxBoxSizer* create_material_selection();
    wxBoxSizer* create_ratio_slider();
    wxBoxSizer* create_triangle_picker();
    wxBoxSizer* create_gradient_section();
    wxBoxSizer* create_recommendation_grid();
    wxBoxSizer* create_button_panel();

    void on_filament_changed();
    void on_ratio_changed(int new_ratio_a);
    void on_gradient_toggled();
    void on_gradient_direction_changed();
    void on_gradient_curve_changed();
    void on_per_part_gradient_toggled();
    void on_add_material();
    void on_remove_material();
    void on_recommendation_clicked(unsigned int comp_a, unsigned int comp_b);
    void on_recommendation_clicked_triple(unsigned int a, unsigned int b, unsigned int c);
    void apply_manual_ratio(size_t idx, int value);
    void apply_dragged_triangle_ratio(int r0, int r1, int r2);
    void reset_manual_ratio_state();
    void refresh_ratio_labels();
    void sync_triangle_weights_from_ratios();
    void start_ratio_editor(size_t idx, wxWindow* anchor, const wxRect& anchor_rect);
    void commit_ratio_editor(bool apply);
    void commit_ratio_editor_from_background(wxMouseEvent& e);
    void update_preview();
    void update_ok_button_state();
    void update_gradient_direction_items();
    void update_component_count_ui();
    // Picks dialog (width, height) based on current state so the gradient curve
    // editor and the recommendation list stay visible at the same time.
    wxSize compute_dialog_size() const;
    void rebuild_all_combos();
    void rebuild_recommendation_items();
    void refresh_curve_editor_colors();
    void paint_warning_panel(wxPaintEvent& evt);

    wxBitmap make_swatch_bitmap(size_t idx);

    // Reserves the same width on every material row label so the combo boxes line up.
    static void apply_uniform_label_width(wxStaticText* lbl);
    // Appends one "Filament N" label + combo row to m_material_rows_sizer.  N follows the
    // number of rows already there, so callers must not renumber anything themselves.
    void append_material_row();

    // Helpers for component/ratio access
    size_t          num_components() const { return m_result.components.size(); }
    unsigned int    comp(size_t i) const { return (i < m_result.components.size()) ? m_result.components[i] : 1; }
    int             ratio(size_t i) const { return (i < m_result.ratios.size()) ? m_result.ratios[i] : 0; }
    wxColour        comp_colour(size_t i) const;

    MixedFilamentResult         m_result;
    std::vector<std::string>    m_physical_colors;
    std::vector<std::string>    m_physical_names;
    std::vector<std::string>    m_physical_types;
    wxString                    m_type_mismatch_msg;

    // Combo item index -> 1-based physical filament index (per combo)
    std::vector<std::vector<unsigned int>> m_combo_to_physical;

    // UI controls
    wxPanel*                    m_preview_canvas{nullptr};
    wxPanel*                    m_summary_panel{nullptr};
    std::vector<ComboBox*>      m_combo_filaments;
    wxBoxSizer*                 m_material_rows_sizer{nullptr};
    wxPanel*                    m_ratio_bar{nullptr};
    wxPanel*                    m_triangle_panel{nullptr};
    RatioLabelPanel*            m_label_ratio_a{nullptr};
    RatioLabelPanel*            m_label_ratio_b{nullptr};
    wxPanel*                    m_ratio_editor_panel{nullptr};
    wxTextCtrl*                 m_ratio_editor{nullptr};
    CheckBox*                   m_chk_gradient{nullptr};
    wxStaticText*               m_label_gradient{nullptr};
    ComboBox*                   m_combo_gradient_dir{nullptr};
    wxBoxSizer*                 m_gradient_sizer{nullptr};
    GradientCurveEditor*        m_curve_editor{nullptr};
    wxBoxSizer*                 m_curve_sizer{nullptr};
    CheckBox*                   m_chk_per_part_gradient{nullptr};
    wxStaticText*               m_label_per_part_gradient{nullptr};
    wxBoxSizer*                 m_per_part_gradient_sizer{nullptr};
    Button*                     m_btn_add_material{nullptr};
    Button*                     m_btn_remove_material{nullptr};
    Button*                     m_btn_ok{nullptr};
    Button*                     m_btn_cancel{nullptr};
    wxBoxSizer*                 m_warning_sizer{nullptr};
    wxPanel*                    m_warning_panel{nullptr};

    wxBoxSizer*                 m_ratio_sizer{nullptr};
    wxBoxSizer*                 m_triangle_sizer{nullptr};
    wxBoxSizer*                 m_right_sizer{nullptr};

    wxScrolledWindow*           m_recommendation_scroll{nullptr};
    wxWrapSizer*                m_recommendation_grid{nullptr};

    // Drag state. The ratio bar and the triangle picker capture the mouse
    // independently, so they must not share a flag: a mouse-up on one would
    // otherwise clear the other's flag and skip its ReleaseMouse().
    bool   m_ratio_dragging{false};
    bool   m_tri_dragging{false};
    std::vector<size_t> m_ratio_manual_order;
    size_t m_ratio_editor_idx{0};
    bool   m_ratio_editor_committing{false};
    wxWindow* m_ratio_editor_anchor{nullptr};
    // Triangle picker drag point (barycentric weights)
    double m_tri_wx{0.333}, m_tri_wy{0.333}, m_tri_wz{0.334};

    std::array<RatioLabelPanel*, 3> m_triangle_ratio_labels{nullptr, nullptr, nullptr};
};

} // namespace GUI
} // namespace Slic3r

namespace Slic3r { namespace GUI {

// Snapmaker: forward declarations (only pointers are stored)
class MixedGradientSelector;
class MixedColorMatchPanel;
class MatchRangeSlider;

// Snapmaker: Full Spectrum mixed-filament dialog (ratio / cycle / match / gradient modes),
// working on Slic3r::MixedFilament (PresetBundle::mixed_filaments). Renamed from MixedFilamentDialog
// so it can live next to upstream's MixedFilamentDialog (FilamentMixer based mixed slots).
class SnapmakerMixedFilamentDialog : public DPIDialog
{
public:
    SnapmakerMixedFilamentDialog(wxWindow* parent, const std::vector<std::string>& filament_colours);
    SnapmakerMixedFilamentDialog(wxWindow* parent, const std::vector<std::string>& filament_colours,
                      const Slic3r::MixedFilament& existing);

    const Slic3r::MixedFilament& GetResult() const { return m_result; }
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    void rebuild_filament_rows();
    void on_mode_changed(int mode_index);
    void update_preview();
    void update_gradient_selector_colors();
    void build_swatch_grid();
    void rebuild_swatch_sizer();
    void sync_rows_to_result();
    void resize_gradient_ids(int target_count);
    void update_compatibility_warning();
    wxString get_ratio_warning_msg();
    void display_warning(const wxString& msg);
    void set_error(const wxString& msg);
    std::string compute_preview_color();
    wxBitmap make_color_bitmap(const wxColour& c, int size);
    int max_filaments_for_mode(int mode_index) const;
    void collect_result();
    void draw_strip(wxDC& dc, wxPanel* panel);

    static constexpr int MODE_RATIO    = 0;
    static constexpr int MODE_CYCLE    = 1;
    static constexpr int MODE_MATCH    = 2;
    static constexpr int MODE_GRADIENT = 3;

    // Segmented mode buttons
    class StaticBox*        m_mode_btn_container    = nullptr;
    std::vector<Button*>    m_mode_buttons;
    int                     m_mode_btn_selected     = MODE_RATIO;
    ScalableButton*         m_btn_add_filament    = nullptr;
    ScalableButton*         m_btn_remove_filament = nullptr;
    wxStaticText*           m_filament_card_title  = nullptr;
    wxPanel*                m_filament_rows_panel = nullptr;
    wxBoxSizer*             m_filament_rows_sizer = nullptr;
    wxPanel*                m_preview_panel       = nullptr;
    wxPanel*                m_preview_blend_panel = nullptr;
    MixedGradientSelector*  m_gradient_selector   = nullptr;
    wxPanel*                m_legend_panel         = nullptr;
    wxBoxSizer*             m_legend_sizer         = nullptr;
    std::vector<wxStaticText*> m_legend_labels;
    wxPanel*                m_tri_picker          = nullptr;
    wxSizerItem*            m_ratio_gradient_spacer = nullptr;
    wxSizerItem*            m_ratio_tri_spacer      = nullptr;
    wxPanel*                m_strip_panel         = nullptr;
    wxPanel*                m_cycle_strip_panel   = nullptr;
    wxPanel*                m_swatch_grid_panel   = nullptr;
    std::vector<int>         m_swatch_min_weights;
    wxPanel*                m_error_panel           = nullptr;
    Label*                   m_error_text            = nullptr;
    wxPanel*                m_warning_panel         = nullptr;
    Label*                   m_warning_text          = nullptr;
    Button*                 m_btn_cancel          = nullptr;
    Button*                 m_btn_confirm         = nullptr;
    wxTextCtrl*             m_pattern_ctrl        = nullptr;
    std::vector<class MixedFilamentBadge*>  m_pattern_quick_buttons;

    // Card containers (StaticBox for rounded-corner cards)
    class StaticBox*        m_filament_card         = nullptr;
    wxBoxSizer*             m_filament_card_sizer   = nullptr;
    class StaticBox*        m_ratio_card            = nullptr;
    wxBoxSizer*             m_ratio_card_sizer      = nullptr;
    class StaticBox*        m_cycle_card            = nullptr;
    wxBoxSizer*             m_cycle_card_sizer      = nullptr;
    class StaticBox*        m_swatch_card           = nullptr;
    wxBoxSizer*             m_swatch_card_sizer     = nullptr;
    class StaticBox*        m_gradient_effect_card   = nullptr;
    wxBoxSizer*             m_gradient_effect_card_sizer = nullptr;
    wxScrolledWindow*       m_scrolled_content      = nullptr;
    wxPanel*                m_cycle_blend_panel     = nullptr;
    wxPanel*                m_cycle_legend_panel    = nullptr;
    wxSizer*                m_cycle_legend_sizer    = nullptr;
    std::vector<wxStaticText*> m_cycle_legend_labels;

    // Match mode
    wxBoxSizer*             m_match_panel_sizer   = nullptr;
    MixedColorMatchPanel*   m_match_panel         = nullptr;

    // Match mode's own Mix Ratio card (separate from ratio card)
    class StaticBox*        m_match_ratio_card       = nullptr;
    wxBoxSizer*             m_match_ratio_card_sizer = nullptr;
    MixedGradientSelector*  m_match_gradient_selector = nullptr;
    wxPanel*                m_match_tri_picker       = nullptr;
    wxSizerItem*            m_match_gradient_spacer   = nullptr;
    wxSizerItem*            m_match_tri_spacer        = nullptr;
    wxPanel*                m_match_legend_panel     = nullptr;
    wxBoxSizer*             m_match_legend_sizer     = nullptr;
    std::vector<wxStaticText*> m_match_legend_labels;
    wxPanel*                m_match_strip_panel      = nullptr;
    wxPanel*                m_match_blend_panel      = nullptr;

    // Card 1 (Match mode: filament badges + target color)
    class StaticBox*        m_match_input_card      = nullptr;
    wxPanel*                m_match_badges_panel    = nullptr;
    wxWrapSizer*            m_match_badges_sizer    = nullptr;
    wxPanel*                m_match_target_picker   = nullptr;
    wxTextCtrl*             m_match_hex_input       = nullptr;
    wxPanel*                m_match_hex_wrapper     = nullptr;
    bool                    m_match_hex_error       = false;
    wxPanel*                m_match_target_swatch   = nullptr;

    // Range slider row (inside ratio card, match-mode only)
    wxPanel*                m_match_range_row       = nullptr;
    MatchRangeSlider*       m_match_range_slider    = nullptr;
    wxStaticText*           m_match_range_value     = nullptr;
    int                     m_match_min_pct         = 15;
    // True once Match-mode state has been saved into m_result during a mode
    // switch. Guards re-entry so the user's adjustments are restored instead
    // of being overwritten by the 2:1:1 default / recomputed target color.
    bool                    m_match_state_persisted { false };

    ScalableButton*         m_btn_swap_gradient_dir = nullptr;
    int                     m_gradient_direction   = 0;

    double m_tri_wx{1.0/3.0}, m_tri_wy{1.0/3.0}, m_tri_wz{1.0/3.0};
    bool   m_tri_dragging{false};

    // Match-specific tri-picker state (fully independent from ratio)
    std::vector<int>    m_match_tri_indices;
    std::vector<double> m_match_tri_weights;
    bool                m_match_tri_dragging{false};
    double m_match_tri_wx{1.0/3.0}, m_match_tri_wy{1.0/3.0}, m_match_tri_wz{1.0/3.0};

    void build_tri_picker(wxWindow* parent = nullptr);
    void build_match_tri_picker(wxWindow* parent);
    void set_combo_combined_icon(class ComboBox* cb, int filament_idx);
    void rebuild_legend();
    void update_legend_text();
    void rebuild_match_legend();
    void update_match_legend_labels();    void rebuild_cycle_legend();
    void validate_cycle_pattern();
    void update_ratio_or_tri_visibility();
    // Combo box helpers
    void populate_combo(ComboBox* cb, const std::set<int>& exclude_ids, int select_id,
                        std::vector<int>& out_filament_indices);
    void refresh_all_combos();
    void rebuild_all_combos_with_selections(const std::vector<int>& selections);
    int  get_filament_index(int row_idx) const;

    struct FilamentRow {
        wxPanel*         swatch           = nullptr;
        ComboBox*        combo            = nullptr;
        std::vector<int> filament_indices; // combo index → actual filament index
    };
    std::vector<FilamentRow> m_filament_rows;

    const std::vector<std::string>& m_filament_colours;
    Slic3r::MixedFilament            m_result;
    int                              m_current_mode = MODE_RATIO;
};

}} // namespace Slic3r::GUI

#endif // slic3r_MixedFilamentDialog_hpp_
