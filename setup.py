import os
import sys
import glob
import subprocess
import platform
import re
from setuptools import setup
from distutils.core import Extension
from distutils.command.build_ext import build_ext
import setup_libuv

SOURCES = glob.glob("fastwsgi/*.c") + glob.glob("llhttp/src/*.c")

module = Extension(
    "_fastpysgi",
    sources=SOURCES,
    include_dirs=["llhttp/include", "libuv/include"],
)

def get_version():
    with open("fastpysgi.py", "r", encoding = "utf-8") as file:
        content = file.read()
    match = re.search(r'^__version__\s*=\s*["\']([^"\']+)["\']', content, re.M)
    if match:
        return match.group(1)
    raise RuntimeError("Cannot find __version__ in fastpysgi.py")

def get_compiler_version(exe_name):
    version = None
    ver_major = None
    ver_minor = None
    try:
        res = subprocess.run([exe_name, '--version'], stdout=subprocess.PIPE)
        if res.returncode == 0:
            version = res.stdout.decode('utf8')
            ver_s = re.search(r"[\D+](\d+)[.](\d+)[\D+]", version)
            if ver_s:
                ver_major = int(ver_s.group(1))
                ver_minor = int(ver_s.group(2))
    except:
        pass
    return version, ver_major, ver_minor


current_compiler = os.getenv('CC', "")

# Forced use clang compiler, if installed
if platform.system() == "Linux" and current_compiler == "":
    for cc_ver in range(40, 7, -1):
        cc = 'clang-%d' % cc_ver
        text, ver_major, ver_minor = get_compiler_version(cc)
        if text and ver_major:
            current_compiler = cc
            print('Change current compiler to {}'.format(cc))
            os.environ['CC'] = cc
            break


class build_all(build_ext):
    def initialize_options(self):
        build_ext.initialize_options(self)

    def build_extensions(self):
        global module

        setup_libuv.build_libuv(self)

        compiler_type = self.compiler.compiler_type
        print("Current compiler type:", compiler_type)
        try:
            compiler_cmd = self.compiler.compiler
            print('Current compiler: {}'.format(compiler_cmd))
        except:
            compiler_cmd = None
        
        compiler = ""
        if compiler_cmd:
            compiler = compiler_cmd[0]
        if compiler_type == "msvc":
            compiler = "msvc"
        
        compiler_ver_major = 0
        if compiler_type == 'unix' and compiler:
            version, c_ver_major, c_ver_minor = get_compiler_version(compiler)
            print('Current compiler version: {}.{}'.format(c_ver_major, c_ver_minor))
            if c_ver_major:
                compiler_ver_major = c_ver_major

        for ext in self.extensions:
            if ext == module:
                if compiler_type == 'msvc':
                    ext.extra_compile_args = [ '/Oi', '/Oy-', '/W3', '/WX-', '/Gd', '/GS' ]
                    ext.extra_compile_args += [ '/Zc:forScope', '/Zc:inline', '/fp:precise', '/analyze-' ]
                else:
                    ext.extra_compile_args = [ "-O3", "-fno-strict-aliasing", "-fcommon", "-g", "-Wall" ]
                    ext.extra_compile_args += [ "-Wno-unused-function", "-Wno-unused-variable" ]
                    if compiler.startswith("gcc") and compiler_ver_major >= 8:
                        ext.extra_compile_args += [ "-Wno-unused-but-set-variable" ]
                    if compiler.startswith("clang") and compiler_ver_major >= 13:
                        ext.extra_compile_args += [ "-Wno-unused-but-set-variable" ]
        
        build_ext.build_extensions(self)


setup(
    version = get_version(),
    ext_modules = [ module ],
    cmdclass = { "build_ext": build_all },
)
