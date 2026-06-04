// Theme — see header. Resolves the OKLCH token values from DESIGN.md §2.6/§2.7
// to sRGB and assembles the application QSS.
#include "Theme.h"

#include "Color.h"

#include <QtGui/QFontDatabase>
#include <QtWidgets/QApplication>

namespace chainapi::desktop::theming {

namespace {

// All neutrals are tinted toward the accent hue (DESIGN.md §2.3): indigo-violet.
constexpr double kAccentHue = 285.0;

[[nodiscard]] Palette resolveLight() {
    Palette p;
    // DESIGN.md §2.6 — light theme values.
    p.surfaceBase = oklch(0.985, 0.003, kAccentHue);
    p.surfaceRaised = oklch(0.995, 0.002, kAccentHue);
    p.surfaceSunken = oklch(0.960, 0.004, kAccentHue);
    p.surfaceOverlay = oklch(0.995, 0.002, kAccentHue);
    p.borderSubtle = oklch(0.900, 0.005, kAccentHue);
    p.borderStrong = oklch(0.780, 0.008, kAccentHue);
    p.textPrimary = oklch(0.240, 0.010, kAccentHue);
    p.textSecondary = oklch(0.470, 0.009, kAccentHue);
    p.textDisabled = oklch(0.680, 0.006, kAccentHue);
    p.textInverse = oklch(0.985, 0.002, kAccentHue);
    p.accentBase = oklch(0.550, 0.190, kAccentHue);
    p.accentHover = oklch(0.480, 0.200, kAccentHue);
    p.accentMuted = oklch(0.950, 0.030, kAccentHue);

    p.statusIdle = oklch(0.680, 0.006, kAccentHue);
    p.statusRunning = oklch(0.600, 0.140, 230.0);
    p.statusSuccess = oklch(0.560, 0.150, 150.0);
    p.statusWarning = oklch(0.700, 0.150, 75.0);
    p.statusError = oklch(0.560, 0.200, 27.0);
    p.statusCancelled = oklch(0.550, 0.010, kAccentHue);
    p.statusBlocked = oklch(0.560, 0.150, 320.0);

    // Tint tokens (§2.9): accent hue, surface-level lightness, opaque.
    p.tintCache = oklch(0.955, 0.020, kAccentHue);
    p.tintSubstituted = oklch(0.945, 0.035, kAccentHue);
    // Diff tints: success/error hue at a light, low-saturation surface
    // lightness so text stays readable on top (§2.9 / §6.4).
    p.tintDiffAdd = oklch(0.930, 0.040, 150.0);
    p.tintDiffRemove = oklch(0.930, 0.045, 27.0);

    // HTTP method vocabulary (§6.2a): mnemonic hues, distinct from status.
    // GET blue(255), POST green(150), PUT orange(55), PATCH yellow(95),
    // DELETE red(27). Mid lightness for AA text on a light surface.
    p.methodGet = oklch(0.550, 0.150, 255.0);
    p.methodPost = oklch(0.560, 0.150, 150.0);
    p.methodPut = oklch(0.600, 0.150, 55.0);
    p.methodPatch = oklch(0.620, 0.140, 95.0);
    p.methodDelete = oklch(0.560, 0.200, 27.0);
    return p;
}

[[nodiscard]] Palette resolveDark() {
    Palette p;
    // DESIGN.md §2.7 — dark theme values (brighter accents/status for 4.5:1).
    p.surfaceBase = oklch(0.180, 0.006, kAccentHue);
    p.surfaceRaised = oklch(0.220, 0.007, kAccentHue);
    p.surfaceSunken = oklch(0.140, 0.006, kAccentHue);
    p.surfaceOverlay = oklch(0.260, 0.008, kAccentHue);
    p.borderSubtle = oklch(0.300, 0.008, kAccentHue);
    p.borderStrong = oklch(0.420, 0.010, kAccentHue);
    p.textPrimary = oklch(0.940, 0.004, kAccentHue);
    p.textSecondary = oklch(0.680, 0.006, kAccentHue);
    p.textDisabled = oklch(0.480, 0.006, kAccentHue);
    p.textInverse = oklch(0.180, 0.006, kAccentHue);
    p.accentBase = oklch(0.680, 0.170, kAccentHue);
    p.accentHover = oklch(0.750, 0.160, kAccentHue);
    p.accentMuted = oklch(0.300, 0.060, kAccentHue);

    p.statusIdle = oklch(0.480, 0.006, kAccentHue);
    p.statusRunning = oklch(0.750, 0.130, 230.0);
    p.statusSuccess = oklch(0.780, 0.160, 150.0);
    p.statusWarning = oklch(0.800, 0.150, 75.0);
    p.statusError = oklch(0.700, 0.180, 27.0);
    p.statusCancelled = oklch(0.680, 0.010, kAccentHue);
    p.statusBlocked = oklch(0.720, 0.160, 320.0);

    p.tintCache = oklch(0.280, 0.040, kAccentHue);
    p.tintSubstituted = oklch(0.300, 0.055, kAccentHue);
    // Diff tints: success/error hue at a low, low-saturation surface lightness
    // so light text stays readable on top in dark mode (§2.9 / §6.4).
    p.tintDiffAdd = oklch(0.300, 0.050, 150.0);
    p.tintDiffRemove = oklch(0.310, 0.060, 27.0);

    // HTTP method vocabulary (§6.2a): brighter in dark to clear AA on the
    // raised surface, same hues as light (§6.2a).
    p.methodGet = oklch(0.720, 0.140, 255.0);
    p.methodPost = oklch(0.780, 0.160, 150.0);
    p.methodPut = oklch(0.770, 0.150, 55.0);
    p.methodPatch = oklch(0.820, 0.150, 95.0);
    p.methodDelete = oklch(0.700, 0.180, 27.0);
    return p;
}

}  // namespace

Theme Theme::resolve(Appearance appearance) {
    Theme theme;
    theme.appearance_ = appearance;
    theme.palette_ = (appearance == Appearance::Dark) ? resolveDark() : resolveLight();
    return theme;
}

QColor Theme::status(StatusToken token) const noexcept {
    switch (token) {
        case StatusToken::Idle:
            return palette_.statusIdle;
        case StatusToken::Running:
            return palette_.statusRunning;
        case StatusToken::Success:
            return palette_.statusSuccess;
        case StatusToken::Warning:
            return palette_.statusWarning;
        case StatusToken::Error:
            return palette_.statusError;
        case StatusToken::Cancelled:
            return palette_.statusCancelled;
        case StatusToken::Blocked:
            return palette_.statusBlocked;
        case StatusToken::Skipped:
            return palette_.statusIdle;
    }
    return palette_.statusIdle;
}

QColor Theme::statusTint(StatusToken token) const noexcept {
    // Opaque mix of the status colour toward the raised surface — a precomputed
    // tint rather than an alpha composite (DESIGN.md §2.9), so contrast is
    // predictable regardless of what sits behind the pill. ~16% status weight.
    const QColor base = status(token);
    const QColor surface = palette_.surfaceRaised;
    constexpr double kStatusWeight = 0.16;
    const auto mix = [&](int s, int b) {
        return static_cast<int>((s * kStatusWeight) + (b * (1.0 - kStatusWeight)));
    };
    return QColor{mix(base.red(), surface.red()),
                  mix(base.green(), surface.green()),
                  mix(base.blue(), surface.blue())};
}

QColor Theme::method(MethodColor token) const noexcept {
    switch (token) {
        case MethodColor::Get:
            return palette_.methodGet;
        case MethodColor::Post:
            return palette_.methodPost;
        case MethodColor::Put:
            return palette_.methodPut;
        case MethodColor::Patch:
            return palette_.methodPatch;
        case MethodColor::Delete:
            return palette_.methodDelete;
        case MethodColor::Neutral:
            return palette_.textSecondary;
    }
    return palette_.textSecondary;
}

QColor Theme::methodTint(MethodColor token) const noexcept {
    // Same opaque-mix technique as statusTint (DESIGN.md §2.9): blend the
    // method hue toward the raised surface so the chip fill is predictable.
    const QColor base = method(token);
    const QColor surface = palette_.surfaceRaised;
    constexpr double kMethodWeight = 0.18;
    const auto mix = [&](int s, int b) {
        return static_cast<int>((s * kMethodWeight) + (b * (1.0 - kMethodWeight)));
    };
    return QColor{mix(base.red(), surface.red()),
                  mix(base.green(), surface.green()),
                  mix(base.blue(), surface.blue())};
}

int Theme::space(Space step) noexcept {
    switch (step) {
        case Space::Xs:
            return 4;
        case Space::Sm:
            return 8;
        case Space::Md:
            return 12;
        case Space::Lg:
            return 16;
        case Space::Xl:
            return 24;
        case Space::Xxl:
            return 32;
    }
    return 8;
}

QFont Theme::font(TextStyle style) const {
    // Derive from the system base font so OS scaling is respected (§4.1/§4.2).
    QFont base = QApplication::font();
    const double basePt = base.pointSizeF() > 0 ? base.pointSizeF() : 13.0;

    const auto scaled = [basePt](double ratio) {
        return basePt * ratio;
    };

    switch (style) {
        case TextStyle::Title:
            base.setPointSizeF(scaled(1.30));
            base.setWeight(QFont::DemiBold);
            break;
        case TextStyle::Subtitle:
            base.setPointSizeF(scaled(1.15));
            base.setWeight(QFont::DemiBold);
            break;
        case TextStyle::Body:
            base.setPointSizeF(scaled(1.00));
            base.setWeight(QFont::Normal);
            break;
        case TextStyle::Label:
            base.setPointSizeF(scaled(0.92));
            base.setWeight(QFont::Medium);
            break;
        case TextStyle::Caption:
            base.setPointSizeF(scaled(0.85));
            base.setWeight(QFont::Normal);
            break;
        case TextStyle::Mono: {
            QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            mono.setPointSizeF(scaled(0.92));
            mono.setStyleHint(QFont::Monospace);
            // Tabular figures so columns of status codes, durations, and byte
            // counts don't jitter as digit widths vary (DESIGN.md §4.3).
            mono.setFeature("tnum", 1);
            return mono;
        }
    }
    return base;
}

QString Theme::styleSheet() const {
    const Palette& p = palette_;
    const auto hex = [](const QColor& c) {
        return c.name(QColor::HexRgb);
    };

    // Spacing pulled from the scale so QSS padding stays consistent with §5.1.
    const int sm = space(Space::Sm);
    const int md = space(Space::Md);
    const int xs = space(Space::Xs);

    // One central sheet, token-derived (DESIGN.md §13: no inline per-widget
    // styles, no raw literals). Object-name selectors target specific roles.
    return QStringLiteral(R"(
QWidget {
    background-color: %1;
    color: %2;
}
QMainWindow, QDialog {
    background-color: %1;
}

/* Panels sit one level up from the window base (§2.4 surface.raised). */
QSplitter::handle {
    background-color: %3;
}
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

/* Toolbar + menu chrome */
QToolBar {
    background-color: %4;
    border: none;
    border-bottom: 1px solid %3;
    padding: %5px;
    spacing: %6px;
}
QToolBar QToolButton {
    background-color: transparent;
    color: %2;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: %7px %5px;
}
QToolBar QToolButton:hover {
    background-color: %8;
}
QToolBar QToolButton:pressed {
    background-color: %9;
}
QMenuBar {
    background-color: %4;
    border-bottom: 1px solid %3;
}
QMenuBar::item { padding: %7px %5px; background: transparent; }
QMenuBar::item:selected { background-color: %8; border-radius: 4px; }
QMenu {
    background-color: %10;
    border: 1px solid %3;
    padding: %7px;
}
QMenu::item { padding: %7px %6px; border-radius: 4px; }
QMenu::item:selected { background-color: %11; color: %12; }

QStatusBar {
    background-color: %4;
    border-top: 1px solid %3;
    color: %13;
}

/* Surface elevation (DESIGN.md §2.8: depth from surface lightness, not
   shadow). The explorer sits on the deepest surface.base; the center and
   right workspaces are elevated onto surface.raised so the three panes read
   as distinct layers. Set on the panels via object name + WA_StyledBackground. */
QWidget#explorerPanel {
    background-color: %1;
    border-right: 1px solid %3;
}
QWidget#workspacePanel {
    background-color: %4;
}

/* Request "address bar": method + path + Send, pinned to the top of the
   center pane (Postman/Apidog convention). A single sunken field strip — the
   one framed element in the editor; everything below it is flat. */
QWidget#requestLineBar {
    background-color: %14;
    border: 1px solid %3;
    border-radius: 8px;
}

/* Inputs inside the address bar are chromeless — the bar is the single frame.
   The method combo keeps a trailing divider so it reads as a segment. */
QWidget#requestLineBar QLineEdit {
    background-color: transparent;
    border: none;
    border-radius: 0;
    padding-left: 8px;
}
QWidget#requestLineBar QComboBox {
    background-color: transparent;
    border: none;
    border-radius: 0;
}
/* Keep a visible focus state for keyboard users (DESIGN.md §11.1 / NFR-5.1):
   a 1px accent underline rather than a full ring, so it still reads as part of
   the single bar frame. */
QWidget#requestLineBar QLineEdit:focus,
QWidget#requestLineBar QComboBox:focus {
    border: none;
    border-bottom: 1px solid %15;
}
QWidget#requestLineBar QComboBox#methodCombo {
    border: none;
    border-right: 1px solid %3;
    border-radius: 0;
    padding: 0 12px;
}
QWidget#requestLineBar QComboBox#methodCombo:focus {
    border: none;
    border-right: 1px solid %3;
    border-bottom: 1px solid %15;
}

/* Method pill badge in the request address bar. Per-method colour comes from
   the dedicated method vocabulary (DESIGN.md §6.2a) via a dynamic `methodClass`
   property — a mnemonic hue per verb, distinct from the status palette so a
   method chip never reads as a run state. Backgrounds are the opaque methodTint. */
QLabel#methodPill {
    margin-left: 8px;
    margin-top: 4px;
    margin-bottom: 4px;
    border-radius: 4px;
    padding: 2px 8px;
    font-weight: 600;
}
QLabel#methodPill[methodClass="get"]    { background-color: %20; color: %25; }
QLabel#methodPill[methodClass="post"]   { background-color: %21; color: %26; }
QLabel#methodPill[methodClass="put"]    { background-color: %22; color: %27; }
QLabel#methodPill[methodClass="patch"]  { background-color: %23; color: %28; }
QLabel#methodPill[methodClass="delete"] { background-color: %24; color: %29; }

/* Trees, lists, tables — the data surfaces. Flat: no card border, separated
   by surface colour and panel dividers (Apidog/Linear read, DESIGN.md §15 "no
   nested cards"). */
QTreeWidget, QTreeView, QListWidget, QListView, QTableWidget, QTableView {
    background-color: %4;
    alternate-background-color: %4;
    border: none;
    outline: none;
}
QTreeWidget::item, QListWidget::item, QTableWidget::item {
    padding: %7px %6px;
    border: none;
}
QTreeWidget::item:hover, QListWidget::item:hover {
    background-color: %8;
}
QTreeWidget::item:selected, QListWidget::item:selected,
QTableWidget::item:selected {
    background-color: %11;
    color: %2;
}
QHeaderView::section {
    background-color: %1;
    color: %13;
    border: none;
    border-bottom: 1px solid %3;
    padding: %7px %6px;
}

/* Inputs. Generous internal padding (sm vertical, md horizontal) so values
   never crowd the field edge — the single biggest "looks professional" win. */
QLineEdit, QPlainTextEdit, QTextEdit {
    background-color: %14;
    color: %2;
    border: 1px solid %3;
    border-radius: 6px;
    padding: %5px %6px;
    selection-background-color: %11;
    selection-color: %2;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus {
    border: 1px solid %15;
}
QComboBox {
    background-color: %14;
    color: %2;
    border: 1px solid %3;
    border-radius: 6px;
    padding: %5px %6px;
    min-height: 20px;
}
QComboBox:focus { border: 1px solid %15; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background-color: %10;
    border: 1px solid %3;
    selection-background-color: %11;
    selection-color: %2;
    outline: none;
}

/* Buttons: secondary by default, the primary one opts in by object name */
QPushButton {
    background-color: %4;
    color: %2;
    border: 1px solid %16;
    border-radius: 6px;
    padding: %7px %17px;
    min-height: 22px;
}
QPushButton:hover { background-color: %8; }
QPushButton:pressed { background-color: %9; }
QPushButton:disabled { color: %18; border-color: %3; }
QPushButton:focus { border: 1px solid %15; }

QPushButton#primaryAction {
    background-color: %15;
    color: %12;
    border: 1px solid %15;
    font-weight: 600;
}
QPushButton#primaryAction:hover { background-color: %19; border-color: %19; }
QPushButton#primaryAction:disabled { background-color: %3; border-color: %3; color: %18; }

/* Ghost action: a low-emphasis button (no fill, no border at rest) for rare
   actions like Save to Project, so it never competes with Send (DESIGN.md
   §7.2 button hierarchy). */
QPushButton#ghostAction {
    background-color: transparent;
    border: 1px solid transparent;
    color: %13;
}
QPushButton#ghostAction:hover { background-color: %8; color: %2; }
QPushButton#ghostAction:disabled { color: %18; border-color: transparent; }

/* Actor session chip: a first-class concept (§8), so it reads as a tinted
   pill, not faint metadata. The accent-muted fill ties it to the run identity;
   the "no actor" variant stays neutral. */
QLabel#actorChip {
    background-color: %11;
    color: %2;
    border-radius: 10px;
    padding: 2px 10px;
}
QLabel#actorChip[hasActor="false"] {
    background-color: transparent;
    color: %13;
    padding: 2px 0;
}

/* Checkboxes */
QCheckBox { color: %2; spacing: %6px; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    border: 1px solid %16;
    border-radius: 4px;
    background-color: %14;
}
QCheckBox::indicator:checked {
    background-color: %15;
    border-color: %15;
}
QCheckBox:focus { color: %2; }

/* Tabs — flat, underline-indicator only (no boxed pane). */
QTabWidget::pane {
    border: none;
    border-top: 1px solid %3;
    top: -1px;
}
QTabBar::tab {
    background-color: transparent;
    color: %13;
    padding: %5px %6px;
    border: none;
    border-bottom: 2px solid transparent;
}
QTabBar::tab:hover { color: %2; }
QTabBar::tab:selected {
    color: %2;
    border-bottom: 2px solid %15;
}

/* Group boxes — section containers in the request editor */
QGroupBox {
    border: 1px solid %3;
    border-radius: 6px;
    margin-top: %6px;
    padding-top: %6px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: %6px;
    padding: 0 %7px;
    color: %13;
}

/* Scrollbars — slim, neutral */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical {
    background-color: %16;
    border-radius: 5px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover { background-color: %13; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal {
    background-color: %16;
    border-radius: 5px;
    min-width: 24px;
}
QScrollBar::handle:horizontal:hover { background-color: %13; }

/* Command palette: a floating overlay surface, soft border, no window chrome */
QWidget#commandPalette {
    background-color: %10;
    border: 1px solid %16;
    border-radius: 10px;
}
QLineEdit#paletteSearch {
    background-color: %14;
    border: 1px solid %3;
    border-radius: 6px;
    padding: %6px;
}
QLineEdit#paletteSearch:focus { border: 1px solid %15; }
QListWidget#paletteList {
    background-color: transparent;
    border: none;
}
QListWidget#paletteList::item {
    padding: %7px %6px;
    border-radius: 6px;
}
QListWidget#paletteList::item:selected {
    background-color: %11;
    color: %2;
}

/* Section headings inside panels: a light subtitle, not a framed group box
   (DESIGN.md §5 bans nested cards). */
QLabel[role="sectionHeading"] {
    color: %13;
    font-weight: 600;
    padding-top: %7px;
}

/* KeyValueEditor: a flat table — captions header with a divider, rows with a
   subtle hover, no card border (Apidog read). */
QWidget#kvCaptions {
    border-bottom: 1px solid %3;
}
QWidget#kvCaptions QLabel {
    padding-left: 8px; /* align header text with input text */
}
QWidget#kvRow {
    border-bottom: 1px solid %3;
    background-color: transparent;
}
QWidget#kvRow:hover {
    background-color: %8;
}
QWidget#kvRow QLineEdit {
    background-color: transparent;
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 6px 8px;
}
QWidget#kvRow QLineEdit:focus {
    background-color: %14;
    border: 1px solid %15;
}
QWidget#kvRow QToolButton {
    background-color: transparent;
    border: none;
    border-radius: 4px;
    color: %13;
    font-weight: bold;
    padding: 2px;
    min-width: 20px;
    max-width: 20px;
    min-height: 20px;
    max-height: 20px;
}
QWidget#kvRow QToolButton:hover {
    background-color: %8;
    color: %2;
}
QWidget#kvRow QToolButton:pressed {
    background-color: %9;
}

QToolTip {
    background-color: %10;
    color: %2;
    border: 1px solid %3;
    padding: %7px;
}

/* Compact density (DESIGN.md §5.3): tighten tree/list row padding for users
   with hundreds of operations. Keyed on a dynamic property the shell sets. */
QWidget[density="compact"] QTreeWidget::item,
QWidget[density="compact"] QListWidget::item,
QWidget[density="compact"] QTableWidget::item {
    padding: 1px %7px;
}

/* Execution chain: a flat sunken panel; nodes carry their own method pills. */
QWidget#chainView {
    background-color: %14; /* surfaceSunken */
    border-radius: 6px;
}
)")
        .arg(hex(p.surfaceBase),                    // %1
             hex(p.textPrimary),                    // %2
             hex(p.borderSubtle),                   // %3
             hex(p.surfaceRaised),                  // %4
             QString::number(sm),                   // %5
             QString::number(md),                   // %6
             QString::number(xs))                   // %7
        .arg(hex(p.accentMuted),                    // %8
             hex(p.borderStrong),                   // %9
             hex(p.surfaceOverlay),                 // %10
             hex(p.accentMuted),                    // %11  selected row tint
             hex(p.textInverse),                    // %12
             hex(p.textSecondary),                  // %13
             hex(p.surfaceSunken),                  // %14
             hex(p.accentBase))                     // %15
        .arg(hex(p.borderStrong),                   // %16
             QString::number(space(Space::Lg)),     // %17
             hex(p.textDisabled),                   // %18
             hex(p.accentHover))                    // %19
        .arg(hex(methodTint(MethodColor::Get)),     // %20  GET pill bg
             hex(methodTint(MethodColor::Post)),    // %21  POST pill bg
             hex(methodTint(MethodColor::Put)),     // %22  PUT pill bg
             hex(methodTint(MethodColor::Patch)),   // %23  PATCH pill bg
             hex(methodTint(MethodColor::Delete)),  // %24  DELETE pill bg
             hex(method(MethodColor::Get)),         // %25  GET pill fg
             hex(method(MethodColor::Post)),        // %26  POST pill fg
             hex(method(MethodColor::Put)),         // %27  PUT pill fg
             hex(method(MethodColor::Patch)),       // %28  PATCH pill fg
             hex(method(MethodColor::Delete)));     // %29  DELETE pill fg
}

}  // namespace chainapi::desktop::theming
