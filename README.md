<p align="center"><img src="./logo.png"></p>

--------------------------------------------------------------------
[![Pypi](https://img.shields.io/pypi/v/fastpysgi.svg?style=flat)](https://pypi.python.org/pypi/fastpysgi)


# FastPySGI

FastPySGI is an ultra fast WSGI/ASGI server for Python 3.

Its written in C and uses [libuv](https://github.com/libuv/libuv) and [llhttp](https://github.com/nodejs/llhttp) under the hood for blazing fast performance.


## Supported Platforms

| Platform | Linux | MacOs | Windows |
| :------: | :---: | :---: | :-----: |
| <b>Support</b>  | :white_check_mark: |  :white_check_mark: |  :white_check_mark: |


## Performance

FastPySGI is one of the fastest general use WSGI servers out there!

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
    headers = [('Content-Type', 'text/plain')]
    start_response('200 OK', headers)
    return [b'Hello, World!']

if __name__ == '__main__':
    fastpysgi.run(wsgi_app=app, host='0.0.0.0', port=5000)
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
    return 'Hello, World!', 200

if __name__ == '__main__':
    fastpysgi.run(wsgi_app=app, host='127.0.0.1', port=5000)
```


## Testing

To run the test suite using [pytest](https://docs.pytest.org/en/latest/getting-started.html), run the following command:

```bash
python3 -m pytest
```
