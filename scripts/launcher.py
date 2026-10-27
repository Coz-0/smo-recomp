#!/usr/bin/env python3
"""A small window to set up and start the native build, for players who would rather not use the command line.

    python scripts/launcher.py

Game page: the folder of your own decrypted dump (docs/building.md), Set up / Update (scripts/setup.py --dump <folder>:
every step, its output under Show details, a progress bar; Cancel stops it and its compilers, and the next Set up
resumes there) and PLAY (scripts/run.py play --exe <the native program> --seconds 0 [--userdata <save data folder>]).
Settings page: the keys of graphics.ini in cards (Display, Quality; every other key under Advanced, folded), and the save
data folder.

Why a separate script and not a screen inside the program: the program does not exist until setup has made it from your
dump, and setup and run already do the work; this only drives them. Standard library only (tkinter: part of the
python.org installers for Windows; on Linux the distribution's tk package). The look: launcher_look.py.

The settings come from the runtime's own source: the default graphics.ini text (k_default_ini in
runtime/gfx/gfx_options.c) gives every key, its default, its choices ('; key = a | b | c') and its explanation, so the
window offers exactly what the runtime reads. Saving changes only the 'key = value' lines of the file and keeps its
comments; without a file it writes the documented default file with the changed values. The file is where the runtime
reads it: SMO_GRAPHICS_CONFIG, else build/config/graphics.ini (gfx_options.c). The dump folder and the save data folder
are remembered in build/launcher.json. tests/check_launcher.py checks the graphics.ini handling.
"""
import json
import os
import queue
import re
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import launcher_look as look  # noqa: E402  (scripts/, beside this file)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

PY = sys.executable
EXE = getattr(T, 'EXE', 'smo_native.exe')
SETUP_STEPS = ['deps', 'import', 'functions', 'lift', 'compile', 'tables', 'sdk', 'link', 'shaders']   # scripts/setup.py, without check
STATE = T.build('launcher.json')
GFX_SOURCE = os.path.join(REPO, 'runtime', 'gfx', 'gfx_options.c')
# no console window behind the launcher; its own process group, so Cancel stops setup's compilers too (kill_tree)
POPEN_FLAGS = ({'creationflags': subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt'
               else {'start_new_session': True})


def kill_tree(proc):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)], capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
    else:
        import signal
        os.killpg(proc.pid, signal.SIGTERM)


# ---- graphics.ini
def gfx_path():
    return os.environ.get('SMO_GRAPHICS_CONFIG') or T.build('config', 'graphics.ini')


def default_ini():
    """The runtime's default graphics.ini, as gfx_options.c writes it (the C string literal k_default_ini)."""
    with open(GFX_SOURCE, encoding='utf-8') as f:
        src = f.read()
    body = src[src.index('k_default_ini ='):]
    body = body[:body.index(';\n')]
    text = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    return re.sub(r'\\(.)', lambda m: {'r': '\r', 'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), text)


def ini_keys(text):
    """[(key, default, choices, help)] in the file's order. choices: the values of a '; key = a | b' comment line when
    each is a plain word (a list), else None (a free value such as '<number>' or '1.0'); help: the comment lines under it."""
    keys, doc = [], {}
    current = None
    for line in text.splitlines():
        m = re.match(r';\s*(\w+)\s*=\s*(.*)$', line)
        if m and not line.startswith(';  '):
            current = m.group(1)
            values = [v.strip() for v in re.split(r'\s{2,}', m.group(2))[0].split('|')]
            choices = values if len(values) > 1 and all(re.fullmatch(r'[\w.]+', v) for v in values) else None
            doc[current] = [choices, []]
            continue
        if line.startswith(';') and current:
            doc[current][1].append(line[1:].strip())
            continue
        m = re.match(r'(\w+)\s*=\s*(.*?)\s*$', line)
        if m:
            choices, lines = doc.get(m.group(1), [None, []])
            keys.append((m.group(1), m.group(2), choices, ' '.join(' '.join(lines).split())))
            current = None
    return keys


def read_values(path):
    values = {}
    if os.path.exists(path):
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'\s*(\w+)\s*=\s*(.*?)\s*$', line)
                if m and not line.lstrip().startswith((';', '#')):
                    values[m.group(1)] = m.group(2)
    return values


def write_values(path, changed):
    """Set the keys in `changed` in the file at `path` (made from the default text when missing), keeping every other line."""
    if os.path.exists(path):
        with open(path, encoding='utf-8', errors='replace', newline='') as f:
            text = f.read()
    else:
        text = default_ini()
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    left = dict(changed)
    for i, line in enumerate(lines):
        m = re.match(r'(\s*)(\w+)(\s*=\s*)(.*?)\s*$', line)
        if m and m.group(2) in left and not line.lstrip().startswith((';', '#')):
            lines[i] = m.group(1) + m.group(2) + m.group(3) + left.pop(m.group(2))
    if left:                                          # keys the file did not have yet
        if lines and lines[-1] == '':
            lines.pop()
        lines += ['%s = %s' % kv for kv in left.items()] + ['']
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(nl.join(lines))


# ---- launcher state
def load_state():
    try:
        with open(STATE, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(state):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    with open(STATE, 'w', encoding='utf-8') as f:
        json.dump(state, f, indent=1)


def built():
    return os.path.exists(T.build('bin', EXE))


# ---- the window
# Settings shown first, in these cards; every other key of graphics.ini goes to Advanced (folded), new ones included.
GROUPS = [('Display', ['window', 'width', 'height', 'vsync']), ('Quality', ['profile', 'resolution_scale', 'anisotropic_filtering'])]
LABELS = {'vsync': 'Vertical sync', 'gpu': 'GPU', 'unfocused': 'In the background', 'shader_wait': 'Shader not ready',
          'frame_time_overlay': 'Frame time in the title'}


def label_of(key):
    return LABELS.get(key) or key.replace('_', ' ').capitalize()


def shown(value):                                     # 'borderless_window' -> 'Borderless window'; numbers as they are
    return value if re.fullmatch(r'[\d.]+', value) else value.replace('_', ' ').capitalize()


class Launcher:
    def __init__(self, root):
        self.root = root
        self.proc = None
        self.task = None
        self.out = queue.Queue()
        self.state = load_state()
        root.title('Super Mario Odyssey - native build')
        root.geometry('900x820')
        root.minsize(820, 740)
        self.family = look.apply_theme(root)
        top = look.banner(root, self.family, 'SUPER MARIO ODYSSEY', 'Native build of version %s, made on this PC from your own dump' % T.VERSION)
        top.pack(fill='x')
        self.play_btn = look.Button(top, 'PLAY', self.play, kind='accent', width=170, height=52, size=14, bold=True, bg=look.BG,
                                    family=self.family)
        top.bind('<Configure>', lambda e: top.coords(self.play_win, e.width - 28, 75), add='+')
        self.play_win = top.create_window(0, 75, window=self.play_btn, anchor='e')
        body = tk.Frame(root, background=look.BG)
        body.pack(fill='both', expand=True)
        self.nav = look.Nav(body, self.family, [('game', 'Game'), ('settings', 'Settings')], self.show)
        self.nav.pack(side='left', fill='y')
        self.pages = {'game': self.game_page(body), 'settings': self.settings_page(body)}
        self.show('game')
        self.refresh()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.poll)

    def show(self, key):
        for k, page in self.pages.items():
            page.pack_forget()
        self.pages[key].pack(side='left', fill='both', expand=True, padx=24, pady=20)
        self.nav.select(key)

    def game_page(self, body):
        page = tk.Frame(body, background=look.BG)
        outer, f = look.card(page, 'Game files')
        outer.pack(fill='x')
        ttk.Label(f, text='The folder of your own decrypted dump of the game (docs/building.md says what it holds).',
                  style='Card.Muted.TLabel').grid(row=1, column=0, columnspan=2, sticky='w', pady=(0, 10))
        self.dump = tk.StringVar(value=self.state.get('dump', ''))
        ttk.Entry(f, textvariable=self.dump).grid(row=2, column=0, sticky='ew')
        look.Button(f, 'Browse...', self.browse, width=110, family=self.family).grid(row=2, column=1, padx=(10, 0))
        self.status = ttk.Label(f, style='Card.TLabel')
        self.status.grid(row=3, column=0, columnspan=2, sticky='w', pady=(16, 6))
        self.progress = ttk.Progressbar(f, maximum=len(SETUP_STEPS), style='Accent.Horizontal.TProgressbar')
        self.progress.grid(row=4, column=0, columnspan=2, sticky='ew')
        row = ttk.Frame(f, style='Card.TFrame')
        row.grid(row=5, column=0, columnspan=2, sticky='ew', pady=(16, 0))
        self.setup_btn = look.Button(row, 'Set up', self.run_setup, kind='accent', width=130, family=self.family)
        self.cancel_btn = look.Button(row, 'Cancel', self.cancel, width=110, family=self.family)
        self.setup_btn.pack(side='left')
        self.cancel_btn.pack(side='left', padx=(10, 0))
        self.details = ttk.Label(row, text='Show details', style='Card.Muted.TLabel', cursor='hand2')
        self.details.pack(side='right')
        self.details.bind('<Button-1>', lambda e: self.toggle_log())
        f.columnconfigure(0, weight=1)
        self.log_card, lf = look.card(page, 'Details')
        self.log = tk.Text(lf, height=12, wrap='none', state='disabled', relief='flat', borderwidth=0, padx=10, pady=8,
                           font='TkFixedFont', background='#16161a', foreground='#c9c9d2', highlightthickness=0)
        scroll = ttk.Scrollbar(lf, command=self.log.yview)
        self.log['yscrollcommand'] = scroll.set
        self.log.grid(row=1, column=0, sticky='nsew')
        scroll.grid(row=1, column=1, sticky='ns')
        lf.columnconfigure(0, weight=1)
        lf.rowconfigure(1, weight=1)
        return page

    def toggle_log(self, show=None):
        on = not self.log_card.winfo_ismapped() if show is None else show
        if on:
            self.log_card.pack(fill='both', expand=True, pady=(16, 0))
        else:
            self.log_card.pack_forget()
        self.details.config(text='Hide details' if on else 'Show details')

    def settings_page(self, body):
        page = tk.Frame(body, background=look.BG)
        self.ini = gfx_path()
        current = read_values(self.ini)
        keys = ini_keys(default_ini())
        grouped = {k for _, ks in GROUPS for k in ks}
        groups = GROUPS + [('Advanced', [k for k, _, _, _ in keys if k not in grouped])]
        info = {k: (d, c, t) for k, d, c, t in keys}
        self.fields = {}
        cols = tk.Frame(page, background=look.BG)
        cols.pack(fill='x')
        for n, (title, names) in enumerate(groups):
            outer, f = look.card(page if title == 'Advanced' else cols, None if title == 'Advanced' else title)
            if title == 'Advanced':
                outer.pack(fill='x', pady=(16, 0))
                self.adv_head = ttk.Label(f, text='\u25b8  Advanced', style='CardTitle.TLabel', cursor='hand2')
                self.adv_head.grid(row=0, column=0, columnspan=2, sticky='w')
                self.adv_head.bind('<Button-1>', lambda e: self.toggle_advanced())
                self.adv_rows = []
            else:
                outer.grid(row=0, column=n, sticky='nsew', padx=(0, 16) if n == 0 else 0)
                cols.columnconfigure(n, weight=1, uniform='cards')
            two = title == 'Advanced'                # Advanced: two columns of label + value
            for i, key in enumerate([k for k in names if k in info]):
                default, choices, text = info[key]
                r, c = (1 + i // 2, (i % 2) * 2) if two else (1 + i, 0)
                lab = ttk.Label(f, text=label_of(key), style='Card.TLabel')
                lab.grid(row=r, column=c, sticky='w', padx=(24 if c else 0, 12), pady=4)
                var = tk.StringVar()
                if choices:
                    to_value = {shown(c): c for c in choices}
                    w = ttk.Combobox(f, textvariable=var, values=list(to_value), state='readonly', width=21)
                    var.set(shown(current.get(key, default)))
                else:
                    to_value = None
                    w = ttk.Entry(f, textvariable=var, width=20)
                    var.set(current.get(key, default))
                w.grid(row=r, column=c + 1, sticky='ew', pady=4)
                for wid in (w, lab):
                    wid.bind('<Enter>', lambda e, k=key, t=text: self.help.config(text='%s: %s' % (label_of(k), t)))
                w.bind('<FocusIn>', lambda e, k=key, t=text: self.help.config(text='%s: %s' % (label_of(k), t)))
                self.fields[key] = (var, default, to_value)
                if title == 'Advanced':
                    self.adv_rows += [lab, w]
            f.columnconfigure(1, weight=1)
            if two:
                f.columnconfigure(3, weight=1)
                self.save_folder_row(f, 1 + (len(self.adv_rows) // 2 + 1) // 2)      # under the rows of keys
        for w in self.adv_rows:
            w.grid_remove()
        actions = tk.Frame(page, background=look.BG)
        actions.pack(fill='x', pady=(18, 0))
        look.Button(actions, 'Save', self.save_settings, kind='accent', width=120, bg=look.BG, family=self.family).pack(side='left')
        look.Button(actions, 'Defaults', self.default_settings, width=110, bg=look.BG, family=self.family).pack(side='left', padx=(10, 0))
        self.saved = ttk.Label(actions, style='Muted.TLabel')
        self.saved.pack(side='left', padx=(16, 0))
        self.help = ttk.Label(page, style='Muted.TLabel', wraplength=620, justify='left', text='Point at a setting to read what it does.')
        self.help.pack(fill='x', pady=(16, 0))
        return page

    def save_folder_row(self, f, row):
        """Where the game keeps its saves: scripts/run.py --userdata (SMO_USERDATA); not a key of graphics.ini."""
        text = ('Save data folder: where the game keeps its saves (scripts/run.py --userdata). Empty: %s. A new, empty folder '
                'starts a new game; a copy of another one continues from there.' % os.path.relpath(T.build('userdata'), REPO))
        self.userdata = tk.StringVar(value=self.state.get('userdata', ''))
        lab = ttk.Label(f, text='Save data folder', style='Card.TLabel')
        lab.grid(row=row, column=0, sticky='w', padx=(0, 12), pady=(10, 4))
        ent = ttk.Entry(f, textvariable=self.userdata)
        ent.grid(row=row, column=1, columnspan=2, sticky='ew', pady=(10, 4))
        btn = look.Button(f, 'Browse...', self.browse_saves, width=110, family=self.family)
        btn.grid(row=row, column=3, sticky='w', padx=(10, 0), pady=(10, 4))
        for w in (lab, ent, btn):
            w.bind('<Enter>', lambda e: self.help.config(text=text))
        self.adv_rows += [lab, ent, btn]

    def browse_saves(self):
        d = filedialog.askdirectory(initialdir=self.userdata.get() or T.build('userdata'), title='Save data folder')
        if d:
            self.userdata.set(d)

    def toggle_advanced(self):
        on = not self.adv_rows[0].winfo_ismapped()
        for w in self.adv_rows:
            w.grid() if on else w.grid_remove()
        self.adv_head.config(text=('\u25be' if on else '\u25b8') + '  Advanced')

    # -- game
    def browse(self):
        d = filedialog.askdirectory(initialdir=self.dump.get() or os.path.expanduser('~'), title='Folder of your decrypted dump')
        if d:
            self.dump.set(d)

    def refresh(self):
        running = self.proc is not None and self.proc.poll() is None
        self.setup_btn.config(text='Update' if built() else 'Set up', state='disabled' if running else 'normal')
        self.cancel_btn.config(state='normal' if running and self.task == 'setup' else 'disabled')
        self.play_btn.config(state='normal' if built() and not running else 'disabled')
        self.setup_btn.kind = 'second' if built() else 'accent'
        self.setup_btn.draw()
        if not running:
            self.status.config(text='Ready to play.' if built() else 'Not set up yet: choose the folder and press Set up.')
        self.nav.footer.config(text='Ready to play' if built() else 'Not set up yet')

    def write_log(self, text):
        self.log.config(state='normal')
        self.log.insert('end', text)
        self.log.see('end')
        self.log.config(state='disabled')

    def start(self, task, cmd):
        self.task = task
        self.write_log('> %s\n' % ' '.join(os.path.relpath(c, REPO) if c.startswith(REPO) else c for c in cmd[1:]))
        self.proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding='utf-8', errors='replace', bufsize=1, env=dict(os.environ, PYTHONUNBUFFERED='1'), **POPEN_FLAGS)
        threading.Thread(target=self.reader, args=(self.proc,), daemon=True).start()
        self.refresh()

    def reader(self, proc):                           # a thread: the window reads the queue (tkinter is not thread-safe)
        for line in proc.stdout:
            self.out.put(line)
        self.out.put(None)

    def poll(self):
        try:
            while True:
                line = self.out.get_nowait()
                if line is None:
                    self.finished()
                    continue
                self.write_log(line)
                m = re.match(r'\[(\w+)\]', line)
                if self.task == 'setup' and m and m.group(1) in SETUP_STEPS:
                    self.progress['value'] = SETUP_STEPS.index(m.group(1)) + 1
                    self.status.config(text='Setting up: %s (step %d of %d)' % (m.group(1), SETUP_STEPS.index(m.group(1)) + 1, len(SETUP_STEPS)))
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def finished(self):
        code = self.proc.wait()
        if self.task == 'setup':
            self.write_log('setup %s\n' % ('finished' if code == 0 else 'stopped (exit code %d); Set up resumes where it stopped' % code))
            if code == 0:
                self.progress['value'] = len(SETUP_STEPS)
        self.refresh()
        if self.task == 'setup' and code != 0:
            self.status.config(text='Setup stopped: see the details below (docs/building.md has the troubleshooting).')
            self.toggle_log(True)

    def run_setup(self):
        dump = self.dump.get().strip()
        if not os.path.isdir(dump):
            messagebox.showerror('Set up', 'Choose the folder of your decrypted dump first (docs/building.md says what it holds).')
            return
        self.state['dump'] = dump
        save_state(self.state)
        self.progress['value'] = 0
        self.start('setup', [PY, os.path.join(REPO, 'scripts', 'setup.py'), '--dump', dump])

    def cancel(self):
        if self.proc and self.proc.poll() is None:
            kill_tree(self.proc)

    def play(self):
        self.status.config(text='Playing.')
        saves = self.state.get('userdata')
        self.start('play', [PY, os.path.join(REPO, 'scripts', 'run.py'), 'play', '--exe', EXE, '--seconds', '0']
                   + (['--userdata', saves] if saves else []))

    # -- settings
    def save_settings(self):
        current = read_values(self.ini)
        values = {k: (to_value or {}).get(v.get(), v.get().strip()) for k, (v, default, to_value) in self.fields.items()}
        changed = {k: v for k, v in values.items() if v != current.get(k, self.fields[k][1])}
        if changed or not os.path.exists(self.ini):
            write_values(self.ini, changed)
        self.state['userdata'] = self.userdata.get().strip()
        save_state(self.state)
        self.saved.config(text='Saved. It applies the next time the game starts.')
        self.root.after(4000, lambda: self.saved.config(text=''))

    def default_settings(self):
        for var, default, to_value in self.fields.values():
            var.set(shown(default) if to_value else default)
        self.userdata.set('')

    def close(self):
        if self.proc and self.proc.poll() is None and self.task == 'setup':
            if not messagebox.askyesno('Quit', 'Setup is still running. Stop it and quit? (Set up resumes where it stopped.)'):
                return
            kill_tree(self.proc)
        self.root.destroy()


def main():
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == '__main__':
    main()
