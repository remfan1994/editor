#!/usr/bin/env python3
"""Real terminal integration checks (pexpect is a test dependency only)."""
import os
import pathlib
import sys
import tempfile
import pexpect

for exe in sys.argv[1:]:
    with tempfile.TemporaryDirectory(prefix='editor-wrap-') as work:
        root = pathlib.Path(work)
        env = dict(os.environ, HOME=work, XDG_CONFIG_HOME=work+'/config',
                   XDG_STATE_HOME=work+'/state', TERM='xterm', LC_ALL='C')
        (root/'config').mkdir(); (root/'state').mkdir()
        path = root/'long.txt'
        original = b'alpha beta gamma delta epsilon zeta eta theta iota kappa lambda\r\nsecond line\r\n'
        path.write_bytes(original)
        conf = root/'config/editor/config'
        def spawn(*args):
            p = pexpect.spawn(exe, ['--cli', *args, str(path)], cwd=work, env=env,
                              dimensions=(20, 28), encoding='ascii', timeout=5)
            p.delaybeforesend = 0.02
            return p
        def cmd(p, command, expected=None):
            p.send('\x1b'); p.expect('Command>'); p.send(command+'\r')
            if expected: p.expect_exact(expected)
        def quit(p):
            cmd(p,'quit!'); p.expect(pexpect.EOF); p.close(); assert p.exitstatus == 0
        p=spawn(); p.expect_exact('epsilon zeta eta theta')
        cmd(p,'toggle-wrap','Word wrap OFF (saved).'); quit(p)
        assert 'wrap=off' in conf.read_text()
        p=spawn('--wrap'); p.expect_exact('epsilon zeta eta theta'); quit(p)
        assert 'wrap=off' in conf.read_text()
        p=spawn('--no-wrap'); p.expect_exact('1:1 | long.txt')
        cmd(p,'toggle-wrap','Word wrap ON (saved).'); quit(p)
        assert 'wrap=on' in conf.read_text()
        p=spawn(); p.expect_exact('epsilon zeta eta theta')
        p.setwinsize(20, 40); p.expect_exact('alpha beta gamma delta epsilon zeta')
        cmd(p,'goto 2'); cmd(p,'write','Already saved (unchanged).'); cmd(p,'write copy.txt','Written'); quit(p)
        assert path.read_bytes()==original and (root/'copy.txt').read_bytes()==original
        print('PASS actual PTY default, live toggle, saved setting, launch overrides, resize, byte-identical write:', pathlib.Path(exe).name)
