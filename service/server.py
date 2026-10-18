#!/usr/bin/env python3
import http.server
import socketserver
import json
import os
import subprocess
import urllib.parse
import sys
from io import BytesIO
import time
import shlex
import hashlib
import base64
import re

ROOT = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(ROOT, 'static')
REPO_ROOT = os.path.abspath(os.path.join(ROOT, '..'))
BIN_DIR = os.path.join(REPO_ROOT, 'build', 'bin')
QUMIRC = os.path.join(BIN_DIR, 'qumirc')
QUMIRI = os.path.join(BIN_DIR, 'qumiri')
SHARED_DIR = os.path.join(ROOT, 'shared')  # filesystem storage for shared links

PORT = int(os.environ.get('PORT', '8080'))

HTML_INDEX = 'index.html'

class Handler(http.server.SimpleHTTPRequestHandler):
    def _log(self, msg: str):
        ts = time.strftime('%Y-%m-%d %H:%M:%S')
        print(f"[{ts}] {msg}")

    def _fmt_argv(self, argv):
        try:
            return shlex.join(argv)
        except AttributeError:
            return ' '.join(shlex.quote(a) for a in argv)

    def translate_path(self, path):
        # Serve files from STATIC_DIR by default
        path = path.split('?',1)[0].split('#',1)[0]
        if path == '/':
            path = '/' + HTML_INDEX
        full = os.path.join(STATIC_DIR, path.lstrip('/'))
        return full

    def _send_json(self, obj, code=200):
        data = json.dumps(obj).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_bytes(self, data: bytes, content_type: str, code=200, headers=None):
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        if headers:
            for k,v in headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self):
        # Simple CORS for local dev
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET,POST,OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type, X-Qumir-O')
        self.end_headers()

    def do_POST(self):
        length = int(self.headers.get('Content-Length','0'))
        raw = self.rfile.read(length)
        # Raw body protocol for compile endpoints
        if self.path in ('/api/compile-ir','/api/compile-llvm','/api/compile-asm','/api/compile-wasm','/api/compile-wasm-text'):
            code = raw.decode('utf-8', 'ignore')
            if not code:
                return self._send_json({'error':'empty body'}, 400)
            olevel = self.headers.get('X-Qumir-O') or '0'
            try:
                oi = int(olevel)
            except Exception:
                return self._send_json({'error':'X-Qumir-O must be numeric'}, 400)
            if oi < 0: oi = 0
            if oi > 3: oi = 3
            olevel = str(oi)
            if self.path == '/api/compile-ir':
                return self._compile_emit(code, olevel, mode='ir')
            if self.path == '/api/compile-llvm':
                return self._compile_emit(code, olevel, mode='llvm')
            if self.path == '/api/compile-asm':
                return self._compile_emit(code, olevel, mode='asm')
            if self.path == '/api/compile-wasm':
                return self._compile_wasm(code, olevel)
            if self.path == '/api/compile-wasm-text':
                return self._compile_wasm_text(code, olevel)

        # JSON only for share
        try:
            payload = json.loads(raw.decode('utf-8'))
        except Exception:
            return self._send_json({'error':'invalid json'}, 400)
        if self.path == '/api/share':
            return self._api_share_create(payload)
        #if self.path == '/api/run-ir':
        #    return self._run_interpreter(payload, jit=False)
        #if self.path == '/api/run-jit':
        #    return self._run_interpreter(payload, jit=True)

        return self._send_json({'error':'unknown endpoint'}, 404)

    def do_GET(self):
        # API: list examples and fetch example content
        parsed = urllib.parse.urlparse(self.path)
    # default: not a static response (used by end_headers to inject headers)
        self._is_static = False
        if parsed.path == '/api/version':
            return self._api_version()
        if parsed.path == '/api/examples':
            return self._api_list_examples()
        if parsed.path == '/api/example':
            qs = urllib.parse.parse_qs(parsed.query or '')
            rel = (qs.get('path') or [''])[0]
            return self._api_get_example(rel)
        if parsed.path == '/api/share':
            qs = urllib.parse.parse_qs(parsed.query or '')
            sid = (qs.get('id') or [''])[0]
            return self._api_share_get(sid)
        # Pretty share link: /s/<id> -> /index.html?share=<id>
        if parsed.path.startswith('/s/'):
            sid = parsed.path[len('/s/'):] or ''
            if not self._is_valid_share_id(sid):
                return self._send_json({'error':'invalid id'}, 400)
            # If share doesn't exist, 404
            spath = self._share_path(sid)
            if not os.path.isfile(spath):
                return self._send_json({'error':'not found'}, 404)
            dest = f'/{HTML_INDEX}?share={urllib.parse.quote(sid)}'
            self.send_response(302)
            self.send_header('Location', dest)
            self.end_headers()
            return
        # Fallback to static
        self._is_static = True
        return super().do_GET()

    def end_headers(self):
        # For static assets, set short caching (1 minute)
        try:
            if getattr(self, '_is_static', False):
                path_only = (self.path or '').split('?', 1)[0].lower()
                if path_only.endswith(('.js', '.css', '.html', '.htm')):
                    self.send_header('Cache-Control', 'public, max-age=60')
        except Exception:
            pass
        return super().end_headers()

    def _git(self, args):
        try:
            proc = subprocess.run(['git'] + args, cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            if proc.returncode != 0:
                return None
            return proc.stdout.decode('utf-8', 'ignore').strip()
        except Exception:
            return None

    def _api_version(self):
        short = self._git(['rev-parse', '--short', 'HEAD']) or '-'
        date = self._git(['log', '-1', '--date=iso-strict', "--format=%cd"]) or '-'
        branch = self._git(['rev-parse', '--abbrev-ref', 'HEAD']) or '-'
        return self._send_json({ 'hash': short, 'date': date, 'branch': branch })

    def _api_list_examples(self):
        base = os.path.join(REPO_ROOT, 'examples')
        items = []
        for root, dirs, files in os.walk(base):
            # sort for stable order
            dirs.sort(); files.sort()
            rel_root = os.path.relpath(root, base)
            for fn in files:
                if not fn.lower().endswith('.kum'):
                    continue
                full = os.path.join(root, fn)
                rel = os.path.normpath(os.path.join(rel_root, fn)) if rel_root != '.' else fn
                items.append({
                    'path': rel.replace('\\','/'),
                    'name': fn,
                })
        return self._send_json({'examples': items})

    def _api_get_example(self, rel_path: str):
        if not rel_path:
            return self._send_json({'error':'path required'}, 400)
        base = os.path.join(REPO_ROOT, 'examples')
        # normalize and ensure inside base
        candidate = os.path.normpath(os.path.join(base, rel_path))
        if not candidate.startswith(base):
            return self._send_json({'error':'invalid path'}, 400)
        if not os.path.isfile(candidate) or not candidate.lower().endswith('.kum'):
            return self._send_json({'error':'not found'}, 404)
        try:
            with open(candidate, 'rb') as f:
                data = f.read()
            return self._send_bytes(data, 'text/plain; charset=utf-8', headers={'Cache-Control':'no-store'})
        except Exception as e:
            return self._send_json({'error': str(e)}, 500)

    def _write_temp_source(self, code_text: str) -> str:
        import tempfile
        # place temp source in system temp dir
        fd, path = tempfile.mkstemp(suffix='.kum', prefix='qumir_', dir=None)
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(code_text)
        return path

    def _run(self, argv, input_bytes: bytes=None, timeout=10):
        in_len = len(input_bytes) if input_bytes is not None else 0
        self._log(f"RUN: {self._fmt_argv(argv)} | stdin={in_len}B timeout={timeout}s")
        try:
            proc = subprocess.run(argv, input=input_bytes, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
            rc, so, se = proc.returncode, proc.stdout, proc.stderr
            self._log(f"EXIT: rc={rc} | stdout={len(so)}B stderr={len(se)}B")
            return rc, so, se
        except subprocess.TimeoutExpired:
            self._log("EXIT: rc=124 (timeout)")
            return 124, b'', b'timeout'
        except FileNotFoundError:
            msg = f'not found: {argv[0]}'
            self._log(f"EXIT: rc=127 ({msg})")
            return 127, b'', msg.encode('utf-8')

    def _compile_emit(self, code: str, olevel: str, mode: str):
        if not code:
            return self._send_json({'error':'empty code'}, 400)
        src = self._write_temp_source(code)
        try:
            if mode == 'ir':
                out = src + '.ir'
                rc, so, se = self._run([QUMIRC, '--ir', '-O', olevel, '-o', out, src])
                if rc != 0:
                    return self._send_json({'error':se.decode('utf-8','ignore')}, 400)
                try:
                    with open(out,'rb') as f:
                        data = f.read()
                    return self._send_bytes(data, 'text/plain; charset=utf-8')
                finally:
                    try: os.remove(out)
                    except Exception: pass
            elif mode == 'llvm':
                out = src + '.ll'
                rc, so, se = self._run([QUMIRC, '--llvm', '-O', olevel, '-o', out, src])
                if rc != 0:
                    return self._send_json({'error':se.decode('utf-8','ignore')}, 400)
                try:
                    with open(out,'rb') as f:
                        data = f.read()
                    return self._send_bytes(data, 'text/plain; charset=utf-8')
                finally:
                    try: os.remove(out)
                    except Exception: pass
            elif mode == 'asm':
                out = os.path.splitext(src)[0] + '.s'
                rc, so, se = self._run([QUMIRC, '-S', '-O', olevel, src])
                if rc != 0:
                    return self._send_json({'error':se.decode('utf-8','ignore')}, 400)
                try:
                    with open(out,'rb') as f:
                        data = f.read()
                    return self._send_bytes(data, 'text/plain; charset=utf-8')
                finally:
                    try: os.remove(out)
                    except Exception: pass
        finally:
            try:
                os.remove(src)
            except Exception:
                pass

    def _compile_wasm(self, code: str, olevel: str):
        if not code:
            return self._send_json({'error':'empty code'}, 400)
        src = self._write_temp_source(code)
        out = os.path.splitext(src)[0] + '.wasm'
        try:
            debug = self.headers.get('X-Qumir-Debug-Points') == '1'
            args = [QUMIRC, '--wasm', '-O', '0' if debug else olevel, '-o', out, src]
            if debug:
                args.insert(1, '--debug-points')
            if self.headers.get('X-Qumir-Syntax') == 'core':
                args.insert(1, '--core')
            rc, so, se = self._run(args)
            if rc != 0:
                return self._send_json({'error':se.decode('utf-8','ignore')}, 400)
            with open(out,'rb') as f:
                data = f.read()
            return self._send_bytes(data, 'application/wasm', headers={'Cache-Control':'no-store'})
        finally:
            try:
                os.remove(src)
            except Exception:
                pass
            try:
                os.remove(out)
            except Exception:
                pass

    def _compile_wasm_text(self, code: str, olevel: str):
        if not code:
            return self._send_json({'error':'empty code'}, 400)
        src = self._write_temp_source(code)
        out = os.path.splitext(src)[0] + '.s'
        try:
            # Generate textual wasm (WAT-like) using --wasm -S
            rc, so, se = self._run([QUMIRC, '--wasm', '-S', '-O', olevel, src])
            if rc != 0:
                return self._send_json({'error':se.decode('utf-8','ignore')}, 400)
            try:
                with open(out,'rb') as f:
                    data = f.read()
                return self._send_bytes(data, 'text/plain; charset=utf-8')
            finally:
                try: os.remove(out)
                except Exception: pass
        finally:
            try: os.remove(src)
            except Exception: pass

    def _run_interpreter(self, payload, jit: bool):
        code = payload.get('code','')
        olevel = str(payload.get('O','0'))
        if not code:
            return self._send_json({'error':'empty code'}, 400)
        args = [QUMIRI, '--input-file', '-']
        if jit:
            args.insert(1, '--jit')
            args.extend(['-O', olevel])
        rc, so, se = self._run(args, input_bytes=code.encode('utf-8'))
        if rc != 0:
            return self._send_json({'error':se.decode('utf-8','ignore')}, 400)
        return self._send_bytes(so, 'text/plain; charset=utf-8')

    # ========== Shared links support (filesystem-based, no DB) ==========

    _ID_RE = re.compile(r'^[A-Za-z0-9_-]{6,64}$')

    def _ensure_shared_dir(self):
        try:
            os.makedirs(SHARED_DIR, exist_ok=True)
        except Exception:
            pass

    def _is_valid_share_id(self, sid: str) -> bool:
        return bool(self._ID_RE.match(sid or ''))

    def _share_id_for_code(self, code_text: str) -> str:
        # content-addressed ID: urlsafe base64 of sha256 digest prefix
        digest = hashlib.sha256(code_text.encode('utf-8')).digest()
        # 12 chars from first 9 bytes (~72 bits) to keep links short with negligible collision risk
        sid = base64.urlsafe_b64encode(digest[:9]).decode('ascii').rstrip('=')
        return sid

    def _share_id_for_bundle(self, code_text: str, args: str, stdin: str) -> str:
        # content-addressed ID from code+args+stdin (stable JSON, UTF-8)
        bundle = json.dumps({
            'code': code_text,
            'args': args or '',
            'stdin': stdin or ''
        }, ensure_ascii=False, separators=(',', ':'), sort_keys=True)
        digest = hashlib.sha256(bundle.encode('utf-8')).digest()
        sid = base64.urlsafe_b64encode(digest[:9]).decode('ascii').rstrip('=')
        return sid

    def _share_path(self, sid: str) -> str:
        return os.path.join(SHARED_DIR, f'{sid}.kum')

    def _share_meta_path(self, sid: str) -> str:
        return os.path.join(SHARED_DIR, f'{sid}.json')

    def _api_share_create(self, payload):
        code = payload.get('code','')
        args = payload.get('args') or ''
        stdin = payload.get('stdin') or ''
        if not code:
            return self._send_json({'error':'empty code'}, 400)
        # Optional preferred id, must be valid
        pref = payload.get('id') or ''
        if pref and not self._is_valid_share_id(pref):
            return self._send_json({'error':'invalid id'}, 400)

        self._ensure_shared_dir()
        # Compute ID from the full bundle (code + args + stdin) unless a valid preferred is given
        sid = pref or self._share_id_for_bundle(code, args, stdin)
        path = self._share_path(sid)
        mpath = self._share_meta_path(sid)

        # If file exists, ensure same content; otherwise, pick a longer ID variant
        if os.path.exists(path):
            try:
                with open(path, 'r', encoding='utf-8') as f:
                    existing = f.read()
                # Compare both code and metadata if present
                same_meta = True
                if os.path.exists(mpath):
                    try:
                        with open(mpath, 'r', encoding='utf-8') as mf:
                            meta = json.load(mf)
                        same_meta = (meta.get('args','') == args) and (meta.get('stdin','') == stdin)
                    except Exception:
                        same_meta = False
                if existing != code or not same_meta:
                    # Extend ID using more digest bytes until unique
                    digest = hashlib.sha256(json.dumps({'code': code, 'args': args, 'stdin': stdin}, ensure_ascii=False, separators=(',', ':'), sort_keys=True).encode('utf-8')).digest()
                    for n in range(10, 17):  # up to ~128 bits
                        sid2 = base64.urlsafe_b64encode(digest[:n]).decode('ascii').rstrip('=')
                        path2 = self._share_path(sid2)
                        if not os.path.exists(path2):
                            sid, path = sid2, path2
                            mpath = self._share_meta_path(sid2)
                            break
                    else:
                        # Fallback to time-based suffix
                        suffix = base64.urlsafe_b64encode(os.urandom(6)).decode('ascii').rstrip('=')
                        sid = f'{sid}-{suffix}'
                        path = self._share_path(sid)
                        mpath = self._share_meta_path(sid)
            except Exception:
                pass

        # Write file if not exists
        if not os.path.exists(path):
            try:
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(code)
            except Exception as e:
                return self._send_json({'error': f'write failed: {e}'}, 500)

        # Write/update metadata JSON alongside (args/stdin)
        try:
            with open(mpath, 'w', encoding='utf-8') as mf:
                json.dump({'args': args, 'stdin': stdin}, mf, ensure_ascii=False)
        except Exception:
            # Non-fatal: sharing still works with just code
            pass

        host = self.headers.get('Host') or f'localhost:{PORT}'
        base = f'http://{host}'
        return self._send_json({
            'id': sid,
            'url': f'{base}/s/{sid}',
            'raw_url': f'{base}/api/share?id={urllib.parse.quote(sid)}',
        })

    def _api_share_get(self, sid: str):
        if not self._is_valid_share_id(sid):
            return self._send_json({'error':'invalid id'}, 400)
        path = self._share_path(sid)
        mpath = self._share_meta_path(sid)
        if not os.path.isfile(path):
            return self._send_json({'error':'not found'}, 404)
        try:
            # If metadata exists, return a JSON bundle { code, args, stdin }
            if os.path.isfile(mpath):
                with open(path, 'r', encoding='utf-8') as f:
                    code = f.read()
                meta = {'args': '', 'stdin': ''}
                try:
                    with open(mpath, 'r', encoding='utf-8') as mf:
                        m = json.load(mf)
                        if isinstance(m, dict):
                            meta['args'] = m.get('args','')
                            meta['stdin'] = m.get('stdin','')
                except Exception:
                    pass
                return self._send_json({'code': code, 'args': meta['args'], 'stdin': meta['stdin']})
            # Otherwise return raw code as text
            with open(path, 'rb') as f:
                data = f.read()
            return self._send_bytes(data, 'text/plain; charset=utf-8', headers={'Cache-Control':'public, max-age=31536000, immutable'})
        except Exception as e:
            return self._send_json({'error': str(e)}, 500)


def main():
    os.chdir(STATIC_DIR)
    with socketserver.TCPServer(('', PORT), Handler) as httpd:
        print(f'Serving on http://localhost:{PORT}')
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
