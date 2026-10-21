// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0)
#pragma once

#include "GUI_Utils.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class wxBoxSizer;
class wxFlexGridSizer;
class wxScrolledWindow;
class wxWindow;
class CheckBox;
class ComboBox;
class SpinInput;
class TextInput;
class Button;

namespace Slic3r { namespace GUI {

// NEOTKO_GCODE_REPROCESSOR: editor for the project key "expert_gcode_reprocessor_rules", the rules the
// Expert G-code Reprocessor applies to the exported G-code (see libslic3r/GCode/ExpertGCodeReprocessor.hpp).
class ExpertGCodeReprocessorDialog : public DPIDialog
{
public:
    explicit ExpertGCodeReprocessorDialog(wxWindow *parent);

    // Shows the dialog and stores the edited rules in the project configuration on OK.
    static void show_and_save(wxWindow *parent);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct RuleData
    {
        bool        enabled{ true };
        int         type{ 0 };  // index into the type list
        int         tool{ -1 }; // -1 == all filaments
        int         layer_from{ 0 };
        int         layer_to{ -1 };
        double      value{ 100. };
        bool        avoid_wipetower{ false };
    };
    struct RuleRow
    {
        ::CheckBox  *enabled{ nullptr };
        ::ComboBox  *type{ nullptr };
        ::ComboBox  *tool{ nullptr };
        ::SpinInput *layer_from{ nullptr };
        ::TextInput *layer_to{ nullptr };
        ::TextInput *value{ nullptr };
        ::CheckBox  *avoid_wipetower{ nullptr };
        ::Button    *remove{ nullptr };
    };

    void        load(const std::string &rules_json);
    std::string to_json() const;
    void        collect_rows();
    void        rebuild_rows();

    bool                    m_master_enabled{ true };
    ::CheckBox             *m_master{ nullptr };
    wxScrolledWindow       *m_rows_panel{ nullptr };
    wxFlexGridSizer        *m_rows_sizer{ nullptr };
    std::vector<RuleData>   m_rules;
    std::vector<RuleRow>    m_rows;
    std::vector<nlohmann::json> m_unknown_rules; // rules of types this build does not edit, kept as they are
    size_t                  m_filament_count{ 1 };
};

}} // namespace Slic3r::GUI
