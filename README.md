<p align="center"><img src="https://raw.githubusercontent.com/remittor/fastpysgi/refs/heads/master/logo.png"></p>

--------------------------------------------------------------------
[![Pypi](https://img.shields.io/pypi/v/fastpysgi.svg?style=flat)](https://pypi.python.org/pypi/fastpysgi)
[![Python 3.8-3.14](https://img.shields.io/badge/python-3.8%20%7C%203.14-blue?style=flat)](https://pypi.python.org/pypi/fastpysgi)
[![Github All Releases](https://img.shields.io/github/downloads/remittor/fastpysgi/total.svg)](https://github.com/remittor/fastpysgi/releases)
[![Github Latest Release](https://img.shields.io/github/downloads/remittor/fastpysgi/latest/total.svg)](https://github.com/remittor/fastpysgi/releases)
[![ViewCount](https://views.whatilearened.today/views/github/remittor/fastpysgi.svg)](https://github.com/remittor/fastpysgi)
[![Donations Page](https://github.com/andry81-cache/gh-content-static-cache/raw/master/common/badges/donate/donate.svg)](https://github.com/remittor/donate)

# FastPySGI

FastPySGI is an ultra fast WSGI/ASGI server for Python 3.

Its written in C and uses [libuv](https://github.com/libuv/libuv) and [llhttp](https://github.com/nodejs/llhttp) under the hood for blazing fast performance.


## Dependencies

None


## Supported Platforms

| Platform       | Linux | MacOs | Windows |
| :------------: | :---: | :---: | :-----: |
| <b>Support</b> | ✅    |  ✅  |  ✅     |


## Performance

FastPySGI is one of the fastest general use WSGI/ASGI servers out there!

Benchmark results: [www.http-arena.com/leaderboard](https://www.http-arena.com/leaderboard/#v=h1iso&type=all,engine,infrastructure,production,tuned&lang=Python)

For a comparison against other popular WSGI servers, see [PERFORMANCE.md](./performance_benchmarks/PERFORMANCE.md)


## Installation

Install using the [pip](https://pip.pypa.io/en/stable/) package manager.

```bash
pip install fastpysgi
```


## Quick start

Create a new file `example.py` with the following:

```python
import fastpysgi

def app(environ, start_response):
    headers = [ ('Content-Type', 'text/plain') ]
    start_response('200 OK', headers)
    return [ b'Hello, World!\n' ]

if __name__ == '__main__':
    fastpysgi.run(app, host='0.0.0.0', port=5000)
```

Run the server using:

```bash
python3 example.py
```

Or, by using the `fastpysgi` command:

```bash
fastpysgi example:app
```


## Example usage with Flask

```python
import fastpysgi
from flask import Flask

app = Flask(__name__)

@app.get('/')
def hello_world():
    return 'Hello, World!\n', 200

if __name__ == '__main__':
    fastpysgi.run(app, host='127.0.0.1', port=5000)
```


## Example usage with FastAPI

```python
import fastpysgi
from fastapi import FastAPI
from fastapi.responses import PlainTextResponse

app = FastAPI()

@app.get("/")
async def hello_world():
    return PlainTextResponse(b"Hello, World!\n")

if __name__ == '__main__':
    fastpysgi.run(app, host='127.0.0.1', port=5000)
```


## Testing

To run the test suite using [pytest](https://docs.pytest.org/en/latest/getting-started.html), run the following command:

```bash
python3 -m pytest
```

Results of testing for compliance with HTTP/1.x standards: https://www.http-probe.com/probe-results/
