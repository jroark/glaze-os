"""Glaze USB installer GUI and isolated operation worker."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import traceback

from engine import Installer, digest
from app_selection import load_catalog, select, customize_logical


def main():
    runtime = Path(sys.executable).parent
    if os.name == 'nt' and (runtime / 'tcl').exists():
        os.environ['TCL_LIBRARY'] = str(runtime / 'tcl/tcl8.6')
        os.environ['TK_LIBRARY'] = str(runtime / 'tcl/tk8.6')
        dll_directory = os.add_dll_directory(str(runtime / 'DLLs'))  # keep the handle alive
    default_resources = Path(__file__).resolve().parent / 'resources'
    if getattr(sys, 'frozen', False) and sys.platform == 'darwin':
        # PyInstaller splits data and Mach-O files across Resources/Frameworks.
        default_resources = Path(sys.executable).resolve().parents[1] / 'Resources/resources'
    parser = argparse.ArgumentParser()
    parser.add_argument('--resources', type=Path, default=default_resources)
    parser.add_argument('--worker', choices=['probe', 'prepare', 'install', 'restore', 'install-device'])
    parser.add_argument('--argument', default='')
    parser.add_argument('--skip-backup', action='store_true')
    parser.add_argument('--skip-wince-verification', action='store_true')
    parser.add_argument('--apps', help='JSON list of selected app IDs')
    parser.add_argument('--check-bundle', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--device', default='auto')
    parser.add_argument('--smoke-test', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.check_bundle:
        installer = Installer(args.resources, lambda e: None)
        installer.verify()
        catalog = load_catalog(args.resources)
        if len(installer.n.encode([bytes(520)])) != 528:
            raise RuntimeError('Bundled inventory helper failed')
        for name in ('glaze-installer-usb', 'nand-inventory', 'mkfsimg'):
            result = subprocess.run([str(args.resources / 'bin' / (name + installer.ext))], capture_output=True)
            if result.returncode != 2:
                raise RuntimeError('Bundled helper cannot start: ' + name)
        print(json.dumps({'ok': True, 'apps': len(catalog), 'runtime': sys.version.split()[0]}))
        return 0
    if args.worker:
        def emit(event):
            print(json.dumps(event), flush=True)
        try:
            installer = Installer(args.resources, emit, args.device)
            if args.worker == 'probe':
                installer.verify()
                installer.probe()
            elif args.worker == 'install-device':
                installer.install_device(args.argument, json.loads(args.apps) if args.apps else None,
                                         save_backup=not args.skip_backup, verify_wince=not args.skip_wince_verification)
            elif args.worker == 'prepare':
                installer.prepare(args.argument, json.loads(args.apps) if args.apps else None,
                                  verify_wince=not args.skip_wince_verification)
            else:
                installer.apply(args.argument, args.worker == 'restore')
        except Exception as error:
            emit({'event': 'error', 'text': str(error), 'detail': traceback.format_exc()})
            return 1
        return 0

    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    import tempfile
    root = tk.Tk()
    root.title('Glaze Installer')
    root.geometry('620x470')
    root.minsize(600, 450)
    try:
        catalog = load_catalog(args.resources)
    except Exception as error:
        messagebox.showerror('Installer unavailable', str(error))
        root.destroy()
        return 1
    selected = set(select(catalog))
    frame = ttk.Frame(root, padding=26)
    frame.pack(fill='both', expand=True)
    ttk.Label(frame, text='Install Glaze', font=('', 24, 'bold')).pack(anchor='w')
    ttk.Label(frame, text='Connect your BE-300 by USB and plug in power.', wraplength=560).pack(anchor='w', pady=(8, 18))
    device = tk.StringVar(value=args.device)
    save_backup = tk.BooleanVar(value=True)
    verify_wince = tk.BooleanVar(value=True)
    backup_folder = tk.StringVar(value=str(Path.home() / 'Documents/Glaze Backups'))
    status = tk.StringVar(value='Looking for your BE-300…')
    consequence = tk.StringVar()
    events = queue.Queue()
    state = {'busy': False, 'confirming': False, 'operation': None, 'done': False, 'details': False,
             'last_error': None, 'backup': None}
    controls = []
    if args.smoke_test:
        log_folder = Path(tempfile.mkdtemp(prefix='glaze-ui-smoke-'))
    elif sys.platform == 'darwin':
        log_folder = Path.home() / 'Library/Logs/Glaze Installer'
    else:
        log_folder = Path(os.environ.get('LOCALAPPDATA', tempfile.gettempdir())) / 'Glaze Installer/Logs'
    log_path = log_folder / ('session-' + __import__('datetime').datetime.now().strftime('%Y%m%d-%H%M%S') + '.jsonl')

    def selected_app_count():
        return sum(a['id'] in selected for a in catalog if a['kind'] != 'firmware')

    def choose_apps():
        window = tk.Toplevel(root)
        window.title('Choose apps')
        window.transient(root)
        window.grab_set()
        panel = ttk.Frame(window, padding=18)
        panel.pack(fill='both', expand=True)
        ttk.Label(panel, text='Files and Settings are always included.').pack(anchor='w', pady=8)
        variables = {}
        for kind, title in [('builtin', 'Built-in apps'), ('package', 'Additional apps'), ('firmware', 'Optional hardware')]:
            group = ttk.LabelFrame(panel, text=title, padding=10)
            group.pack(fill='x', pady=5)
            for index, app in enumerate(a for a in catalog if a['kind'] == kind):
                variable = tk.BooleanVar(value=app['id'] in selected)
                variables[app['id']] = variable
                button = ttk.Checkbutton(group, text=app['name'], variable=variable)
                button.grid(row=index // 3, column=index % 3, sticky='w', padx=8, pady=3)
                if app.get('required'):
                    button.configure(state='disabled')
        row = ttk.Frame(panel)
        row.pack(fill='x', pady=10)
        def preset(all_apps):
            wanted = {a['id'] for a in catalog if (all_apps and a['kind'] != 'firmware') or a.get('required')}
            if all_apps:
                wanted.update(a['id'] for a in catalog if a['kind'] == 'firmware' and variables[a['id']].get())
            for key, variable in variables.items():
                variable.set(key in wanted)
        def save():
            chosen = select(catalog, [key for key, value in variables.items() if value.get()])
            try:
                customize_logical((args.resources / 'glaze.bin').read_bytes(), catalog, chosen, args.resources)
            except ValueError as error:
                messagebox.showerror('Choose fewer options', str(error), parent=window)
                return
            selected.clear()
            selected.update(chosen)
            apps_button.configure(text=f'Choose apps… ({selected_app_count()} selected)')
            window.destroy()
        ttk.Button(row, text='Select all', command=lambda: preset(True)).pack(side='left')
        ttk.Button(row, text='Required only', command=lambda: preset(False)).pack(side='left', padx=8)
        ttk.Button(row, text='Done', command=save).pack(side='right')
    apps_button = ttk.Button(frame, text=f'Choose apps… ({selected_app_count()} selected)', command=choose_apps)
    apps_button.pack(anchor='w', pady=(0, 14))
    controls.append(apps_button)
    options = ttk.Frame(frame)
    options.pack(fill='x')
    def backup_changed():
        consequence.set('No Windows CE restore copy will be kept.' if not save_backup.get() else '')
        folder_button.configure(state='normal' if save_backup.get() else 'disabled')
    backup_check = ttk.Checkbutton(options, text='Save Windows CE backup', variable=save_backup, command=backup_changed)
    backup_check.grid(row=0, column=0, sticky='w')
    def change_folder():
        path = filedialog.askdirectory(title='Save backups here', initialdir=str(Path.home()))
        if path:
            backup_folder.set(path)
    folder_button = ttk.Button(options, text='Backup folder…', command=change_folder)
    folder_button.grid(row=0, column=1, padx=14)
    verify_check = ttk.Checkbutton(options, text='Verify Windows CE (extra checks)', variable=verify_wince)
    verify_check.grid(row=1, column=0, columnspan=2, sticky='w', pady=5)
    controls += [backup_check, folder_button, verify_check]
    ttk.Label(frame, textvariable=consequence, wraplength=560).pack(anchor='w', pady=(2, 12))
    ttk.Label(frame, textvariable=status, wraplength=560).pack(anchor='w', pady=(0, 8))
    progress = ttk.Progressbar(frame, mode='indeterminate')
    progress.pack(fill='x')
    actions = ttk.Frame(frame)
    actions.pack(fill='x', pady=(18, 10))

    def launch(operation, argument=''):
        if state['busy']:
            return
        state.update(busy=True, operation=operation, last_error=None)
        for control in controls:
            control.configure(state='disabled')
        if operation != 'probe':
            progress.configure(mode='indeterminate')
            progress.start()
        executable = Path(sys.executable)
        if executable.name.lower() == 'pythonw.exe':
            executable = executable.with_name('python.exe')
        command = [str(executable)]
        if not getattr(sys, 'frozen', False):
            command.append(str(Path(__file__).resolve()))
        command += ['--resources', str(args.resources), '--worker', operation,
                    '--argument', argument, '--device', device.get(), '--apps', json.dumps(sorted(selected))]
        if not save_backup.get():
            command.append('--skip-backup')
        if not verify_wince.get():
            command.append('--skip-wince-verification')
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
                events.put({'event': 'worker-done', 'code': p.wait()})
            except Exception as error:
                events.put({'event': 'error', 'text': str(error)})
                events.put({'event': 'worker-done', 'code': 1})
        threading.Thread(target=work, daemon=True).start()

    def install():
        if state['done']:
            root.destroy()
            return
        backup_text = 'A Windows CE backup will be saved.' if save_backup.get() else 'No Windows CE backup will be kept.'
        state['confirming'] = True
        try:
            confirmed = messagebox.askyesno('Install Glaze?', 'Windows CE and its files will be replaced.\n\n' + backup_text)
        finally:
            state['confirming'] = False
        if confirmed:
            status.set('Connecting to your BE-300…')
            launch('install-device', backup_folder.get())
    install_button = ttk.Button(actions, text='Install Glaze', command=install)
    install_button.pack(side='right')
    controls.append(install_button)
    def restore():
        state['confirming'] = True
        try:
            path = filedialog.askopenfilename(title='Choose a saved installation.json', filetypes=[('Installation record', '*.json')])
            confirmed = path and messagebox.askyesno('Restore Windows CE?', 'Current Glaze files will be replaced with your saved Windows CE backup.')
        finally:
            state['confirming'] = False
        if confirmed:
            status.set('Restoring Windows CE…')
            launch('restore', path)
    def open_backup():
        if state['backup']:
            if sys.platform == 'darwin':
                subprocess.Popen(['open', state['backup']])
            elif os.name == 'nt':
                os.startfile(state['backup'])
    backup_button = ttk.Button(actions, text='Open backup', command=open_backup)
    restore_button = ttk.Button(actions, text='Restore Windows CE…', command=restore)
    restore_button.pack(side='left')
    controls.append(restore_button)
    details_frame = ttk.Frame(frame)
    log = tk.Text(details_frame, height=9, wrap='word', state='disabled')
    log.pack(fill='both', expand=True)
    device_row = ttk.Frame(details_frame)
    device_row.pack(fill='x', pady=4)
    ttk.Label(device_row, text='USB location:').pack(side='left')
    entry = ttk.Entry(device_row, textvariable=device, width=15)
    entry.pack(side='left', padx=6)
    controls.append(entry)
    def show_details():
        state['details'] = not state['details']
        if state['details']:
            details_frame.pack(fill='both', expand=True, pady=8)
            root.geometry(f'620x{base_height + 220}')
        else:
            details_frame.pack_forget()
            root.geometry(f'620x{base_height}')
        details_button.configure(text='Hide details' if state['details'] else 'Show details')
    details_button = ttk.Button(frame, text='Show details', command=show_details)
    details_button.pack(anchor='w')
    if sys.platform == 'win32':
        def driver_setup():
            path = args.resources / 'drivers/zadig-2.9.exe'
            try:
                manifest = json.loads((args.resources / 'manifest.json').read_text())
                if digest(path) != manifest['files'].get('drivers/zadig-2.9.exe'):
                    raise ValueError('USB driver setup failed verification')
                messagebox.showinfo('USB driver setup', 'In Zadig, choose Options → List All Devices. Select the BE-300 (07CF:2002), then install WinUSB. Close Zadig when finished. This replaces the old CE sync driver.')
                os.startfile(path)
            except Exception as error:
                messagebox.showerror('USB driver setup', str(error))
        driver_button = ttk.Button(frame, text='USB driver setup…', command=driver_setup)
        driver_button.pack(anchor='w', pady=4)
        controls.append(driver_button)

    def poll():
        while not events.empty():
            event = events.get_nowait()
            try:
                log_path.parent.mkdir(parents=True, exist_ok=True)
                with log_path.open('a', encoding='utf-8') as stream:
                    stream.write(json.dumps(event) + '\n')
            except OSError:
                pass
            kind = event.get('event')
            text = event.get('text', '')
            if text:
                log.configure(state='normal')
                log.insert('end', text + '\n')
                log.see('end')
                log.configure(state='disabled')
            if kind == 'device' and state['operation'] == 'probe':
                status.set('BE-300 connected. Ready to install.')
            if kind == 'backup-saved':
                state['backup'] = event['path']
            if kind in ('status', 'programming'):
                status.set(text)
                progress.stop()
                progress.configure(mode='indeterminate')
                progress.start()
            if kind == 'progress':
                progress.stop()
                progress.configure(mode='determinate', maximum=event['total'], value=event['current'])
                if 'backup pass 2' in event.get('phase', ''):
                    status.set('Checking Windows CE…')
                elif 'final verification' in event.get('phase', ''):
                    status.set('Checking the installation…')
            if kind == 'success':
                state['done'] = True
                status.set(text)
                progress.stop()
                progress.configure(mode='determinate', maximum=100, value=100)
                install_button.configure(text='Done')
                if state['backup']:
                    backup_button.pack(side='left', padx=8)
            if kind == 'error':
                state['last_error'] = text
                friendly = text
                for command, message in [('boot', 'Couldn’t start Glaze. Check USB and try again.'),
                                         ('backup', 'Couldn’t read your BE-300. Check USB and try again.'),
                                         ('snapshot', 'Couldn’t read your BE-300. Check USB and try again.'),
                                         ('apply', 'Installation stopped. Keep USB connected and open Details.')]:
                    if text.startswith(command + ' failed.'):
                        friendly = message
                status.set('Connect your BE-300 to continue.' if state['operation'] == 'probe' else friendly)
            if kind == 'worker-done':
                state['busy'] = False
                progress.stop()
                if event['code'] and not state['last_error']:
                    status.set('The operation stopped. Open Details for more information.')
                for control in controls:
                    control.configure(state='normal')
                folder_button.configure(state='normal' if save_backup.get() else 'disabled')
                if state['operation'] == 'probe' and event['code']:
                    root.after(3000, auto_detect)
        root.after(100, poll)

    def auto_detect():
        if state['confirming']:
            root.after(3000, auto_detect)
            return
        if state['operation'] not in (None, 'probe'):
            return
        if not state['busy'] and not state['done']:
            launch('probe')
        elif not state['done']:
            root.after(3000, auto_detect)
    def close():
        if state['busy'] and state['operation'] != 'probe':
            messagebox.showinfo('Installation in progress', 'Keep power and USB connected until installation finishes.')
        else:
            root.destroy()
    root.protocol('WM_DELETE_WINDOW', close)
    root.update_idletasks()
    base_height = max(450, frame.winfo_reqheight() + 10)
    root.minsize(600, base_height)
    root.geometry(f'620x{base_height}')
    poll()
    if args.smoke_test:
        root.after(50, choose_apps)
        root.after(500, root.destroy)
    else:
        root.after(300, auto_detect)
    root.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
