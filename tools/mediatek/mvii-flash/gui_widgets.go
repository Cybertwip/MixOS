//go:build gui

// The small presentational pieces the window is assembled from.
//
// Fyne's stock widgets have no notion of "this text is secondary" or "this text
// is a state", so the three things this window needs to say in more than plain
// body type -- a heading, a caption and a status -- are wrapped here instead of
// being spelled out at each use. RichText is the vehicle because it is the only
// stock widget that takes a *theme colour name* rather than a colour, so the
// layout code below never has to name a grey.
package main

import (
	"fmt"
	"image/color"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

func guiStyledText(text string, style widget.RichTextStyle) *widget.RichText {
	rich := widget.NewRichText(&widget.TextSegment{Style: style, Text: text})
	rich.Wrapping = fyne.TextWrapOff
	return rich
}

// guiHeading is the window's one piece of large type.
func guiHeading(text string) *widget.RichText {
	return guiStyledText(text, widget.RichTextStyle{
		SizeName:  theme.SizeNameSubHeadingText,
		TextStyle: fyne.TextStyle{Bold: true},
	})
}

// guiCaption is the window's one piece of secondary prose: the transport line
// under the title. Small and muted, so it never competes with what it annotates.
func guiCaption(text string) *widget.RichText {
	return guiStyledText(text, widget.RichTextStyle{
		SizeName:  theme.SizeNameCaptionText,
		ColorName: theme.ColorNamePlaceHolder,
	})
}

// guiKeyHeight is an invisible spacer that gives whatever it is stacked with a
// minimum height. The FLASH key is the one control in this window, and a key you
// are meant to find without looking is taller than its label plus padding --
// which is all a Fyne button asks for on its own.
func guiKeyHeight(height float32) fyne.CanvasObject {
	spacer := canvas.NewRectangle(color.Transparent)
	spacer.SetMinSize(fyne.NewSize(0, height))
	return spacer
}

func guiSetRichText(rich *widget.RichText, text string) {
	if segment, ok := rich.Segments[0].(*widget.TextSegment); ok {
		if segment.Text == text {
			return
		}
		segment.Text = text
	}
	rich.Refresh()
}

/* ------------------------------------------------------------------------- */
/* Panels                                                                    */
/* ------------------------------------------------------------------------- */

// guiSurface is a panel: the second of the theme's three surfaces, painted with
// the card radius, a hairline border and no shadow.
//
// Fyne's Card cannot do this -- it fills with the *page* colour and separates
// itself with a drop shadow, which is right for something floating above the
// content and wrong for a pane that has to read as recessed into it. Being a
// widget rather than a bare canvas.Rectangle is what makes it follow a light/dark
// switch, since a rectangle holds a resolved colour and never hears about the
// change.
type guiSurface struct {
	widget.BaseWidget
	content fyne.CanvasObject
}

func newGUISurface(content fyne.CanvasObject) *guiSurface {
	surface := &guiSurface{content: content}
	surface.ExtendBaseWidget(surface)
	return surface
}

func (s *guiSurface) CreateRenderer() fyne.WidgetRenderer {
	renderer := &guiSurfaceRenderer{surface: s, rect: canvas.NewRectangle(color.Transparent)}
	renderer.applyTheme()
	return renderer
}

type guiSurfaceRenderer struct {
	surface *guiSurface
	rect    *canvas.Rectangle
}

func (r *guiSurfaceRenderer) applyTheme() {
	r.rect.FillColor = theme.Color(theme.ColorNameHeaderBackground)
	r.rect.StrokeColor = theme.Color(theme.ColorNameInputBorder)
	r.rect.StrokeWidth = theme.Size(theme.SizeNameInputBorder)
	r.rect.CornerRadius = theme.Size(theme.SizeNameCardRadius)
}

func (r *guiSurfaceRenderer) inset() float32 {
	return theme.Size(theme.SizeNameInnerPadding)
}

func (r *guiSurfaceRenderer) Layout(size fyne.Size) {
	r.rect.Resize(size)
	inset := r.inset()
	r.surface.content.Move(fyne.NewPos(inset, inset))
	r.surface.content.Resize(fyne.NewSize(size.Width-2*inset, size.Height-2*inset))
}

func (r *guiSurfaceRenderer) MinSize() fyne.Size {
	inset := r.inset()
	min := r.surface.content.MinSize()
	return fyne.NewSize(min.Width+2*inset, min.Height+2*inset)
}

func (r *guiSurfaceRenderer) Refresh() {
	r.applyTheme()
	r.rect.Refresh()
	r.surface.content.Refresh()
}

func (r *guiSurfaceRenderer) Objects() []fyne.CanvasObject {
	return []fyne.CanvasObject{r.rect, r.surface.content}
}

func (r *guiSurfaceRenderer) Destroy() {}

/* ------------------------------------------------------------------------- */
/* The status line                                                           */
/* ------------------------------------------------------------------------- */

// guiStatusLine is the single line that says what the window is doing, in the
// colour that says how it is going: a dot plus a few words, top right.
//
// The dot is drawn rather than typed. A coloured glyph would depend on the
// bundled font having it, and this is the one pixel in the window an operator
// checks before pressing something irreversible.
type guiStatusLine struct {
	widget fyne.CanvasObject
	dot    *canvas.Circle
	text   *widget.RichText
}

func newGUIStatusLine() *guiStatusLine {
	status := &guiStatusLine{
		dot: canvas.NewCircle(color.Transparent),
		text: guiStyledText("", widget.RichTextStyle{
			ColorName: theme.ColorNamePlaceHolder,
		}),
	}
	// A fixed 9px box keeps the dot a dot: circles otherwise stretch to whatever
	// the row's height turns out to be.
	dot := container.NewGridWrap(fyne.NewSize(9, 9), status.dot)
	status.widget = container.NewHBox(container.NewCenter(dot), status.text)
	status.set(guiLevelIdle, "")
	return status
}

// set is safe to call before the window is shown and from fyne.Do afterwards; it
// resolves the dot's colour on the spot, which it can do because the palette is
// fixed (see gui_theme.go) and cannot change under it.
func (s *guiStatusLine) set(level guiLevel, format string, args ...any) {
	name := guiStatusColor(level)
	s.dot.FillColor = theme.Color(name)
	s.dot.Refresh()
	if segment, ok := s.text.Segments[0].(*widget.TextSegment); ok {
		segment.Text = fmt.Sprintf(format, args...)
		segment.Style.ColorName = name
	}
	s.text.Refresh()
}
