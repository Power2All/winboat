# httpput.py <port> <file>: receives one HTTP PUT on 127.0.0.1:<port> into <file>, then exits (sp.sh's SP_HTTP --get)
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        n = int(self.headers['Content-Length'])
        with open(sys.argv[2], 'wb') as f:
            while n > 0:
                b = self.rfile.read(min(n, 1 << 20)); f.write(b); n -= len(b)
        self.send_response(200); self.end_headers()
    def log_message(self, *a): pass
http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H).handle_request()
