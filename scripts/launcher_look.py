"""The look of scripts/launcher.py: colours, ttk's clam theme styled to them, and the pieces ttk has no style for (the
banner, rounded buttons, the navigation). Standard library only.

Why clam styled by hand and not a theme package: every colour of every widget is set here, so the accent can be the
game's red, and nothing depends on fonts only one system has.
"""
import sys
import tkinter as tk
from tkinter import font as tkfont
from tkinter import ttk

BG = '#141416'          # window
PANEL = '#1c1c20'       # navigation
CARD = '#232328'        # cards
FIELD = '#2e2e35'       # entries, lists
LINE = '#3a3a42'
TEXT = '#ececf1'
MUTED = '#9a9aa6'
ACCENT = '#e4000f'      # the red of the game's logo
ACCENT_HOVER = '#ff2a35'
ACCENT_OFF = '#4a2a2d'
SECOND = '#34343c'
SECOND_HOVER = '#40404a'


def ui_family():
    """A UI font the system has: Tk's own default is a bitmap font on many Linux desktops."""
    families = set(tkfont.families())
    if len(families) < 5 and not ui_family.warned:  # a Tk built without Xft (uv's standalone Python on Linux): no desktop fonts
        ui_family.warned = True
        print('launcher: this Python\'s Tk sees no desktop fonts, so the window falls back to a bitmap font; a virtual '
              'environment made from the system\'s Python (its tk package) shows it as intended', file=sys.stderr)
    for f in ('Segoe UI', 'SF Pro Text', 'Inter', 'Noto Sans', 'Cantarell', 'Ubuntu', 'DejaVu Sans'):
        if f in families:
            return f
    return tkfont.nametofont('TkDefaultFont').actual()['family']


ui_family.warned = False


def apply_theme(root):
    """Style ttk (clam) in the colours above. Returns the font family used."""
    family = ui_family()
    for name in ('TkDefaultFont', 'TkTextFont', 'TkMenuFont', 'TkHeadingFont', 'TkCaptionFont'):
        tkfont.nametofont(name).configure(family=family, size=10)
    st = ttk.Style(root)
    st.theme_use('clam')
    st.configure('.', background=BG, foreground=TEXT, fieldbackground=FIELD, bordercolor=LINE, lightcolor=FIELD,
                 darkcolor=FIELD, troughcolor=FIELD, selectbackground=ACCENT, selectforeground='#ffffff',
                 insertcolor=TEXT, focuscolor=ACCENT, font=(family, 10))
    st.map('.', foreground=[('disabled', '#5c5c66')])
    for w in ('TFrame', 'TLabel', 'TCheckbutton'):
        st.configure('Card.' + w, background=CARD)
    st.configure('Muted.TLabel', foreground=MUTED)
    st.configure('Card.Muted.TLabel', background=CARD, foreground=MUTED)
    st.configure('CardTitle.TLabel', background=CARD, foreground=TEXT, font=(family, 12, 'bold'))
    st.configure('PageTitle.TLabel', background=BG, foreground=TEXT, font=(family, 16, 'bold'))
    st.configure('TEntry', padding=(8, 6), bordercolor=LINE)
    st.map('TEntry', bordercolor=[('focus', ACCENT)], lightcolor=[('focus', ACCENT)])
    st.configure('TCombobox', padding=(8, 5), arrowcolor=TEXT, background=FIELD, bordercolor=LINE, arrowsize=14)
    st.map('TCombobox', fieldbackground=[('readonly', FIELD)], foreground=[('readonly', TEXT)],
           background=[('active', SECOND_HOVER), ('readonly', FIELD)], bordercolor=[('focus', ACCENT)],
           selectbackground=[('readonly', FIELD)], selectforeground=[('readonly', TEXT)])
    st.configure('Accent.Horizontal.TProgressbar', troughcolor=FIELD, background=ACCENT, bordercolor=FIELD,
                 lightcolor=ACCENT, darkcolor=ACCENT, thickness=8)
    st.configure('Vertical.TScrollbar', background=SECOND, troughcolor=CARD, bordercolor=CARD, arrowcolor=MUTED,
                 lightcolor=SECOND, darkcolor=SECOND, gripcount=0)
    st.map('Vertical.TScrollbar', background=[('active', SECOND_HOVER)])
    root.configure(background=BG)
    root.option_add('*TCombobox*Listbox.background', FIELD)
    root.option_add('*TCombobox*Listbox.foreground', TEXT)
    root.option_add('*TCombobox*Listbox.selectBackground', ACCENT)
    root.option_add('*TCombobox*Listbox.selectForeground', '#ffffff')
    root.option_add('*TCombobox*Listbox.font', (family, 10))
    return family


def rounded(canvas, x0, y0, x1, y1, r, **kw):
    pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
    return canvas.create_polygon(pts, smooth=True, **kw)


class Button(tk.Canvas):
    """A rounded flat button: kind 'accent' (red) or 'second' (grey)."""

    def __init__(self, parent, text, command, kind='second', width=120, height=36, size=10, bold=False, bg=CARD, family=None):
        super().__init__(parent, width=width, height=height, background=bg, highlightthickness=0, borderwidth=0, cursor='hand2')
        self.command, self.kind, self.enabled, self.hover = command, kind, True, False
        self.w, self.h = width, height
        self.font = (family or ui_family(), size, 'bold' if bold else 'normal')
        self.text = text
        self.bind('<Enter>', lambda e: self.set_hover(True))
        self.bind('<Leave>', lambda e: self.set_hover(False))
        self.bind('<ButtonRelease-1>', lambda e: self.enabled and self.command())
        self.draw()

    def colours(self):
        if self.kind == 'accent':
            return (ACCENT_HOVER if self.hover else ACCENT, '#ffffff') if self.enabled else (ACCENT_OFF, '#8a6a6c')
        return (SECOND_HOVER if self.hover else SECOND, TEXT) if self.enabled else ('#2a2a30', '#5c5c66')

    def draw(self):
        self.delete('all')
        fill, fg = self.colours()
        rounded(self, 1, 1, self.w - 1, self.h - 1, min(12, self.h // 2), fill=fill, outline=fill)
        self.create_text(self.w // 2, self.h // 2, text=self.text, fill=fg, font=self.font)

    def set_hover(self, on):
        self.hover = on and self.enabled
        self.draw()

    def config(self, text=None, state=None):
        if text is not None:
            self.text = text
        if state is not None:
            self.enabled = state == 'normal'
            self.configure(cursor='hand2' if self.enabled else 'arrow')
        self.draw()


def banner(parent, family, title, subtitle, height=150):
    """The top band: the accent red fading into the window colour, faint rings (the game's moons, abstract), the title."""
    c = tk.Canvas(parent, height=height, background=BG, highlightthickness=0, borderwidth=0)

    def paint(event=None):
        c.delete('bg')
        w = max(c.winfo_width(), 1)
        r0, g0, b0 = int(ACCENT[1:3], 16), int(ACCENT[3:5], 16), int(ACCENT[5:7], 16)
        r1, g1, b1 = int(BG[1:3], 16), int(BG[3:5], 16), int(BG[5:7], 16)
        steps = 64
        for i in range(steps):                       # a diagonal fade: strips of the gradient
            t = (i / (steps - 1)) ** 0.8
            col = '#%02x%02x%02x' % (int(r0 + (r1 - r0) * t), int(g0 + (g1 - g0) * t), int(b0 + (b1 - b0) * t))
            x = w * i / steps
            c.create_polygon(x - 80, height, x + w / steps + 40 - 80, height, x + w / steps + 40, 0, x, 0, fill=col, outline=col, tags='bg')
        for cx, cy, r in ((w * 0.62, 30, 110), (w * 0.78, 140, 70), (w * 0.9, 20, 46)):
            c.create_oval(cx - r, cy - r, cx + r, cy + r, outline='#3a1b1f', width=2, tags='bg')
        c.tag_lower('bg')

    c.create_text(28, 58, text=title, anchor='w', fill='#ffffff', font=(family, 26, 'bold'))
    c.create_text(30, 96, text=subtitle, anchor='w', fill='#f2d6d8', font=(family, 11))
    c.bind('<Configure>', paint)
    return c


class Nav(tk.Frame):
    """The page list at the left: a red bar marks the page shown."""

    def __init__(self, parent, family, pages, on_pick):
        super().__init__(parent, background=PANEL, width=190)
        self.pack_propagate(False)
        self.items, self.on_pick = {}, on_pick
        tk.Frame(self, background=PANEL, height=16).pack(fill='x')
        for key, label in pages:
            row = tk.Frame(self, background=PANEL, cursor='hand2')
            row.pack(fill='x', pady=2)
            bar = tk.Frame(row, background=PANEL, width=4, height=36)
            bar.pack(side='left', fill='y')
            text = tk.Label(row, text=label, background=PANEL, foreground=MUTED, font=(family, 11), anchor='w', padx=18, pady=8)
            text.pack(side='left', fill='x', expand=True)
            for w in (row, text):
                w.bind('<Button-1>', lambda e, k=key: self.on_pick(k))
            self.items[key] = (row, bar, text)
        self.footer = tk.Label(self, background=PANEL, foreground=MUTED, font=(family, 9), justify='left', wraplength=160, anchor='w')
        self.footer.pack(side='bottom', fill='x', padx=18, pady=16)

    def select(self, key):
        for k, (row, bar, text) in self.items.items():
            on = k == key
            for w in (row, text):
                w.configure(background='#26262c' if on else PANEL)
            bar.configure(background=ACCENT if on else PANEL)
            text.configure(foreground=TEXT if on else MUTED)


def card(parent, title=None, family=None):
    """A panel of the page: the card colour, a title, padding. Returns its inner frame."""
    outer = tk.Frame(parent, background=CARD, highlightthickness=1, highlightbackground='#2c2c33')
    inner = ttk.Frame(outer, style='Card.TFrame', padding=(20, 16, 20, 18))
    inner.pack(fill='both', expand=True)
    if title:
        ttk.Label(inner, text=title, style='CardTitle.TLabel').grid(row=0, column=0, columnspan=4, sticky='w', pady=(0, 10))
    return outer, inner
