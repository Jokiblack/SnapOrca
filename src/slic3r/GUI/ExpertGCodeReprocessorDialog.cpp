// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0)
#include "ExpertGCodeReprocessorDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SpinInput.hpp"
#include "Widgets/TextInput.hpp"

#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>

namespace Slic3r { namespace GUI {

namespace {

using json = nlohmann::json;

// Rule types in the order of the type combo box.
const char *const RULE_TYPES[] = { "speed_multiplier", "flow_multiplier", "fan_override", "z_offset" };
constexpr double  RULE_DEFAULT[] = { 100., 100., 255., 0. };
constexpr double  RULE_MIN[]     = { 1., 20., 0., -0.3 };
constexpr double  RULE_MAX[]     = { 300., 200., 255., 0.3 };

int type_index(const std::string &type)
{
    for (int i = 0; i < 4; ++i)
        if (type == RULE_TYPES[i])
            return i;
    return -1;
}

const char *RULES_KEY = "expert_gcode_reprocessor_rules";

} // namespace

ExpertGCodeReprocessorDialog::ExpertGCodeReprocessorDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Expert G-code Reprocessor"),
                wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_filament_count = std::max<size_t>(1, wxGetApp().preset_bundle->filament_presets.size());
    const DynamicPrintConfig &project = wxGetApp().preset_bundle->project_config;
    load(project.has(RULES_KEY) ? project.opt_string(RULES_KEY) : std::string());

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    auto *intro = new Label(this, _L("Rewrites the exported G-code by layer range: print speed (M220), flow (M221), fan (M106) and "
                                     "Z offset (SET_GCODE_OFFSET). A rule for one filament only acts while that filament prints. "
                                     "Layers are numbered as in the preview."),
                            LB_AUTO_WRAP);
    intro->SetMaxSize(wxSize(FromDIP(760), -1));
    intro->Wrap(FromDIP(760));
    sizer->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

    auto *master_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_master            = new ::CheckBox(this);
    m_master->SetValue(m_master_enabled);
    master_sizer->Add(m_master, 0, wxALIGN_CENTER_VERTICAL);
    master_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Reprocessor enabled")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(master_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_rows_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(780), FromDIP(260)), wxVSCROLL);
    m_rows_panel->SetBackgroundColour(*wxWHITE);
    m_rows_panel->SetScrollRate(0, FromDIP(10));
    m_rows_sizer = new wxFlexGridSizer(8, FromDIP(4), FromDIP(8));
    m_rows_panel->SetSizer(m_rows_sizer);
    sizer->Add(m_rows_panel, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto *add_btn = new Button(this, _L("Add rule"));
    add_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        collect_rows();
        m_rules.emplace_back();
        rebuild_rows();
    });
    sizer->Add(add_btn, 0, wxALL, FromDIP(10));

    auto *btns = new DialogButtons(this, { "OK", "Cancel" });
    btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        collect_rows();
        EndModal(wxID_OK);
    });
    btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    sizer->Add(btns, 0, wxEXPAND);

    rebuild_rows();
    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void ExpertGCodeReprocessorDialog::load(const std::string &rules_json)
{
    if (rules_json.empty())
        return;
    json root;
    try {
        root = json::parse(rules_json);
    } catch (const std::exception &) {
        return;
    }
    if (!root.is_object())
        return;
    m_master_enabled = !root.contains("enabled") || !root["enabled"].is_boolean() || root["enabled"].get<bool>();
    if (!root.contains("rules") || !root["rules"].is_array())
        return;
    for (const json &r : root["rules"]) {
        const int type = (r.is_object() && r.contains("type") && r["type"].is_string()) ? type_index(r["type"].get<std::string>()) : -1;
        if (type < 0) {
            m_unknown_rules.push_back(r);
            continue;
        }
        RuleData rule;
        rule.type            = type;
        rule.enabled         = r.contains("enabled") && r["enabled"].is_boolean() && r["enabled"].get<bool>();
        rule.tool            = (r.value("mode", std::string("global")) == "by_tool" && r.contains("tool") && r["tool"].is_number_integer()) ?
                                   r["tool"].get<int>() : -1;
        rule.layer_from      = (r.contains("layer_from") && r["layer_from"].is_number_integer()) ? std::max(0, r["layer_from"].get<int>()) : 0;
        rule.layer_to        = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.value           = (r.contains("value") && r["value"].is_number()) ? r["value"].get<double>() : RULE_DEFAULT[type];
        rule.avoid_wipetower = r.contains("avoid_wipetower") && r["avoid_wipetower"].is_boolean() && r["avoid_wipetower"].get<bool>();
        m_rules.push_back(rule);
    }
}

std::string ExpertGCodeReprocessorDialog::to_json() const
{
    json rules = json::array();
    for (const RuleData &rule : m_rules) {
        json r;
        r["type"]       = RULE_TYPES[rule.type];
        r["enabled"]    = rule.enabled;
        r["mode"]       = rule.tool >= 0 ? "by_tool" : "global";
        if (rule.tool >= 0)
            r["tool"] = rule.tool;
        r["layer_from"] = rule.layer_from;
        r["layer_to"]   = rule.layer_to;
        if (rule.type == 3)
            r["value"] = std::round(rule.value * 100.) / 100.;
        else
            r["value"] = int(std::lround(rule.value));
        r["avoid_wipetower"] = rule.avoid_wipetower;
        rules.push_back(r);
    }
    for (const json &unknown : m_unknown_rules)
        rules.push_back(unknown);
    if (rules.empty() && m_master_enabled)
        return std::string(); // nothing to apply: keep the project key at its default
    json root;
    root["enabled"] = m_master_enabled;
    root["rules"]   = rules;
    return root.dump();
}

// Reads the widgets back into m_rules, clamping every value to what the reprocessor accepts.
void ExpertGCodeReprocessorDialog::collect_rows()
{
    m_master_enabled = m_master->GetValue();
    for (size_t i = 0; i < m_rows.size() && i < m_rules.size(); ++i) {
        const RuleRow &row  = m_rows[i];
        RuleData      &rule = m_rules[i];
        rule.enabled         = row.enabled->GetValue();
        rule.type            = std::clamp(row.type->GetSelection(), 0, 3);
        rule.tool            = row.tool->GetSelection() - 1;
        rule.layer_from      = std::max(0, row.layer_from->GetValue() - 1);
        long to              = 0;
        const wxString to_text = row.layer_to->GetTextCtrl()->GetValue().Trim().Trim(false);
        rule.layer_to        = (!to_text.empty() && to_text.ToLong(&to) && to >= 1) ? std::max(int(to) - 1, rule.layer_from) : -1;
        const std::string value_text = into_u8(row.value->GetTextCtrl()->GetValue());
        size_t            parsed     = 0;
        const double      value      = string_to_double_decimal_point(value_text, &parsed);
        if (parsed > 0 && std::isfinite(value))
            rule.value = value; // otherwise keep the previous value
        rule.value = std::clamp(rule.value, RULE_MIN[rule.type], RULE_MAX[rule.type]);
        rule.avoid_wipetower = row.avoid_wipetower->GetValue();
    }
}

void ExpertGCodeReprocessorDialog::rebuild_rows()
{
    m_rows_panel->Freeze();
    m_rows_sizer->Clear(true);
    m_rows.clear();

    for (const wxString &title : { _L("On"), _L("Rule"), _L("Filament"), _L("From layer"), _L("To layer"), _L("Value"),
                                   _L("Skip wipe tower"), wxString() })
        m_rows_sizer->Add(new wxStaticText(m_rows_panel, wxID_ANY, title), 0, wxALIGN_CENTER_VERTICAL);

    const wxString types[] = {
        // xgettext:no-c-format, no-boost-format
        _L("Speed (%)"),
        // xgettext:no-c-format, no-boost-format
        _L("Flow (%)"),
        _L("Fan (0-255)"),
        _L("Z offset (mm)"),
    };
    for (size_t i = 0; i < m_rules.size(); ++i) {
        const RuleData &rule = m_rules[i];
        RuleRow         row;

        row.enabled = new ::CheckBox(m_rows_panel);
        row.enabled->SetValue(rule.enabled);

        row.type = new ::ComboBox(m_rows_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(140), -1), 0, nullptr,
                                  wxCB_READONLY);
        for (const wxString &t : types)
            row.type->Append(t);
        row.type->SetSelection(rule.type);

        row.tool = new ::ComboBox(m_rows_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(120), -1), 0, nullptr,
                                  wxCB_READONLY);
        row.tool->Append(_L("All"));
        for (size_t f = 0; f < std::max(m_filament_count, size_t(rule.tool + 1)); ++f)
            row.tool->Append(format_wxstr(_L("Filament %1%"), f + 1));
        row.tool->SetSelection(rule.tool + 1);

        row.layer_from = new ::SpinInput(m_rows_panel, wxEmptyString, "", wxDefaultPosition, wxSize(FromDIP(80), -1), wxSP_ARROW_KEYS, 1,
                                         100000, rule.layer_from + 1);

        row.layer_to = new ::TextInput(m_rows_panel, rule.layer_to >= 0 ? wxString::Format("%d", rule.layer_to + 1) : wxString(), "", "",
                                       wxDefaultPosition, wxSize(FromDIP(80), -1));
        row.layer_to->SetToolTip(_L("Leave empty to apply the rule up to the last layer."));

        row.value = new ::TextInput(m_rows_panel, from_u8(float_to_string_decimal_point(rule.value, rule.type == 3 ? 2 : 0)), "", "",
                                    wxDefaultPosition, wxSize(FromDIP(80), -1));

        // A new type starts from its neutral value: a speed of 100 would clamp to a 0.3 mm Z offset.
        row.type->Bind(wxEVT_COMBOBOX, [type = row.type, value = row.value](wxCommandEvent &e) {
            const int t = std::clamp(type->GetSelection(), 0, 3);
            value->GetTextCtrl()->SetValue(from_u8(float_to_string_decimal_point(RULE_DEFAULT[t], t == 3 ? 2 : 0)));
            e.Skip();
        });

        row.avoid_wipetower = new ::CheckBox(m_rows_panel);
        row.avoid_wipetower->SetValue(rule.avoid_wipetower);
        row.avoid_wipetower->SetToolTip(_L("Do not apply the rule inside the wipe tower G-code."));

        row.remove = new Button(m_rows_panel, _L("Delete"));
        row.remove->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        row.remove->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) {
            // Rebuilding destroys the clicked button: leave its event handler first.
            CallAfter([this, i]() {
                collect_rows();
                if (i < m_rules.size())
                    m_rules.erase(m_rules.begin() + i);
                rebuild_rows();
            });
        });

        for (wxWindow *w : std::initializer_list<wxWindow *>{ row.enabled, row.type, row.tool, row.layer_from, row.layer_to, row.value,
                                                              row.avoid_wipetower, row.remove })
            m_rows_sizer->Add(w, 0, wxALIGN_CENTER_VERTICAL);
        m_rows.push_back(row);
    }

    m_rows_panel->FitInside();
    m_rows_panel->Layout();
    m_rows_panel->Thaw();
    wxGetApp().UpdateDarkUIWin(m_rows_panel);
}

void ExpertGCodeReprocessorDialog::on_dpi_changed(const wxRect &)
{
    collect_rows();
    rebuild_rows();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void ExpertGCodeReprocessorDialog::show_and_save(wxWindow *parent)
{
    ExpertGCodeReprocessorDialog dlg(parent);
    if (dlg.ShowModal() != wxID_OK)
        return;
    DynamicPrintConfig &project = wxGetApp().preset_bundle->project_config;
    const std::string   rules   = dlg.to_json();
    if (project.has(RULES_KEY) && project.opt_string(RULES_KEY) == rules)
        return;
    project.set_key_value(RULES_KEY, new ConfigOptionString(rules));
    wxGetApp().plater()->set_plater_dirty(true);
    // The key is ignored by the slicing steps: this only hands the rules to the next export.
    wxGetApp().plater()->update();
}

}} // namespace Slic3r::GUI
