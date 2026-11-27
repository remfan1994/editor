#!/usr/bin/env python3
"""Targeted terminal integration tests; pexpect required only for testing."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import pexpect

normal, restricted, baseline = map(lambda p: str(pathlib.Path(p).resolve()), sys.argv[1:])

with tempfile.TemporaryDirectory(prefix='editor-rollback-') as work:
    root = pathlib.Path(work)
    env = dict(os.environ, HOME=work, XDG_CONFIG_HOME=work+'/config',
               XDG_STATE_HOME=work+'/state', TERM='xterm', LC_ALL='C')
    (root/'config').mkdir(); (root/'state').mkdir()
    path = root/'sample.txt'
    path.write_bytes(b'one\r\ntwo\r\nthree')
    def spawn(exe, args):
        p = pexpect.spawn(exe, ['--cli','--no-mouse']+args, cwd=work, env=env,
                          dimensions=(24,100), encoding='utf-8', codec_errors='replace', timeout=4)
        p.delaybeforesend = 0.04
        p.expect('editor ')
        return p
    def cmd(p, text, expected=None):
        p.send('\x1b')
        p.expect('Command>')
        p.send(text+'\r')
        if expected is not None:
            p.expect(expected)
        time.sleep(0.08)
    def quit(p):
        cmd(p,'quit!')
        p.expect(pexpect.EOF)
        p.close()
        assert p.exitstatus == 0, p.exitstatus
    p=spawn(normal,[str(path)])
    cmd(p,'dline 2-end')
    cmd(p,'write','Written')
    quit(p)
    assert path.read_bytes()==b'one', path.read_bytes()
    # Existing pre-rollback journal format must still be readable by 0.6.14.
    p=spawn(baseline,[str(path)])
    cmd(p,'undo','undone')
    cmd(p,'write','Written')
    quit(p)
    assert path.read_bytes()==b'one\r\ntwo\r\nthree'
    p=spawn(normal,[str(path)])
    cmd(p,'define save goto eof','Defined user command save')
    cmd(p,'bind F6 save','F6')
    cmd(p,'save')
    # save is now a movement command, not a write; write still works.
    cmd(p,'dline eof')
    cmd(p,'write','Written')
    quit(p)
    assert path.read_bytes()==b'one\r\ntwo'
    print('PASS actual PTY symbolic deletion, save, baseline undo compatibility, custom synonym and binding')
    p=spawn(normal,[str(path)])
    cmd(p,'undefine save','Removed user command save')
    cmd(p,'define dangerous shell touch marker','Defined user command dangerous')
    cmd(p,'bind F7 dangerous','F7')
    quit(p)
    script=root/'blocked.editor'
    script.write_text('dangerous\n')
    script.chmod(0o600)
    for exe,args in [(normal,['--no-cmd']), (restricted,[])]:
        p=spawn(exe,args+[str(path)])
        cmd(p,'shell touch marker','External commands disabled')
        cmd(p,'dangerous','External commands disabled')
        cmd(p,'run blocked.editor','External commands disabled|Script stopped')
        p.send('\x1b[18~')  # F7
        p.expect('External commands disabled')
        cmd(p,'dline eof')
        cmd(p,'undo','undone')
        quit(p)
        assert not (root/'marker').exists()
    print('PASS actual PTY runtime and compile-time restrictions, persisted hotkey, nested script, normal edits')
    literal=root/'--no-cmd'; literal.write_bytes(b'file')
    p=spawn(normal,['--','--no-cmd'])
    cmd(p,'shell touch marker','Command succeeded')
    quit(p)
    assert (root/'marker').exists()
    print('PASS literal --no-cmd filename after -- retains normal command policy')
