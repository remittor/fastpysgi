import os
import sys
import subprocess

from fastpysgi import __version__ as fastpysgi_version


def call_fastwsgi(parameters = None, arguments = None, envs = None):
    arguments = arguments or []
    parameters = parameters or []
    envs = envs or {}
    env = os.environ.copy()
    env.update(envs)

    proc = subprocess.run(
        ["fastpysgi"] + arguments + parameters,
        capture_output=True,
        text=True,
        env=env,
    )

    class Result:
        exit_code = proc.returncode
        output = proc.stdout + proc.stderr

    return Result()


class TestCLI:
    def test_help(self):
        result = call_fastwsgi(parameters=["--help"])
        assert result.exit_code == 0
        assert "Run FastPySGI server from CLI" in result.output

    def test_version(self):
        result = call_fastwsgi(parameters=["--version"])
        assert result.exit_code == 0
        assert result.output.strip() == fastpysgi_version

    def test_run_from_cli_invalid_module(self):
        result = call_fastwsgi(arguments=["module:wrong"])
        assert result.exit_code == 1
        assert "Error importing WSGI app: No module named 'module'" in result.output
