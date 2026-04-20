import os
import sys
import signal
import argparse
import _fastpysgi

__version__ = '0.5'

LL_DISABLED    = 0
LL_FATAL_ERROR = 1
LL_CRIT_ERROR  = 2
LL_ERROR       = 3
LL_WARNING     = 4
LL_NOTICE      = 5
LL_INFO        = 6
LL_DEBUG       = 7
LL_TRACE       = 8

class _Server():
    def __init__(self):
        self.app = None
        self.def_host = "0.0.0.0"
        self.def_port = 5000
        self.bindlist = [ ( self.def_host, self.def_port ) ]
        self.backlog = 2048
        self.loglevel = LL_ERROR
        self.hook_sigint = 1            # 0 = ignore Ctrl-C; 1 = stop server on Ctrl-C; 2 = halt process on Ctrl-C
        self.allow_keepalive = True
        self.add_header_date = True
        self.add_header_server = "FastPySGI/{}".format(__version__)
        self.max_content_length = None  # def value: 10_000_000 bytes (request body size limit)
        self.max_chunk_size = None      # def value: 256 KiB (WSGI: size of chunk that is used to read the response body of APP)
        self.read_buffer_size = None    # def value: 64 KiB; min value: 32KiB; for TLS connection double size is allocated
        self.tcp_nodelay = 0            # 0 = Nagle's algo enabled; 1 = Nagle's algo disabled;
        self.tcp_keepalive = 0          # -1 = disabled; 0 = system default; 1...N = timeout in seconds
        self.tcp_send_buf_size = 0      # 0 = system default; 1...N = size in bytes
        self.tcp_recv_buf_size = 0      # 0 = system default; 1...N = size in bytes
        self.nowait = 0
        self.num_workers = 1
        self.worker_list = [ ]
        self.loop = None                # ASGI: borrowed aio loop (None = new_event_loop)
        self.loop_timeout = 3           # ASGI: timeout for CPU relax (millisec)
        self.lifespan = 2               # ASGI: 0 = off, 1 = on, 2 = auto
        self.lifespan_fose = 0          # ASGI: 0 = log and continue, 1 = server abort (fail_on_startup_error)
        self.req_hdr_lower = 1          # ASGI: 0 = not change case for header names, 1 = force lowercase
        self.resp_hdr_lower = None      # None = default mode, 0 = not change case for header names, 1 = force lowercase
        self.root_path = None           # root path for requests; None value equ '/'
        self.tls_list = None            # list of tuple ( certfile, keyfile, ca_certs )

    def check_version(self):
        so_ver = _fastpysgi.get_version()
        if so_ver != __version__:
            raise Exception(f'Incorrect version of module "_fastpysgi" = {so_ver} (expected: {__version__})')

    def check_bindlist(self):
        if not self.bindlist:
            raise Exception("Param bindlist is empty!")
        if not isinstance(self.bindlist, list) or not all(isinstance(item, tuple) for item in self.bindlist):
            raise Exception("Param bindlist is incorrect!")

    def check_ssl_params(self):
        if self.tls_list is None:
            return
        if not isinstance(self.tls_list, list) or not self.tls_list:
            raise Exception("Param tls_list is incorrect!")
        for idx, srv in enumerate(self.bindlist):
            if idx < len(self.tls_list):
                tls = self.tls_list[idx]
                if tls is not None:
                    if not isinstance(tls, tuple) or len(tls) < 1:
                        raise Exception("Param tls_list is incorrect!")
                    if not os.path.isfile(tls[0]):
                        raise FileNotFoundError(f'TLS certfile not found: {tls[0]!r}')
                    if len(tls) > 1 and tls[1] is not None and not os.path.isfile(tls[1]):
                        raise FileNotFoundError(f'TLS keyfile not found: {tls[1]!r}')
                    if len(tls) > 2 and tls[2] is not None and not os.path.isfile(tls[2]):
                        raise FileNotFoundError(f'TLS ca-certs not found: {tls[2]!r}')

    def check_all(self):
        self.check_version()
        self.check_bindlist()
        self.check_ssl_params()

    def delete_all_binds(self):
        self.bindlist = [ ]
        self.tls_list = None

    def add_bind(self, host = None, port = None, tls = None):
        if not host or not port:
            raise ValueError("Incorrect host or port!")
        self.bindlist.append( ( host, port ) )
        if not self.tls_list:
            self.tls_list = [ ]
        self.tls_list.append( tls )

    def init(self, app, host = None, port = None, loglevel = None, workers = None):
        self.app = app
        if isinstance(host, list):
            self.bindlist = [ ]
            if len(host) < 1 or port is not None:
                raise Exception("Incorrect host/port arguments!")
            for bind in host:
                self.bindlist.append( ( bind[0], bind[1] ) )
        elif isinstance(host, str) or isinstance(port, int):
            self.bindlist = [ ( host if host else self.def_host, port if port > 0 else self.def_port ) ]
        self.check_all()
        self.loglevel = loglevel if loglevel is not None else self.loglevel
        self.num_workers = workers if workers is not None else self.num_workers
        if self.num_workers > 1:
            return 0
        return _fastpysgi.init_server(self)

    def set_allow_keepalive(self, value):
        self.allow_keepalive = value
        _fastpysgi.change_setting(self, "allow_keepalive")

    def run(self):
        self.check_all()
        if self.nowait:
            if self.num_workers > 1:
                raise Exception('Incorrect server options')
            return _fastpysgi.run_nowait(self)
        if self.num_workers > 1:
            return self.multi_run()
        ret = _fastpysgi.run_server(self)
        self.close()
        return ret
        
    def close(self):
        return _fastpysgi.close_server(self)

    def multi_run(self, num_workers = None):
        if num_workers is not None:
            self.num_workers = num_workers
        for _ in range(self.num_workers):
            pid = os.fork()
            if pid > 0:
                self.worker_list.append(pid)
                print(f"Worker process added with PID: {pid}")
                continue
            try:
                _fastpysgi.init_server(self)
                _fastpysgi.run_server(self)
            except KeyboardInterrupt:
                pass
            sys.exit(0)
        try:
            for _ in range(self.num_workers):
                os.wait()
        except KeyboardInterrupt:
            print("\n" + "Stopping all workers")
            for worker in self.worker_list:
                os.kill(worker, signal.SIGINT)
        return 0

    def get_bind_addr(self, idx = 0):
        scheme = 'http'
        if self.tls_list and len(self.tls_list) > idx and self.tls_list[idx]:
            if self.tls_list[idx][0]:
                scheme = 'https'
        host = self.bindlist[idx][0]
        port = self.bindlist[idx][1]
        return f'{scheme}://{host}:{port}'

server = _Server()

# -------------------------------------------------------------------------------------

def import_from_string(import_str):
    import importlib
    import importlib.util
    module_str, _, attrs_str = import_str.partition(":")
    if not module_str or not attrs_str:
        raise ImportError("Import string should be in the format <module>:<attribute>")

    try:
        relpath = f"{module_str}.py"
        if os.path.isfile(relpath):
            spec = importlib.util.spec_from_file_location(module_str, relpath)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)

        else:
            module = importlib.import_module(module_str)

        for attr_str in attrs_str.split("."):
            module = getattr(module, attr_str)
    except AttributeError:
        raise ImportError(f'Attribute "{attrs_str}" not found in module "{module_str}"')

    return module

# -------------------------------------------------------------------------------------

def run_from_cli():
    parser = argparse.ArgumentParser(
        prog="fastpysgi",
        description="Run FastPySGI server from CLI",
    )
    parser.add_argument(
        "--version",
        action="version",
        version=__version__,
    )
    parser.add_argument(
        "app_import_string",
        metavar="APP",
        type=str,
        help="WSGI/ASGI app in the format module:attribute (e.g. myapp:app)",
    )
    parser.add_argument(
        "--host",
        type=str,
        default=server.def_host,
        help=f"Host the socket is bound to (default: {server.def_host})",
    )
    parser.add_argument(
        "-p", "--port",
        type=int,
        default=server.def_port,
        help=f"Port the socket is bound to (default: {server.def_port})",
    )
    parser.add_argument(
        "-l", "--loglevel",
        type=int,
        default=server.loglevel,
        help=f"Logging level 0..8 (default: {server.loglevel})",
    )
    parser.add_argument(
        "--certfile",
        type=str,
        default=None,
        help="Path to the TLS certificate PEM file (enables HTTPS)",
    )
    parser.add_argument(
        "--keyfile",
        type=str,
        default=None,
        help="Path to the PEM file of the private TLS key",
    )
    parser.add_argument(
        "--ca-certs",
        type=str,
        default=None,
        help="Path to CA bundle for validating client certificates",
    )

    args = parser.parse_args()

    try:
        app = import_from_string(args.app_import_string)
    except ImportError as e:
        print(f"Error importing WSGI app: {e}", file=sys.stderr)
        sys.exit(1)

    if args.certfile:
        server.tls_list = [ ( args.certfile, args.keyfile, args.ca_certs ) ]

    server.init(app, args.host, args.port, args.loglevel)
    print(f"FastPySGI server listening at", server.get_bind_addr())
    server.run()

# -------------------------------------------------------------------------------------

def run(app = None, host = None, port = None, loglevel = None, workers = None, wsgi_app = None):
    if app and wsgi_app:
        raise Exception("It is not allowed to specify several applications at once.")
    if app is None:
        app = wsgi_app
    if app is None:
        raise Exception("app not specify.")
    server.init(app, host, port, loglevel, workers)
    addon = " multiple workers" if server.num_workers > 1 else ""
    print(f"FastPySGI server{addon} listening at", server.get_bind_addr())
    print(f"FastPySGI server running on PID: {os.getpid()}")
    server.run()
