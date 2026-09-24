//go:build gui

// The window's theme.
//
// Fyne's stock theme is a general-purpose one: rounded, roomy, mid-blue, sized
// for touch. This window is an instrument panel for a board on a bench -- it is
// a path, a log, and one button that can brick hardware -- and the stock look
// made that read like a settings dialog. So the palette, the metrics and the
// type are set here rather than inherited.
//
// Four rules the numbers below follow:
//
//   - The whole thing is metal. Every surface is a dim neutral grey, none of it
//     black and none of it white, and the surfaces differ only in value -- a
//     recessed pane is darker, a raised control is lighter, a bevel is lighter
//     still. That is what makes it read as one machined panel instead of a page
//     with widgets on it.
//   - Only three surfaces. Page, panel, and input. Anything that needs to stand
//     out more than that gets a border or the accent, not a fourth grey.
//   - The accent is polished metal, and it is a state rather than decoration.
//     The one accented thing in the window is the FLASH key; amber means caution
//     and red means it failed. Nothing is tinted for looks, so a coloured pixel
//     is always worth reading.
//   - Corners stay nearly square (3-4px). A flasher that looks like a phone app
//     invites phone-app confidence about what the FLASH button does.
package main

import (
	"image/color"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/theme"
)

// guiPalette is the window's colour. There is one of these and it is used for
// both theme variants on purpose: a light-mode host would otherwise turn this
// into a white page, and the panel is meant to look like the aluminium the board
// is sitting on no matter what the desktop around it is doing.
type guiPalette struct {
	page        color.NRGBA // the window itself
	panel       color.NRGBA // the log pane
	input       color.NRGBA // entries, unpressed buttons
	hover       color.NRGBA
	pressed     color.NRGBA
	border      color.NRGBA
	separator   color.NRGBA
	text        color.NRGBA
	textMuted   color.NRGBA // placeholders, secondary labels
	textOnAcc   color.NRGBA
	accent      color.NRGBA // polished metal: the FLASH key, and focus
	accentSoft  color.NRGBA // selection/focus wash: the accent at low alpha
	success     color.NRGBA
	warning     color.NRGBA
	failure     color.NRGBA
	scrollBar   color.NRGBA
	shadow      color.NRGBA
	disabled    color.NRGBA
	disabledBox color.NRGBA
}

// guiMetalPalette is a value ramp through one neutral grey, lit from the top the
// way a horizontal brushed face is: the page is the chassis, the log pane is
// milled into it, the entry and the buttons stand proud of it, and the borders
// are the bright edge where the cut catches the light.
//
// Nothing here is 0x00 or 0xff except the glyphs, which have to be legible. The
// darkest surface is 0x3a and the lightest is 0xb4 -- all of it in the middle of
// the range, which is what "metal" means here rather than "dark mode".
var guiMetalPalette = guiPalette{
	page:      color.NRGBA{R: 0x4e, G: 0x51, B: 0x55, A: 0xff}, // brushed face
	panel:     color.NRGBA{R: 0x3f, G: 0x42, B: 0x46, A: 0xff}, // milled recess
	input:     color.NRGBA{R: 0x5c, G: 0x60, B: 0x65, A: 0xff}, // raised key
	hover:     color.NRGBA{R: 0x68, G: 0x6c, B: 0x72, A: 0xff},
	pressed:   color.NRGBA{R: 0x3c, G: 0x3f, B: 0x43, A: 0xff}, // pressed = recessed
	border:    color.NRGBA{R: 0x73, G: 0x78, B: 0x7e, A: 0xff}, // the lit edge
	separator: color.NRGBA{R: 0x62, G: 0x66, B: 0x6b, A: 0xff},
	text:      color.NRGBA{R: 0xf1, G: 0xf3, B: 0xf5, A: 0xff},
	textMuted: color.NRGBA{R: 0xbe, G: 0xc3, B: 0xc9, A: 0xff},
	// Dark engraving on the polished key, the way legends on a machined button are
	// cut into it rather than printed on top.
	textOnAcc: color.NRGBA{R: 0x1e, G: 0x21, B: 0x24, A: 0xff},
	// Polished aluminium. The FLASH key is the only accented thing in the window,
	// and being the brightest surface in it is the whole of how it stands out --
	// no hue, because a blue or a red key would stop looking like part of the
	// panel.
	accent:      color.NRGBA{R: 0xb0, G: 0xb6, B: 0xbc, A: 0xff},
	accentSoft:  color.NRGBA{R: 0xd2, G: 0xd8, B: 0xde, A: 0x3d},
	success:     color.NRGBA{R: 0x7a, G: 0xd2, B: 0x9c, A: 0xff},
	warning:     color.NRGBA{R: 0xe6, G: 0xbb, B: 0x66, A: 0xff},
	failure:     color.NRGBA{R: 0xf0, G: 0x84, B: 0x7f, A: 0xff},
	scrollBar:   color.NRGBA{R: 0x8e, G: 0x94, B: 0x9a, A: 0xcc},
	shadow:      color.NRGBA{A: 0x66},
	disabled:    color.NRGBA{R: 0x8b, G: 0x90, B: 0x96, A: 0xff},
	disabledBox: color.NRGBA{R: 0x4a, G: 0x4d, B: 0x51, A: 0xff},
}

// guiTheme is the window's fyne.Theme. It answers every colour and metric it has
// an opinion about and defers the rest -- icons and fonts -- to the stock theme,
// which already ships a decent set of both.
type guiTheme struct{}

var _ fyne.Theme = guiTheme{}

// palette ignores the variant. See guiPalette: the panel is the same metal in a
// light-mode session as in a dark-mode one, which also means nothing in the
// window has to react to the host switching between them.
func (guiTheme) palette(fyne.ThemeVariant) guiPalette {
	return guiMetalPalette
}

func (t guiTheme) Color(name fyne.ThemeColorName, variant fyne.ThemeVariant) color.Color {
	p := t.palette(variant)
	switch name {
	case theme.ColorNameBackground:
		return p.page
	// Buttons and entries share the input surface, so a row of them reads as one
	// control strip instead of a scatter of chips.
	case theme.ColorNameButton, theme.ColorNameInputBackground:
		return p.input
	case theme.ColorNameHeaderBackground, theme.ColorNameMenuBackground,
		theme.ColorNameOverlayBackground:
		return p.panel
	case theme.ColorNameHover:
		return p.hover
	case theme.ColorNamePressed:
		return p.pressed
	case theme.ColorNameInputBorder, theme.ColorNameInnerWindowBorder:
		return p.border
	case theme.ColorNameInnerWindowBorderInactive, theme.ColorNameSeparator:
		return p.separator
	case theme.ColorNameForeground:
		return p.text
	case theme.ColorNamePlaceHolder:
		return p.textMuted
	case theme.ColorNamePrimary, theme.ColorNameHyperlink:
		return p.accent
	// Focus and selection are the same accent at low alpha rather than a
	// separate hue, so a focused entry looks lit rather than recoloured.
	case theme.ColorNameFocus, theme.ColorNameSelection:
		return p.accentSoft
	case theme.ColorNameSuccess:
		return p.success
	case theme.ColorNameWarning:
		return p.warning
	case theme.ColorNameError:
		return p.failure
	case theme.ColorNameForegroundOnPrimary, theme.ColorNameForegroundOnError,
		theme.ColorNameForegroundOnSuccess, theme.ColorNameForegroundOnWarning:
		return p.textOnAcc
	case theme.ColorNameScrollBar:
		return p.scrollBar
	case theme.ColorNameScrollBarBackground:
		return color.NRGBA{}
	case theme.ColorNameShadow:
		return p.shadow
	case theme.ColorNameDisabled:
		return p.disabled
	case theme.ColorNameDisabledButton:
		return p.disabledBox
	default:
		return theme.DefaultTheme().Color(name, variant)
	}
}

func (guiTheme) Font(style fyne.TextStyle) fyne.Resource {
	return theme.DefaultTheme().Font(style)
}

func (guiTheme) Icon(name fyne.ThemeIconName) fyne.Resource {
	return theme.DefaultTheme().Icon(name)
}

func (guiTheme) Size(name fyne.ThemeSizeName) float32 {
	switch name {
	case theme.SizeNameText:
		return 13
	case theme.SizeNameCaptionText:
		return 11
	case theme.SizeNameSubHeadingText:
		return 15
	case theme.SizeNameHeadingText:
		return 18
	// Padding is the single biggest reason the stock look felt like a phone
	// screen: at 4 the rows below sit close enough to read as a form.
	case theme.SizeNamePadding:
		return 4
	case theme.SizeNameInnerPadding:
		return 7
	case theme.SizeNameLineSpacing:
		return 3
	case theme.SizeNameInputBorder:
		return 1
	case theme.SizeNameSeparatorThickness:
		return 1
	case theme.SizeNameScrollBar:
		return 10
	case theme.SizeNameScrollBarSmall:
		return 4
	case theme.SizeNameScrollBarRadius:
		return 5
	// Near-square corners. See the file header: this is a deliberate signal.
	case theme.SizeNameInputRadius, theme.SizeNameButtonRadius,
		theme.SizeNameSelectionRadius:
		return 3
	case theme.SizeNameCardRadius, theme.SizeNameDialogRadius,
		theme.SizeNamePopupRadius, theme.SizeNameMenuRadius:
		return 4
	default:
		return theme.DefaultTheme().Size(name)
	}
}

// guiStatusColor maps the window's own notion of severity onto the theme, so the
// status line and the log pane cannot drift apart on what "bad" looks like.
func guiStatusColor(level guiLevel) fyne.ThemeColorName {
	switch level {
	case guiLevelBusy:
		return theme.ColorNamePrimary
	case guiLevelGood:
		return theme.ColorNameSuccess
	case guiLevelWarn:
		return theme.ColorNameWarning
	case guiLevelBad:
		return theme.ColorNameError
	default:
		return theme.ColorNamePlaceHolder
	}
}

// guiLevel is the severity of whatever the status line is currently reporting.
type guiLevel int

const (
	guiLevelIdle guiLevel = iota
	guiLevelBusy
	guiLevelGood
	guiLevelWarn
	guiLevelBad
)
