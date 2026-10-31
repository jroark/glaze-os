"""Glaze USB installer GUI and isolated operation worker."""
import argparse
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading
import traceback

from engine import Installer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--resources', type=Path, default=Path(__file__).resolve().parent / 'resources')
    parser.add_argument('--worker', choices=['probe', 'prepare', 'install', 'restore'])
    parser.add_argument('--argument', default='')
    parser.add_argument('--device', default='auto')
    parser.add_argument('--smoke-test', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        def emit(event):
            print(json.dumps(event), flush=True)
        try:
            installer = Installer(args.resources, emit, args.device)
            if args.worker == 'probe':
                installer.verify()
                installer.probe()
            elif args.worker == 'prepare':
                installer.prepare(args.argument)
            else:
                installer.apply(args.argument, args.worker == 'restore')
        except Exception as error:
            emit({'event': 'error', 'text': str(error), 'detail': traceback.format_exc()})
            return 1
        return 0

    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    import webbrowser
    root = tk.Tk()
    root.title('Glaze — BE-300 USB Installer')
    root.geometry('760x560')
    frame = ttk.Frame(root, padding=20)
    frame.pack(fill='both', expand=True)
    ttk.Label(frame, text='Install Glaze on your BE-300', font=('', 20, 'bold')).pack(anchor='w')
    ttk.Label(frame, text='Connect USB, external power, and wake the device. No CF card is needed.\n'
              'Save important CE RAM files first. The RAM bootstrap clears the CE object store.\n'
              'Restore requires your saved backup and a working CE or Glaze USB bootstrap.', wraplength=710).pack(anchor='w', pady=12)
    row = ttk.Frame(frame)
    row.pack(fill='x')
    ttk.Label(row, text='USB location (auto for one device):').pack(side='left')
    device = tk.StringVar(value='auto')
    entry = ttk.Entry(row, textvariable=device, width=20)
    entry.pack(side='left', padx=8)
    status = tk.StringVar(value='Choose Prepare installation to create a verified backup first.')
    ttk.Label(frame, textvariable=status, wraplength=710).pack(fill='x', pady=12)
    progress = ttk.Progressbar(frame, mode='indeterminate')
    progress.pack(fill='x')
    log = tk.Text(frame, height=13, wrap='word', state='disabled')
    log.pack(fill='both', expand=True, pady=10)
    buttons = ttk.Frame(frame)
    buttons.pack(fill='x')
    events = queue.Queue()
    state = {'busy': False, 'record': None, 'log': [], 'folder': None}
    controls = []

    def launch(operation, argument=''):
        state['busy'] = True
        for control in controls:
            control.configure(state='disabled')
        entry.configure(state='disabled')
        progress.start()
        command = [sys.executable]
        if not getattr(sys, 'frozen', False):
            command.append(str(Path(__file__).resolve()))
        command += ['--resources', str(args.resources), '--worker', operation,
                    '--argument', argument, '--device', device.get()]
        def work():
            try:
                p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, encoding='utf-8', errors='replace',
                                     creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
                for line in p.stdout:
                    try:
                        events.put(json.loads(line))
                    except ValueError:
                        events.put({'event': 'log', 'text': line.strip()})
                code = p.wait()
                events.put({'event': 'worker-done', 'code': code})
            except Exception as error:
                events.put({'event': 'error', 'text': str(error)})
                events.put({'event': 'worker-done', 'code': 1})
        threading.Thread(target=work, daemon=True).start()

    def prepare():
        directory = filedialog.askdirectory(title='Choose a persistent folder for your CE backup')
        if directory:
            state['record'] = None
            state['folder'] = None
            launch('prepare', directory)

    def install():
        if state['record'] and messagebox.askyesno('Install Glaze',
                'Replace CE and its NAND files with Glaze?\n\nA verified backup has been saved. Keep power and USB connected until verification completes.'):
            launch('install', state['record'])

    def restore():
        path = filedialog.askopenfilename(title='Choose installation.json from your saved backup', filetypes=[('Installation record', '*.json')])
        if path and messagebox.askyesno('Restore Windows CE', 'Restore this device’s saved CE NAND? Current Glaze files will be erased. Keep power and USB connected.'):
            state['folder'] = str(Path(path).parent)
            launch('restore', path)

    for title, action in [('Detect device', lambda: launch('probe')), ('Prepare installation', prepare),
                          ('Install Glaze', install), ('Restore CE…', restore)]:
        button = ttk.Button(buttons, text=title, command=action)
        button.pack(side='left', padx=3)
        controls.append(button)
    controls[2].configure(state='disabled')
    if sys.platform == 'win32':
        ttk.Button(frame, text='Windows USB driver setup', command=lambda: messagebox.showinfo('WinUSB setup',
            'Open https://zadig.akeo.ie/ and download Zadig. With the BE-300 connected, choose Options → List All Devices.\n\n'
            'Select only the BE-300 with USB ID 07CF:2002 and install WinUSB. This replaces its sync driver; CE sync software may stop working until that driver is restored in Device Manager.\n\n'
            'Return here and click Detect device.')).pack(anchor='w', pady=5)
        ttk.Button(frame, text='Open Zadig website', command=lambda: webbrowser.open('https://zadig.akeo.ie/')).pack(anchor='w')

    def poll():
        while not events.empty():
            event = events.get_nowait()
            state['log'].append(json.dumps(event))
            kind = event.get('event')
            text = event.get('text', json.dumps(event))
            log.configure(state='normal')
            log.insert('end', text + '\n')
            log.see('end')
            log.configure(state='disabled')
            if kind == 'device':
                status.set('Detected ' + event['runtime'] + ' at USB ' + event['location'])
            if kind == 'folder':
                state['folder'] = event['path']
            if kind == 'prepared':
                state['record'] = event['record']
            if kind in ('phase', 'prepared', 'programming', 'verified', 'success', 'error'):
                status.set(text)
            if kind == 'worker-done':
                state['busy'] = False
                progress.stop()
                for control in controls:
                    control.configure(state='normal')
                controls[2].configure(state='normal' if state['record'] else 'disabled')
                entry.configure(state='normal')
            if state['folder']:
                try:
                    with (Path(state['folder']) / 'installer-log.jsonl').open('a', encoding='utf-8') as stream:
                        stream.write('\n'.join(state['log']) + '\n')
                    state['log'].clear()
                except OSError as error:
                    status.set('Cannot save diagnostic log: ' + str(error))
        root.after(100, poll)

    def close():
        if state['busy']:
            messagebox.showinfo('Operation in progress', 'Keep this window open until the USB operation finishes.')
        else:
            root.destroy()
    root.protocol('WM_DELETE_WINDOW', close)
    poll()
    if args.smoke_test:
        root.after(500, root.destroy)
    root.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
